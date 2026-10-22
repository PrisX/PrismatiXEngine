#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::native {

// One platform's library as an extension manifest describes it (`native`).
struct NativeLibrarySpec {
    struct File {
        // Relative to the manifest (`native/<platform>/<name>`).
        std::string path;
        std::string sha256;
    };
    std::string extensionId;
    std::string version;
    std::string platform;
    std::uint32_t abi = 0;
    std::vector<std::string> services;
    // Uncompressed P-256 point, lowercase hex.
    std::string publicKey;
    std::string signature;
    // The library first, then the files it loads from its folder.
    std::vector<File> files;
};

// The platform this Player loads native libraries for, or empty (the Web
// Player, Android and the WASM Preview load none).
[[nodiscard]] std::string_view CurrentPlatform();

// The text a library's signature covers (the SDK's nativeSignatureMessage).
[[nodiscard]] std::string SignatureMessage(const NativeLibrarySpec& spec);

struct NativeFailure {
    // PXJS7901: unavailable, PXJS7902: the service failed, PXJS7903: the
    // library cannot be verified or loaded.
    std::string code;
    std::string message;
};

// A verified, loaded and initialized library and the services it
// registered. Destroying it calls px_plugin_shutdown and unloads it.
class NativePluginHost {
public:
    using Log = std::function<void(int level, const std::string& message)>;

    // Finds the library in `<directory>/<extension id>/` of the first
    // directory that has it, checks every file's SHA-256 and the signature,
    // then loads it and runs px_plugin_init.
    static std::unique_ptr<NativePluginHost> Load(
        const NativeLibrarySpec& spec,
        const std::vector<std::string>& directories, Log log,
        NativeFailure& failure);

    ~NativePluginHost();
    NativePluginHost(const NativePluginHost&) = delete;
    NativePluginHost& operator=(const NativePluginHost&) = delete;

    [[nodiscard]] std::vector<std::string> Services() const;
    [[nodiscard]] bool Has(std::string_view service) const;
    // Calls the service on this (the game) thread.
    std::optional<std::vector<std::uint8_t>> Request(
        std::string_view service, const std::uint8_t* data, std::size_t size,
        NativeFailure& failure);
    // The messages the library posted for `service`, oldest first.
    std::vector<std::vector<std::uint8_t>> TakeMessages(std::string_view service);

    struct Service {
        void* handler = nullptr;
        void* userData = nullptr;
    };
    struct State;

private:
    NativePluginHost();
    std::unique_ptr<State> m_state;
};

}  // namespace px::native
