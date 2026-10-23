#include "Engine/Native/NativePluginHost.h"

#include "Engine/Native/PxPluginAbi.h"
#include "Engine/SDK/ContractVersions.h"

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <optional>
#include <string>

#if !defined(__EMSCRIPTEN__)
#define PRISMATIX_NATIVE_PLUGINS 1
#include "Engine/IO/Crypto.h"
#include "Engine/IO/OverlaySignature.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <array>
#include <fstream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif
#endif

struct PxResponse {
    std::vector<std::uint8_t> data;
    bool responded = false;
    // Set by respond_error: the request fails with this message.
    std::optional<std::string> error;
};

namespace px::native {

std::string_view CurrentPlatform() {
#if !defined(PRISMATIX_NATIVE_PLUGINS)
    return {};
#elif defined(__ANDROID__) && defined(__aarch64__)
    return "android-arm64";
#elif defined(__ANDROID__)
    return {};
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "win32-x64";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin-x64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    return {};
#endif
}

std::string SignatureMessage(const NativeLibrarySpec& spec) {
    std::vector<std::string> services = spec.services;
    std::ranges::sort(services);
    std::string joined;
    for (const auto& service : services) {
        if (!joined.empty()) joined += ',';
        joined += service;
    }
    std::string message = "PrismatiX native plugin v1\n";
    message += "id " + spec.extensionId + "\n";
    message += "version " + spec.version + "\n";
    message += "platform " + spec.platform + "\n";
    message += "abi " + std::to_string(spec.abi) + "\n";
    message += "services " + joined + "\n";
    for (const auto& file : spec.files)
        message += "file " + file.sha256 + " " + file.path + "\n";
    return message;
}

struct NativePluginHost::State {
    std::string extensionId;
    std::string engineVersion;
    std::string platform;
    std::string pluginDirectory;
    std::vector<std::string> declared;
    std::map<std::string, Service, std::less<>> services;
    Log log;
    PxHostApi api{};
    bool initializing = false;
    void* library = nullptr;
    PxPluginShutdownFn shutdown = nullptr;
    std::mutex messagesMutex;
    bool open = true;
    std::map<std::string, std::deque<std::vector<std::uint8_t>>, std::less<>> messages;
};

namespace {

// The host functions exist only where libraries load (not on the Web).
#if defined(PRISMATIX_NATIVE_PLUGINS)

constexpr std::size_t kMaxLibraryBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaxQueuedMessages = 256;

NativePluginHost::State* StateOf(void* host) {
    return static_cast<NativePluginHost::State*>(host);
}

PxStatus RegisterService(void* host, const char* name, PxServiceHandler handler,
                         void* userData) {
    auto* state = StateOf(host);
    if (!state || !name || !handler) return PX_ERROR_INVALID_ARGUMENT;
    if (!state->initializing) return PX_ERROR_STATE;
    if (std::ranges::find(state->declared, std::string_view(name)) == state->declared.end())
        return PX_ERROR_NOT_DECLARED;
    state->services.insert_or_assign(
        std::string(name),
        NativePluginHost::Service{reinterpret_cast<void*>(handler), userData});
    return PX_OK;
}

PxStatus Respond(PxResponse* response, const std::uint8_t* data, const std::size_t size) {
    if (!response) return PX_ERROR_INVALID_ARGUMENT;
    if (response->responded) return PX_ERROR_STATE;
    if (size > PX_PLUGIN_MAX_BUFFER) return PX_ERROR_TOO_LARGE;
    if (size > 0 && !data) return PX_ERROR_INVALID_ARGUMENT;
    response->data.assign(data, data + size);
    response->responded = true;
    return PX_OK;
}

PxStatus RespondError(PxResponse* response, const char* message) {
    if (!response || !message) return PX_ERROR_INVALID_ARGUMENT;
    if (response->responded) return PX_ERROR_STATE;
    std::string text(message, ::strnlen(message, PX_PLUGIN_MAX_ERROR_MESSAGE));
    // Never cut a UTF-8 sequence in half.
    if (text.size() == PX_PLUGIN_MAX_ERROR_MESSAGE) {
        std::size_t end = text.size();
        while (end > 0 && (static_cast<unsigned char>(text[end - 1]) & 0xc0) == 0x80) --end;
        if (end > 0 && static_cast<unsigned char>(text[end - 1]) >= 0xc0) text.resize(end - 1);
    }
    response->error = std::move(text);
    response->responded = true;
    return PX_OK;
}

PxStatus PostServiceMessage(void* host, const char* service, const std::uint8_t* data,
                     const std::size_t size) {
    auto* state = StateOf(host);
    if (!state || !service || (size > 0 && !data)) return PX_ERROR_INVALID_ARGUMENT;
    if (size > PX_PLUGIN_MAX_BUFFER) return PX_ERROR_TOO_LARGE;
    if (std::ranges::find(state->declared, std::string_view(service)) == state->declared.end())
        return PX_ERROR_NOT_DECLARED;
    const std::lock_guard lock(state->messagesMutex);
    if (!state->open) return PX_ERROR_STATE;
    auto& queue = state->messages[std::string(service)];
    if (queue.size() >= kMaxQueuedMessages) return PX_ERROR_TOO_LARGE;
    queue.emplace_back(data, data + size);
    return PX_OK;
}

void LogMessage(void* host, const std::int32_t level, const char* message) {
    auto* state = StateOf(host);
    if (!state || !message || !state->log) return;
    state->log(level, "[" + state->extensionId + "] " + message);
}

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string FileName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Reads a file while keeping it open (on Windows, shared for reading only, so
// it cannot be replaced between the hash check and LoadLibrary).
class VerifiedFile {
public:
    VerifiedFile() = default;
    VerifiedFile(const VerifiedFile&) = delete;
    VerifiedFile& operator=(const VerifiedFile&) = delete;
    VerifiedFile(VerifiedFile&& other) noexcept { *this = std::move(other); }
    VerifiedFile& operator=(VerifiedFile&& other) noexcept {
#if defined(_WIN32)
        std::swap(m_handle, other.m_handle);
#else
        (void)other;
#endif
        return *this;
    }
    ~VerifiedFile() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#endif
    }

    bool Open(const std::filesystem::path& path, std::string& contents) {
#if defined(_WIN32)
        m_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(m_handle, &size) || size.QuadPart < 0 ||
            static_cast<unsigned long long>(size.QuadPart) > kMaxLibraryBytes)
            return false;
        contents.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < contents.size()) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1U << 24));
            if (!ReadFile(m_handle, contents.data() + offset, chunk, &read, nullptr) || read == 0)
                return false;
            offset += read;
        }
        return true;
