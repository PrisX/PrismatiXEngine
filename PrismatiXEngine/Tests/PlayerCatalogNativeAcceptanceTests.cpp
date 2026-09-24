#include "Engine/IO/Crypto.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/Persist.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Tests/TestSupport/TestHarness.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
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

#if defined(_WIN32)
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
#endif

std::string ShellQuote(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::string value=path.string();
    std::string escaped;
    escaped.reserve(value.size()+2);
    escaped.push_back('"');
    for(const char character:value){
        if(character=='"')escaped.push_back('\\');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
#else
    std::string value=path.string();
    std::string escaped="'";
    for(const char character:value){
        if(character=='\'')escaped+="'\\''";
        else escaped.push_back(character);
    }
    escaped.push_back('\'');
    return escaped;
#endif
}

class AutomatedChildProcess {
public:
    ~AutomatedChildProcess(){
        if(m_result.valid())m_result.wait();
    }

    bool Start(const std::filesystem::path& executable,
               const std::filesystem::path& workingDirectory) {
        if(m_result.valid())return false;
#if defined(_WIN32)
        const std::string command="cd /d "+ShellQuote(workingDirectory)+
            " && set PRISMATIX_E2E_JOURNEY=catalog&& "+ShellQuote(executable);
#else
        const std::string command="cd "+ShellQuote(workingDirectory)+
            " && PRISMATIX_E2E_JOURNEY=catalog "+ShellQuote(executable);
#endif
        m_result=std::async(std::launch::async,[command]{
            return std::system(command.c_str());
        });
        return true;
    }

    [[nodiscard]] std::optional<int> CompletedExitCode(){
        if(m_completed)return m_exitCode;
        if(!m_result.valid()||m_result.wait_for(0ms)!=std::future_status::ready)
            return std::nullopt;
        m_exitCode=m_result.get();
        m_completed=true;
        return m_exitCode;
    }

private:
    std::future<int> m_result;
    std::optional<int> m_exitCode;
    bool m_completed=false;
};

void CreateProjectFixture(const std::filesystem::path& root,
                          const std::filesystem::path& contentRoot,
                          const std::filesystem::path& fontSource,
                          const std::filesystem::path& imageSource,
                          const std::filesystem::path& audioSource) {
    Copy(contentRoot / "UI/Title.pxui", root / "Content/UI/Title.pxui");
    Copy(contentRoot / "UI/HUD.pxui", root / "Content/UI/HUD.pxui");
    Copy(contentRoot / "UI/Gallery.pxui", root / "Content/UI/Gallery.pxui");
    std::string galleryUi = Read(root / "Content/UI/Gallery.pxui");
    const std::string sampleGalleryAsset =
        "40000000-0000-4000-8000-000000000001";
    const std::string fixtureGalleryAsset =
        "33333333-3333-4333-8333-333333333333";
    for (std::size_t offset = 0;
         (offset = galleryUi.find(sampleGalleryAsset, offset)) !=
         std::string::npos;
         offset += fixtureGalleryAsset.size()) {
        galleryUi.replace(offset, sampleGalleryAsset.size(), fixtureGalleryAsset);
    }
    Write(root / "Content/UI/Gallery.pxui", galleryUi);
    Copy(fontSource, root / "Content/Fonts/NotoSansTC-Bold.ttf");
    Copy(imageSource, root / "Assets/ending.png");
    Copy(imageSource, root / "Assets/Locales/zh-TW/opening.png");
    Copy(imageSource, root / "Assets/Locales/ja-JP/opening.png");
    for (int index = 1; index <= 3; ++index)
        Copy(imageSource, root / ("Assets/phone-" + std::to_string(index) + ".png"));
    Copy(audioSource, root / "Assets/Locales/zh-TW/opening.wav");
    Copy(audioSource, root / "Assets/Locales/ja-JP/opening.wav");

    auto titleUi = nlohmann::json::parse(Read(root / "Content/UI/Title.pxui"));
    const std::string titleRootId = titleUi.at("rootId").get<std::string>();
    titleUi["animations"] = {
        {"clips", nlohmann::json::array({{
            {"id", "a1000000-0000-4000-8000-000000000001"},
            {"name", "Title pulse"}, {"duration", 0.5}, {"loop", true},
            {"tracks", nlohmann::json::array({{
                {"id", "a1000000-0000-4000-8000-000000000002"},
                {"nodeId", titleRootId}, {"property", "opacity"},
                {"keys", nlohmann::json::array({
                    {{"id", "a1000000-0000-4000-8000-000000000003"},
                     {"time", 0.0}, {"value", 0.82}, {"easing", "linear"},
                     {"interpolation", "linear"}},
                    {{"id", "a1000000-0000-4000-8000-000000000004"},
                     {"time", 0.5}, {"value", 1.0}, {"easing", "easeInOut"},
                     {"interpolation", "linear"}}})}}})}}})},
        {"stateMachine", {
            {"entryStateId", "a1000000-0000-4000-8000-000000000005"},
            {"parameters", nlohmann::json::array()},
            {"states", nlohmann::json::array({{
                {"id", "a1000000-0000-4000-8000-000000000005"},
                {"name", "Animated"},
                {"clipId", "a1000000-0000-4000-8000-000000000001"},
                {"position", {{"x", 80}, {"y", 80}}}}})},
            {"transitions", nlohmann::json::array()}}}};
    Write(root / "Content/UI/Title.pxui", titleUi.dump(2));

    auto phoneUi = titleUi;
    phoneUi["id"] = "a2000000-0000-4000-8000-000000000001";
    phoneUi["name"] = "Phone";
    for (auto& node : phoneUi["nodes"])
        if (node.at("id").get<std::string>() == titleRootId)
            node["name"] = "PhoneRoot";
    for (int index = 1; index <= 3; ++index) {
        phoneUi["nodes"].push_back({
            {"id", "a2000000-0000-4000-8000-00000000000" + std::to_string(index + 1)},
            {"parentId", titleRootId}, {"order", index + 10},
            {"kind", "image"}, {"runtimeType", "TextureRect"},
            {"name", "PhoneImage" + std::to_string(index)},
            {"visible", true}, {"locked", false},
            {"layout", {{"mode", "free"}, {"x", 550 + index * 110},
                        {"y", 250}, {"width", 100}, {"height", 100},
                        {"anchorX", 0}, {"anchorY", 0}, {"anchorRight", 0},
                        {"anchorBottom", 0}, {"pivotX", 0}, {"pivotY", 0},
                        {"margin", 0}, {"alignment", "start"},
                        {"sizeRule", "fixed"}}},
            {"runtimeProperties", {{"path", "Assets/phone-" +
                                                std::to_string(index) + ".png"},
                                   {"scaleMode", "Stretch"}}},
            {"bindings", nlohmann::json::object()}});
    }
    phoneUi.erase("animations");
    Write(root / "Content/UI/Phone.pxui", phoneUi.dump(2));

    auto replayUi = titleUi;
    replayUi["id"] = "a3000000-0000-4000-8000-000000000001";
    replayUi["name"] = "Replay selector";
    for (auto& node : replayUi["nodes"])
        if (node.at("id").get<std::string>() == titleRootId)
            node["name"] = "ReplayRoot";
    replayUi.erase("animations");
    Write(root / "Content/UI/Replay.pxui", replayUi.dump(2));

    Write(root / "Content/Extensions/default.pxextension", R"({
      "format":"PrismatiXExtension","schemaRevision":2,
      "id":"player-catalog-native-acceptance",
      "version":"1.0.0","requiredEngineVersion":"^0.2.0",
      "language":"javascript",
       "entry":"acceptance.js",
       "capabilities":["runtime","ui"],
       "safety":{"previewSafe":true,"deterministic":true,
                 "seekSafe":false,"rollbackSafe":false},
       "commands":[],
      "actions":[{
        "id":"acceptance.packaged-action",
        "displayName":"Packaged Action",
        "description":"Packaged Player typed Action acceptance fixture.",
        "category":"Tests",
        "reentry":"ignoreWhileRunning",
        "capabilities":["runtime","ui"],
        "parameters":[
          {"name":"amount","displayName":"Amount","type":"integer",
           "required":true,"default":null,"enum":[],"resourceFilter":"",
           "editorHint":"default","range":{"minimum":1,"maximum":10}},
          {"name":"asset","displayName":"Asset","type":"resource",
           "required":true,"default":null,"enum":[],"resourceFilter":"image/*",
           "editorHint":"resource","range":null}
        ]
      },{
        "id":"acceptance.phase2-title","displayName":"Phase 2 Title",
        "description":"Exercises title async and an arbitrary route.",
        "category":"Tests","reentry":"ignoreWhileRunning",
        "capabilities":["runtime","ui"],"parameters":[]
      },{
        "id":"acceptance.phase2-menu","displayName":"Phase 2 Menu",
        "description":"Exercises menu async and public VN models.",
        "category":"Tests","reentry":"ignoreWhileRunning",
        "capabilities":["runtime","ui"],"parameters":[]
      }]
    })");
    Write(root / "Content/Extensions/acceptance.js", R"(
Engine.RegisterAction("acceptance.packaged-action", async (args, context) => {
  if (args.amount !== 3) throw new Error("unexpected amount");
  if (args.asset.id !== "33333333-3333-4333-8333-333333333333") throw new Error("unexpected asset id");
  if (args.asset.path !== "Assets/ending.png") throw new Error("unexpected asset path");
  if (context.scene !== "Runtime/Locales/zh-TW/main.pxir") throw new Error("unexpected scene");
  if (context.preview !== false) throw new Error("unexpected preview mode");
  Engine.log("packaged-action-start", args.amount, args.asset.id, args.asset.path, context.scene, context.preview);
  await Engine.WaitSeconds(0);
  Engine.SetVariable("packaged_action_result", args.amount * 2);
  const result = Engine.GetVariable("packaged_action_result");
  if (result !== 6) throw new Error("unexpected variable result");
  Engine.log("packaged-action-complete", result, args.asset.id, args.asset.path, context.scene, context.preview);
});
Engine.RegisterAction("acceptance.phase2-title", async () => {
  await Engine.WaitSeconds(0.05);
  if (!Engine.PushRoute("phone")) throw new Error("phone route rejected");
  await Engine.WaitSeconds(0.25);
  if (!Engine.BackRoute()) throw new Error("phone route back rejected");
  Engine.log("phase2-title-async-complete");
});
Engine.RegisterAction("acceptance.phase2-menu", async () => {
  const dialogue = Engine.GetDialogue();
  const choices = Engine.GetChoices();
  const backlog = Engine.GetBacklog();
  const settings = Engine.GetSettings();
  if (!dialogue.active || !Array.isArray(choices) || backlog.length < 1)
    throw new Error("public VN models unavailable");
  if (!Engine.ShowModal("replay")) throw new Error("replay modal rejected");
  await Engine.WaitSeconds(0.25);
  if (!Engine.SetSetting("textScale", settings.textScale))
    throw new Error("public settings update rejected");
  if (!Engine.CloseModal()) throw new Error("replay modal close rejected");
  Engine.log("phase2-menu-async-complete", dialogue.speaker, backlog.length,
             settings.language);
});
)");

    Write(root / "Content/game.pxgame", R"({
      "format":"PrismatiXGame","schemaRevision":2,
      "variables":[
        {"name":"affinity","type":"integer","default":7,"scope":"profile"},
        {"name":"routeName","type":"string","default":"common","scope":"session"}
      ],
      "inputBindings":[{"key":"Escape","command":"screen.open","argument":"settings"}],
      "inputActions":[{"id":"screen.open.gallery","context":"game","keys":["G"],"route":"gallery"}],
      "gallery":[{
        "id":"ending-rin","title":"Rin Ending",
        "image":{"id":"33333333-3333-4333-8333-333333333333","path":"Assets/ending.png"},
        "thumbnail":{"id":"33333333-3333-4333-8333-333333333333","path":"Assets/ending.png"}
      }],
      "unlockables":[]
    })");

    Write(root / "Content/Runtime/start.pxir", R"({
      "format":"PrismatiXRuntimeIR","schemaRevision":2,
      "documentId":"99999999-9999-4999-8999-999999999999",
      "committedRevision":1,
      "operations":[
        {"operationId":"localized-background","sourceId":"localized-background","sourceLine":1,
         "kind":"background","text":"",
         "arguments":{"asset":"Assets/Locales/zh-TW/opening.png"}},
        {"operationId":"localized-voice","sourceId":"localized-voice","sourceLine":2,
         "kind":"voice","text":"",
         "arguments":{"asset":"Assets/Locales/zh-TW/opening.wav"}},
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
    Write(root / "Content/Runtime/start.pxmap", R"({
      "format":"PrismatiXSourceMap","schemaRevision":2,
      "documentId":"99999999-9999-4999-8999-999999999999",
      "mappings":[
        {"operationId":"localized-background","sourceId":"localized-background","sourceUri":"Story/Entry.pxstory","startLine":1,"startColumn":1,"endLine":1,"endColumn":10},
        {"operationId":"localized-voice","sourceId":"localized-voice","sourceUri":"Story/Entry.pxstory","startLine":2,"startColumn":1,"endLine":2,"endColumn":10},
        {"operationId":"unlock-ending","sourceId":"unlock-ending","sourceUri":"Story/Entry.pxstory","startLine":1,"startColumn":1,"endLine":1,"endColumn":10},
        {"operationId":"typed-action","sourceId":"typed-action","sourceUri":"Story/Entry.pxstory","startLine":2,"startColumn":1,"endLine":2,"endColumn":10},
        {"operationId":"line-1","sourceId":"line-1","sourceUri":"Story/Entry.pxstory","startLine":3,"startColumn":1,"endLine":3,"endColumn":5}
      ]
    })");
    Copy(root / "Content/Runtime/start.pxir",
         root / "Runtime/Locales/zh-TW/main.pxir");
    Copy(root / "Content/Runtime/start.pxmap",
         root / "Runtime/Locales/zh-TW/main.pxmap");
    std::string japaneseIr = Read(root / "Content/Runtime/start.pxir");
    for (std::size_t offset = 0;
         (offset = japaneseIr.find("hello", offset)) != std::string::npos;
         offset += std::string_view("こんにちは").size())
        japaneseIr.replace(offset, std::string_view("hello").size(),
                           "こんにちは");
    for (const auto& [source, localized] :
         std::array<std::pair<std::string_view, std::string_view>, 2>{
             {{"Assets/Locales/zh-TW/opening.png",
               "Assets/Locales/ja-JP/opening.png"},
              {"Assets/Locales/zh-TW/opening.wav",
               "Assets/Locales/ja-JP/opening.wav"}}}) {
        for (std::size_t offset = 0;
             (offset = japaneseIr.find(source, offset)) != std::string::npos;
             offset += localized.size())
            japaneseIr.replace(offset, source.size(), localized);
    }
    Write(root / "Runtime/Locales/ja-JP/main.pxir", japaneseIr);
    std::string japaneseMap = Read(root / "Content/Runtime/start.pxmap");
    for (std::size_t offset = 0;
         (offset = japaneseMap.find("Story/Entry.pxstory", offset)) !=
         std::string::npos;
         offset += std::string_view("Story/Entry.ja.pxstory").size())
        japaneseMap.replace(offset,
                            std::string_view("Story/Entry.pxstory").size(),
                            "Story/Entry.ja.pxstory");
    Write(root / "Runtime/Locales/ja-JP/main.pxmap", japaneseMap);

    Write(root / "project.pxproject", R"({
      "format":"PrismatiXProject","schemaRevision":2,
      "id":"player-catalog-native-acceptance","name":"Player Catalog Acceptance",
      "version":"0.2.0","contentVersion":"player-catalog-native-acceptance-v1","saveVersion":1,
      "resolution":{"width":1280,"height":720},
      "entry":{"story":"main","ui":"title"},
      "defaultLocale":"zh-TW","supportedLocales":["zh-TW","ja-JP"],
      "storyIndex":"Story/story.pxindex","gameCatalog":"Content/game.pxgame",
      "extensions":["Content/Extensions/default.pxextension"],
      "uiEntryPoints":{
        "title":"Content/UI/Title.pxui",
        "phone":"Content/UI/Phone.pxui",
        "replay":"Content/UI/Replay.pxui"
      },
      "assets":[{
        "id":"33333333-3333-4333-8333-333333333333",
        "name":"Ending","kind":"cg","source":"Assets/ending.png",
        "sourceFileName":"ending.png","tags":[],"size":1,"fingerprint":"fixture"
      }],
      "settings":{"player.presentationTransitions":{"routeOverrides":[
        {"from":"title","to":"phone","preset":"none","duration":0}
      ]}},
      "characters":[]
    })");
    Write(root / "Content/Localization/zh-TW.json", R"({
      "format":"PrismatiXLocale","schemaRevision":2,"locale":"zh-TW",
      "fontChain":["Content/Fonts/NotoSansTC-Bold.ttf"],
      "strings":{}
    })");
    Write(root / "Content/Localization/ja-JP.json", R"({
      "format":"PrismatiXLocale","schemaRevision":2,"locale":"ja-JP",
      "fontChain":["Content/Fonts/NotoSansTC-Bold.ttf"],
      "strings":{}
    })");
    Write(root / "Story/Entry.pxstory", "Rin: hello\n[end]\n");
    Write(root / "Story/Entry.ja.pxstory", "Rin: こんにちは\n[end]\n");
    Write(root / "Story/story.pxindex", R"({
      "format":"PrismatiXStoryIndex","schemaRevision":2,"id":"main",
      "entryScene":"main",
      "chapters":[{"id":"chapter-1","title":"Chapter 1","scenes":["main"]}],
      "scenes":[{"id":"main","sources":{"zh-TW":"Story/Entry.pxstory","ja-JP":"Story/Entry.ja.pxstory"}}]
    })");
}

