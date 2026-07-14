#include "Editor/Application/EditorApp.h"

#include "Engine/Support/Logger.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/IO/AtomicFile.h"
#include "Editor/Theme/EditorIcon.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
//
#include <shellapi.h>

#endif

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace px::editor {

namespace {
const std::array<const char*, 8> kAssetTypes = { "all", "image", "audio", "script", "ui", "font", "lua", "other" };

namespace fs = std::filesystem;

fs::path PathFromUtf8(const char* text) {
    if (!text) {
        return {};
    }
#ifdef _WIN32
    const int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (wideSize > 0) {
        std::vector<wchar_t> wide(static_cast<size_t>(wideSize));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), wideSize);
        return fs::path(wide.data());
    }
#endif
    return fs::path(text);
}

std::string Utf8Path(const fs::path& path) noexcept {
    try {
        const std::u8string value = path.generic_u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    } catch (...) {
        return "<無法顯示的路徑>";
    }
}

fs::path ImportFolderForType(const std::string& type) {
    if (type == "image") return fs::path("Content") / "Images" / "Imported";
    if (type == "audio") return fs::path("Content") / "Audio" / "Imported";
    if (type == "script") return fs::path("Content") / "Scenario";
    if (type == "ui") return fs::path("Content") / "UI";
    if (type == "font") return fs::path("Content") / "Fonts";
    if (type == "lua") return fs::path("Content") / "Extensions";
    return fs::path("Content") / "Imported";
}

fs::path UniqueDestination(const fs::path& requested) {
    if (!fs::exists(requested)) {
        return requested;
    }
    const fs::path parent = requested.parent_path();
    const std::string stem = requested.stem().string();
    const std::string ext = requested.extension().string();
    for (int i = 1; i < 10000; ++i) {
        fs::path candidate = parent / (stem + "_" + std::to_string(i) + ext);
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }
    return parent / (stem + "_copy" + ext);
}