#else
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return false;
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        if (size < 0 || static_cast<unsigned long long>(size) > kMaxLibraryBytes) return false;
        contents.resize(static_cast<std::size_t>(size));
        stream.seekg(0, std::ios::beg);
        return static_cast<bool>(stream.read(contents.data(), static_cast<std::streamsize>(contents.size())));
#endif
    }

private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#endif
};

void* OpenLibrary(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) error = "LoadLibrary failed with error " + std::to_string(GetLastError());
    return module;
#else
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        const char* message = dlerror();
        error = message ? message : "dlopen failed";
    }
    return module;
#endif
}

void* Symbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void CloseLibrary(void* library) {
    if (!library) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

struct CloseStream {
    void operator()(SDL_IOStream* stream) const { SDL_CloseIO(stream); }
};

// Copies `source` (opened with SDL, so on Android a relative path reads the
// APK's assets) to `target`, unless `target` already holds the same bytes.
bool MirrorFile(const std::string& source, const std::filesystem::path& target, std::string& error) {
    std::unique_ptr<SDL_IOStream, CloseStream> input(SDL_IOFromFile(source.c_str(), "rb"));
    if (!input) {
        error = "it is not in the package (" + std::string(SDL_GetError()) + ")";
        return false;
    }
    const Sint64 size = SDL_GetIOSize(input.get());
    if (size < 0 || static_cast<unsigned long long>(size) > kMaxLibraryBytes) {
        error = "its size cannot be read or is over 256 MiB";
        return false;
    }
    std::vector<char> buffer(1U << 16);
    std::error_code code;
    if (std::filesystem::is_regular_file(target, code) &&
        std::filesystem::file_size(target, code) == static_cast<std::uintmax_t>(size) && !code) {
        std::ifstream existing(target, std::ios::binary);
        std::vector<char> held(buffer.size());
        bool same = static_cast<bool>(existing);
        while (same) {
            const std::size_t read = SDL_ReadIO(input.get(), buffer.data(), buffer.size());
            if (read == 0) {
                same = SDL_GetIOStatus(input.get()) == SDL_IO_STATUS_EOF;
                break;
            }
            existing.read(held.data(), static_cast<std::streamsize>(read));
            same = static_cast<std::size_t>(existing.gcount()) == read &&
                   std::memcmp(buffer.data(), held.data(), read) == 0;
        }
        if (same) return true;
        if (SDL_SeekIO(input.get(), 0, SDL_IO_SEEK_SET) != 0) {
            error = "it cannot be read again (" + std::string(SDL_GetError()) + ")";
            return false;
        }
    }
    auto partial = target;
    partial += ".partial";
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        bool written = static_cast<bool>(output);
        while (written) {
            const std::size_t read = SDL_ReadIO(input.get(), buffer.data(), buffer.size());
            if (read == 0) {
                written = SDL_GetIOStatus(input.get()) == SDL_IO_STATUS_EOF;
                break;
            }
            written = static_cast<bool>(output.write(buffer.data(), static_cast<std::streamsize>(read)));
        }
        output.close();
        if (!written || !output) {
            std::filesystem::remove(partial, code);
            error = "it cannot be written to its folder";
            return false;
        }
    }
#if defined(_WIN32)
    std::filesystem::remove(target, code);
#endif
    std::filesystem::rename(partial, target, code);
    if (code) {
        std::filesystem::remove(partial, code);
        error = "it cannot be moved into place";
        return false;
    }
#if !defined(_WIN32)
    // Read-only, as Android asks of code an app loads from its own storage.
    std::filesystem::permissions(target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, code);
#endif
    return true;
}

