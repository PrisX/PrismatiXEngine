#include "Engine/IO/Crypto.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/SDK/Packager.h"
#include "Tests/TestSupport/TestHarness.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void Copy(const std::filesystem::path& source,
          const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing);
}

void CopySceneAsCurrentVersion(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
    std::string scene = Read(source);
    const std::string header = "@pxscene 1 ";
    if (scene.starts_with(header)) scene.replace(0, header.size(), "@pxscene 4 ");
    const std::string schema = "uiSchemaVersion = 1";
    if (const auto offset = scene.find(schema); offset != std::string::npos) {
        scene.replace(offset, schema.size(), "uiSchemaVersion = 5");
    }
    if (const auto offset = scene.find("theme = res("); offset != std::string::npos) {
        const auto end = scene.find('\n', offset);
        scene.erase(offset, end == std::string::npos ? scene.size() - offset
                                                      : end - offset + 1);
    }
    Write(destination, scene);
}

px::sdk::PackageInput Input(const std::filesystem::path& root,
                            const std::string& uri) {
    const auto path = root / std::filesystem::path(uri);
    return {uri, px::sdk::ComputePackageFingerprint(path),
            std::filesystem::file_size(path)};
}

bool WaitUntil(const std::chrono::milliseconds timeout,
               const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(25ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return condition();
}

bool WaitForLog(const std::filesystem::path& log,
                const std::string_view expected,
                const std::chrono::milliseconds timeout = 10s) {
    return WaitUntil(timeout, [&] {
        std::error_code error;
        if (!std::filesystem::is_regular_file(log, error) || error) return false;
        return Read(log).find(expected) != std::string::npos;
    });
}

struct WindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM context) {
    auto& search = *reinterpret_cast<WindowSearch*>(context);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == search.processId && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == nullptr) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindWindowForProcess(const DWORD processId) {
    WindowSearch search{processId, nullptr};
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool PostKey(HWND window, const UINT virtualKey) {
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    const LPARAM down = 1 | (static_cast<LPARAM>(scanCode) << 16);
    const LPARAM up = down | (static_cast<LPARAM>(1) << 30) |
                       (static_cast<LPARAM>(1) << 31);
    return PostMessageW(window, WM_KEYDOWN, virtualKey, down) != 0 &&
           PostMessageW(window, WM_KEYUP, virtualKey, up) != 0;
}

struct CaptureSummary {
    int width = 0;
    int height = 0;
    std::size_t sampledColors = 0;
    std::uint64_t hash = 1469598103934665603ull;
};

CaptureSummary CaptureWindow(HWND window) {
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return {};
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return {};

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels,
                                      nullptr, 0);
    HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool captured = bitmap &&
        (PrintWindow(window, memory, PW_RENDERFULLCONTENT) != 0 ||
         BitBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top,
                SRCCOPY) != 0);

    CaptureSummary summary;
    summary.width = width;
    summary.height = height;
    if (captured && pixels) {
        const auto* values = static_cast<const std::uint32_t*>(pixels);
        const std::size_t count = static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height);
        const std::size_t stride = (std::max)(std::size_t{1}, count / 4096);
        std::set<std::uint32_t> colors;
        for (std::size_t index = 0; index < count; index += stride) {
            colors.insert(values[index]);
            summary.hash ^= values[index];
            summary.hash *= 1099511628211ull;
        }
        summary.sampledColors = colors.size();
    }

    if (previous) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return summary;
}