bool IsPathWithin(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    fs::path childAbs = fs::weakly_canonical(child, ec);
    if (ec) {
        ec.clear();
        childAbs = fs::absolute(child, ec);
    }
    ec.clear();
    fs::path parentAbs = fs::weakly_canonical(parent, ec);
    if (ec) {
        ec.clear();
        parentAbs = fs::absolute(parent, ec);
    }
    ec.clear();
    const fs::path rel = fs::relative(childAbs, parentAbs, ec);
    if (ec) {
        return false;
    }
    for (const fs::path& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string RuntimePath(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const fs::path rel = fs::relative(path, root, ec);
    return ec ? path.generic_string() : rel.generic_string();
}

void PrepareImGuiRenderer(SDL_Renderer* renderer) {
    const ImGuiIO& io = ImGui::GetIO();
    const float scaleX = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const float scaleY = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;

    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderScale(renderer, scaleX, scaleY);
}
}

void DiagnosticToastQueue::Push(const diag::Diagnostic& diagnostic) {
    if (diagnostic.severity < diag::Severity::Warning) return;
    m_items.push_back({diagnostic, std::chrono::steady_clock::now()});
    while (m_items.size() > 5) m_items.pop_front();
}

void DiagnosticToastQueue::Prune(std::chrono::steady_clock::time_point now) {
    while (!m_items.empty() && now - m_items.front().created > std::chrono::seconds(8))
        m_items.pop_front();
}

EditorApp::EditorApp() : m_project([this](const std::string& m) { Log(m); }), m_assets([this](const std::string& m) { Log(m); }), m_scripts([this](const std::string& m) { Log(m); }) {}

void EditorApp::Log(const std::string& message) {
    m_console.push_back(message);
    if (m_console.size() > 500) {
        m_console.erase(m_console.begin(), m_console.begin() + (m_console.size() - 500));
    }
    PX_LOG_INFO("[editor] {}", message);
}

void EditorApp::ImportAssetFiles(const std::vector<std::filesystem::path>& paths) {
    QueueImportReview(paths);
}

void EditorApp::QueueImportReview(const std::vector<std::filesystem::path>& paths) {
    const auto reportFailure = [](std::string code, const fs::path& path,
                                  std::string message, std::string details = {}) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
            .code = std::move(code), .category = "Editor.Import",
            .message = std::move(message), .details = std::move(details)};
        diagnostic.source.path = Utf8Path(path);
        diag::Emit(std::move(diagnostic));
    };
    try {
        const ProjectContext& context = m_project.Context();
        if (!context.IsOpen()) {
            reportFailure("PXIMPORT9017", {}, "請先開啟專案再匯入素材");
            return;
        }
        m_importSources.clear();
        std::error_code ec;
        constexpr std::size_t kMaximumReviewItems = 50000;
        bool reachedLimit = false;
        for (const auto& raw : paths) {
            if (raw.empty() || reachedLimit) continue;
            ec.clear();
            auto source = fs::absolute(raw, ec);
            if (ec) { ec.clear(); source = raw; }
            if (!fs::exists(source, ec)) {
                reportFailure("PXIMPORT9018", source, "找不到匯入來源", ec.message());
                ec.clear();
                continue;
            }
            if (IsPathWithin(source, context.DataRoot())) {
                if (fs::is_regular_file(source, ec)) {
                    m_selectedAsset = RuntimePath(context.root, source);
                    m_assetSelectionModel.selected = {m_selectedAsset};
                }
                ec.clear();
                continue;
            }
            if (fs::is_directory(source, ec)) {
                ec.clear();
                fs::recursive_directory_iterator it(
                    source, fs::directory_options::skip_permission_denied, ec), end;
                if (ec) {
                    reportFailure("PXIMPORT9019", source, "無法讀取匯入資料夾", ec.message());
                    ec.clear();
                    continue;
                }
                std::unordered_set<std::string> visitedDirectories;
                visitedDirectories.insert(Utf8Path(fs::weakly_canonical(source, ec)));
                ec.clear();
                while (it != end) {
                    ec.clear();
                    if (it->is_directory(ec)) {
                        const fs::file_status linkStatus = it->symlink_status(ec);
                        const fs::path canonical = fs::weakly_canonical(it->path(), ec);
                        std::string key = Utf8Path(canonical);
#ifdef _WIN32
                        std::transform(key.begin(), key.end(), key.begin(),
                            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
                        const bool repeats = !key.empty() && !visitedDirectories.insert(key).second;
                        const bool outsideSource = ec || !IsPathWithin(canonical, source);
                        if (ec || fs::is_symlink(linkStatus) || repeats || outsideSource)
                            it.disable_recursion_pending();
                        ec.clear();
                    }
                    bool regular = it->is_regular_file(ec);
                    if (!ec && regular && it->path().extension() != ".pxmeta") {
                        auto relative = fs::relative(it->path(), source, ec);
                        if (ec) { ec.clear(); relative = it->path().filename(); }
                        m_importSources.push_back({it->path(), relative});
                        if (m_importSources.size() >= kMaximumReviewItems) {
                            reachedLimit = true;
                            break;
                        }
                    }
                    ec.clear();
                    it.increment(ec);
                    if (ec) {
                        // skip_permission_denied handles common access failures; other
                        // iterator failures end this source without crashing the editor.
                        reportFailure("PXIMPORT9020", source,
                                      "掃描匯入資料夾時略過無法讀取的內容", ec.message());
                        ec.clear();
                        break;
                    }
                }
            } else {
                ec.clear();
                if (fs::is_regular_file(source, ec) && source.extension() != ".pxmeta")
                    m_importSources.push_back({source, source.filename()});
                ec.clear();
            }
        }
        if (reachedLimit) {
            diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
                .code = "PXIMPORT9021", .category = "Editor.Import",
                .message = "單次匯入最多顯示 50,000 個檔案",
                .details = "請分批匯入大型素材資料夾。"};
            diag::Emit(std::move(diagnostic));
        }
        std::sort(m_importSources.begin(), m_importSources.end(), [](const auto& a,const auto& b){
            return a.path.generic_u8string() < b.path.generic_u8string();
        });
        m_importSources.erase(std::unique(m_importSources.begin(),m_importSources.end(),
            [](const auto& a,const auto& b){return a.path==b.path;}),m_importSources.end());
        if (m_importSources.empty()) {
            if (m_selectedAsset.empty())
                reportFailure("PXIMPORT9022", {}, "沒有可匯入的素材");
            return;
        }
        const fs::path destination = context.root /
            (m_assetDir.empty() ? fs::path("Content") : fs::path(m_assetDir));
        ec.clear();
        m_importDestinationText = fs::relative(destination, context.root, ec).generic_string();
        if (ec) m_importDestinationText = "Content";
        auto prepared = m_importService.Prepare(context.root, destination, m_importSources,
                                                m_importAutoOrganize,
                                                m_importPreserveFolders);
        if (!prepared) return;
        m_importReview = prepared.TakeValue();
        m_importReview->preserveIdentity = m_importPreserveIdentity;
        m_reportedImportFailure.clear();
        m_showImportReview = true;
    } catch (const fs::filesystem_error& error) {
        reportFailure("PXIMPORT9023", error.path1(),
                      "建立匯入預覽時發生檔案系統錯誤", error.what());
    } catch (const std::exception& error) {
        reportFailure("PXIMPORT9024", {}, "建立匯入預覽時發生未預期錯誤", error.what());
    } catch (...) {
        reportFailure("PXIMPORT9025", {}, "建立匯入預覽時發生未知錯誤");
    }
}