#endif

}  // namespace

bool CopyBundledFiles(const NativeLibrarySpec& spec, const std::string_view assetFolder,
                      const std::string& directory, NativeFailure& failure) {
    const auto fail = [&failure, &spec](std::string message) {
        failure = {"PXJS7903", "Native library of '" + spec.extensionId + "' (" +
                                   spec.platform + "): " + std::move(message)};
        return false;
    };
#if !defined(PRISMATIX_NATIVE_PLUGINS)
    (void)assetFolder;
    (void)directory;
    return fail("this Player does not load native libraries");
#else
    std::vector<std::string> names;
    for (const auto& file : spec.files) names.push_back(FileName(file.path));
    names.insert(names.end(), spec.userFiles.begin(), spec.userFiles.end());
    const auto folder = Utf8Path(directory) / Utf8Path(spec.extensionId);
    std::error_code code;
    std::filesystem::create_directories(folder, code);
    if (code) return fail("the folder for its files cannot be created: " + code.message());
    for (const auto& name : names) {
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
            return fail("the file name '" + name + "' is not a plain file name");
        std::string error;
        if (!MirrorFile(std::string(assetFolder) + "/" + spec.extensionId + "/" + name,
                        folder / Utf8Path(name), error))
            return fail(name + " cannot be copied out of the package: " + error);
    }
    return true;
#endif
}

std::string BundledPluginDirectory() {
#if defined(__ANDROID__)
    // The code cache: private to the app, never backed up, and emptied when
    // the app is updated, so the next start copies the new files.
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(env ? SDL_GetAndroidActivity() : nullptr);
    if (!activity) return {};
    const auto cleared = [env] {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    };
    std::string path;
    jclass type = env->GetObjectClass(activity);
    jmethodID getCodeCacheDir = env->GetMethodID(type, "getCodeCacheDir", "()Ljava/io/File;");
    jobject file = getCodeCacheDir ? env->CallObjectMethod(activity, getCodeCacheDir) : nullptr;
    if (cleared()) file = nullptr;
    if (file) {
        jclass fileType = env->GetObjectClass(file);
        jmethodID getAbsolutePath = env->GetMethodID(fileType, "getAbsolutePath", "()Ljava/lang/String;");
        auto text = static_cast<jstring>(getAbsolutePath ? env->CallObjectMethod(file, getAbsolutePath) : nullptr);
        if (cleared()) text = nullptr;
        if (text) {
            if (const char* chars = env->GetStringUTFChars(text, nullptr)) {
                path = chars;
                env->ReleaseStringUTFChars(text, chars);
            }
            env->DeleteLocalRef(text);
        }
        env->DeleteLocalRef(fileType);
        env->DeleteLocalRef(file);
    }
    cleared();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return path.empty() ? path : path + "/Plugins";
#else
    return {};
#endif
}