class ChildProcess {
public:
    ~ChildProcess() {
        if (m_process) {
            if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(m_process, 1);
                WaitForSingleObject(m_process, 2000);
            }
            CloseHandle(m_process);
        }
    }

    bool Start(const std::filesystem::path& executable,
               const std::filesystem::path& workingDirectory) {
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr,
                            FALSE, 0, nullptr, workingDirectory.c_str(), &startup,
                            &process)) {
            return false;
        }
        CloseHandle(process.hThread);
        m_process = process.hProcess;
        m_processId = process.dwProcessId;
        return true;
    }

    [[nodiscard]] DWORD Id() const { return m_processId; }

    [[nodiscard]] std::optional<DWORD> CompletedExitCode() const {
        if (!m_process || WaitForSingleObject(m_process, 0) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(m_process, &exitCode)) return std::nullopt;
        return exitCode;
    }

    bool Close(HWND window, DWORD& exitCode) {
        if (!m_process || !window) return false;
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(m_process, 10'000) != WAIT_OBJECT_0) return false;
        return GetExitCodeProcess(m_process, &exitCode) != 0;
    }

private:
    HANDLE m_process = nullptr;
    DWORD m_processId = 0;
};

void CreateProjectFixture(const std::filesystem::path& root,
                          const std::filesystem::path& contentRoot,
                          const std::filesystem::path& imageSource) {
    CopySceneAsCurrentVersion(contentRoot / "UI/Title.pxscene",
                              root / "Content/UI/Title.pxscene");
    CopySceneAsCurrentVersion(contentRoot / "UI/HUD.pxscene",
                              root / "Content/UI/HUD.pxscene");
    CopySceneAsCurrentVersion(contentRoot / "UI/Gallery.pxscene",
                              root / "Content/UI/Gallery.pxscene");
    Copy(contentRoot / "UI/PrismatiX.pxtheme",
         root / "Content/UI/PrismatiX.pxtheme");
    Copy(contentRoot / "Fonts/NotoSansTC-Bold.ttf",
         root / "Content/Fonts/NotoSansTC-Bold.ttf");
    Copy(imageSource, root / "Assets/ending.png");

    Write(root / "Content/Extensions/default.pxextension", R"({
      "format":"PrismatiXExtension","version":4,
      "id":"player-catalog-native-acceptance",
      "entry":"acceptance.lua",
      "capabilities":["runtime"],
      "commands":[],
      "actions":[{
        "id":"acceptance.packaged-action",
        "displayName":"Packaged Action",
        "description":"Packaged Player typed Action acceptance fixture.",
        "category":"Tests",
        "reentry":"ignoreWhileRunning",
        "capabilities":["runtime"],
        "destructiveInPreview":false,
        "allowAdditionalArguments":false,
        "parameters":[
          {"name":"amount","displayName":"Amount","type":"integer",
           "required":true,"default":null,"enum":[],"resourceFilter":"",
           "editorHint":"default","range":{"minimum":1,"maximum":10}},
          {"name":"asset","displayName":"Asset","type":"resource",
           "required":true,"default":null,"enum":[],"resourceFilter":"image/*",
           "editorHint":"resource","range":null}
        ]
      }]
    })");
    Write(root / "Content/Extensions/acceptance.lua", R"(
Engine.RegisterAction('acceptance.packaged-action', function(args, context)
  assert(args.amount == 3)
  assert(args.asset.id == '33333333-3333-4333-8333-333333333333')
  assert(args.asset.path == 'Assets/ending.png')
  assert(context.scene == 'Content/Runtime/start.pxir')
  assert(context.preview == false)
  print('packaged-action-start', args.amount, args.asset.id, args.asset.path, context.scene, context.preview)
  Engine.AwaitSeconds(0)
  Engine.SetVariable('packaged_action_result', args.amount * 2)
  local result = Engine.GetVariable('packaged_action_result')
  assert(result == 6)
  print('packaged-action-complete', result, args.asset.id, args.asset.path, context.scene, context.preview)
end)
)");

    Write(root / "Content/Game.pxres", R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog
[node "00000000-0000-4000-8000-000000000010", "", "OpenGallery", "InputBinding"]
key = "G"
command = "screen.open"
argument = "gallery"
[node "00000000-0000-4000-8000-000000000011", "", "Affinity", "Variable"]
name = "affinity"
default = 7
persistent = true
[node "00000000-0000-4000-8000-000000000012", "", "EndingCG", "GalleryItem"]
id = "ending-rin"
title = "Rin Ending"
image = res("33333333-3333-4333-8333-333333333333", "Assets/ending.png")
thumbnail = res("33333333-3333-4333-8333-333333333333", "Assets/ending.png")
)");

    Write(root / "Content/Runtime/start.pxir", R"({
      "format":"PrismatiXRuntimeIR","schemaRevision":1,
      "documentId":"99999999-9999-4999-8999-999999999999",
      "committedRevision":1,
      "operations":[
        {"operationId":"unlock-ending","sourceId":"unlock-ending","sourceLine":1,
         "kind":"customNode","text":"",
         "arguments":{"type":"unlock","value":"{\"kind\":\"cg\",\"id\":\"ending-rin\"}"}},
        {"operationId":"typed-action","sourceId":"typed-action","sourceLine":2,
         "kind":"customNode","text":"",
         "arguments":{"type":"action","value":"{\"id\":\"acceptance.packaged-action\",\"arguments\":{\"amount\":3,\"asset\":\"asset:33333333-3333-4333-8333-333333333333\"}}"}},
        {"operationId":"line-1","sourceId":"line-1","sourceLine":3,
         "kind":"dialogue","text":"hello",
         "arguments":{"speaker":"Rin","text":"hello"}}
      ]
    })");

    Write(root / "project.pxproject", R"({
      "format":"PrismatiXProject","schemaRevision":1,
      "assets":[{
        "id":"33333333-3333-4333-8333-333333333333",
        "name":"Ending","kind":"cg","source":"Assets/ending.png",
        "sourceFileName":"ending.png","tags":[],"size":1,"fingerprint":"fixture"
      }],
      "characters":[]
    })");
}