void EditorApp::ImportClipboardAssets() {
#ifdef _WIN32
    std::vector<fs::path> paths;
    if (!OpenClipboard(nullptr)) {
        Log("Clipboard is not available.");
        return;
    }
    if (HANDLE data = GetClipboardData(CF_HDROP)) {
        HDROP drop = reinterpret_cast<HDROP>(data);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) {
                continue;
            }
            std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1);
            DragQueryFileW(drop, i, buffer.data(), len + 1);
            paths.emplace_back(buffer.data());
        }
    }
    CloseClipboard();

    if (paths.empty()) {
        Log("Clipboard has no copied files to import.");
        return;
    }
    ImportAssetFiles(paths);
#else
    Log("Clipboard file import is only implemented on Windows.");
#endif
}

bool EditorApp::Init() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        PX_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    if (!TTF_Init()) {
        PX_LOG_ERROR("TTF_Init failed: {}", SDL_GetError());
        return false;
    }
    if (!m_window.Create("PrismatiX Editor", 1600, 960, true)) {
        return false;
    }
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (const char* base = SDL_GetBasePath()) {
        m_basePath = base;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    std::filesystem::path settingsRoot;
#ifdef _WIN32
    char* local = nullptr;
    std::size_t localLength = 0;
    if (_dupenv_s(&local, &localLength, "LOCALAPPDATA") == 0 && local && *local) {
        settingsRoot = std::filesystem::path(local) / "PrismatiXEditor";
    }
    std::free(local);
#endif
    if (settingsRoot.empty()) settingsRoot = std::filesystem::path(m_basePath) / ".editor";
    std::error_code settingsError;
    std::filesystem::create_directories(settingsRoot, settingsError);
    m_iniPath = (settingsRoot / "EditorLayout-v4.ini").string();
    m_editorSettingsPath = settingsRoot / "EditorSettings.pxres";
    m_editorSessionPath = settingsRoot / "EditorSession.pxres";
    io.IniFilename = m_iniPath.c_str();
    m_buildLayout = !std::filesystem::exists(m_iniPath);
    LoadFonts();
    ApplyTheme();

    if (!ImGui_ImplSDL3_InitForSDLRenderer(m_window.Handle(), m_window.Renderer())) {
        PX_LOG_ERROR("ImGui SDL3 init failed");
        return false;
    }
    if (!ImGui_ImplSDLRenderer3_Init(m_window.Renderer())) {
        PX_LOG_ERROR("ImGui SDLRenderer3 init failed");
        return false;
    }
    m_imguiReady = true;
    m_running = true;

    m_preview = std::make_unique<RuntimeHost>(m_window.Renderer());
    ConfigureDesigner(m_designer);
    m_textures = std::make_unique<EditorTextures>(m_window.Renderer());
    m_nodeHeaderTex = m_textures->LoadId(m_basePath + "EditorAssets/NodeHeader.png", &m_nodeHeaderW, &m_nodeHeaderH);
    m_flow.SetHeaderTexture(m_nodeHeaderTex, m_nodeHeaderW, m_nodeHeaderH);

    const std::string cwd = std::filesystem::current_path().string();
    std::snprintf(m_openPath, sizeof(m_openPath), "%s", cwd.c_str());
    std::snprintf(m_newPath, sizeof(m_newPath), "%s", cwd.c_str());
    std::snprintf(m_newName, sizeof(m_newName), "%s", "MyGame");
    LoadRecentProjects();
    BuildCommands();
    diag::Global().SetListener([this](const diag::Diagnostic& diagnostic) {
        m_console.push_back("[" + diagnostic.code + "] " + diagnostic.message);
        if (m_console.size() > 500) m_console.erase(m_console.begin(),m_console.begin()+static_cast<std::ptrdiff_t>(m_console.size()-500));
        m_toasts.Push(diagnostic);
    });
    Log("PrismatiX Editor started.");
    return true;
}