NativePluginHost::NativePluginHost() : m_state(std::make_unique<State>()) {}

NativePluginHost::~NativePluginHost() {
    {
        const std::lock_guard lock(m_state->messagesMutex);
        m_state->open = false;
    }
#if defined(PRISMATIX_NATIVE_PLUGINS)
    if (m_state->shutdown) m_state->shutdown();
    CloseLibrary(m_state->library);
#endif
}

std::unique_ptr<NativePluginHost> NativePluginHost::Load(
    const NativeLibrarySpec& spec, const std::vector<std::string>& directories,
    Log log, NativeFailure& failure) {
    const auto fail = [&failure, &spec](std::string message) {
        failure = {"PXJS7903", "Native library of '" + spec.extensionId + "' (" +
                                   spec.platform + "): " + std::move(message)};
        return std::unique_ptr<NativePluginHost>{};
    };
#if !defined(PRISMATIX_NATIVE_PLUGINS)
    (void)directories;
    (void)log;
    return fail("this Player does not load native libraries");
#else
    if (CurrentPlatform().empty() || spec.platform != CurrentPlatform())
        return fail("this Player runs on " + std::string(CurrentPlatform()));
    if (spec.abi != PX_PLUGIN_ABI_MAJOR)
        return fail("it is built for ABI " + std::to_string(spec.abi) +
                    "; this Player provides ABI " + std::to_string(PX_PLUGIN_ABI_MAJOR));
    if (spec.files.empty()) return fail("the manifest lists no library");
    if (spec.signature.empty() || spec.publicKey.empty() ||
        !io::VerifyEcdsaP256Signature(SignatureMessage(spec), spec.signature, spec.publicKey))
        return fail("the signature does not verify with the manifest's publicKey");

    std::optional<std::filesystem::path> folder;
    for (const auto& directory : directories) {
        if (directory.empty()) continue;
        const auto candidate = Utf8Path(directory) / Utf8Path(spec.extensionId);
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / Utf8Path(FileName(spec.files.front().path)), error)) {
            folder = candidate;
            break;
        }
    }
    if (!folder) return fail("the library " + FileName(spec.files.front().path) +
                             " is not in any Plugins folder");
    std::vector<VerifiedFile> held;
    held.reserve(spec.files.size());
    for (const auto& file : spec.files) {
        std::string contents;
        VerifiedFile opened;
        if (!opened.Open(*folder / Utf8Path(FileName(file.path)), contents))
            return fail(FileName(file.path) + " cannot be read");
        if (crypto::Sha256Hex(contents) != file.sha256)
            return fail(FileName(file.path) + " does not match the SHA-256 its manifest pins");
        held.push_back(std::move(opened));
    }

    std::unique_ptr<NativePluginHost> host(new NativePluginHost());
    auto& state = *host->m_state;
    state.extensionId = spec.extensionId;
    state.platform = spec.platform;
    {
        const auto absolute = std::filesystem::absolute(*folder).lexically_normal().u8string();
        state.pluginDirectory.assign(reinterpret_cast<const char*>(absolute.data()), absolute.size());
        while (state.pluginDirectory.size() > 1 &&
               (state.pluginDirectory.back() == '/' || state.pluginDirectory.back() == '\\'))
            state.pluginDirectory.pop_back();
    }
    state.declared = spec.services;
    state.log = std::move(log);
    std::string error;
    state.library = OpenLibrary(*folder / Utf8Path(FileName(spec.files.front().path)), error);
    held.clear();
    if (!state.library) return fail(error);
    const auto version = reinterpret_cast<PxPluginAbiVersionFn>(Symbol(state.library, "px_plugin_abi_version"));
    const auto init = reinterpret_cast<PxPluginInitFn>(Symbol(state.library, "px_plugin_init"));
    state.shutdown = reinterpret_cast<PxPluginShutdownFn>(Symbol(state.library, "px_plugin_shutdown"));
    if (!version || !init) {
        state.shutdown = nullptr;
        return fail("it does not export px_plugin_abi_version and px_plugin_init");
    }
    const std::uint32_t built = version();
    if ((built >> 16) != PX_PLUGIN_ABI_MAJOR || (built & 0xffffU) > PX_PLUGIN_ABI_MINOR) {
        state.shutdown = nullptr;
        return fail("it was built for ABI " + std::to_string(built >> 16) + "." +
                    std::to_string(built & 0xffffU) + "; this Player provides " +
                    std::to_string(PX_PLUGIN_ABI_MAJOR) + "." + std::to_string(PX_PLUGIN_ABI_MINOR));
    }
    state.api.abi_version = PX_PLUGIN_ABI_VERSION;
    state.api.struct_size = static_cast<std::uint32_t>(sizeof(PxHostApi));
    state.api.host = &state;
    state.engineVersion = std::string(::px::sdk::kEngineVersion);
    state.api.engine_version = state.engineVersion.c_str();
    state.api.extension_id = state.extensionId.c_str();
    state.api.platform = state.platform.c_str();
    state.api.register_service = &RegisterService;
    state.api.respond = &Respond;
    state.api.post_message = &PostServiceMessage;
    state.api.log = &LogMessage;
    state.api.respond_error = &RespondError;
    state.api.plugin_directory = state.pluginDirectory.c_str();
    state.initializing = true;
    const PxStatus status = init(&state.api);
    state.initializing = false;
    if (status != PX_OK) {
        state.shutdown = nullptr;
        return fail("px_plugin_init failed with status " + std::to_string(status));
    }
    for (const auto& service : state.declared)
        if (!state.services.contains(service) && state.log)
            state.log(PX_LOG_WARNING, "[" + state.extensionId + "] the library did not register the declared service " + service);
    return host;