px::sdk::PackageRequest PackageRequest(const std::filesystem::path& root,
                                       const std::filesystem::path& output,
                                       const std::filesystem::path& player) {
    px::sdk::PackageRequest request;
    request.requestId = "player-catalog-native-acceptance";
    request.projectRoot = root;
    request.outputDir = output;
    request.playerExecutable = player;
    request.title = "PrismatiX Player Catalog Acceptance";
    request.width = 960;
    request.height = 540;
    request.startScript = "Content/Runtime/start.pxir";
    request.startRoute = "title";
    request.routes = {{"title", "Content/UI/Title.pxscene"},
                      {"hud", "Content/UI/HUD.pxscene"},
                      {"gallery", "Content/UI/Gallery.pxscene"}};
    request.contentVersion = "player-catalog-native-acceptance-v1";
    request.encryption = false;
    request.compression = px::sdk::PackageCompression::Fast;
    const std::vector<std::string> uris = {
        "Assets/ending.png",
        "Content/Fonts/NotoSansTC-Bold.ttf",
        "Content/Extensions/acceptance.lua",
        "Content/Extensions/default.pxextension",
        "Content/Game.pxres",
        "Content/Runtime/start.pxir",
        "Content/UI/Gallery.pxscene",
        "Content/UI/HUD.pxscene",
        "Content/UI/PrismatiX.pxtheme",
        "Content/UI/Title.pxscene",
        "project.pxproject",
    };
    for (const auto& uri : uris) request.inputs.push_back(Input(root, uri));
    request.cancelFile = root / ".cancel-package";
    return request;
}

}  // namespace