void EditorApp::ConfigureDesigner(UIDesigner& designer) {
    designer.SetImageSizeResolver([this](const std::string& path)->std::optional<Vec2>{return m_preview?m_preview->ImageSize(path):std::nullopt;});
    designer.SetIdentityRegistrar([this](const std::filesystem::path& path){return EnsureAssetIdentity(path);});
    designer.SetAnimationPreview([this](const Uuid& clip,float time,bool playing){return m_preview?m_preview->PreviewUIAnimation(clip,time,playing):Status::Ok();});
    designer.SetBehaviorDebugProvider([this]{return m_preview?m_preview->UIBehaviorState():ui::BehaviorRuntimeState{};});
    designer.SetAnimationDebugProvider([this]{return m_preview?m_preview->UIAnimationState():ui::UIAnimationRuntimeState{};});
    designer.SetAnimationParameterTester([this](const std::string_view parameter,const Variant& value){return m_preview?m_preview->SetUIAnimationParameter(parameter,value):Status::Ok();});
    designer.SetComponentWriter([this](const std::filesystem::path& requested,const std::string& text)->Result<ResourceRefValue>{
        const auto absolute=requested.is_absolute()?requested:m_project.Context().root/requested;std::error_code error;std::filesystem::create_directories(absolute.parent_path(),error);if(error)return Result<ResourceRefValue>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3040",.category="Editor.UIDesigner",.message="Cannot create component directory",.details=error.message()});
        const Status written=io::AtomicFile::WriteText(absolute,text);if(!written)return Result<ResourceRefValue>::Failure(written.Diagnostics());auto registered=m_assetRegistry.RegisterAsset(m_project.Context().root,absolute,".pxcomponent");if(!registered)return Result<ResourceRefValue>::Failure(registered.Diagnostics());m_assets.Scan(m_project.Context());std::error_code relativeError;const auto relative=std::filesystem::relative(absolute,m_project.Context().root,relativeError).generic_string();return Result<ResourceRefValue>::Success(ResourceRefValue{registered.Value().id,relativeError?absolute.generic_string():relative});
    });
    designer.SetOpenResource([this](const ResourceRefValue& reference){
        if(!reference.lastKnownPath.empty())OpenDocTab(reference.lastKnownPath);
    });
    designer.SetOnEdit([this] {
        if (m_preview && m_designer.Document()) {
            std::error_code error;
            const std::string runtimePath = std::filesystem::relative(
                m_designer.Document()->Path(), m_project.Context().root, error).generic_string();
            m_preview->ApplyUIDocumentPatch(m_designer.Document()->Data(),
                                            error ? m_designer.Document()->Path().generic_string() : runtimePath);
            m_docs.SetDirty(m_designer.Document()->Path(), m_designer.Dirty(),
                            m_designer.Document()->History().Cursor());
        }
    });
}

Status EditorApp::EnsureAssetIdentity(const std::filesystem::path& path){
    return EnsureAssetIdentities({path});
}

Status EditorApp::EnsureAssetIdentities(const std::vector<std::filesystem::path>& paths){
    if(!m_project.Context().IsOpen())return Status::Ok();
    for(const auto& path:paths){
        auto registered=m_assetRegistry.RegisterAsset(m_project.Context().root,path,AssetDatabase::Classify(path));
        if(!registered)return Status::Fail(registered.Diagnostics());
    }
    m_assets.Scan(m_project.Context());
    const Status scanned=m_assetRegistry.Scan(m_project.Context().root);
    if(!scanned)m_showAssetIdentity=true;
    return Status::Ok();
}