px::sdk::PackageRequest PackageRequest(const std::filesystem::path& root,
                                       const std::filesystem::path& output,
                                       const std::filesystem::path& player) {
    px::sdk::PackageRequest request;
    request.requestId = "player-catalog-native-acceptance";
    request.gameId = "player-catalog-native-acceptance";
    request.projectRoot = root;
    request.outputDir = output;
    request.playerExecutable = player;
    request.title = "PrismatiX Player Catalog Acceptance";
    request.width = 1280;
    request.height = 720;
    request.startScript = "Content/Runtime/start.pxir";
    request.sourceMap = "Content/Runtime/start.pxmap";
    request.extensions = {"Content/Extensions/default.pxextension"};
    request.startRoute = "title";
    request.routes = {{"title", "Content/UI/Title.pxui"},
                      {"hud", "Content/UI/HUD.pxui"},
                      {"gallery", "Content/UI/Gallery.pxui"},
                      {"phone", "Content/UI/Phone.pxui"},
                      {"replay", "Content/UI/Replay.pxui"}};
    request.contentVersion = "player-catalog-native-acceptance-v1";
    request.saveVersion = 1;
    request.encryption = false;
    request.compression = px::sdk::PackageCompression::Fast;
    request.archiveShards = 4;
    const std::vector<std::string> uris = {
        "Assets/ending.png",
        "Assets/Locales/zh-TW/opening.png",
        "Assets/Locales/ja-JP/opening.png",
        "Assets/phone-1.png",
        "Assets/phone-2.png",
        "Assets/phone-3.png",
        "Assets/Locales/zh-TW/opening.wav",
        "Assets/Locales/ja-JP/opening.wav",
        "Content/Fonts/NotoSansTC-Bold.ttf",
        "Content/Extensions/acceptance.js",
        "Content/Extensions/default.pxextension",
        "Content/game.pxgame",
        "Content/Localization/ja-JP.json",
        "Content/Localization/zh-TW.json",
        "Content/Runtime/start.pxir",
        "Content/Runtime/start.pxmap",
        "Runtime/Locales/zh-TW/main.pxir",
        "Runtime/Locales/zh-TW/main.pxmap",
        "Runtime/Locales/ja-JP/main.pxir",
        "Runtime/Locales/ja-JP/main.pxmap",
        "Story/Entry.ja.pxstory",
        "Content/UI/Gallery.pxui",
        "Content/UI/HUD.pxui",
        "Content/UI/Phone.pxui",
        "Content/UI/Replay.pxui",
        "Content/UI/Title.pxui",
        "Story/Entry.pxstory",
        "Story/story.pxindex",
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
        suite.Require(argc == 6,
                      "test receives Player, Content root, font, image, and audio fixture paths");
        const std::filesystem::path player =
            std::filesystem::absolute(std::filesystem::path(argv[1]));
        const std::filesystem::path contentRoot =
            std::filesystem::absolute(std::filesystem::path(argv[2]));
        const std::filesystem::path fontSource =
            std::filesystem::absolute(std::filesystem::path(argv[3]));
        const std::filesystem::path imageSource =
            std::filesystem::absolute(std::filesystem::path(argv[4]));
        const std::filesystem::path audioSource =
            std::filesystem::absolute(std::filesystem::path(argv[5]));
        suite.Require(std::filesystem::is_regular_file(player),
                      "built native Player exists");
        suite.Require(std::filesystem::is_regular_file(fontSource),
                      "native font fixture exists");
        suite.Require(std::filesystem::is_regular_file(imageSource),
                      "native Gallery image fixture exists");
        suite.Require(std::filesystem::is_regular_file(audioSource),
                      "native localized audio fixture exists");

        px::test::TempDirectory fixture("player-catalog-native");
        const auto root = fixture.path / "Project";
        const auto output = fixture.path / "Build/Game";
        CreateProjectFixture(root, contentRoot, fontSource, imageSource,
                             audioSource);
        const auto packaged = px::sdk::RunPackager(
            PackageRequest(root, output, player));
        if (!packaged.Completed()) {
            for (const auto& diagnostic : packaged.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message
                          << '\n';
        }
        suite.Require(packaged.Completed(),
                      "Packager produces the native Player fixture");
        suite.Require(std::filesystem::is_regular_file(
                          output / "Package/manifest.json"),
                      "packaged output contains its explicit package manifest");
        const auto startupManifest = px::sdk::detail::ParsePackageManifest(
            Read(output / "Package/manifest.json"));
        suite.Require(startupManifest.Valid(),
                      "packaged manifest supplies the stable settings key");
        suite.Require(startupManifest.manifest.archives.size() > 1,
                      "Packager writes multiple stable content shards");
        for (const auto& archive : startupManifest.manifest.archives)
            suite.Require(std::filesystem::is_regular_file(output / archive.file),
                          "every declared content shard exists");
        const px::crypto::Key startupKey =
            px::progress::DeriveGameDataPersistenceKey(
                startupManifest.manifest.persistenceDataId);
        const int galleryOverride = px::Input::KeyCode("F8").value_or(0);
        suite.Require(galleryOverride > 0,
                      "F8 is a recognized keyboard override");
        px::progress::GameSettings startupSettings;
        startupSettings.keyboardBindings["screen.open.gallery"] =
            {galleryOverride};
        std::filesystem::create_directories(output / "Save");
        suite.Require(startupSettings.Save(
                          (output / "Save/config.dat").string(), &startupKey),
                      "encrypted settings can override an authored screen shortcut");

        const auto packagedPlayer = output / player.filename();
        const auto launchDirectory = fixture.path / "UnrelatedWorkingDirectory";
        std::filesystem::create_directories(launchDirectory / "Save");
        const auto unrelatedSave = launchDirectory / "Save/profile.dat";
        {
            std::ofstream sentinel(unrelatedSave, std::ios::binary);
            sentinel << "unrelated-player-data";
        }
        AutomatedChildProcess process;
        suite.Require(process.Start(packagedPlayer, launchDirectory),
                      "packaged Player process launches independently of the caller working directory");
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

        suite.Require(
            WaitForLog(log,
                       "Player Phase 2 authored title animation advanced"),
            "the authored title animation advances through the public UI scene contract");
        suite.Require(
            WaitForLog(log,
                       "[javascript] phase2-title-async-complete"),
            "a public async Action advances while the animated title route owns presentation");
        suite.Require(WaitForLog(log,
                                 "Player route visuals preloading route=phone images=3"),
                      "an explicit none transition still waits for its authored route images");
        suite.Require(
            WaitForLog(log,
                       "Player Phase 2 title async and custom phone route complete"),
            "the packaged Player installs and renders an arbitrary authored phone route");

        suite.Require(WaitForLog(log, "Player game started script=Runtime/Locales/zh-TW/main.pxir"),
                      "automated native input activates Start and runs packaged Runtime IR");
        suite.Require(WaitForLog(log,
                                 "Player progression unlocked kind=cg id=ending-rin"),
                      "Runtime IR unlock reaches Player persistence");
        suite.Require(
            WaitForLog(
                log,
                "[javascript] packaged-action-complete 6 33333333-3333-4333-8333-333333333333 Assets/ending.png Runtime/Locales/zh-TW/main.pxir false"),
            "packaged Runtime IR dispatches the typed JavaScript Action with exact Player context, resolved UUID resource, and observable variable side effect");
        suite.Require(
            WaitForLog(log, "[javascript] phase2-menu-async-complete Rin 1 zh-TW"),
            "a custom replay menu reads public dialogue/backlog/settings models and completes async while modal");
        suite.Require(
            WaitForLog(log,
                       "Player Phase 2 menu async and save/load stack complete"),
            "save/load restores the exact custom modal route and authored UI surface stack");
        suite.Require(WaitForLog(log, "Player save menu survived a rendered frame"),
                      "SAVE remains visible after the gameplay frame that opened it");
        suite.Require(WaitForLog(log, "Player save picker checkpoint restored gameplay HUD"),
                      "saving through the picker restores its underlying gameplay surface and route stack");
        suite.Require(
            WaitForLog(log,
                       "Player E2E locale switched locale=ja-JP text=こんにちは"),
            "running Player transactionally switches to aligned localized Story IR without restarting");
        suite.Require(
            WaitForLog(log,
                       "Player E2E localized media background=Assets/Locales/ja-JP/opening.png voice=Assets/Locales/ja-JP/opening.wav"),
            "locale switching transactionally replaces the live stage image and retained replay voice resource");
        suite.Require(
            WaitForLog(log,
                       "Player release F2 policy ignored demo reset shortcut"),
            "shipping Player ignores the removed F2 demo reset shortcut");

        suite.Require(WaitForLog(log,
                                 "Player gallery presented total=1 unlocked=1"),
                      "catalog InputBinding opens Gallery with the unlocked UUID asset");
        suite.Require(WaitForLog(log, "Player E2E gallery frame width="),
                      "Gallery is read back from the real native framebuffer");
        suite.Require(WaitForLog(log, "Player overlay closed"),
                      "automated native cancel input closes the Gallery overlay");
        suite.Require(WaitForLog(log, "Player E2E journey complete name=catalog"),
                      "Player completes the deterministic packaged journey");
        std::optional<int> exitCode;
        suite.Require(WaitUntil(35s, [&] {
                          exitCode = process.CompletedExitCode();
                          return exitCode.has_value();
                      }),
                      "native Player exits within the bounded deadline");
        if (exitCode.has_value() && *exitCode != 0) {
            std::cout << "Player exited with code 0x" << std::hex
                      << static_cast<unsigned int>(*exitCode) << std::dec << '\n';
        }
        suite.Expect(exitCode.has_value() && *exitCode == 0,
                     "native Player exits cleanly");

        const auto savePath = output / "Save/profile.dat";
        suite.Require(std::filesystem::is_regular_file(savePath) &&
                          std::filesystem::file_size(savePath) > 0,
                      "Player writes the encrypted global profile");
        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(output / "Package/manifest.json"));
        suite.Require(manifest.Valid(),
                      "packaged manifest can derive the same save identity as Player");
        const px::crypto::Key saveKey =
            px::progress::DeriveGameDataPersistenceKey(
                manifest.manifest.persistenceDataId);
        px::progress::GlobalProfile profile;
        suite.Require(px::progress::LoadGlobalProfile(profile, savePath.string(), &saveKey),
                      "saved Player profile reloads with the package identity");
        suite.Expect(profile.CGUnlocked("ending-rin"),
                     "Gallery unlock survives Player shutdown and reload");
        px::progress::GameSettings persistedSettings;
        suite.Require(persistedSettings.Load(
                          (output / "Save/config.dat").string(), &startupKey),
                      "Player preserves its encrypted keyboard settings");
        suite.Expect(persistedSettings.keyboardBindings.at(
                         "screen.open.gallery") ==
                         std::vector<int>{galleryOverride},
                     "authored Gallery shortcut remains remapped after Player exit");
        suite.Expect(Read(unrelatedSave) == "unrelated-player-data" &&
                         !std::filesystem::exists(launchDirectory / "logs"),
                     "package launch never reads or writes the caller's persistence or logs");
    });
    return suite.Finish();
}