int main(int argc, char* argv[]) {
    px::test::Suite suite("PlayerCatalogNativeAcceptance");
    suite.Run("PackagedPlayerInputGalleryAndPersistence", [&] {
        suite.Require(argc == 4,
                      "test receives Player, Content root, and image fixture paths");
        const std::filesystem::path player =
            std::filesystem::absolute(std::filesystem::path(argv[1]));
        const std::filesystem::path contentRoot =
            std::filesystem::absolute(std::filesystem::path(argv[2]));
        const std::filesystem::path imageSource =
            std::filesystem::absolute(std::filesystem::path(argv[3]));
        suite.Require(std::filesystem::is_regular_file(player),
                      "built Windows Player exists");
        suite.Require(std::filesystem::is_regular_file(imageSource),
                      "native Gallery image fixture exists");

        px::test::TempDirectory fixture("player-catalog-native");
        const auto root = fixture.path / "Project";
        const auto output = fixture.path / "Build/Game";
        CreateProjectFixture(root, contentRoot, imageSource);
        const auto packaged = px::sdk::RunPackager(
            PackageRequest(root, output, player));
        suite.Require(packaged.Completed(),
                      "Packager produces the native Player fixture");

        const auto packagedPlayer = output / player.filename();
        ChildProcess process;
        suite.Require(process.Start(packagedPlayer, output),
                      "packaged Player process launches");
        const auto log = output / "logs/PrismatiXPlayer.log";
        const bool titleReady =
            WaitForLog(log, "Player presentation ready route=title");
        if (!titleReady) {
            if (const auto exitCode = process.CompletedExitCode()) {
                std::cout << "Player exited before title route with code 0x"
                          << std::hex << *exitCode << std::dec << '\n';
            } else {
                std::cout << "Player remained active before title-route timeout\n";
            }
        }
        suite.Require(titleReady,
                      "native Player reaches the title route");

        HWND window = nullptr;
        suite.Require(WaitUntil(10s, [&] {
                          window = FindWindowForProcess(process.Id());
                          return window != nullptr;
                      }),
                      "native Player creates a visible Windows window");
        suite.Require(PostKey(window, VK_TAB) && PostKey(window, VK_RETURN),
                      "keyboard input activates Start through the real window");
        suite.Require(WaitForLog(log, "Player game started script=Content/Runtime/start.pxir"),
                      "native Player starts packaged Runtime IR");
        suite.Require(WaitForLog(log,
                                 "Player progression unlocked kind=cg id=ending-rin"),
                      "Runtime IR unlock reaches Player persistence");
        suite.Require(
            WaitForLog(
                log,
                "[lua] packaged-action-complete\t6\t33333333-3333-4333-8333-333333333333\tAssets/ending.png\tContent/Runtime/start.pxir\tfalse"),
            "packaged Runtime IR dispatches the typed Lua Action with exact Player context, resolved UUID resource, and observable variable side effect");

        suite.Require(PostKey(window, 'G'),
                      "catalog InputBinding is delivered to the real window");
        suite.Require(WaitForLog(log,
                                 "Player gallery presented total=1 unlocked=1"),
                      "InputBinding opens Gallery with the unlocked UUID asset");
        const CaptureSummary capture = CaptureWindow(window);
        suite.Expect(capture.width >= 640 && capture.height >= 360 &&
                         capture.sampledColors >= 8,
                     "Gallery renders a non-blank native Player frame");
        std::cout << "Native Gallery capture " << capture.width << 'x'
                  << capture.height << " colors=" << capture.sampledColors
                  << " hash=" << capture.hash << '\n';

        suite.Require(PostKey(window, VK_ESCAPE),
                      "keyboard closes the Gallery overlay");
        suite.Require(WaitForLog(log, "Player overlay closed"),
                      "Player returns from the Gallery overlay");
        DWORD exitCode = 1;
        suite.Require(process.Close(window, exitCode),
                      "native Player exits within the bounded deadline");
        suite.Expect(exitCode == 0, "native Player exits cleanly");

        const auto savePath = output / "Save/profile.dat";
        suite.Require(std::filesystem::is_regular_file(savePath) &&
                          std::filesystem::file_size(savePath) > 0,
                      "Player writes the encrypted global profile");
        const px::crypto::Key saveKey = px::crypto::DeriveKey(
            "PrismatiX Player Catalog Acceptance|px-save");
        px::progress::GlobalProfile profile;
        suite.Require(px::progress::LoadGlobalProfile(profile, savePath.string(), &saveKey),
                      "saved Player profile reloads with the package identity");
        suite.Expect(profile.CGUnlocked("ending-rin"),
                     "Gallery unlock survives Player shutdown and reload");
    });
    return suite.Finish();
}