void EditorApp::Run() {
    while (m_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                ImportAssetFiles({ PathFromUtf8(event.drop.data) });
            }
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
                m_project.Context().IsOpen()) {
                // Pick up files changed outside the editor (Unity-style refresh).
                m_assets.Scan(m_project.Context());
                m_flowStale = true;
                CheckExternalDocuments();
            }
            if (event.type == SDL_EVENT_QUIT) {
                m_running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        BuildUI();

        ImGui::Render();
        PrepareImGuiRenderer(m_window.Renderer());
        m_window.Clear(Color{ 18, 20, 26, 255 });
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_window.Renderer());
        m_window.Present();
    }
}

void EditorApp::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/msjh.ttc",
#endif
        m_basePath + "EditorAssets/UIFont.ttf",
        "Resources/Fonts/NotoSansTC-Regular.ttf",
        "Resources/Fonts/NotoSansTC-Bold.ttf",
        "../../../Resources/Fonts/NotoSansTC-Bold.ttf" };
    ImFont* loaded = nullptr;
    for (const std::string& path : candidates) {
        if (std::filesystem::exists(path)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            loaded = io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());
            if (loaded) {
                Log("UI font: " + path);
                break;
            }
        }
    }
    if (!loaded) {
        io.Fonts->AddFontDefault();
        Log("UI font: built-in (UIFont.ttf not found).");
    }
    const std::string iconCandidates[]{m_basePath+"EditorAssets/FontAwesomeFree-Solid-900.otf",
        m_basePath+"EditorAssets/fa-solid-900.ttf"};
    for(const auto& path:iconCandidates)if(std::filesystem::exists(path)){
        static const ImWchar ranges[]{0xf000,0xf8ff,0};ImFontConfig config;config.MergeMode=true;
        config.PixelSnapH=true;config.GlyphMinAdvanceX=15.0f;
        if(io.Fonts->AddFontFromFileTTF(path.c_str(),15.0f,&config,ranges)){m_iconFontLoaded=true;Log("Editor icon font: "+path);break;}
    }
}

void EditorApp::ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 7);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(4, 4);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 10.0f;
    s.TabBarBorderSize = 0.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* c = s.Colors;
    const ImVec4 bg0(0.086f, 0.102f, 0.125f, 1.00f); // #161A20
    const ImVec4 bg1(0.125f, 0.145f, 0.176f, 1.00f); // #20252D
    const ImVec4 bg2(0.161f, 0.184f, 0.224f, 1.00f); // #292F39
    const ImVec4 bg3(0.204f, 0.231f, 0.278f, 1.00f); // #343B47
    const ImVec4 accent(0.278f, 0.549f, 0.749f, 1.00f); // #478CBF
    const ImVec4 accentDim(0.20f, 0.38f, 0.52f, 1.00f);
    const ImVec4 text(0.847f, 0.871f, 0.914f, 1.00f); // #D8DEE9
    const ImVec4 textDim(0.553f, 0.596f, 0.659f, 1.00f); // #8D98A8

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = bg0;
    c[ImGuiCol_Border] = ImVec4(0.16f, 0.18f, 0.22f, 0.9f);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive] = bg3;
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = accentDim;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = accentDim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = bg2;
    c[ImGuiCol_HeaderHovered] = bg3;
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Separator] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_Tab] = bg1;
    c[ImGuiCol_TabHovered] = accentDim;
    c[ImGuiCol_TabSelected] = bg3;
    c[ImGuiCol_TabDimmed] = bg1;
    c[ImGuiCol_TabDimmedSelected] = bg2;
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_PlotHistogram] = accent;
}


void EditorApp::Shutdown() {
    SaveEditorSession();
    diag::Global().SetListener({});
    if (Status recovery = m_recovery.EndSession(); !recovery) {
        for (auto diagnostic : recovery.Diagnostics()) px::diag::Emit(std::move(diagnostic));
    }
    if (m_imguiReady) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    m_textures.reset();
    m_preview.reset();
    m_window.Destroy();
    TTF_Quit();
    SDL_Quit();
}

}  // namespace px::editor