#endif
}

std::vector<std::string> NativePluginHost::Services() const {
    std::vector<std::string> names;
    for (const auto& [name, service] : m_state->services) names.push_back(name);
    return names;
}

bool NativePluginHost::Has(const std::string_view service) const {
    return m_state->services.find(service) != m_state->services.end();
}

std::optional<std::vector<std::uint8_t>> NativePluginHost::Request(
    const std::string_view service, const std::uint8_t* data, const std::size_t size,
    NativeFailure& failure) {
    const auto found = m_state->services.find(service);
    if (found == m_state->services.end()) {
        failure = {"PXJS7901", "The native service '" + std::string(service) + "' is not available"};
        return std::nullopt;
    }
    if (size > PX_PLUGIN_MAX_BUFFER) {
        failure = {"PXJS7902", "A native request is at most 16 MiB"};
        return std::nullopt;
    }
    PxResponse response;
    const auto handler = reinterpret_cast<PxServiceHandler>(found->second.handler);
    static const std::uint8_t empty = 0;
    const PxStatus status = handler(found->second.userData, size == 0 ? &empty : data, size, &response);
    if (response.error) {
        failure = {"PXJS7902", "The native service '" + std::string(service) + "' failed: " + *response.error};
        return std::nullopt;
    }
    if (status != PX_OK) {
        failure = {"PXJS7902", "The native service '" + std::string(service) +
                                   "' failed with status " + std::to_string(status)};
        return std::nullopt;
    }
    return std::move(response.data);
}

std::vector<std::vector<std::uint8_t>> NativePluginHost::TakeMessages(const std::string_view service) {
    const std::lock_guard lock(m_state->messagesMutex);
    const auto queue = m_state->messages.find(service);
    if (queue == m_state->messages.end()) return {};
    std::vector<std::vector<std::uint8_t>> messages(std::make_move_iterator(queue->second.begin()),
                                                    std::make_move_iterator(queue->second.end()));
    queue->second.clear();
    return messages;
}

}  // namespace px::native
