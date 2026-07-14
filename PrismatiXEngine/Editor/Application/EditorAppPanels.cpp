#include "Editor/Application/EditorApp.h"

#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/IO/AtomicFile.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/Styles/StyleResolver.h"
#include "Editor/Theme/EditorIcon.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace px::editor {

namespace {
const std::array<const char*, 8> kAssetTypes = { "all", "image", "audio", "script", "ui", "font", "lua", "other" };
constexpr const char* kResourcePayload = "PX_RESOURCE_PATH";
namespace fs = std::filesystem;

std::string Utf8Path(const fs::path& path) noexcept {
    try {
        const std::u8string value = path.generic_u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    } catch (...) {
        return "<無法顯示的路徑>";
    }
}

// SDL folder dialog result; the callback may run off the UI thread.
struct FolderPickState {
    std::mutex mutex;
    std::string result;
    bool pending = false;
    int target = 0;  // 1 = open-project path, 2 = new-project location
};
FolderPickState g_folderPick;

struct AssetPickState {
    std::mutex mutex;
    std::vector<std::filesystem::path> paths;
    std::string error;
    bool completed = false;
    bool dialogOpen = false;
};
AssetPickState g_assetPick;

bool BeginAssetPicker() {
    std::lock_guard lock(g_assetPick.mutex);
    if (g_assetPick.dialogOpen) return false;
    g_assetPick.paths.clear();
    g_assetPick.error.clear();
    g_assetPick.completed = false;
    g_assetPick.dialogOpen = true;
    return true;
}

void SDLCALL OnFolderPicked(void*, const char* const* filelist, int) {
    std::lock_guard<std::mutex> lock(g_folderPick.mutex);
    if (filelist && filelist[0]) {
        g_folderPick.result = filelist[0];
        g_folderPick.pending = true;
    }
}

ImU32 AssetTypeColor(const std::string& type) {
    if (type == "image") return IM_COL32(82, 148, 226, 255);
    if (type == "audio") return IM_COL32(190, 132, 80, 255);
    if (type == "script") return IM_COL32(103, 190, 144, 255);
    if (type == "ui") return IM_COL32(170, 120, 220, 255);
    if (type == "font") return IM_COL32(200, 180, 90, 255);
    if (type == "lua") return IM_COL32(90, 160, 200, 255);
    return IM_COL32(120, 128, 140, 255);
}

void SDLCALL OnAssetsPicked(void*, const char* const* filelist, int) {
    // Never allow a path conversion/allocation exception to cross SDL's C callback
    // boundary. Doing so calls std::terminate and looks like a random editor crash.
    std::vector<std::filesystem::path> paths;
    std::string error;
    try {
        if (filelist) {
            for (std::size_t index = 0; filelist[index]; ++index)
                paths.emplace_back(std::u8string(
                    reinterpret_cast<const char8_t*>(filelist[index])));
        }
    } catch (const std::exception& exception) {
        error = std::string("無法解析檔案選擇結果：") + exception.what();
        paths.clear();
    } catch (...) {
        error = "無法解析檔案選擇結果：未知錯誤";
        paths.clear();
    }
    std::lock_guard lock(g_assetPick.mutex);
    g_assetPick.paths = std::move(paths);
    g_assetPick.error = std::move(error);
    g_assetPick.completed = true;
    g_assetPick.dialogOpen = false;
}

std::string HumanSize(std::uintmax_t bytes) {
    static const char* units[]{"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes); int unit = 0;
    while (value >= 1024.0 && unit < 3) { value /= 1024.0; ++unit; }
    std::ostringstream out; out << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
                                << value << ' ' << units[unit]; return out.str();
}

std::string HumanTime(const std::filesystem::path& path) {
    std::error_code ec; const auto value = std::filesystem::last_write_time(path, ec);
    if (ec) return "—";
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    const std::time_t time = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream out; out << std::put_time(&local, "%Y-%m-%d %H:%M"); return out.str();
}

std::size_t DirectoryItemCount(const std::filesystem::path& path) {
    std::error_code ec;
    std::size_t count = 0;
    for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->path().extension() != ".pxmeta") ++count;
    }
    return count;
}

struct ImportUndoState {
    std::filesystem::path root;
    std::vector<ImportCommitRecord> records;
    ~ImportUndoState() {
        std::error_code error;
        if (!root.empty()) std::filesystem::remove_all(root, error);
    }
};

Status ImportUndoError(const std::filesystem::path& path, std::string message,
                       std::string details = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXIMPORT9013",
        .category="Editor.ImportUndo",.message=std::move(message),.details=std::move(details)};
    diagnostic.source.path=path.generic_string();
    return Status::Fail(std::move(diagnostic));
}

Status SetImportApplied(const std::shared_ptr<ImportUndoState>& state, bool applied) {
    std::error_code error;
    std::vector<std::size_t> completed;
    const auto move = [&](const fs::path& from, const fs::path& to) -> bool {
        error.clear();
        std::filesystem::create_directories(to.parent_path(), error);
        if (error) return false;
        std::filesystem::rename(from, to, error);
        return !error;
    };
    for (std::size_t index=0; index<state->records.size(); ++index) {
        const auto& record=state->records[index];
        bool ok=true;
        if (!applied) {
            ok=move(record.target,record.importedBackup);
            if(ok&&record.replaced)ok=move(record.originalBackup,record.target);
            else if(ok&&std::filesystem::exists(record.meta))ok=move(record.meta,record.metaBackup);
        } else if(record.replaced) {
            ok=move(record.target,record.originalBackup);
            if(ok)ok=move(record.importedBackup,record.target);
        } else {
            ok=move(record.importedBackup,record.target);
            if(ok&&std::filesystem::exists(record.metaBackup))ok=move(record.metaBackup,record.meta);
        }
        if(!ok){
            for(auto iterator=completed.rbegin();iterator!=completed.rend();++iterator){
                const auto& done=state->records[*iterator];std::error_code ignored;
                if(!applied){
                    if(done.replaced){std::filesystem::rename(done.target,done.originalBackup,ignored);ignored.clear();}
                    std::filesystem::rename(done.importedBackup,done.target,ignored);
                    if(!done.replaced&&std::filesystem::exists(done.metaBackup)){ignored.clear();std::filesystem::rename(done.metaBackup,done.meta,ignored);}
                }else{
                    std::filesystem::rename(done.target,done.importedBackup,ignored);
                    if(done.replaced){ignored.clear();std::filesystem::rename(done.originalBackup,done.target,ignored);}
                    else if(std::filesystem::exists(done.meta)){ignored.clear();std::filesystem::rename(done.meta,done.metaBackup,ignored);}
                }
            }
            return ImportUndoError(record.target, applied?"無法重做素材匯入":"無法復原素材匯入",error.message());
        }
        completed.push_back(index);
    }
    return Status::Ok();
}
}
void EditorApp::BuildDockLayout(unsigned int dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    if(m_workspace==EditorWorkspace::UI){
        ImGuiID center=dockspaceId;ImGuiID bottom=ImGui::DockBuilderSplitNode(center,ImGuiDir_Down,.24f,nullptr,&center);
        ImGui::DockBuilderDockWindow("UI Designer",center);ImGui::DockBuilderDockWindow("Assets",bottom);ImGui::DockBuilderDockWindow("Open Documents",bottom);ImGui::DockBuilderDockWindow("Problems",bottom);ImGui::DockBuilderDockWindow("Console",bottom);ImGui::DockBuilderFinish(dockspaceId);return;
    }

    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID storyPreview = 0;
    if (m_workspace == EditorWorkspace::Story)
        storyPreview = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.46f, nullptr, &right);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);

    ImGui::DockBuilderDockWindow("Open Documents", left);
    ImGui::DockBuilderDockWindow("Assets", leftBottom);
    if (m_workspace == EditorWorkspace::Story)
        ImGui::DockBuilderDockWindow("Story Library", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    if (storyPreview) ImGui::DockBuilderDockWindow("Story Preview", storyPreview);
    switch (m_workspace) {
        case EditorWorkspace::UI: break;
        case EditorWorkspace::Story:
            ImGui::DockBuilderDockWindow("Node Editor", center);
            ImGui::DockBuilderDockWindow("Problems", bottom);
            ImGui::DockBuilderDockWindow("Console", bottom);
            break;
        case EditorWorkspace::Flow:
            ImGui::DockBuilderDockWindow("Flow", center);
            ImGui::DockBuilderDockWindow("Problems", bottom);
            ImGui::DockBuilderDockWindow("Console", bottom);
            break;
        case EditorWorkspace::Script:
            ImGui::DockBuilderDockWindow("Scripting", center);
            ImGui::DockBuilderDockWindow("Problems", bottom);
            ImGui::DockBuilderDockWindow("Console", bottom);
            break;
    }
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorApp::RenderWelcome() {
    // Pick up an async folder-dialog result.
    {
        std::lock_guard<std::mutex> lock(g_folderPick.mutex);
        if (g_folderPick.pending) {
            char* dst = g_folderPick.target == 2 ? m_newPath : m_openPath;
            std::snprintf(dst, g_folderPick.target == 2 ? sizeof(m_newPath) : sizeof(m_openPath),
                          "%s", g_folderPick.result.c_str());
            g_folderPick.pending = false;
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.027f, 0.031f, 0.039f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const ImVec2 panel(560, 420);
    ImGui::SetCursorPos(ImVec2((vp->Size.x - panel.x) * 0.5f, (vp->Size.y - panel.y) * 0.5f));
    ImGui::BeginChild("welcomePanel", panel, ImGuiChildFlags_Borders);

    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextColored(ImVec4(0.247f, 0.553f, 0.949f, 1.0f), "PrismatiX");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Visual Novel Engine");
    ImGui::Dummy(ImVec2(0, 16));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 12));

    const ImVec2 btn(panel.x - 24, 42);
    if (ImGui::BeginTabBar("welcomeTabs")) {
        if (!m_recentProjects.empty() && ImGui::BeginTabItem("Recent")) {
            ImGui::Dummy(ImVec2(0, 6));
            for (const std::string& path : m_recentProjects) {
                const std::string label =
                    std::filesystem::path(path).filename().string() + "  —  " + path;
                if (ImGui::Button(label.c_str(), ImVec2(panel.x - 24, 36))) {
                    OpenProject(std::filesystem::path(path));
                    m_screen = Screen::Workspace;
                    break;
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Open")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextDisabled("Project folder");
            ImGui::SetNextItemWidth(panel.x - 24 - 86);
            ImGui::InputText("##path", m_openPath, sizeof(m_openPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##open")) {
                g_folderPick.target = 1;
                SDL_ShowOpenFolderDialog(&OnFolderPicked, nullptr, m_window.Handle(), nullptr,
                                         false);
            }
            ImGui::Dummy(ImVec2(0, 10));
            if (ImGui::Button("Open Project", btn)) {
                OpenProject(std::filesystem::path(m_openPath));
                m_screen = Screen::Workspace;
            }
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::Button("Open Current Folder", btn)) {
                OpenProject(std::filesystem::current_path());
                m_screen = Screen::Workspace;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("New Project")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextDisabled("Project name");
            ImGui::SetNextItemWidth(panel.x - 24);
            ImGui::InputText("##name", m_newName, sizeof(m_newName));
            ImGui::TextDisabled("Location (a subfolder with the name is created here)");
            ImGui::SetNextItemWidth(panel.x - 24 - 86);
            ImGui::InputText("##newpath", m_newPath, sizeof(m_newPath));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##new")) {
                g_folderPick.target = 2;
                SDL_ShowOpenFolderDialog(&OnFolderPicked, nullptr, m_window.Handle(), nullptr,
                                         false);
            }
            ImGui::Dummy(ImVec2(0, 10));
            if (ImGui::Button("Create Project", btn)) {
                const std::filesystem::path root = std::filesystem::path(m_newPath) / m_newName;
                if (m_project.Create(root, m_newName, m_basePath + "EditorAssets/UIFont.ttf")) {
                    OpenProject(root);
                    m_screen = Screen::Workspace;
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Dummy(ImVec2(0, 14));
    if (ImGui::Button("Quit", btn)) {
        m_running = false;
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void EditorApp::BuildUI() {
    if (m_screen == Screen::Welcome) {
        RenderWelcome();
        return;
    }
    static const char* dockNames[]{"PrismatiX.UI.Dock", "PrismatiX.Story.Dock",
                                   "PrismatiX.Flow.Dock", "PrismatiX.Script.Dock"};
    const auto workspaceIndex = static_cast<std::size_t>(m_workspace);
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(ImGui::GetID(dockNames[workspaceIndex]),
                                                        ImGui::GetMainViewport());
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
    if (m_buildLayout || m_workspaceLayoutDirty[workspaceIndex] || dockNode == nullptr || dockNode->IsLeafNode()) {
        BuildDockLayout(dockId);
        m_buildLayout = false;
        m_workspaceLayoutDirty[workspaceIndex] = false;
    }
    HandleShortcuts();
    SyncDesigner();
    SyncDocumentStates();
    RenderMenuBar();
    RenderCommandPalette();
    RenderShortcutsWindow();
    RenderOpenDocuments();
    RenderAssets();
    RenderImportReview();
    if(m_workspace!=EditorWorkspace::UI)RenderInspector();
    switch (m_workspace) {
        case EditorWorkspace::UI:
            RenderPreview();
            RenderProblems(); RenderConsole(); break;
        case EditorWorkspace::Story:
            RenderNodeEditor(); RenderStoryLibrary(); RenderStoryPreview(); RenderProblems(); RenderConsole(); break;
        case EditorWorkspace::Flow:
            RenderFlow(); RenderProblems(); RenderConsole(); break;
        case EditorWorkspace::Script:
            RenderScripting(); RenderProblems(); RenderConsole(); break;
    }
    RenderRecoveryCenter();
    RenderAssetIdentityResolver();
    RenderProjectTrash();
    RenderBuild();
    RenderLocalization();
    RenderExternalDocumentConflict();
    RenderStatusBar();
    RenderDiagnosticToasts();

    if (auto* document = m_designer.Document(); document && document->History().Dirty() &&
        m_recovery.ShouldSnapshot(document->DocumentId(), m_designer.LastEditTime())) {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(document->Path(), ec).time_since_epoch().count();
        const Status snapshot = m_recovery.SaveSnapshot(document->DocumentId(), document->Path(),
                                                         ec ? std::string{} : std::to_string(stamp),
                                                         document->Serialize());
        if (!snapshot) for (const auto& diagnostic : snapshot.Diagnostics()) diag::Emit(diagnostic);
    }
    for(const auto& [_,session]:m_inactiveDesigners){
        if(!session.editor||!session.editor->Document())continue;auto* document=session.editor->Document();
        if(!document->History().Dirty()||!m_recovery.ShouldSnapshot(document->DocumentId(),session.editor->LastEditTime()))continue;
        std::error_code error;const auto stamp=std::filesystem::last_write_time(document->Path(),error).time_since_epoch().count();
        const Status snapshot=m_recovery.SaveSnapshot(document->DocumentId(),document->Path(),error?std::string{}:std::to_string(stamp),document->Serialize());
        if(!snapshot)for(const auto& diagnostic:snapshot.Diagnostics())diag::Emit(diagnostic);
    }

}

Status EditorApp::ActivateUIDocument(const std::filesystem::path& absolutePath,
                                     const std::string& requestedRuntimePath) {
    const auto canonical=DocumentManager::Canonical(absolutePath);
    const std::string key=canonical.generic_string();
    if(!m_designerPath.empty()&&DocumentManager::Canonical(m_designerPath)==canonical){
        (void)m_docs.Activate(canonical);m_uiFocusRequest=canonical;return Status::Ok();
    }
    if(m_designer.Document()){
        const auto current=DocumentManager::Canonical(m_designer.Document()->Path());
        DesignerDocumentSession session{.canonicalPath=current,
            .editor=std::make_unique<UIDesigner>(std::move(m_designer))};
        m_inactiveDesigners[current.generic_string()]=std::move(session);
    }
    if(auto found=m_inactiveDesigners.find(key);found!=m_inactiveDesigners.end()){
        m_designer=std::move(*found->second.editor);m_inactiveDesigners.erase(found);
    }else{
        m_designer=UIDesigner{};ConfigureDesigner(m_designer);
        const Status opened=m_designer.Open(canonical);if(!opened)return opened;
    }
    ConfigureDesigner(m_designer);
    m_designerPath=canonical.string();
    TrackDocument(canonical,DocumentType::UIScene,m_designer.Dirty());
    (void)m_docs.Activate(canonical);
    m_uiFocusRequest=canonical;
    if(const auto* session=m_docs.Find(canonical)){
        m_designer.ViewportState().zoom=session->viewport.zoom;
        m_designer.ViewportState().scrollX=session->viewport.scrollX;
        m_designer.ViewportState().scrollY=session->viewport.scrollY;
        m_designer.ViewportState().fitToViewport=session->viewport.fitToViewport;
        m_designer.ViewportState().leftPanelVisible=session->viewport.leftPanelVisible;
        m_designer.ViewportState().rightPanelVisible=session->viewport.rightPanelVisible;
        m_designer.ViewportState().bottomPanelVisible=session->viewport.bottomPanelVisible;
        m_designer.ViewportState().leftPanelWidth=session->viewport.leftPanelWidth;
        m_designer.ViewportState().rightPanelWidth=session->viewport.rightPanelWidth;
        m_designer.ViewportState().bottomPanelHeight=session->viewport.bottomPanelHeight;
        m_designer.ViewportState().applyStoredScroll=true;
    }
    std::string runtimePath=requestedRuntimePath;
    if(runtimePath.empty()){
        std::error_code error;runtimePath=fs::relative(canonical,m_project.Context().root,error).generic_string();
        if(error)runtimePath=canonical.generic_string();
    }
    if(m_preview&&m_designer.Document())m_preview->LoadUIDocument(m_designer.Document()->Data(),runtimePath);
    return Status::Ok();
}

void EditorApp::SyncDesigner() {
    if (!m_preview) {
        return;
    }
    const bool uiMode = m_previewMode == 0;
    if (!uiMode) {
        return;
    }
    if (!m_designer.Document()) {
        const std::string runtimePath = m_project.Context().StartScenePath();
        if (runtimePath.empty()) return;
        const Status status = ActivateUIDocument(m_project.Context().root / runtimePath, runtimePath);
        if (!status) {
            for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
        }
        return;
    }
    std::error_code error;
    const std::string runtimePath = fs::relative(m_designer.Document()->Path(),
                                                 m_project.Context().root, error).generic_string();
    if (!error && m_preview->CurrentUIPath() != runtimePath) {
        // The active document is the source of truth. Preview navigation must
        // never reactivate an older scene on the following frame.
        m_preview->LoadUIDocument(m_designer.Document()->Data(), runtimePath);
    }
}

void EditorApp::SetWorkspace(EditorWorkspace workspace) {
    if (m_workspace == workspace) return;
    m_workspace = workspace;
    if (workspace == EditorWorkspace::UI) m_previewMode = 0;
    else m_previewMode = 1;
}

void EditorApp::RenderWorkspaceSwitcher() {
    const char* labels[]{"介面", "劇情", "流程", "腳本"};
    const float totalWidth = 4.0f * 64.0f + 3.0f * ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 12.0f,
                                  ImGui::GetWindowWidth() * 0.5f - totalWidth * 0.5f));
    for (int index = 0; index < 4; ++index) {
        if (index) ImGui::SameLine();
        const bool active = static_cast<int>(m_workspace) == index;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.278f, 0.549f, 0.749f, 1));
        if (ImGui::Button(labels[index], ImVec2(64, 0)))
            SetWorkspace(static_cast<EditorWorkspace>(index));
        if (active) ImGui::PopStyleColor();
    }
}

void EditorApp::RenderMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("專案")) {
        if (ImGui::MenuItem("開啟目前資料夾")) {
            OpenProject(std::filesystem::current_path());
        }
        if (ImGui::MenuItem("重新掃描素材")) {
            m_assets.Scan(m_project.Context());
        }
        if (ImGui::MenuItem("匯入素材…", "Ctrl+Shift+I")) {
            static const SDL_DialogFileFilter filters[]{{"素材檔案", "png;jpg;jpeg;webp;bmp;mp3;ogg;wav;flac;opus;mp4;webm;ttf;otf;pxscenario;pxanim;pxscene;pxres;pxextension;lua"}, {"所有檔案", "*"}};
            if (BeginAssetPicker())
                SDL_ShowOpenFileDialog(&OnAssetsPicked, nullptr, m_window.Handle(), filters,
                                       static_cast<int>(std::size(filters)), nullptr, true);
        }
        if (ImGui::MenuItem("從剪貼簿匯入", "Ctrl+V")) {
            ImportClipboardAssets();
        }
        if(ImGui::MenuItem("Build 設定…"))m_showBuildWindow=true;
        if (ImGui::MenuItem("全部儲存", "Ctrl+Shift+S")) {
            SaveAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("離開")) {
            m_running = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("編輯")) {
        const bool projectHistory = m_assetsFocused;
        const bool uiHistory = !projectHistory && m_previewMode == 0 && m_designer.Document();
        NodeGraphEditor* graphHistory=ActiveDocPtr();
        const bool canUndo = projectHistory ? m_projectHistory.CanUndo() : (uiHistory ? m_designer.Document()->History().CanUndo() : (graphHistory&&graphHistory->CanUndo()));
        const bool canRedo = projectHistory ? m_projectHistory.CanRedo() : (uiHistory ? m_designer.Document()->History().CanRedo() : (graphHistory&&graphHistory->CanRedo()));
        const std::string undoLabel = projectHistory ? m_projectHistory.NextUndoLabel() : (uiHistory ? m_designer.Document()->History().NextUndoLabel() : std::string("劇情編輯"));
        const std::string redoLabel = projectHistory ? m_projectHistory.NextRedoLabel() : (uiHistory ? m_designer.Document()->History().NextRedoLabel() : std::string("劇情編輯"));
        if (ImGui::MenuItem(("Undo " + undoLabel).c_str(), "Ctrl+Z", false, canUndo)) {
            if(projectHistory)m_projectHistory.Undo();else if (uiHistory) m_designer.Undo(); else if(graphHistory)graphHistory->Undo();
        }
        if (ImGui::MenuItem(("Redo " + redoLabel).c_str(), "Ctrl+Y", false, canRedo)) {
            if(projectHistory)m_projectHistory.Redo();else if (uiHistory) m_designer.Redo(); else if(graphHistory)graphHistory->Redo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("鍵盤快捷鍵", "F1", m_showShortcuts)) {
            m_showShortcuts = !m_showShortcuts;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("檢視")) {
        if (ImGui::MenuItem("快速開啟", "Ctrl+K")) {
            m_quickOpenOpen = true;
            m_quickOpenFilter[0] = 0;
        }
        if (ImGui::MenuItem("命令面板", "Ctrl+P")) {
            m_paletteOpen = true;
            m_paletteFocus = true;
            m_paletteFilter[0] = 0;
        }
        if (ImGui::MenuItem("重設目前工作區配置")) {
            m_workspaceLayoutDirty[static_cast<std::size_t>(m_workspace)] = true;
        }
        if (ImGui::MenuItem("開啟的文件", nullptr, m_showOpenDocuments)) {
            m_showOpenDocuments = !m_showOpenDocuments;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("執行")) {
        if (ImGui::MenuItem("執行專案", "F6")) RunDev();
        if (ImGui::MenuItem("停止", "F8", false, false)) {}
        if (ImGui::MenuItem("Build")) RunBuild();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("工具")) {
        if(ImGui::MenuItem("Localization…"))m_showLocalizationWindow=true;
        if(ImGui::MenuItem("專案垃圾桶…"))m_showProjectTrash=true;
        if (ImGui::MenuItem("Recovery Center", nullptr, m_showRecoveryCenter)) {
            m_showRecoveryCenter = !m_showRecoveryCenter;
        }
        if (ImGui::MenuItem("Asset Identity Resolver", nullptr, m_showAssetIdentity)) {
            m_showAssetIdentity = !m_showAssetIdentity;
        }
        if (ImGui::MenuItem("回到專案管理員")) {
            m_screen = Screen::Welcome;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("說明")) {
        if (ImGui::MenuItem("快捷鍵", "F1")) m_showShortcuts = true;
        ImGui::EndMenu();
    }
    RenderWorkspaceSwitcher();
    ImGui::SameLine(); ImGui::TextDisabled("  |  "); ImGui::SameLine();
    const std::string runLabel=std::string(m_iconFontLoaded?Icon(EditorIcon::Play):"")+"  執行";
    if (ImGui::SmallButton(runLabel.c_str())) RunDev();
    const std::string stopLabel=std::string(m_iconFontLoaded?Icon(EditorIcon::Stop):"")+"  停止";
    ImGui::SameLine(); if (ImGui::SmallButton(stopLabel.c_str())) {}
    ImGui::SameLine(); if (ImGui::SmallButton("Build")) RunBuild();
    const auto diagnostics = diag::Global().Snapshot();
    const auto errors = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.severity >= diag::Severity::Error; });
    ImGui::SameLine();
    ImGui::TextColored(errors ? ImVec4(1,.35f,.38f,1) : ImVec4(.45f,.8f,.55f,1),
                       errors ? "● %zu" : "● 正常", errors);
    ImGui::EndMainMenuBar();
}

void EditorApp::RenderOpenDocuments() {
    if (m_showOpenDocuments && ImGui::Begin("Open Documents", &m_showOpenDocuments)) {
        static bool currentWorkspaceOnly = false;
        static bool dirtyOnly = false;
        ImGui::Checkbox("目前工作區", &currentWorkspaceOnly);
        ImGui::SameLine(); ImGui::Checkbox("僅未儲存", &dirtyOnly);
        ImGui::Separator();
        for (const auto& session : m_docs.Documents()) {
            if (currentWorkspaceOnly && session.workspace != m_workspace) continue;
            if (dirtyOnly && !session.dirty) continue;
            ImGui::PushID(session.id.canonicalPath.string().c_str());
            if (session.pinned) ImGui::TextColored(ImVec4(.55f,.75f,1,1), "◆");
            else ImGui::TextDisabled("◇");
            ImGui::SameLine();
            const std::string label = session.label + (session.dirty ? "  ●" : "");
            if (ImGui::Selectable(label.c_str(), m_docs.Active() == &session)) {
                (void)m_docs.Activate(session.id);
                std::error_code error;
                const auto runtime = fs::relative(session.id.canonicalPath,
                                                  m_project.Context().root, error).generic_string();
            if (!error && session.type == DocumentType::Scenario) {
                    SetWorkspace(EditorWorkspace::Story);
                    OpenDocTab(runtime);
                } else if (!error && session.type == DocumentType::UIScene && m_preview) {
                    SetWorkspace(EditorWorkspace::UI);
                    const Status activated=ActivateUIDocument(session.id.canonicalPath,runtime);
                    if(!activated)for(const auto& diagnostic:activated.Diagnostics())diag::Emit(diagnostic);
                } else if(!error&&session.type==DocumentType::Lua){
                    SetWorkspace(EditorWorkspace::Script);m_scripts.OpenFile(runtime);
                }
            }
            if (ImGui::BeginPopupContextItem("DocumentMenu")) {
                if (ImGui::MenuItem(session.pinned ? "取消釘選" : "釘選"))
                    m_docs.SetPinned(session.id.canonicalPath, !session.pinned);
                if (ImGui::MenuItem("複製路徑")) ImGui::SetClipboardText(session.id.canonicalPath.string().c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (m_showOpenDocuments) ImGui::End();

    if (m_quickOpenOpen) ImGui::OpenPopup("快速開啟");
    ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("快速開啟", &m_quickOpenOpen,
                               ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##quick-open", "搜尋檔名、路徑或型別…",
                                 m_quickOpenFilter, sizeof(m_quickOpenFilter));
        ImGui::Separator();
        const std::string filter = m_quickOpenFilter;
        int shown = 0;
        for (const AssetRecord& asset : m_assets.Assets()) {
            if (asset.type != "ui" && asset.type != "script" && asset.type != "lua") continue;
            if (!filter.empty() && asset.runtimePath.find(filter) == std::string::npos &&
                asset.type.find(filter) == std::string::npos) continue;
            const bool recent = m_docs.Find(asset.absolutePath) != nullptr;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AssetTypeColor(asset.type)),
                               "%s", asset.type.c_str());
            ImGui::SameLine();
            if (ImGui::Selectable((asset.runtimePath + (recent ? "  • 最近" : "")).c_str())) {
                OpenAssetByType(asset);
                m_quickOpenOpen = false;
                ImGui::CloseCurrentPopup();
            }
            if (++shown >= 200) break;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_quickOpenOpen = false; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::RenderStatusBar() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = 25.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
                                   viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    if (ImGui::Begin("##PrismatiXStatusBar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        const auto* active = m_docs.Active();
        ImGui::TextDisabled("文件"); ImGui::SameLine();
        ImGui::TextUnformatted(active ? active->label.c_str() : "—");
        if (m_workspace == EditorWorkspace::UI && m_designer.Document()) {
            ImGui::SameLine(); ImGui::TextDisabled("  |  %s  |  %s",
                m_designer.SelectionSummary().c_str(),
                ui::ChildLayoutPolicyName(m_designer.SelectedParentPolicy()));
            const auto& state = m_designer.ViewportState();
            ImGui::SameLine(); ImGui::TextDisabled("  |  %.0f%%  G:%s  Snap:%s",
                state.zoom * 100.0f, state.gridVisible ? "開" : "關",
                state.gridSnap ? "開" : "關");
        }
        const auto diagnostics = diag::Global().Snapshot();
        const auto errors = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item){ return item.severity >= diag::Severity::Error; });
        const auto warnings = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item){ return item.severity == diag::Severity::Warning; });
        const std::string right = std::string(active && active->dirty ? "未儲存 • Recovery 待寫入" : "已儲存") +
            "    錯誤 " + std::to_string(errors) + "  警告 " + std::to_string(warnings);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f,
                                 ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 12.0f));
        ImGui::TextDisabled("%s", right.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorApp::RenderDiagnosticToasts() {
    const auto now = std::chrono::steady_clock::now();
    m_toasts.Prune(now);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float y = viewport->WorkPos.y + 42.0f;
    int index = 0;
    for (const auto& toast : m_toasts.Items()) {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 390.0f, y));
        ImGui::SetNextWindowSize(ImVec2(370, 0));
        ImGui::SetNextWindowBgAlpha(.96f);
        const std::string id = "##diagnostic-toast-" + std::to_string(index++);
        if (ImGui::Begin(id.c_str(), nullptr, ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            const bool error = toast.diagnostic.severity >= diag::Severity::Error;
            ImGui::TextColored(error ? ImVec4(1,.35f,.38f,1) : ImVec4(1,.72f,.25f,1),
                               "%s  %s", error ? "錯誤" : "警告", toast.diagnostic.code.c_str());
            ImGui::TextWrapped("%s", toast.diagnostic.message.c_str());
            if (!toast.diagnostic.source.path.empty())
                ImGui::TextDisabled("%s", toast.diagnostic.source.path.c_str());
        }
        y += ImGui::GetWindowSize().y + 8.0f;
        ImGui::End();
    }
}

void EditorApp::RenderExternalDocumentConflict() {
    if (m_externalConflictPath.empty()) return;
    ImGui::OpenPopup("外部修改衝突");
    ImGui::SetNextWindowSize(ImVec2(590, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("外部修改衝突", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("磁碟上的文件已被其他程式修改，但 Editor 內也有未儲存內容：");
    ImGui::TextColored(ImVec4(.65f,.78f,1,1), "%s", m_externalConflictPath.string().c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("重新載入會捨棄本地變更；保留本地只接受目前磁碟版本，之後仍可手動儲存覆寫。");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("另存新檔路徑", m_externalSaveAsPath, sizeof(m_externalSaveAsPath));

    const auto finish = [this] {
        m_externalConflictPath.clear(); m_externalSaveAsPath[0] = 0;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("重新載入", ImVec2(120, 0))) {
        const auto path = m_externalConflictPath;
        const auto* session = m_docs.Find(path);
        std::error_code error;
        const auto runtime = fs::relative(path, m_project.Context().root, error).generic_string();
        bool loaded = false;
        if (session && session->type == DocumentType::UIScene && m_designer.Document() &&
            DocumentManager::Canonical(m_designer.Document()->Path()) == DocumentManager::Canonical(path)) {
            loaded = static_cast<bool>(m_designer.Open(path));
            if (loaded && m_preview) m_preview->LoadUIDocument(m_designer.Document()->Data(), runtime);
        } else if(session&&session->type==DocumentType::UIScene) {
            if(auto found=m_inactiveDesigners.find(DocumentManager::Canonical(path).generic_string());found!=m_inactiveDesigners.end())loaded=static_cast<bool>(found->second.editor->Open(path));
        } else if (session && session->type == DocumentType::Scenario) {
            for (auto& document : m_scriptDocs) if (DocumentManager::Canonical(document->DocumentPath()) == DocumentManager::Canonical(path)) {
                loaded = document->OpenDocument(runtime); break;
            }
        } else if(session&&session->type==DocumentType::Lua) {
            loaded=m_scripts.ReloadFile(runtime);
        }
        if (loaded) { m_docs.AcknowledgeDiskVersion(path); finish(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("保留本地", ImVec2(120, 0))) {
        m_docs.AcknowledgeDiskVersion(m_externalConflictPath); finish();
    }
    ImGui::SameLine();
    if (ImGui::Button("另存新檔", ImVec2(120, 0))) {
        const fs::path copyPath = m_externalSaveAsPath;
        Status written = Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
            .code="PXDOC2104",.category="Editor.DocumentManager",.message="找不到可另存的本地文件"});
        bool uiCopy=false;
        if (m_designer.Document() && DocumentManager::Canonical(m_designer.Document()->Path()) ==
            DocumentManager::Canonical(m_externalConflictPath)) {
            written = io::AtomicFile::WriteText(copyPath, m_designer.Document()->Serialize());
            uiCopy=true;
        } else if(auto found=m_inactiveDesigners.find(DocumentManager::Canonical(m_externalConflictPath).generic_string());found!=m_inactiveDesigners.end()) {
            written=io::AtomicFile::WriteText(copyPath,found->second.editor->Document()->Serialize());
            uiCopy=true;
        } else {
            bool foundLua=false;
            for(const auto& document:m_scripts.OpenDocuments())if(DocumentManager::Canonical(m_project.Context().root/document.runtimePath)==DocumentManager::Canonical(m_externalConflictPath)){
                written=io::AtomicFile::WriteText(copyPath,document.buffer);foundLua=true;break;}
            if(!foundLua)
            for (auto& document : m_scriptDocs) if (DocumentManager::Canonical(document->DocumentPath()) ==
                DocumentManager::Canonical(m_externalConflictPath)) {
                written = io::AtomicFile::WriteText(copyPath, document->Compile());
                break;
            }
        }
        if (!written) for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
        else {if(uiCopy){const Status activated=ActivateUIDocument(copyPath);if(!activated)for(const auto& diagnostic:activated.Diagnostics())diag::Emit(diagnostic);}finish();}
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(90, 0))) finish();
    ImGui::EndPopup();
}

void EditorApp::RenderHierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        if (m_previewMode == 0) {
            m_designer.RenderHierarchy();
        }
        else {
            ImGui::TextDisabled("Switch Preview to UI Scene mode to edit a typed .pxscene.");
        }
    }
    ImGui::End();
}

void EditorApp::RenderInspector() {
    if (ImGui::Begin("Inspector")) {
        if (m_workspace == EditorWorkspace::UI) {
            m_designer.RenderInspector(m_selectedAsset);
        }
        else {
            if (NodeGraphEditor* doc = ActiveDocPtr()) {
                doc->RenderInspector();
            } else {
                ImGui::TextDisabled("No script open.");
            }
            if (m_textures && m_selectedAsset.size() > 4) {
                const std::string ext = m_selectedAsset.substr(m_selectedAsset.size() - 4);
                if (ext == ".png" || ext == ".jpg" || ext == "jpeg" || ext == "webp") {
                    const std::string abs = Utf8Path(m_project.Context().root / m_selectedAsset);
                    int w = 0, h = 0;
                    if (ImTextureID t = m_textures->LoadId(abs, &w, &h); t && w > 0) {
                        const float avail = ImGui::GetContentRegionAvail().x;
                        const float scale = avail / static_cast<float>(w);
                        ImGui::Image(t, ImVec2(avail, h * scale));
                        ImGui::TextDisabled("%dx%d", w, h);
                    }
                }
            }
        }
    }
    ImGui::End();
}

void EditorApp::OpenAssetByType(const AssetRecord& rec) {
    if (rec.type == "script") {
        SetWorkspace(EditorWorkspace::Story);
        OpenDocTab(rec.runtimePath);
    } else if (rec.type == "ui" && m_preview) {
        SetWorkspace(EditorWorkspace::UI);
        m_previewMode = 0;
        const Status status = ActivateUIDocument(rec.absolutePath, rec.runtimePath);
        if (!status) for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
    } else if(rec.type=="lua") {
        SetWorkspace(EditorWorkspace::Script);
        m_scripts.OpenFile(rec.runtimePath);
        TrackDocument(rec.absolutePath,DocumentType::Lua,false);
    } else if (rec.type == "audio" && m_preview) {
        m_preview->AudioRef().PlayBGM(rec.runtimePath, /*loop=*/false, 0);
    } else {
        OpenInExplorer(rec.absolutePath);  // system viewer for images/fonts/etc.
    }
}

void EditorApp::MoveAssetTo(const std::string& runtimePath, const std::filesystem::path& targetDir) {
    const fs::path src = m_project.Context().root / runtimePath;
    const fs::path dst = targetDir / src.filename();
    std::error_code ec;
    if (!fs::exists(src, ec) || fs::equivalent(src.parent_path(), targetDir, ec)) {
        return;
    }
    if (fs::exists(dst, ec)) {
        Log("Move failed, target exists: " + dst.string());
        return;
    }
    const std::string newRel = fs::relative(dst, m_project.Context().root, ec).generic_string();
    const Status status=MoveAssetWithHistory(runtimePath,newRel);
    if(status)Log("已移動 "+runtimePath+" → "+newRel);
}

void EditorApp::SetAssetDirectory(std::string runtimePath, bool recordHistory,
                                  bool clearForwardHistory) {
    if (runtimePath.empty()) runtimePath = "Content";
    fs::path requested = fs::path(runtimePath).lexically_normal();
    if (requested == ".") requested = "Content";

    const fs::path root = m_project.Context().root;
    std::error_code ec;
    if (requested.is_absolute()) requested = fs::relative(requested, root, ec);
    if (ec) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
            .code = "PXFS9301", .category = "Editor.FileSystem",
            .message = "無法開啟資料夾", .details = ec.message()};
        diagnostic.source.path = runtimePath;
        diag::Emit(std::move(diagnostic));
        return;
    }
    const std::string normalized = requested.generic_string();
    const bool contentPath = normalized == "Content" || normalized.starts_with("Content/");
    const fs::path absolute = root / requested;
    ec.clear();
    if (!contentPath || !fs::is_directory(absolute, ec)) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
            .code = "PXFS9302", .category = "Editor.FileSystem",
            .message = "FileSystem 只能開啟專案 Content 內的資料夾",
            .details = ec ? ec.message() : "指定路徑不是資料夾。"};
        diagnostic.source.path = normalized;
        diag::Emit(std::move(diagnostic));
        return;
    }
    if (normalized == m_assetDir) {
        m_assetPathInput = normalized;
        return;
    }
    if (recordHistory) {
        m_assetDirectoryHistory.push_back(m_assetDir);
        if (clearForwardHistory) m_assetDirectoryForward.clear();
    }
    m_folderViewSettings[m_assetDir] = {m_fileSystemView, m_assetSortColumn,
        m_assetSortAscending, m_assetRowHeight, m_assetThumbSize};
    m_assetDir = normalized;
    m_assetPathInput = m_assetDir;
    if (const auto found = m_folderViewSettings.find(m_assetDir); found != m_folderViewSettings.end()) {
        m_fileSystemView = found->second.mode;
        m_assetSortColumn = found->second.sort;
        m_assetSortAscending = found->second.ascending;
        m_assetRowHeight = found->second.rowHeight;
        m_assetThumbSize = found->second.thumbnailSize;
    } else {
        m_fileSystemView = FileSystemViewMode::Details;
        m_assetSortColumn = AssetSortColumn::Name;
        m_assetSortAscending = true;
        m_assetRowHeight = 28.0f;
        m_assetThumbSize = 84.0f;
    }
    m_assetSelectionModel.Clear();
    m_selectedAsset.clear();
}

void EditorApp::RenderAssetTree(const std::filesystem::path& dir, const std::filesystem::path& root,
                                std::size_t depth) {
    if (depth >= 64) {
        ImGui::TextDisabled("資料夾巢狀過深，已停止展開");
        return;
    }
    std::vector<fs::path> subdirs;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) subdirs.push_back(it->path());
    }
    std::sort(subdirs.begin(), subdirs.end());

    for (const fs::path& sub : subdirs) {
        const std::string rel = fs::relative(sub, root, ec).generic_string();
        bool hasChildren = false;
        for (fs::directory_iterator it(sub, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_directory(ec)) {
                hasChildren = true;
                break;
            }
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (rel == m_assetDir) flags |= ImGuiTreeNodeFlags_Selected;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        const bool open = ImGui::TreeNodeEx(sub.filename().string().c_str(), flags);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            SetAssetDirectory(rel);
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
                MoveAssetTo(std::string(static_cast<const char*>(payload->Data),
                                        payload->DataSize > 0 ? payload->DataSize - 1 : 0),
                            sub);
            }
            ImGui::EndDragDropTarget();
        }
        if (open && hasChildren) {
            RenderAssetTree(sub, root, depth + 1);
            ImGui::TreePop();
        }
    }
}

void EditorApp::RenderAssetEntry(const AssetRecord& rec, bool gridMode, float tile) {
    ImGui::PushID(rec.runtimePath.c_str());
    const bool selected = m_assetSelectionModel.selected.contains(rec.runtimePath);
    const std::string name = rec.absolutePath.filename().string();

    if (gridMode) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float labelH = ImGui::GetTextLineHeight() + 6.0f;
        if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(tile, tile + labelH))) {
            if(ImGui::GetIO().KeyCtrl){if(selected)m_assetSelectionModel.selected.erase(rec.runtimePath);else m_assetSelectionModel.selected.insert(rec.runtimePath);}else{m_assetSelectionModel.selected.clear();m_assetSelectionModel.selected.insert(rec.runtimePath);}
            m_selectedAsset = rec.runtimePath;
            m_assetSelectionModel.anchor = rec.runtimePath;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                OpenAssetByType(rec);
            }
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 boxMin(pos.x + 2, pos.y + 2);
        const ImVec2 boxMax(pos.x + tile - 2, pos.y + tile - 2);
        draw->AddRectFilled(boxMin, boxMax, IM_COL32(16, 19, 26, 255), 5.0f);

        int tw = 0, th = 0;
        ImTextureID thumb{};
        if (rec.type == "image" && m_textures) {
            thumb = m_textures->LoadId(Utf8Path(rec.absolutePath), &tw, &th);
        }
        if (thumb && tw > 0 && th > 0) {
            const float availW = boxMax.x - boxMin.x - 8.0f;
            const float availH = boxMax.y - boxMin.y - 8.0f;
            const float s = std::min(availW / tw, availH / th);
            const ImVec2 c((boxMin.x + boxMax.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f);
            const ImVec2 half(tw * s * 0.5f, th * s * 0.5f);
            draw->AddImage(thumb, ImVec2(c.x - half.x, c.y - half.y),
                           ImVec2(c.x + half.x, c.y + half.y));
        } else {
            const std::string tag = rec.type == "audio"   ? "AUDIO"
                                    : rec.type == "script" ? "Scenario"
                                    : rec.type == "ui"     ? "UI"
                                    : rec.type == "font"   ? "FONT"
                                    : rec.type == "lua"    ? "LUA"
                                                           : "FILE";
            const ImVec2 sz = ImGui::CalcTextSize(tag.c_str());
            draw->AddRectFilled(boxMin, boxMax, (AssetTypeColor(rec.type) & 0x00FFFFFF) | 0x30000000,
                                5.0f);
            draw->AddText(ImVec2((boxMin.x + boxMax.x - sz.x) * 0.5f,
                                 (boxMin.y + boxMax.y - sz.y) * 0.5f),
                          AssetTypeColor(rec.type), tag.c_str());
        }
        // filename, clipped to the tile width
        draw->PushClipRect(ImVec2(pos.x, pos.y + tile), ImVec2(pos.x + tile, pos.y + tile + labelH),
                           true);
        draw->AddText(ImVec2(pos.x + 2, pos.y + tile + 2),
                      selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 198, 210, 255),
                      name.c_str());
        draw->PopClipRect();
    } else {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(ImVec2(pos.x, pos.y + 3), ImVec2(pos.x + 8, pos.y + 13),
                            AssetTypeColor(rec.type), 2.0f);
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if(ImGui::GetIO().KeyCtrl){if(selected)m_assetSelectionModel.selected.erase(rec.runtimePath);else m_assetSelectionModel.selected.insert(rec.runtimePath);}else{m_assetSelectionModel.selected.clear();m_assetSelectionModel.selected.insert(rec.runtimePath);}
            m_selectedAsset = rec.runtimePath;
            m_assetSelectionModel.anchor = rec.runtimePath;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                OpenAssetByType(rec);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)", rec.runtimePath.c_str(), rec.type.c_str());
        }
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(kResourcePayload, rec.runtimePath.c_str(),
                                  rec.runtimePath.size() + 1);
        ImGui::TextUnformatted(rec.runtimePath.c_str());
        ImGui::TextDisabled("%s", rec.type.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##assetmenu")) {
        m_selectedAsset = rec.runtimePath;
        if (ImGui::MenuItem("Open")) {
            OpenAssetByType(rec);
        }
        if (rec.type == "audio" && m_preview) {
            if (ImGui::MenuItem("Play")) {
                m_preview->AudioRef().PlayBGM(rec.runtimePath, false, 0);
            }
            if (ImGui::MenuItem("Stop")) {
                m_preview->AudioRef().StopBGM(0);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            m_assetRenameFrom = rec.runtimePath;
            std::snprintf(m_assetRenameBuf, sizeof(m_assetRenameBuf), "%s", name.c_str());
        }
        if(ImGui::MenuItem("Duplicate"))DuplicateAssetWithHistory(rec.runtimePath);
        if(ImGui::MenuItem("Reimport")){const Status scanned=m_assetRegistry.Scan(m_project.Context().root);if(!scanned)m_showAssetIdentity=true;Log("重新匯入檢查："+rec.runtimePath);}
        if (ImGui::MenuItem("Reveal in Explorer")) {
            OpenInExplorer(rec.absolutePath.parent_path());
        }
        if (ImGui::MenuItem("Copy Path")) {
            ImGui::SetClipboardText(rec.runtimePath.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete...")) {
            m_assetPendingDelete = rec.runtimePath;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void EditorApp::RenderAssets() {
    std::vector<fs::path> pickedPaths;
    std::string pickError;
    bool pickCompleted = false;
    {
        std::lock_guard lock(g_assetPick.mutex);
        if (g_assetPick.completed) {
            pickedPaths = std::move(g_assetPick.paths);
            pickError = std::move(g_assetPick.error);
            g_assetPick.paths.clear();
            g_assetPick.error.clear();
            g_assetPick.completed = false;
            pickCompleted = true;
        }
    }
    // Path enumeration and all UI/diagnostic work stays on the main thread.
    if (pickCompleted && !pickError.empty()) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
            .code = "PXIMPORT9026", .category = "Editor.Import",
            .message = "無法處理檔案選擇結果", .details = pickError};
        diag::Emit(std::move(diagnostic));
    } else if (pickCompleted && !pickedPaths.empty()) {
        QueueImportReview(pickedPaths);
    }
    if (ImGui::Begin("Assets")) {
        m_assetsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const fs::path root = m_project.Context().root;
        if (m_assetsFocused && !ImGui::GetIO().WantTextInput && !m_selectedAsset.empty()) {
            std::error_code selectedError;
            const bool selectedIsDirectory =
                fs::is_directory(root / m_selectedAsset, selectedError);
            if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                if (selectedIsDirectory) {
                    SetAssetDirectory(m_selectedAsset);
                } else {
                    for (const auto& record : m_assets.Assets())
                        if (record.runtimePath == m_selectedAsset) { OpenAssetByType(record); break; }
                }
            }
            if (!selectedIsDirectory && ImGui::IsKeyPressed(ImGuiKey_F2)) {
                m_assetRenameFrom = m_selectedAsset;
                std::snprintf(m_assetRenameBuf, sizeof(m_assetRenameBuf), "%s",
                              fs::path(m_selectedAsset).filename().string().c_str());
            }
            if (!selectedIsDirectory && ImGui::IsKeyPressed(ImGuiKey_Delete))
                m_assetPendingDelete = m_selectedAsset;
        }

        // --- Toolbar ---
        const std::string importLabel=std::string(m_iconFontLoaded?Icon(EditorIcon::Import):"＋")+"  匯入素材";
        if (ImGui::Button(importLabel.c_str())) ImGui::OpenPopup("AssetImportMenu");
        if (ImGui::BeginPopup("AssetImportMenu")) {
            if (ImGui::MenuItem("匯入檔案…")) {
                static const SDL_DialogFileFilter filters[]{{"素材檔案", "png;jpg;jpeg;webp;bmp;mp3;ogg;wav;flac;opus;mp4;webm;ttf;otf;pxscenario;pxanim;pxscene;pxres;pxextension;lua"}, {"所有檔案", "*"}};
                if (BeginAssetPicker())
                    SDL_ShowOpenFileDialog(&OnAssetsPicked, nullptr, m_window.Handle(), filters,
                                           static_cast<int>(std::size(filters)), nullptr, true);
            }
            if (ImGui::MenuItem("匯入資料夾…") && BeginAssetPicker())
                SDL_ShowOpenFolderDialog(&OnAssetsPicked, nullptr, m_window.Handle(), nullptr, false);
            if (ImGui::MenuItem("從剪貼簿匯入", "Ctrl+V")) ImportClipboardAssets();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##filter", "搜尋素材與路徑…", m_assetFilter, sizeof(m_assetFilter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(105);
        if (ImGui::BeginCombo("##type", kAssetTypes[m_assetTypeIndex])) {
            for (int i = 0; i < static_cast<int>(kAssetTypes.size()); ++i) {
                if (ImGui::Selectable(kAssetTypes[i], m_assetTypeIndex == i)) m_assetTypeIndex = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(118);
        static const char* statusFilters[]{"全部狀態","包含於 Build","排除 Build","缺少 GUID","GUID 衝突","有問題"};
        ImGui::Combo("##asset-status", &m_assetStatusFilter, statusFilters,
                     static_cast<int>(std::size(statusFilters)));
        ImGui::SameLine();
        const auto viewButton=[&](const char* label,FileSystemViewMode mode){const bool active=m_fileSystemView==mode;if(active)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));if(ImGui::SmallButton(label))m_fileSystemView=mode;if(active)ImGui::PopStyleColor();};
        const std::string details=std::string(m_iconFontLoaded?Icon(EditorIcon::Details):"")+" 清單",compact=std::string(m_iconFontLoaded?Icon(EditorIcon::Compact):"")+" 緊湊",thumbnails=std::string(m_iconFontLoaded?Icon(EditorIcon::Thumbnails):"")+" 縮圖";
        viewButton(details.c_str(),FileSystemViewMode::Details);ImGui::SameLine(0,2);viewButton(compact.c_str(),FileSystemViewMode::Compact);ImGui::SameLine(0,2);viewButton(thumbnails.c_str(),FileSystemViewMode::Thumbnails);
        ImGui::SameLine();ImGui::SetNextItemWidth(82);
        if(m_fileSystemView==FileSystemViewMode::Details)ImGui::SliderFloat("##density",&m_assetRowHeight,22,42,"列 %.0f");else ImGui::SliderFloat("##thumb",&m_assetThumbSize,48,160,"%.0f");
        ImGui::SameLine();
        if (ImGui::SmallButton("新增")) {
            ImGui::OpenPopup("##assetNewMenu");
        }
        if (ImGui::BeginPopup("##assetNewMenu")) {
            if (ImGui::MenuItem("Folder")) { m_assetNewKind = 0; m_assetNewNameBuf[0] = 0; }
            if (ImGui::MenuItem("Scenario (.pxscenario)")) { m_assetNewKind = 1; m_assetNewNameBuf[0] = 0; }
            if (ImGui::MenuItem("UI Scene (.pxscene)")) { m_assetNewKind = 2; m_assetNewNameBuf[0] = 0; }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("重新掃描")) {
            m_assets.Scan(m_project.Context());
        }
        ImGui::Separator();

        // One navigation model drives both the tree and the content pane.
        const bool treeVisible = m_showAssetFolderTree;
        if (treeVisible) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton("資料夾")) m_showAssetFolderTree = !m_showAssetFolderTree;
        if (treeVisible) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("顯示或隱藏資料夾樹");
        ImGui::SameLine();
        ImGui::BeginDisabled(m_assetDirectoryHistory.empty());
        if (ImGui::SmallButton("‹##asset-back")) {
            const std::string target = m_assetDirectoryHistory.back();
            m_assetDirectoryHistory.pop_back();
            m_assetDirectoryForward.push_back(m_assetDir);
            SetAssetDirectory(target, false, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2);
        ImGui::BeginDisabled(m_assetDirectoryForward.empty());
        if (ImGui::SmallButton("›##asset-forward")) {
            const std::string target = m_assetDirectoryForward.back();
            m_assetDirectoryForward.pop_back();
            m_assetDirectoryHistory.push_back(m_assetDir);
            SetAssetDirectory(target, false, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2);
        const bool atContentRoot = m_assetDir == "Content";
        ImGui::BeginDisabled(atContentRoot);
        if (ImGui::SmallButton("↑##asset-parent"))
            SetAssetDirectory(fs::path(m_assetDir).parent_path().generic_string());
        ImGui::EndDisabled();
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##asset-path", "Content/UI", &m_assetPathInput,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            SetAssetDirectory(m_assetPathInput);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("輸入專案內路徑並按 Enter，例如 Content/UI");
        ImGui::Separator();

        // --- Folder tree | content ---
        if (m_showAssetFolderTree &&
            ImGui::BeginChild("##assettree", ImVec2(220, 0), ImGuiChildFlags_ResizeX)) {
            ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_SpanAvailWidth |
                                           ImGuiTreeNodeFlags_DefaultOpen;
            if (m_assetDir == "Content") rootFlags |= ImGuiTreeNodeFlags_Selected;
            const bool rootOpen = ImGui::TreeNodeEx("Content", rootFlags);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                SetAssetDirectory("Content");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
                    MoveAssetTo(std::string(static_cast<const char*>(payload->Data),
                                            payload->DataSize > 0 ? payload->DataSize - 1 : 0),
                                root / "Content");
                }
                ImGui::EndDragDropTarget();
            }
            if (rootOpen) {
                std::error_code ec;
                if (fs::exists(root / "Content", ec)) {
                    RenderAssetTree(root / "Content", root);
                }
                ImGui::TreePop();
            }
        }
        if (m_showAssetFolderTree) {
            ImGui::EndChild();
            ImGui::SameLine();
        }

        if (ImGui::BeginChild("##assetcontent")) {
            if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl &&
                ImGui::GetIO().MouseWheel != 0.0f &&
                m_fileSystemView != FileSystemViewMode::Details) {
                m_assetThumbSize = std::clamp(m_assetThumbSize + ImGui::GetIO().MouseWheel * 8.0f,
                                              48.0f, 160.0f);
            }
            const auto allDiagnostics = diag::Global().Snapshot();
            const auto matchesStatus = [&](const AssetRecord& record) {
                const auto* identity = m_assetRegistry.FindPath(record.absolutePath);
                if (m_assetStatusFilter == 1) return identity && identity->includeInBuild;
                if (m_assetStatusFilter == 2) return identity && !identity->includeInBuild;
                if (m_assetStatusFilter == 3) return identity == nullptr;
                bool problem = false, guidConflict = false;
                for (const auto& diagnostic : allDiagnostics) {
                    const bool samePath = diagnostic.source.path == record.runtimePath ||
                        fs::path(diagnostic.source.path) == record.absolutePath;
                    if (!samePath) continue;
                    problem = true;
                    if (diagnostic.code == "PXASSET-E1007") guidConflict = true;
                }
                if (m_assetStatusFilter == 4) return guidConflict;
                if (m_assetStatusFilter == 5) return problem;
                return true;
            };
            const bool searching = m_assetFilter[0] != 0 || m_assetTypeIndex != 0;
            if (searching) {
                auto filtered = m_assets.Filter(m_assetFilter, kAssetTypes[m_assetTypeIndex]);
                std::erase_if(filtered, [&](const AssetRecord& record){return !matchesStatus(record);});
                ImGui::Text("搜尋結果");
                ImGui::SameLine(); ImGui::TextDisabled("%zu 個項目", filtered.size());
                ImGui::Separator();
                ImGuiListClipper clipper;clipper.Begin(static_cast<int>(filtered.size()),m_assetRowHeight);
                while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
                    RenderAssetEntry(filtered[static_cast<std::size_t>(row)], false, 0.0f);
            } else {
                // Clickable breadcrumb mirrors the editable path bar above.
                std::string accum;
                std::stringstream crumbs(m_assetDir);
                std::string part;
                bool first = true;
                while (std::getline(crumbs, part, '/')) {
                    accum = first ? part : accum + "/" + part;
                    if (!first) {
                        ImGui::SameLine(0, 2);
                        ImGui::TextDisabled(">");
                        ImGui::SameLine(0, 2);
                    }
                    const std::string crumbLabel = (first && m_iconFontLoaded ?
                        std::string(Icon(EditorIcon::Folder)) + "  " + part : part);
                    if (ImGui::SmallButton((crumbLabel + "##crumb" + accum).c_str())) {
                        SetAssetDirectory(accum);
                    }
                    first = false;
                }
                ImGui::Separator();

                std::error_code ec;
                const fs::path cur = root / m_assetDir;
                std::vector<fs::path> dirs;
                std::vector<const AssetRecord*> files;
                for (fs::directory_iterator it(cur, ec), end; it != end && !ec;
                     it.increment(ec)) {
                    if (it->is_directory(ec)) {
                        dirs.push_back(it->path());
                    } else if (it->path().extension() != ".pxmeta") {
                        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
                        for (const AssetRecord& rec : m_assets.Assets()) {
                            if (rec.runtimePath == rel) {
                                files.push_back(&rec);
                                break;
                            }
                        }
                    }
                }
                std::erase_if(files, [&](const AssetRecord* record){return !matchesStatus(*record);});
                std::sort(dirs.begin(), dirs.end());
                const auto compareFiles = [&](const AssetRecord* a, const AssetRecord* b) {
                    int order = 0;
                    if (m_assetSortColumn == AssetSortColumn::Name)
                        order = a->runtimePath.compare(b->runtimePath);
                    else if (m_assetSortColumn == AssetSortColumn::Type)
                        order = a->type == b->type ? a->runtimePath.compare(b->runtimePath)
                                                  : a->type.compare(b->type);
                    else if (m_assetSortColumn == AssetSortColumn::Size)
                        order = a->size == b->size ? a->runtimePath.compare(b->runtimePath)
                                                  : (a->size < b->size ? -1 : 1);
                    else {
                        std::error_code leftError, rightError;
                        const auto left = fs::last_write_time(a->absolutePath, leftError);
                        const auto right = fs::last_write_time(b->absolutePath, rightError);
                        order = left == right ? a->runtimePath.compare(b->runtimePath)
                                              : (left < right ? -1 : 1);
                    }
                    return m_assetSortAscending ? order < 0 : order > 0;
                };
                std::sort(files.begin(), files.end(), compareFiles);

                const float tile = m_assetThumbSize;
                const float cellW = tile + 10.0f;
                float x = 0.0f;
                const float maxW = ImGui::GetContentRegionAvail().x;

                if (m_fileSystemView == FileSystemViewMode::Details) {
                    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                        ImGuiTableFlags_SizingStretchProp;
                    if (ImGui::BeginTable("##assetDetails", 7, flags)) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("名稱", ImGuiTableColumnFlags_DefaultSort, 0.36f);
                        ImGui::TableSetupColumn("型別", ImGuiTableColumnFlags_WidthFixed, 90);
                        ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 75);
                        ImGui::TableSetupColumn("修改時間", ImGuiTableColumnFlags_WidthFixed, 135);
                        ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthFixed, 84);
                        ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 55);
                        ImGui::TableSetupColumn("狀態", ImGuiTableColumnFlags_WidthFixed, 70);
                        ImGui::TableHeadersRow();
                        if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty && specs->SpecsCount) {
                            switch (specs->Specs[0].ColumnIndex) {
                                case 1: m_assetSortColumn=AssetSortColumn::Type;break;
                                case 2: m_assetSortColumn=AssetSortColumn::Size;break;
                                case 3: m_assetSortColumn=AssetSortColumn::Modified;break;
                                default:m_assetSortColumn=AssetSortColumn::Name;break;
                            }
                            m_assetSortAscending=specs->Specs[0].SortDirection!=ImGuiSortDirection_Descending;
                            std::sort(files.begin(),files.end(),compareFiles);specs->SpecsDirty=false;
                        }
                        for (const auto& directory : dirs) {
                            std::error_code pathError;
                            const std::string runtimeDirectory =
                                fs::relative(directory, root, pathError).generic_string();
                            const std::string name = Utf8Path(directory.filename());
                            const bool selected = m_selectedAsset == runtimeDirectory;
                            const std::string label = std::string(m_iconFontLoaded ?
                                Icon(EditorIcon::Folder) : "[資料夾]") + "  " + name +
                                "##folder-" + runtimeDirectory;
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, m_assetRowHeight);
                            ImGui::TableSetColumnIndex(0);
                            if (ImGui::Selectable(label.c_str(), selected,
                                    ImGuiSelectableFlags_SpanAllColumns |
                                    ImGuiSelectableFlags_AllowDoubleClick,
                                    ImVec2(0, m_assetRowHeight - 4))) {
                                m_assetSelectionModel.Clear();
                                m_selectedAsset = runtimeDirectory;
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                    SetAssetDirectory(runtimeDirectory);
                            }
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* payload =
                                        ImGui::AcceptDragDropPayload(kResourcePayload)) {
                                    MoveAssetTo(std::string(static_cast<const char*>(payload->Data),
                                        payload->DataSize > 0 ? payload->DataSize - 1 : 0), directory);
                                }
                                ImGui::EndDragDropTarget();
                            }
                            if (ImGui::BeginPopupContextItem("FolderRowMenu")) {
                                m_selectedAsset = runtimeDirectory;
                                if (ImGui::MenuItem("開啟", "Enter"))
                                    SetAssetDirectory(runtimeDirectory);
                                if (ImGui::MenuItem("在檔案總管顯示")) OpenInExplorer(directory);
                                if (ImGui::MenuItem("複製路徑"))
                                    ImGui::SetClipboardText(runtimeDirectory.c_str());
                                ImGui::EndPopup();
                            }
                            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("資料夾");
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextDisabled("%zu 項", DirectoryItemCount(directory));
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextDisabled("%s", HumanTime(directory).c_str());
                            ImGui::TableSetColumnIndex(4); ImGui::TextDisabled("—");
                            ImGui::TableSetColumnIndex(5); ImGui::TextDisabled("—");
                            ImGui::TableSetColumnIndex(6); ImGui::TextDisabled("正常");
                        }
                        const auto& diagnostics=allDiagnostics;
                        ImGuiListClipper clipper;clipper.Begin(static_cast<int>(files.size()),m_assetRowHeight);
                        while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){const AssetRecord& rec=*files[static_cast<std::size_t>(row)];const bool selected=m_assetSelectionModel.selected.contains(rec.runtimePath);ImGui::PushID(rec.runtimePath.c_str());ImGui::TableNextRow(ImGuiTableRowFlags_None,m_assetRowHeight);ImGui::TableSetColumnIndex(0);
                            if(ImGui::Selectable(rec.absolutePath.filename().string().c_str(),selected,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0,m_assetRowHeight-4))){
                                const auto& io=ImGui::GetIO();
                                if(io.KeyShift&&!m_assetSelectionModel.anchor.empty()){
                                    std::size_t anchor=static_cast<std::size_t>(row);
                                    for(std::size_t find=0;find<files.size();++find)if(files[find]->runtimePath==m_assetSelectionModel.anchor){anchor=find;break;}
                                    if(!io.KeyCtrl)m_assetSelectionModel.selected.clear();
                                    const std::size_t rangeFirst=std::min(anchor,static_cast<std::size_t>(row));
                                    const std::size_t last=std::max(anchor,static_cast<std::size_t>(row));
                                    for(std::size_t select=rangeFirst;select<=last;++select)m_assetSelectionModel.selected.insert(files[select]->runtimePath);
                                }else if(io.KeyCtrl){if(selected)m_assetSelectionModel.selected.erase(rec.runtimePath);else m_assetSelectionModel.selected.insert(rec.runtimePath);}
                                else{m_assetSelectionModel.selected.clear();m_assetSelectionModel.selected.insert(rec.runtimePath);}
                                m_selectedAsset=rec.runtimePath;m_assetSelectionModel.anchor=rec.runtimePath;
                                if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))OpenAssetByType(rec);
                            }
                            if(ImGui::BeginDragDropSource()){ImGui::SetDragDropPayload(kResourcePayload,rec.runtimePath.c_str(),rec.runtimePath.size()+1);ImGui::TextUnformatted(rec.runtimePath.c_str());ImGui::EndDragDropSource();}
                             if(ImGui::BeginPopupContextItem("AssetRowMenu")){m_selectedAsset=rec.runtimePath;if(ImGui::MenuItem("開啟"))OpenAssetByType(rec);if(ImGui::MenuItem("重新命名…","F2")){m_assetRenameFrom=rec.runtimePath;std::snprintf(m_assetRenameBuf,sizeof(m_assetRenameBuf),"%s",rec.absolutePath.filename().string().c_str());}if(ImGui::MenuItem("複製"))DuplicateAssetWithHistory(rec.runtimePath);if(ImGui::MenuItem("重新匯入")){const Status scanned=m_assetRegistry.Scan(m_project.Context().root);if(!scanned)m_showAssetIdentity=true;}if(ImGui::MenuItem("在檔案總管顯示"))OpenInExplorer(rec.absolutePath.parent_path());if(ImGui::MenuItem("複製路徑"))ImGui::SetClipboardText(rec.runtimePath.c_str());ImGui::Separator();if(ImGui::MenuItem("移到垃圾桶","Delete"))m_assetPendingDelete=rec.runtimePath;ImGui::EndPopup();}
                            ImGui::TableSetColumnIndex(1);ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AssetTypeColor(rec.type)),"%s",rec.type.c_str());ImGui::TableSetColumnIndex(2);ImGui::TextDisabled("%s",HumanSize(rec.size).c_str());ImGui::TableSetColumnIndex(3);ImGui::TextDisabled("%s",HumanTime(rec.absolutePath).c_str());
                            const auto* identity=m_assetRegistry.FindPath(rec.absolutePath);ImGui::TableSetColumnIndex(4);if(identity)ImGui::TextDisabled("%s",identity->id.ToString().substr(0,8).c_str());else ImGui::TextColored(ImVec4(1,.55f,.35f,1),"缺少");ImGui::TableSetColumnIndex(5);ImGui::TextUnformatted(identity&&identity->includeInBuild?"是":"否");int problems=0;for(const auto& diagnostic:diagnostics)if(!diagnostic.source.path.empty()&&(diagnostic.source.path==rec.runtimePath||fs::path(diagnostic.source.path)==rec.absolutePath))++problems;ImGui::TableSetColumnIndex(6);if(problems)ImGui::TextColored(ImVec4(1,.45f,.35f,1),"%d 個",problems);else ImGui::TextDisabled("正常");ImGui::PopID();}
                        ImGui::EndTable();
                    }
                } else for (const fs::path& d : dirs) {
                    const std::string dname = Utf8Path(d.filename());
                    std::error_code directoryError;
                    const std::string runtimeDirectory =
                        fs::relative(d, root, directoryError).generic_string();
                    const bool directorySelected = m_selectedAsset == runtimeDirectory;
                    ImGui::PushID(dname.c_str());
                    if (m_fileSystemView == FileSystemViewMode::Thumbnails) {
                        if (x + cellW > maxW && x > 0.0f) x = 0.0f;
                        else if (x > 0.0f) ImGui::SameLine();
                        const ImVec2 pos = ImGui::GetCursorScreenPos();
                        const float labelH = ImGui::GetTextLineHeight() + 6.0f;
                        if (ImGui::Selectable("##dir", directorySelected,
                                              ImGuiSelectableFlags_AllowDoubleClick,
                                              ImVec2(tile, tile + labelH))) {
                            m_assetSelectionModel.Clear();
                            m_selectedAsset = runtimeDirectory;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                SetAssetDirectory(runtimeDirectory);
                        }
                        ImDrawList* draw = ImGui::GetWindowDrawList();
                        // simple folder glyph
                        const float w = tile * 0.55f, h = tile * 0.40f;
                        const ImVec2 c(pos.x + tile * 0.5f, pos.y + tile * 0.5f);
                        draw->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.35f - h * 0.18f),
                                            ImVec2(c.x - w * 0.1f, c.y - h * 0.35f),
                                            IM_COL32(210, 170, 90, 255), 2.0f);
                        draw->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.4f),
                                            ImVec2(c.x + w * 0.5f, c.y + h * 0.6f),
                                            IM_COL32(225, 185, 105, 255), 3.0f);
                        draw->PushClipRect(ImVec2(pos.x, pos.y + tile),
                                           ImVec2(pos.x + tile, pos.y + tile + labelH), true);
                        draw->AddText(ImVec2(pos.x + 2, pos.y + tile + 2),
                                      IM_COL32(210, 218, 230, 255), dname.c_str());
                        draw->PopClipRect();
                        x += cellW;
                    } else {
                        const std::string compactLabel = std::string(m_iconFontLoaded ?
                            Icon(EditorIcon::Folder) : "[資料夾]") + "  " + dname;
                        if (ImGui::Selectable(compactLabel.c_str(), directorySelected,
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            m_assetSelectionModel.Clear();
                            m_selectedAsset = runtimeDirectory;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                SetAssetDirectory(runtimeDirectory);
                        }
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(kResourcePayload)) {
                            MoveAssetTo(std::string(static_cast<const char*>(payload->Data),
                                                    payload->DataSize > 0 ? payload->DataSize - 1
                                                                          : 0),
                                        d);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::PopID();
                }

                if(m_fileSystemView==FileSystemViewMode::Thumbnails){
                    const int columns=std::max(1,static_cast<int>(maxW/cellW));
                    const int rows=static_cast<int>((files.size()+static_cast<std::size_t>(columns)-1)/static_cast<std::size_t>(columns));
                    ImGuiListClipper clipper;clipper.Begin(rows,tile+ImGui::GetTextLineHeight()+16.0f);
                    while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){
                        for(int column=0;column<columns;++column){const std::size_t index=static_cast<std::size_t>(row*columns+column);if(index>=files.size())break;if(column)ImGui::SameLine();RenderAssetEntry(*files[index],true,tile);}
                    }
                }else if(m_fileSystemView==FileSystemViewMode::Compact){
                    ImGuiListClipper clipper;clipper.Begin(static_cast<int>(files.size()),ImGui::GetTextLineHeightWithSpacing());
                    while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)RenderAssetEntry(*files[static_cast<std::size_t>(row)],false,0.0f);
                }
                if (dirs.empty() && files.empty()) {
                    ImGui::TextDisabled("此資料夾是空的");
                }
            }

            if (!m_selectedAsset.empty()) ImGui::TextDisabled("Identity and build inclusion are managed by .pxmeta in Asset Identity Resolver.");
        }
        ImGui::EndChild();

        // --- Create popup ---
        if (m_assetNewKind >= 0) {
            ImGui::OpenPopup("Create Asset");
        }
        if (ImGui::BeginPopupModal("Create Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* kinds[] = { "Folder", "Scenario (.pxscenario)", "UI Scene (.pxscene)" };
            ImGui::TextDisabled("%s in %s/",
                                kinds[std::clamp(m_assetNewKind, 0, 2)], m_assetDir.c_str());
            ImGui::SetNextItemWidth(240);
            const bool submit = ImGui::InputText("##newname", m_assetNewNameBuf,
                                                 sizeof(m_assetNewNameBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if ((submit || ImGui::Button("Create")) && m_assetNewNameBuf[0] != 0) {
                const fs::path cur = root / m_assetDir;
                const fs::path createdPath=m_assetNewKind==0?cur/m_assetNewNameBuf:
                    m_assetNewKind==1?cur/(std::string(m_assetNewNameBuf)+".pxscenario"):
                    cur/(std::string(m_assetNewNameBuf)+".pxscene");
                if(fs::exists(createdPath)){
                    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXASSETCREATE9407",.category="Editor.FileSystem",.message="同名項目已存在"};diagnostic.source.path=createdPath.generic_string();diag::Emit(std::move(diagnostic));
                }else if(const Status status=CreateAssetWithHistory(createdPath,m_assetNewKind);status){
                    Log(std::string("Created ") + m_assetNewNameBuf);m_assetNewKind=-1;ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                m_assetNewKind = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // --- Rename popup ---
        if (!m_assetRenameFrom.empty()) {
            ImGui::OpenPopup("Rename Asset");
        }
        if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("%s", m_assetRenameFrom.c_str());
            ImGui::SetNextItemWidth(260);
            const bool submit = ImGui::InputText("##rename", m_assetRenameBuf,
                                                 sizeof(m_assetRenameBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if ((submit || ImGui::Button("重新命名")) && m_assetRenameBuf[0] != 0) {
                std::error_code ec;
                const fs::path src = root / m_assetRenameFrom;
                const fs::path dst = src.parent_path() / m_assetRenameBuf;
                if (fs::exists(dst, ec)) {
                    Log("重新命名失敗，目的地已存在：" + dst.filename().string());
                } else {
                    const std::string newRel=fs::relative(dst,root,ec).generic_string();
                    const Status status=MoveAssetWithHistory(m_assetRenameFrom,newRel);
                    if(status)Log("已重新命名為 "+newRel);
                }
                m_assetRenameFrom.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消")) {
                m_assetRenameFrom.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (!m_assetPendingDelete.empty()) {
            ImGui::OpenPopup("Delete Asset?");
        }
        if (ImGui::BeginPopupModal("Delete Asset?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("將「%s」移到專案垃圾桶？", m_assetPendingDelete.c_str());
            ImGui::TextDisabled("可使用專案 Undo 完整還原檔案與 .pxmeta。");
            ImGui::Separator();
            if (ImGui::Button("移到垃圾桶", ImVec2(140, 0))) {
                const Status status=TrashAssetWithHistory(m_assetPendingDelete);
                if(status)Log("已移到垃圾桶："+m_assetPendingDelete);
                m_assetPendingDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(120, 0))) {
                m_assetPendingDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    else {
        m_assetsFocused = false;
    }
    ImGui::End();
}

void EditorApp::RenderImportReview() {
    if (!m_showImportReview) return;
    ImGui::OpenPopup("匯入素材");
    ImGui::SetNextWindowSize(ImVec2(920, 620), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("匯入素材", nullptr,
                                ImGuiWindowFlags_NoCollapse)) return;

    const auto progress = m_importService.Progress();
    if (progress.state == ImportState::Idle && m_importReview) {
        ImGui::TextUnformatted("確認來源、目的地與衝突策略後才會寫入專案。");
        ImGui::Separator();
        bool rebuild = false;
        ImGui::SetNextItemWidth(420);
        if (ImGui::InputText("目的地", &m_importDestinationText,
                             ImGuiInputTextFlags_EnterReturnsTrue)) rebuild = true;
        rebuild |= ImGui::Checkbox("依素材型別自動整理", &m_importAutoOrganize);
        ImGui::SameLine();
        rebuild |= ImGui::Checkbox("保留來源資料夾結構", &m_importPreserveFolders);
        ImGui::SameLine();
        ImGui::Checkbox("保留外部 GUID（進階）", &m_importPreserveIdentity);
        if (rebuild) {
            auto prepared = m_importService.Prepare(
                m_project.Context().root,
                m_project.Context().root / fs::path(m_importDestinationText),
                m_importSources, m_importAutoOrganize, m_importPreserveFolders);
            if (prepared) {
                m_importReview = prepared.TakeValue();
                m_importReview->preserveIdentity = m_importPreserveIdentity;
            } else for (const auto& diagnostic : prepared.Diagnostics()) diag::Emit(diagnostic);
        }
        ImGui::Separator();
        if (ImGui::BeginTable("ImportReviewTable", 7,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0, -52))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("匯入", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("來源", ImGuiTableColumnFlags_WidthStretch, 0.28f);
            ImGui::TableSetupColumn("型別", ImGuiTableColumnFlags_WidthFixed, 75);
            ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 75);
            ImGui::TableSetupColumn("目的地", ImGuiTableColumnFlags_WidthStretch, 0.32f);
            ImGui::TableSetupColumn("衝突策略", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 52);
            ImGui::TableHeadersRow();
            const char* policies[]{"使用現有", "保留兩者", "取代", "略過"};
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_importReview->items.size()),
                          ImGui::GetTextLineHeightWithSpacing());
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const std::size_t index = static_cast<std::size_t>(row);
                auto& item=m_importReview->items[index]; ImGui::PushID(row);
                ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::Checkbox("##enabled",&item.enabled);
                ImGui::TableSetColumnIndex(1);
                // Do not run native image decoders on external files in the review
                // screen. A type icon is enough until the transaction is committed.
                if (item.type == "image") {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AssetTypeColor(item.type)),
                                       "%s", m_iconFontLoaded ? Icon(EditorIcon::File) : "IMG");
                    ImGui::SameLine();
                }
                const std::string sourceName = Utf8Path(item.source.path.filename());
                const std::string sourcePath = Utf8Path(item.source.path);
                ImGui::TextUnformatted(sourceName.c_str());
                if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",sourcePath.c_str());
                ImGui::TableSetColumnIndex(2);ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AssetTypeColor(item.type)),"%s",item.type.c_str());
                ImGui::TableSetColumnIndex(3);ImGui::TextDisabled("%s",HumanSize(item.size).c_str());
                ImGui::TableSetColumnIndex(4);std::error_code ec;ImGui::TextWrapped("%s",fs::relative(item.target,m_project.Context().root,ec).generic_string().c_str());
                ImGui::TableSetColumnIndex(5);int policy=static_cast<int>(item.policy);ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##policy",&policy,policies,4)){item.policy=static_cast<ImportConflictPolicy>(policy);m_importService.RecalculateTargets(*m_importReview);}
                if(item.identical&&item.policy==ImportConflictPolicy::UseExisting&&ImGui::IsItemHovered())ImGui::SetTooltip("專案中已有完全相同的內容");
                ImGui::TableSetColumnIndex(6);ImGui::Checkbox("##build",&item.includeInBuild);ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("取消", ImVec2(110,34))) {
            m_showImportReview=false;m_importReview.reset();m_importService.Reset();ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("開始匯入", ImVec2(150,34))) {
            m_importReview->preserveIdentity=m_importPreserveIdentity;
            m_reportedImportFailure.clear();
            const Status started=m_importService.Start(*m_importReview);
            if(!started)for(const auto& diagnostic:started.Diagnostics())diag::Emit(diagnostic);
        }
    } else {
        ImportProgress current = progress;
        if (current.state == ImportState::Ready) {
            const Status committed=m_importService.Commit(m_assetRegistry);
            if(!committed)for(const auto& diagnostic:committed.Diagnostics())diag::Emit(diagnostic);
            current=m_importService.Progress();
            if(current.state==ImportState::Completed){
                if(!m_importService.CommitRecords().empty()){
                    auto state=std::make_shared<ImportUndoState>();
                    state->root=m_importService.UndoRoot();
                    state->records=m_importService.CommitRecords();
                    auto refresh=[this]{m_assets.Scan(m_project.Context());const Status scanned=m_assetRegistry.Scan(m_project.Context().root);if(!scanned)m_showAssetIdentity=true;RefreshProblems();};
                    auto redo=[this,state,refresh]()mutable{const Status status=SetImportApplied(state,true);if(status)refresh();return status;};
                    auto undo=[this,state,refresh]()mutable{const Status status=SetImportApplied(state,false);if(status)refresh();return status;};
                    const Status recorded=m_projectHistory.CommitApplied(std::make_unique<FunctionalProjectCommand>(
                        "匯入 "+std::to_string(state->records.size())+" 個素材",std::move(redo),std::move(undo)));
                    if(!recorded)for(const auto& diagnostic:recorded.Diagnostics())diag::Emit(diagnostic);
                }
                m_assets.Scan(m_project.Context());
                if(!m_importService.CommittedPaths().empty()){
                    std::error_code ec;const auto relative=fs::relative(m_importService.CommittedPaths().front(),m_project.Context().root,ec).generic_string();
                    m_selectedAsset=relative;m_assetSelectionModel.selected={relative};
                }
                RefreshProblems();
            }
        }
        if (current.state == ImportState::Failed &&
            current.message != m_reportedImportFailure) {
            m_reportedImportFailure = current.message;
            diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                .code = "PXIMPORT9027", .category = "Editor.Import",
                .message = "素材匯入失敗", .details = current.message};
            if (m_importReview) diagnostic.source.path = Utf8Path(m_importReview->destination);
            diag::Emit(std::move(diagnostic));
        }
        const char* stateLabel = current.state == ImportState::Completed ? "素材匯入完成" :
            current.state == ImportState::Failed ? "素材匯入失敗" :
            current.state == ImportState::Cancelled ? "素材匯入已取消" :
            current.state == ImportState::Committing ? "正在提交素材…" : "正在準備素材…";
        ImGui::TextUnformatted(stateLabel);
        const float fraction=current.totalBytes?static_cast<float>(current.copiedBytes)/static_cast<float>(current.totalBytes):0.0f;
        ImGui::ProgressBar(std::clamp(fraction,0.0f,1.0f),ImVec2(-1,30));
        ImGui::TextDisabled("%zu / %zu  %s",current.currentItem,current.totalItems,current.currentFile.c_str());
        if(!current.message.empty())ImGui::TextWrapped("%s",current.message.c_str());
        if(current.state==ImportState::Staging){if(ImGui::Button("取消匯入"))m_importService.Cancel();}
        if(current.state==ImportState::Completed||current.state==ImportState::Failed||current.state==ImportState::Cancelled){if(ImGui::Button("關閉",ImVec2(120,34))){m_showImportReview=false;m_importReview.reset();m_importService.Reset();ImGui::CloseCurrentPopup();}}
    }
    ImGui::EndPopup();
}

void EditorApp::RenderConsole() {
    if (ImGui::Begin("Console")) {
        if (ImGui::SmallButton("Clear")) m_console.clear();
        ImGui::Separator();
        if (ImGui::BeginChild("##log")) {
            for (const std::string& line : m_console) {
                ImGui::TextUnformatted(line.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::RenderPreview() {
    if (ImGui::Begin("UI Designer") && m_preview) {
        if(ImGui::BeginTabBar("##ui-document-tabs",ImGuiTabBarFlags_Reorderable|ImGuiTabBarFlags_FittingPolicyScroll)){
            const auto requestedDocument=m_uiFocusRequest.empty()
                ? std::filesystem::path{}
                : DocumentManager::Canonical(m_uiFocusRequest);
            bool focusRequestHandled=false;
            for(const auto& session:m_docs.Documents()){
                if(session.type!=DocumentType::UIScene)continue;
                bool open=true;ImGuiTabItemFlags flags=session.dirty?ImGuiTabItemFlags_UnsavedDocument:0;
                const bool active=m_designer.Document()&&DocumentManager::Canonical(m_designer.Document()->Path())==session.id.canonicalPath;
                const bool requested=!requestedDocument.empty()&&requestedDocument==session.id.canonicalPath;
                if(requested){flags|=ImGuiTabItemFlags_SetSelected;focusRequestHandled=true;}
                const std::string label=std::string(session.pinned?"◆ ":"")+session.label+(session.dirty?" ●":"")+"###ui-"+session.id.canonicalPath.generic_string();
                if(ImGui::BeginTabItem(label.c_str(),session.pinned?nullptr:&open,flags)){
                    // An externally requested document is selected later in this tab-bar pass.
                    // Do not let ImGui's previously selected tab reactivate itself first.
                    if(!active&&(requestedDocument.empty()||requested)){std::error_code error;const auto runtime=fs::relative(session.id.canonicalPath,m_project.Context().root,error).generic_string();if(!error){const Status status=ActivateUIDocument(session.id.canonicalPath,runtime);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}}
                    ImGui::EndTabItem();
                }
                if(!open){m_uiDocumentCloseRequest=session.id.canonicalPath;if(session.dirty)m_uiDocumentClosePopup=true;}
            }
            if(focusRequestHandled)m_uiFocusRequest.clear();
            ImGui::EndTabBar();
        }
        const auto closeUiDocument=[this](const fs::path& path){
            const auto canonical=DocumentManager::Canonical(path);const std::string key=canonical.generic_string();
            const bool active=m_designer.Document()&&DocumentManager::Canonical(m_designer.Document()->Path())==canonical;
            if(active){
                std::optional<DocumentSession> replacement;
                for(const auto& candidate:m_docs.Documents())if(candidate.type==DocumentType::UIScene&&candidate.id.canonicalPath!=canonical){replacement=candidate;break;}
                if(replacement){std::error_code error;const auto runtime=fs::relative(replacement->id.canonicalPath,m_project.Context().root,error).generic_string();if(!error)(void)ActivateUIDocument(replacement->id.canonicalPath,runtime);m_inactiveDesigners.erase(key);}
                else{m_designer=UIDesigner{};ConfigureDesigner(m_designer);m_designerPath.clear();m_previewMode=1;m_preview->LoadVn(m_project.Context().manifest.startScript);}
            }else m_inactiveDesigners.erase(key);
            (void)m_docs.Close(canonical);
        };
        if(!m_uiDocumentCloseRequest.empty()&&!m_uiDocumentClosePopup){closeUiDocument(m_uiDocumentCloseRequest);m_uiDocumentCloseRequest.clear();}
        if(m_uiDocumentClosePopup)ImGui::OpenPopup("關閉未儲存 UI 文件");
        if(ImGui::BeginPopupModal("關閉未儲存 UI 文件",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::Text("「%s」有未儲存的變更。",m_uiDocumentCloseRequest.filename().string().c_str());
            if(ImGui::Button("儲存",ImVec2(110,0))){bool saved=false;const auto canonical=DocumentManager::Canonical(m_uiDocumentCloseRequest);if(m_designer.Document()&&DocumentManager::Canonical(m_designer.Document()->Path())==canonical)saved=m_designer.Save();else if(auto found=m_inactiveDesigners.find(canonical.generic_string());found!=m_inactiveDesigners.end())saved=found->second.editor->Save();if(saved)closeUiDocument(canonical);m_uiDocumentCloseRequest.clear();m_uiDocumentClosePopup=false;ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("捨棄",ImVec2(110,0))){closeUiDocument(m_uiDocumentCloseRequest);m_uiDocumentCloseRequest.clear();m_uiDocumentClosePopup=false;ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("取消",ImVec2(110,0))){m_uiDocumentCloseRequest.clear();m_uiDocumentClosePopup=false;ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        ImGui::SetNextItemWidth(140);
        const char* modes[] = { "UI Scene", "VN Script" };
        if (ImGui::Combo("##mode", &m_previewMode, modes, 2)) {
            if (m_previewMode == 0) {
                SyncDesigner();
            }
            else {
                m_preview->LoadVn(m_project.Context().manifest.startScript);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload")) {
            m_preview->Reload();
        }
        if (m_previewMode == 0) {
            ImGui::SameLine();
            const std::string current = m_preview->CurrentUIPath();
            ImGui::SetNextItemWidth(260);
            if (ImGui::BeginCombo("##screen", current.empty() ? "(screen)" : current.c_str())) {
                for (const AssetRecord& a : m_assets.Filter("", "ui")) {
                    if (ImGui::Selectable(a.runtimePath.c_str(), a.runtimePath == current)) {
                        const Status status = ActivateUIDocument(a.absolutePath, a.runtimePath);
                        if (!status) for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Screen")) {
                m_designer.Save();
            }
            ImGui::SameLine();
            if (ImGui::Button("New Screen")) {
                ImGui::OpenPopup("newScreen");
            }
            if (ImGui::BeginPopup("newScreen")) {
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("name", m_newScreenName, sizeof(m_newScreenName));
                ImGui::SameLine();
                if (ImGui::Button("Create")) {
                    const std::filesystem::path rel = std::filesystem::path("Content/UI") / (std::string(m_newScreenName) + ".pxscene");
                    const std::filesystem::path abs = m_project.Context().root / rel;
                    std::error_code ec;
                    std::filesystem::create_directories(abs.parent_path(), ec);
                    if (!std::filesystem::exists(abs)) {
                        UIDesigner createdDesigner;
                        ConfigureDesigner(createdDesigner);
                        const Status created = createdDesigner.New(abs);
                        if (created) createdDesigner.Save();
                    }
                    m_assets.Scan(m_project.Context());
                    const Status activated = ActivateUIDocument(abs, rel.generic_string());
                    if (!activated) for (const auto& diagnostic : activated.Diagnostics()) diag::Emit(diagnostic);
                    Log("Created screen " + rel.generic_string());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        if (m_previewMode == 0) {
            ImGui::Separator();
            auto& panels=m_designer.ViewportState();
            for(int mode=0;mode<3;++mode){if(mode)ImGui::SameLine();const char* label=mode==0?"Design 設計":mode==1?"Interact 互動":"Animate 動畫";if(ImGui::Selectable(label,panels.authorMode==mode,0,ImVec2(126,0))){panels.authorMode=mode;panels.interactivePreview=false;}}
            if(panels.authorMode==2){ImGui::SameLine();ImGui::TextDisabled("│");ImGui::SameLine();if(ImGui::Selectable("Clip",panels.animateSurface==0,0,ImVec2(64,0)))panels.animateSurface=0;ImGui::SameLine();if(ImGui::Selectable("State Machine",panels.animateSurface==1,0,ImVec2(112,0)))panels.animateSurface=1;}
            ImGui::SameLine();ImGui::TextDisabled("  面板");ImGui::SameLine();if(ImGui::SmallButton(panels.leftPanelVisible?"隱藏左側":"顯示左側"))panels.leftPanelVisible=!panels.leftPanelVisible;ImGui::SameLine();if(ImGui::SmallButton(panels.rightPanelVisible?"隱藏 Inspector":"顯示 Inspector"))panels.rightPanelVisible=!panels.rightPanelVisible;if(!(panels.authorMode==2&&panels.animateSurface==0)){ImGui::SameLine();if(ImGui::SmallButton(panels.bottomPanelVisible?"隱藏 Problems":"顯示 Problems"))panels.bottomPanelVisible=!panels.bottomPanelVisible;}
            if(panels.authorMode==0||(panels.authorMode==2&&panels.animateSurface==0)){ImGui::Separator();m_designer.RenderViewportToolbar();}
        }
        ImGui::Separator();

        const bool designerComposite=m_previewMode==0;auto& state=m_designer.ViewportState();const ImVec2 workspace=ImGui::GetContentRegionAvail();const bool compact=workspace.x<1000.0f;const bool showLeft=designerComposite&&state.leftPanelVisible&&!compact;const bool showRight=designerComposite&&state.rightPanelVisible&&workspace.x>=760.0f;const bool clipTimeline=state.authorMode==2&&state.animateSurface==0;const bool showBottom=designerComposite&&(clipTimeline||state.bottomPanelVisible)&&workspace.y>=420.0f;const float drawerHeight=showBottom?std::clamp(state.bottomPanelHeight,150.0f,std::max(150.0f,workspace.y*.5f)):0.0f;const int canvasColumn=showLeft?1:0;const int inspectorColumn=canvasColumn+1;const int columnCount=1+(showLeft?1:0)+(showRight?1:0);
        if(designerComposite){
            ImGui::BeginTable("##ui-designer-composite",columnCount,ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,ImVec2(0,std::max(240.0f,workspace.y-drawerHeight-(showBottom?6.0f:0.0f))));
            if(showLeft)ImGui::TableSetupColumn("Layers",ImGuiTableColumnFlags_WidthFixed,state.leftPanelWidth);ImGui::TableSetupColumn("Canvas",ImGuiTableColumnFlags_WidthStretch);if(showRight)ImGui::TableSetupColumn("Inspector",ImGuiTableColumnFlags_WidthFixed,state.rightPanelWidth);ImGui::TableNextRow();
            if(showLeft){ImGui::TableSetColumnIndex(0);if(ImGui::BeginChild("##designer-left",ImVec2(0,0))){if(state.authorMode==1)m_designer.RenderInteractionNavigator();else if(state.authorMode==2&&state.animateSurface==1)m_designer.RenderAnimationNavigator();else if(ImGui::BeginTabBar("##left-designer-tabs")){if(ImGui::BeginTabItem("Layers")){m_designer.RenderHierarchy();ImGui::EndTabItem();}if(ImGui::BeginTabItem("Insert")){m_designer.RenderInsert();ImGui::EndTabItem();}if(ImGui::BeginTabItem("Components")){m_designer.RenderComponents();ImGui::EndTabItem();}ImGui::EndTabBar();}}ImGui::EndChild();state.leftPanelWidth=ImGui::GetColumnWidth(0);}ImGui::TableSetColumnIndex(canvasColumn);
        }

        if(designerComposite&&state.authorMode==1){if(ImGui::BeginChild("##interaction-workspace",ImVec2(0,0),ImGuiChildFlags_Borders))m_designer.RenderBehaviorGraph();ImGui::EndChild();}
        else if(designerComposite&&state.authorMode==2&&state.animateSurface==1){if(ImGui::BeginChild("##animation-machine-workspace",ImVec2(0,0),ImGuiChildFlags_Borders))m_designer.RenderAnimationStateMachine();ImGui::EndChild();}
        else {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool uiMode = m_previewMode == 0;
        const bool vnMode = !uiMode;
        const float debugHeight = vnMode ? 230.0f : 0.0f;
        avail.y = std::max(80.0f, avail.y - debugHeight);
        ImVec2 canvasCursor{};
        if(uiMode){
            ImGui::BeginChild("##designer-canvas-scroll",avail,ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_NoNavInputs);
            if(m_designer.ViewportState().applyStoredScroll){ImGui::SetScrollX(m_designer.ViewportState().scrollX);ImGui::SetScrollY(m_designer.ViewportState().scrollY);m_designer.ViewportState().applyStoredScroll=false;}
            avail=ImGui::GetContentRegionAvail();canvasCursor=ImGui::GetCursorPos();
        }
        const float gw = static_cast<float>(m_preview->Width());
        const float gh = static_cast<float>(m_preview->Height());
        constexpr float designerCanvasMargin = 40.0f;
        const float fitWidth = uiMode ? std::max(1.0f, avail.x - designerCanvasMargin) : avail.x;
        const float fitHeight = uiMode ? std::max(1.0f, avail.y - designerCanvasMargin) : avail.y;
        const float fitScale = std::min(fitWidth / gw, fitHeight / gh);
        const float rawScale = uiMode?(m_designer.ViewportState().fitToViewport?fitScale:m_designer.ViewportState().zoom):fitScale;
        const float framebufferScale=std::max(ImGui::GetIO().DisplayFramebufferScale.x,ImGui::GetIO().DisplayFramebufferScale.y);
        const float scale=std::max(.01f,std::round(rawScale*gw*framebufferScale)/(gw*framebufferScale));
        const ImVec2 disp(gw * scale, gh * scale);
        const bool fittedDesignerCanvas=uiMode&&m_designer.ViewportState().fitToViewport;
        const ImVec2 contentSize{uiMode&&!fittedDesignerCanvas?std::max(avail.x,disp.x+designerCanvasMargin):avail.x,uiMode&&!fittedDesignerCanvas?std::max(avail.y,disp.y+designerCanvasMargin):avail.y};
        const ImVec2 viewportMin = ImGui::GetCursorScreenPos();
        const ImRect viewport=uiMode?ImGui::GetCurrentWindow()->InnerRect:ImRect(viewportMin,ImVec2(viewportMin.x+avail.x,viewportMin.y+avail.y));
        ImVec2 p0=viewportMin;p0.x+=std::max(20.0f,(contentSize.x-disp.x)*.5f);p0.y+=std::max(20.0f,(contentSize.y-disp.y)*.5f);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool viewportHovered = viewport.Contains(mouse);
        if(uiMode&&viewportHovered&&ImGui::GetIO().KeyShift&&ImGui::GetIO().MouseWheel!=0.0f)
            ImGui::SetScrollX(std::max(0.0f,ImGui::GetScrollX()-ImGui::GetIO().MouseWheel*48.0f));
        const bool hovered = mouse.x >= p0.x && mouse.x <= p0.x + disp.x && mouse.y >= p0.y && mouse.y <= p0.y + disp.y;
        const float localX = scale > 0 ? (mouse.x - p0.x) / scale : 0.0f;
        const float localY = scale > 0 ? (mouse.y - p0.y) / scale : 0.0f;
        const bool click = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool previewClick = uiMode
                                      ? (m_designer.ViewportState().interactivePreview && click)
                                      : click;
        m_preview->SetDisplayScale(scale*framebufferScale,uiMode&&m_designer.ViewportState().pixelExactPreview);
        m_preview->Tick(ImGui::GetIO().DeltaTime, SDL_GetTicks(), hovered, localX, localY,
                        previewClick);

        if (uiMode) ImGui::PushClipRect(viewport.Min, viewport.Max, true);
        if (m_preview->Target()) {
            ImGui::SetCursorScreenPos(p0);
            ImGui::Image(reinterpret_cast<ImTextureID>(m_preview->Target()), disp);
        }
        if(uiMode&&state.interactivePreview){ImDrawList* draw=ImGui::GetWindowDrawList();const ImVec2 maximum{p0.x+disp.x,p0.y+disp.y};draw->AddRect(p0,maximum,IM_COL32(60,244,136,255),0.0f,ImDrawFlags_None,4.0f);draw->AddRectFilled(p0,{p0.x+184,p0.y+27},IM_COL32(20,82,52,245));draw->AddText({p0.x+8,p0.y+5},IM_COL32(215,255,230,255),"PREVIEW · Esc 返回 Edit");}
        if (vnMode) {
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + 8,
                                             p0.y + disp.y + 6));
            if (ImGui::BeginChild("##vmdebug", ImVec2(0, debugHeight - 10),
                                  ImGuiChildFlags_Borders)) {
                const px::vn::VM& vm = m_preview->VMRef();
                const auto stateName = [](px::vn::VMState s) {
                    switch (s) {
                        case px::vn::VMState::Idle: return "Idle";
                        case px::vn::VMState::Running: return "Running";
                        case px::vn::VMState::WaitingClick: return "WaitingClick";
                        case px::vn::VMState::WaitingChoice: return "WaitingChoice";
                        case px::vn::VMState::WaitingTimer: return "WaitingTimer";
                        case px::vn::VMState::Paused: return "PAUSED";
                        case px::vn::VMState::Finished: return "Finished";
                    }
                    return "?";
                };
                const px::vn::Program& prog = vm.CurrentProgram();
                ImGui::TextDisabled("VM");
                ImGui::SameLine();
                ImGui::Text("%s  |  %s  pc %d/%d", stateName(vm.State()),
                            vm.CurrentScript().c_str(), vm.ProgramCounter(),
                            static_cast<int>(prog.code.size()));
                ImGui::SameLine();
                if (ImGui::SmallButton("Advance")) {
                    m_preview->VMRef().OnAdvance();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Restart")) {
                    m_preview->Reload();
                }
                if (vm.State() == px::vn::VMState::Paused) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.55f, 0.15f, 1.0f));
                    if (ImGui::SmallButton("Step")) {
                        m_preview->VMRef().DebugStep();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Continue")) {
                        m_preview->VMRef().DebugContinue();
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(click a row below to toggle a breakpoint)");

                const int pc = vm.ProgramCounter();
                const float colW = ImGui::GetContentRegionAvail().x;
                if (ImGui::BeginChild("##vmleft", ImVec2(colW * 0.45f, 0))) {
                    if (pc >= 0 && pc < static_cast<int>(prog.code.size())) {
                        const px::vn::Command& cmd = prog.code[static_cast<std::size_t>(pc)];
                        std::string args;
                        for (const auto& a : cmd.args) {
                            args += " " + a.key + "=\"" + a.value + "\"";
                        }
                        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "next: [%s%s] (line %d)",
                                           cmd.type.c_str(), args.c_str(), cmd.line);
                    }
                    for (const std::string& err : prog.errors) {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "compile: %s",
                                           err.c_str());
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Variables");
                    if (m_preview->Vars().All().empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(none)");
                    }
                    if (ImGui::BeginTable("##vmvars", 2, ImGuiTableFlags_SizingStretchProp)) {
                        for (const auto& [name, value] : m_preview->Vars().All()) {
                            ImGui::TableNextColumn();
                            ImGui::Text("%s = %d", name.c_str(), value);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                if (ImGui::BeginChild("##vmcode", ImVec2(0, 0),
                                      ImGuiChildFlags_Borders)) {
                    for (int i = 0; i < static_cast<int>(prog.code.size()); ++i) {
                        const px::vn::Command& cmd = prog.code[static_cast<std::size_t>(i)];
                        const bool hasBp = vm.Breakpoints().count(cmd.line) != 0;
                        const bool isCurrent = i == pc;
                        std::string label = hasBp ? "● " : "   ";
                        label += std::to_string(cmd.line) + "  [" + cmd.type;
                        if (const std::string v =
                                cmd.Get("value", cmd.Get("text", cmd.Get("file", cmd.Get("name"))));
                            !v.empty()) {
                            label += " " + (v.size() > 28 ? v.substr(0, 28) + "…" : v);
                        }
                        label += "]";
                        if (hasBp) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.3f, 1.0f));
                        }
                        if (ImGui::Selectable((label + "##cmd" + std::to_string(i)).c_str(),
                                              isCurrent)) {
                            m_preview->VMRef().ToggleBreakpoint(cmd.line);
                        }
                        if (hasBp) {
                            ImGui::PopStyleColor();
                        }
                        if (isCurrent && pc != m_vmLastPc) {
                            ImGui::SetScrollHereY(0.35f);
                        }
                    }
                    m_vmLastPc = pc;
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();
        }
        if (uiMode) {
            std::string selectedImageAsset;
            for (const AssetRecord& rec : m_assets.Assets()) {
                if (rec.runtimePath == m_selectedAsset && rec.type == "image") {
                    selectedImageAsset = rec.runtimePath;
                    break;
                }
            }
            if(!state.interactivePreview){m_designer.HandleCanvasInteraction(viewport, p0, scale, viewportHovered, selectedImageAsset);m_designer.RenderCanvasOverlay(p0, scale);}
            const ImRect canvasRect(p0, ImVec2(p0.x + disp.x, p0.y + disp.y));
            if (!state.interactivePreview&&ImGui::BeginDragDropTargetCustom(canvasRect, ImGui::GetID("UIDesignerCanvasDrop"))) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
                    std::string asset(static_cast<const char*>(payload->Data),
                                      payload->DataSize > 0 ? payload->DataSize - 1 : 0);
                    if (AssetDatabase::Classify(asset) == "image" && scale > 0.0f) {
                        const ImVec2 dropMouse = ImGui::GetMousePos();
                        m_selectedAsset = asset;
                        m_designer.AddImageAt((dropMouse.x - p0.x) / scale,
                                              (dropMouse.y - p0.y) / scale, asset);
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (uiMode) {
            ImGui::PopClipRect();
            ImGui::SetCursorPos(canvasCursor);ImGui::Dummy(contentSize);
            m_designer.ViewportState().scrollX=ImGui::GetScrollX();m_designer.ViewportState().scrollY=ImGui::GetScrollY();
            ImGui::EndChild();
        }
        }
        if(designerComposite){if(showRight){ImGui::TableSetColumnIndex(inspectorColumn);if(ImGui::BeginChild("##designer-inspector",ImVec2(0,0))){if(state.authorMode==1)m_designer.RenderInteractionInspector();else if(state.authorMode==2&&state.animateSurface==1)m_designer.RenderAnimationInspector();else m_designer.RenderInspector(m_selectedAsset);}ImGui::EndChild();state.rightPanelWidth=ImGui::GetColumnWidth(inspectorColumn);}ImGui::EndTable();
            if(showBottom){ImGui::InvisibleButton("##designer-bottom-splitter",ImVec2(-1,6));if(ImGui::IsItemActive()){state.bottomPanelHeight=std::clamp(state.bottomPanelHeight-ImGui::GetIO().MouseDelta.y,150.0f,std::max(150.0f,workspace.y*.5f));}if(ImGui::IsItemHovered()||ImGui::IsItemActive())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(ImGui::BeginChild("##designer-bottom-drawer",ImVec2(0,drawerHeight-6.0f),ImGuiChildFlags_Borders)){
                if(clipTimeline)m_designer.RenderAnimation();else m_designer.RenderProblems();
            }ImGui::EndChild();}}
    }
    ImGui::End();
}

void EditorApp::RenderNodeEditor() {
    if (ImGui::Begin("Node Editor")) {
        m_nodeEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (m_scriptDocs.empty()) {
            ImGui::TextDisabled("Open a .pxscenario from Assets or Story Map.");
        } else if (ImGui::BeginTabBar("##scenario-doc-tabs",
                                      ImGuiTabBarFlags_Reorderable |
                                          ImGuiTabBarFlags_FittingPolicyScroll)) {
            int closeRequest = -1;
            for (int i = 0; i < static_cast<int>(m_scriptDocs.size()); ++i) {
                NodeGraphEditor& doc = *m_scriptDocs[static_cast<std::size_t>(i)];
                const std::string path = doc.CurrentRuntimePath();
                const auto* session = m_docs.Find(doc.DocumentPath());
                const bool pinned = session && session->pinned;
                const std::string label = std::string(pinned ? "◆ " : "") +
                                          fs::path(path).filename().string() +
                                          (doc.Dirty() ? " ●" : "") + "###doc-" + path;
                bool open = true;
                ImGuiTabItemFlags flags = doc.Dirty() ? ImGuiTabItemFlags_UnsavedDocument : 0;
                if (m_focusDocRequest == i) {
                    flags |= ImGuiTabItemFlags_SetSelected;
                    m_focusDocRequest = -1;
                }
                if (ImGui::BeginTabItem(label.c_str(), pinned ? nullptr : &open, flags)) {
                    m_activeDoc = i;
                    (void)m_docs.Activate(doc.DocumentPath());
                    doc.Render();
                    const ImVec2 dropPosition=ImGui::GetWindowPos(),dropSize=ImGui::GetWindowSize();const ImRect dropArea(dropPosition,ImVec2(dropPosition.x+dropSize.x,dropPosition.y+dropSize.y));
                    if(ImGui::BeginDragDropTargetCustom(dropArea,ImGui::GetID(("scenario-drop-"+path).c_str()))){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload(kResourcePayload)){const std::string asset(static_cast<const char*>(payload->Data),payload->DataSize>0?payload->DataSize-1:0);doc.CreateNodeForAsset(asset);}ImGui::EndDragDropTarget();}
                    ImGui::EndTabItem();
                }
                if (!open) {
                    closeRequest = i;
                }
            }
            ImGui::EndTabBar();
            if (closeRequest >= 0) {
                if (m_scriptDocs[static_cast<std::size_t>(closeRequest)]->Dirty()) {
                    m_documentCloseRequest = closeRequest;
                    m_documentClosePopup = true;
                } else {
                    (void)m_docs.Close(m_scriptDocs[static_cast<std::size_t>(closeRequest)]->DocumentPath());
                    m_scriptDocs.erase(m_scriptDocs.begin() + closeRequest);
                    if (m_activeDoc >= static_cast<int>(m_scriptDocs.size()))
                        m_activeDoc = static_cast<int>(m_scriptDocs.size()) - 1;
                }
            }
        }
        if (m_documentClosePopup) ImGui::OpenPopup("關閉未儲存文件");
        if (ImGui::BeginPopupModal("關閉未儲存文件", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_documentCloseRequest >= 0 &&
                m_documentCloseRequest < static_cast<int>(m_scriptDocs.size())) {
                auto& document = m_scriptDocs[static_cast<std::size_t>(m_documentCloseRequest)];
                ImGui::Text("「%s」有未儲存的變更。", document->DocumentPath().filename().string().c_str());
                ImGui::TextDisabled("關閉前要儲存嗎？此動作不會偷偷覆寫。 ");
                if (ImGui::Button("儲存", ImVec2(110, 0))) {
                    if (document->Save() && EnsureAssetIdentity(document->DocumentPath())) {
                        (void)m_docs.Close(document->DocumentPath());
                        m_scriptDocs.erase(m_scriptDocs.begin() + m_documentCloseRequest);
                    }
                    m_documentClosePopup = false; m_documentCloseRequest = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("捨棄", ImVec2(110, 0))) {
                    (void)m_docs.Close(document->DocumentPath());
                    m_scriptDocs.erase(m_scriptDocs.begin() + m_documentCloseRequest);
                    m_documentClosePopup = false; m_documentCloseRequest = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("取消", ImVec2(110, 0))) {
                    m_documentClosePopup = false; m_documentCloseRequest = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    } else {
        m_nodeEditorFocused = false;
    }
    ImGui::End();
}

void EditorApp::RenderAnimation() {
    if (ImGui::Begin("Animation")) {
        if (ImGui::BeginTabBar("##animation-workspace")) {
            if (ImGui::BeginTabItem("Timeline Clip")) {
                const bool selectedClip = std::filesystem::path(m_selectedAsset).extension() == ".pxanim";
                if (selectedClip && m_timelinePath != m_selectedAsset) {
                    m_timelinePath = m_selectedAsset; m_timelineClip.reset(); m_timelineCursor = 0; m_timelineTrack = -1;
                    std::ifstream stream(m_project.Context().root/m_timelinePath,std::ios::binary);
                    std::ostringstream text; if(stream){text<<stream.rdbuf();auto parsed=animation::ParseAnimationClip(text.str(),m_timelinePath);if(parsed)m_timelineClip=parsed.TakeValue();else for(const auto& diagnostic:parsed.Diagnostics())diag::Emit(diagnostic);}
                }
                if (!m_timelineClip) {
                    ImGui::TextDisabled("Select a .pxanim asset, or create an editable preset copy.");
                    static int presetIndex = 0; const auto presets = animation::OfficialPresets();
                    if (presetIndex >= static_cast<int>(presets.size())) presetIndex = 0;
                    if (ImGui::BeginCombo("Preset",presets[static_cast<std::size_t>(presetIndex)].name.c_str())) {
                        for(int i=0;i<static_cast<int>(presets.size());++i)if(ImGui::Selectable(presets[static_cast<std::size_t>(i)].name.c_str(),i==presetIndex))presetIndex=i;
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button("Create Editable Copy")) {
                        auto clip=presets[static_cast<std::size_t>(presetIndex)];clip.id=Uuid::Random();
                        std::string file=clip.name;std::replace(file.begin(),file.end(),'/','-');
                        m_timelinePath="Content/Animations/"+file+".pxanim";
                        const Status written=io::AtomicFile::WriteText(m_project.Context().root/m_timelinePath,animation::WriteAnimationClip(clip));
                        if(written){m_timelineClip=std::move(clip);m_selectedAsset=m_timelinePath;auto registered=m_assetRegistry.RegisterAsset(m_project.Context().root,m_project.Context().root/m_timelinePath,"resource");if(!registered)for(const auto& d:registered.Diagnostics())diag::Emit(d);m_assets.Scan(m_project.Context());}
                        else for(const auto& d:written.Diagnostics())diag::Emit(d);
                    }
                } else {
                    auto& clip=*m_timelineClip; bool changed=false;
                    changed|=ImGui::InputText("Name",&clip.name);changed|=ImGui::DragFloat("Duration",&clip.duration,.01f,.01f,3600.0f,"%.2f s");changed|=ImGui::Checkbox("Loop",&clip.loop);
                    if(ImGui::Button(m_timelinePlaying?"Pause":"Play"))m_timelinePlaying=!m_timelinePlaying;ImGui::SameLine();if(ImGui::Button("Stop")){m_timelinePlaying=false;m_timelineCursor=0;}
                    if(m_timelinePlaying){m_timelineCursor+=ImGui::GetIO().DeltaTime;if(m_timelineCursor>=clip.duration){if(clip.loop)m_timelineCursor=std::fmod(m_timelineCursor,std::max(.01f,clip.duration));else{m_timelineCursor=clip.duration;m_timelinePlaying=false;}}}
                    changed|=ImGui::SliderFloat("Scrub",&m_timelineCursor,0.0f,std::max(.01f,clip.duration),"%.3f s");
                    ImGui::SeparatorText("Tracks");
                    for(int i=0;i<static_cast<int>(clip.tracks.size());++i){auto& track=clip.tracks[static_cast<std::size_t>(i)];const std::string label=track.binding.target+" · "+track.binding.property;if(ImGui::Selectable((label+"##track"+std::to_string(i)).c_str(),m_timelineTrack==i))m_timelineTrack=i;}
                    if(ImGui::Button("Add Track")){clip.tracks.push_back({{animation::TargetKind::Stage,"$selection","alpha"},{{0.0f,0.0,animation::Curve::Linear},{clip.duration,1.0,animation::Curve::EaseOut}}});m_timelineTrack=static_cast<int>(clip.tracks.size())-1;changed=true;}
                    if(m_timelineTrack>=0&&m_timelineTrack<static_cast<int>(clip.tracks.size())){auto& track=clip.tracks[static_cast<std::size_t>(m_timelineTrack)];changed|=ImGui::InputText("Target",&track.binding.target);changed|=ImGui::InputText("Property",&track.binding.property);ImGui::SeparatorText("Keyframes");for(std::size_t i=0;i<track.keys.size();++i){ImGui::PushID(static_cast<int>(i));auto& key=track.keys[i];changed|=ImGui::DragFloat("Time",&key.time,.01f,0,clip.duration);if(auto* number=key.value.TryGet<double>())changed|=ImGui::DragScalar("Value",ImGuiDataType_Double,number,.01f);else if(auto* integer=key.value.TryGet<std::int64_t>())changed|=ImGui::DragScalar("Value",ImGuiDataType_S64,integer);ImGui::Separator();ImGui::PopID();}}
                    if(changed){for(auto& track:clip.tracks)std::sort(track.keys.begin(),track.keys.end(),[](const auto& a,const auto& b){return a.time<b.time;});}
                    if(ImGui::Button("Save .pxanim")){const Status valid=clip.Validate(m_timelinePath);if(valid){const Status written=io::AtomicFile::WriteText(m_project.Context().root/m_timelinePath,animation::WriteAnimationClip(clip));if(!written)for(const auto& d:written.Diagnostics())diag::Emit(d);}else for(const auto& d:valid.Diagnostics())diag::Emit(d);}
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("UI Scene Animation")) {
                ImGui::Checkbox("Preview in canvas", &m_previewAnims); ImGui::SameLine();
                if (ImGui::SmallButton("Replay") && m_preview) m_preview->Reload();
                m_designer.RenderAnimation(); ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void EditorApp::RenderScripting() {
    if (ImGui::Begin("Scripting")) {
        m_scripts.Render();
    }
    ImGui::End();
}


void EditorApp::RenderProblems() {
    if (ImGui::Begin("Problems")) {
        if (ImGui::SmallButton("Refresh")) RefreshProblems();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear diagnostics")) diag::Global().Clear();
        const auto diagnostics = diag::Global().Snapshot();
        ImGui::SameLine();
        if (m_problems.empty() && diagnostics.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.5f, 1.0f), "No problems detected.");
        }
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "%zu problem(s)", m_problems.size() + diagnostics.size());
        }
        ImGui::Separator();
        if (ImGui::BeginChild("##problemlist")) {
            int idx = 0;
            const auto navigate = [&](const diag::Diagnostic& diagnostic) {
                if (diagnostic.source.path.empty()) return;
                std::filesystem::path path = diagnostic.source.path;
                if (path.extension() != ".pxscenario") return;
                if (path.is_absolute()) path = path.lexically_relative(m_project.Context().root);
                if (!path.has_parent_path()) path = std::filesystem::path("Content/Scenario") / path;
                SetWorkspace(EditorWorkspace::Story);
                if (NodeGraphEditor* document = OpenDocTab(path.generic_string());
                    document && !diagnostic.source.nodeId.empty()) {
                    document->FocusStatement(diagnostic.source.nodeId);
                }
            };
            const auto renderDiagnostic = [&](const diag::Diagnostic& diagnostic,
                                              const char* idPrefix) {
                const ImVec4 color = diagnostic.severity >= diag::Severity::Error ? ImVec4(1.0f,.35f,.38f,1) :
                                     diagnostic.severity == diag::Severity::Warning ? ImVec4(1.0f,.72f,.25f,1) : ImVec4(.55f,.72f,1,1);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                const std::string row = diagnostic.code + "  " + diagnostic.message +
                    (diagnostic.source.path.empty() ? "" : "  —  " + diagnostic.source.path);
                if (ImGui::Selectable((row + "##" + idPrefix + std::to_string(idx++)).c_str()))
                    navigate(diagnostic);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() &&
                    (!diagnostic.details.empty() || !diagnostic.quickFix.empty() ||
                     !diagnostic.source.nodeId.empty() || !diagnostic.source.property.empty())) {
                    ImGui::BeginTooltip();
                    if (!diagnostic.details.empty()) ImGui::TextWrapped("%s", diagnostic.details.c_str());
                    if (!diagnostic.source.nodeId.empty()) ImGui::TextDisabled("Node: %s", diagnostic.source.nodeId.c_str());
                    if (!diagnostic.source.property.empty()) ImGui::TextDisabled("Property: %s", diagnostic.source.property.c_str());
                    if (!diagnostic.quickFix.empty()) ImGui::TextColored({.5f,.8f,1,1}, "Fix: %s", diagnostic.quickFix.c_str());
                    ImGui::EndTooltip();
                }
            };
            for (const auto& diagnostic : m_problems) renderDiagnostic(diagnostic, "project-problem");
            for (const auto& diagnostic : diagnostics) renderDiagnostic(diagnostic, "diagnostic");
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::RenderTheme(){
    if(!ImGui::Begin("Theme")){ImGui::End();return;}
    const std::string selectedTheme=fs::path(m_selectedAsset).extension()==".pxtheme"?m_selectedAsset:std::string{};
    std::string requested=!selectedTheme.empty()?selectedTheme:m_themePath;
    if(requested.empty()&&m_project.Context().IsOpen())requested=m_project.Context().manifest.uiThemePath;
    const auto loadTheme=[&](const std::string& path){m_themePath=path;m_themeDocument.reset();m_themeData.reset();m_themeDirty=false;m_themeToken=m_themeStyle=m_themeAxis=-1;std::ifstream input(m_project.Context().root/path,std::ios::binary);std::ostringstream text;if(!input){diag::Emit({.severity=diag::Severity::Error,.code="PXEDTHEME4101",.category="Editor.Theme",.message="Theme file could not be opened",.details=path});return;}text<<input.rdbuf();auto document=resource::ParseTypedDocument(text.str(),path);if(!document||document.Value().type!="UITheme"){for(const auto& item:document.Diagnostics())diag::Emit(item);return;}const auto value=document.Value().properties.find("styleSystem");if(value==document.Value().properties.end()){diag::Emit({.severity=diag::Severity::Error,.code="PXEDTHEME4102",.category="Editor.Theme",.message="UITheme requires Style System 3",.details=path});return;}auto parsed=ui::ParseStyleTheme(value->second);if(!parsed){for(const auto& item:parsed.Diagnostics())diag::Emit(item);return;}m_themeDocument=document.TakeValue();m_themeData=parsed.TakeValue();};
    if(!requested.empty()&&requested!=m_themePath){if(!m_themeDirty)loadTheme(requested);else{ImGui::TextColored({1,.7f,.3f,1},"%s 尚未儲存；先儲存或捨棄才能切換。",m_themePath.c_str());if(ImGui::Button("捨棄並切換"))loadTheme(requested);}}
    if(!m_themeData||!m_themeDocument){ImGui::TextDisabled("選取 .pxtheme 資源以開啟外部 Theme 文件編輯器。");ImGui::SeparatorText("Scene local overrides");m_designer.RenderTheme();ImGui::End();return;}
    ImGui::Text("%s%s",m_themePath.c_str(),m_themeDirty?"  ●":"");ImGui::SameLine();
    if(ImGui::Button("儲存 Theme")){const ui::StylePropertyRegistry registry;const Status valid=ui::StyleResolver{}.ValidateTheme(*m_themeData,registry);if(valid){m_themeDocument->properties["styleSystem"]=ui::WriteStyleTheme(*m_themeData);const Status written=io::AtomicFile::WriteText(m_project.Context().root/m_themePath,resource::WriteTypedDocument(*m_themeDocument));if(written){m_themeDirty=false;(void)EnsureAssetIdentity(m_project.Context().root/m_themePath);m_assets.Scan(m_project.Context());if(m_preview)m_preview->Reload();}else for(const auto& item:written.Diagnostics())diag::Emit(item);}else for(const auto& item:valid.Diagnostics())diag::Emit(item);}
    ImGui::SameLine();if(ImGui::Button("重新載入")){if(!m_themeDirty)loadTheme(m_themePath);else ImGui::OpenPopup("Theme 尚未儲存");}
    if(ImGui::BeginPopupModal("Theme 尚未儲存",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::Text("重新載入會捨棄目前 Theme 變更。");if(ImGui::Button("捨棄並重新載入")){const auto path=m_themePath;loadTheme(path);ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("取消"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
    auto& theme=*m_themeData;const ui::StylePropertyRegistry propertyRegistry;
    const auto defaultValue=[](const VariantType type)->Variant{switch(type){case VariantType::Bool:return false;case VariantType::Integer:return std::int64_t{0};case VariantType::Number:return 0.0;case VariantType::String:return std::string{};case VariantType::Vec2:return Vec2{};case VariantType::Color:return Color{255,255,255,255};default:return Variant{};}};
    const auto editVariant=[](const char* label,Variant& value)->bool{if(auto* boolean=value.TryGet<bool>())return ImGui::Checkbox(label,boolean);if(auto* integer=value.TryGet<std::int64_t>())return ImGui::DragScalar(label,ImGuiDataType_S64,integer);if(auto* number=value.TryGet<double>())return ImGui::DragScalar(label,ImGuiDataType_Double,number);if(auto* text=value.TryGet<std::string>())return ImGui::InputText(label,text);if(auto* vector=value.TryGet<Vec2>()){float values[2]{vector->x,vector->y};if(ImGui::DragFloat2(label,values)){*vector={values[0],values[1]};return true;}return false;}if(auto* color=value.TryGet<Color>()){float values[4]{color->r/255.f,color->g/255.f,color->b/255.f,color->a/255.f};if(ImGui::ColorEdit4(label,values)){*color={static_cast<std::uint8_t>(values[0]*255),static_cast<std::uint8_t>(values[1]*255),static_cast<std::uint8_t>(values[2]*255),static_cast<std::uint8_t>(values[3]*255)};return true;}return false;}ImGui::TextDisabled("%s：無可用編輯器",label);return false;};
    const auto editStyleValue=[&](const char* label,ui::StyleValue& styleValue,const VariantType expected)->bool{bool changed=false;int mode=styleValue.IsTokenReference()?1:0;ImGui::SetNextItemWidth(92);if(ImGui::Combo((std::string("##mode-")+label).c_str(),&mode,"Literal\0Token\0")){if(mode==0)styleValue=ui::StyleValue::Literal(defaultValue(expected));else{const auto found=std::find_if(theme.tokens.begin(),theme.tokens.end(),[&](const auto& token){return token.type==expected;});if(found!=theme.tokens.end())styleValue=ui::StyleValue::Token(found->id,found->displayName);}changed=true;}ImGui::SameLine();if(styleValue.IsTokenReference()){const auto* token=theme.FindToken(styleValue.TokenReference());const char* preview=token?token->displayName.c_str():"Missing Token";ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo(label,preview)){for(const auto& candidate:theme.tokens)if(candidate.type==expected&&ImGui::Selectable((candidate.displayName+"##"+candidate.id.ToString()).c_str(),candidate.id==styleValue.TokenReference())){styleValue=ui::StyleValue::Token(candidate.id,candidate.displayName);changed=true;}ImGui::EndCombo();}}else{Variant literal=styleValue.IsLiteral()?styleValue.LiteralValue().Clone():defaultValue(expected);if(editVariant(label,literal)){styleValue=ui::StyleValue::Literal(std::move(literal));changed=true;}}return changed;};
    const auto renderProperties=[&](ui::StylePropertyMap& properties,const std::string& scope)->bool{bool changed=false;std::string remove;for(auto& [id,value]:properties){ImGui::PushID((scope+id).c_str());const auto* descriptor=propertyRegistry.Find(id);if(descriptor){if(editStyleValue(descriptor->displayName.c_str(),value,descriptor->valueType))changed=true;}else ImGui::TextColored({1,.4f,.35f,1},"Unknown property: %s",id.c_str());ImGui::SameLine();if(ImGui::SmallButton("×"))remove=id;ImGui::PopID();}if(!remove.empty()){properties.erase(remove);changed=true;}if(ImGui::BeginCombo(("新增屬性##"+scope).c_str(),"選擇 Style Property")){for(const auto* descriptor:propertyRegistry.Descriptors())if(!properties.contains(descriptor->id)&&ImGui::Selectable((descriptor->category+" / "+descriptor->displayName+"##"+descriptor->id).c_str())){properties[descriptor->id]=ui::StyleValue::Literal(defaultValue(descriptor->valueType));changed=true;}ImGui::EndCombo();}return changed;};
    if(ImGui::BeginTabBar("##external-theme-tabs")){
        if(ImGui::BeginTabItem("Tokens")){if(ImGui::BeginChild("##token-list",{220,0},ImGuiChildFlags_Borders)){for(int index=0;index<static_cast<int>(theme.tokens.size());++index)if(ImGui::Selectable((theme.tokens[static_cast<std::size_t>(index)].displayName+"##token").c_str(),m_themeToken==index))m_themeToken=index;if(ImGui::Button("＋ Token")){ui::TokenDefinition token{.id=Uuid::Random(),.displayName="New Token",.type=VariantType::Color,.value=ui::StyleValue::Literal(Color{255,255,255,255})};theme.tokens.push_back(std::move(token));m_themeToken=static_cast<int>(theme.tokens.size())-1;m_themeDirty=true;}}ImGui::EndChild();ImGui::SameLine();if(ImGui::BeginChild("##token-editor",{0,0},ImGuiChildFlags_Borders)){if(m_themeToken>=0&&m_themeToken<static_cast<int>(theme.tokens.size())){auto& token=theme.tokens[static_cast<std::size_t>(m_themeToken)];m_themeDirty|=ImGui::InputText("名稱",&token.displayName);const char* types[]{"Bool","Integer","Number","String","Color"};const VariantType values[]{VariantType::Bool,VariantType::Integer,VariantType::Number,VariantType::String,VariantType::Color};int selected=0;for(int index=0;index<5;++index)if(token.type==values[index])selected=index;if(ImGui::Combo("型別",&selected,types,5)){token.type=values[selected];token.value=ui::StyleValue::Literal(defaultValue(token.type));m_themeDirty=true;}m_themeDirty|=editStyleValue("值",token.value,token.type);if(ImGui::Button("刪除 Token")){const Status removed=theme.RemoveToken(token.id);if(removed){m_themeToken=-1;m_themeDirty=true;}else for(const auto& item:removed.Diagnostics())diag::Emit(item);}}}ImGui::EndChild();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Styles / States")){if(ImGui::BeginChild("##style-list",{230,0},ImGuiChildFlags_Borders)){for(int index=0;index<static_cast<int>(theme.styles.size());++index)if(ImGui::Selectable((theme.styles[static_cast<std::size_t>(index)].displayName+"##style").c_str(),m_themeStyle==index))m_themeStyle=index;if(ImGui::Button("＋ Style")){ui::StyleDefinition style;style.id=Uuid::Random();style.displayName="New Style";style.category="Custom";theme.styles.push_back(std::move(style));m_themeStyle=static_cast<int>(theme.styles.size())-1;m_themeDirty=true;}}ImGui::EndChild();ImGui::SameLine();if(ImGui::BeginChild("##style-editor",{0,0},ImGuiChildFlags_Borders)){if(m_themeStyle>=0&&m_themeStyle<static_cast<int>(theme.styles.size())){auto& style=theme.styles[static_cast<std::size_t>(m_themeStyle)];m_themeDirty|=ImGui::InputText("名稱",&style.displayName);m_themeDirty|=ImGui::InputText("分類",&style.category);ImGui::SeparatorText("Normal");m_themeDirty|=renderProperties(style.properties,"style-normal");for(const auto [state,name]:std::array<std::pair<ui::StyleStateMask,const char*>,5>{{{ui::StateMask(ui::StyleState::Hover),"Hover"},{ui::StateMask(ui::StyleState::Pressed),"Pressed"},{ui::StateMask(ui::StyleState::Focused),"Focused"},{ui::StateMask(ui::StyleState::Checked),"Checked"},{ui::StateMask(ui::StyleState::Disabled),"Disabled"}}}){auto found=style.stateOverrides.find(state);if(found!=style.stateOverrides.end()){if(ImGui::TreeNode(name)){m_themeDirty|=renderProperties(found->second,"state-"+std::to_string(state));if(ImGui::Button((std::string("刪除 ")+name).c_str())){style.stateOverrides.erase(found);m_themeDirty=true;}ImGui::TreePop();}}else if(ImGui::SmallButton((std::string("＋ ")+name).c_str())){style.stateOverrides[state]={};m_themeDirty=true;}ImGui::SameLine();}ImGui::NewLine();if(ImGui::Button("刪除 Style")){const Status removed=theme.RemoveStyle(style.id);if(removed){m_themeStyle=-1;m_themeDirty=true;}else for(const auto& item:removed.Diagnostics())diag::Emit(item);}}}ImGui::EndChild();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Variants")){for(int axisIndex=0;axisIndex<static_cast<int>(theme.variantAxes.size());++axisIndex){auto& axis=theme.variantAxes[static_cast<std::size_t>(axisIndex)];ImGui::PushID(axisIndex);if(ImGui::TreeNodeEx(axis.displayName.c_str(),axisIndex==m_themeAxis?ImGuiTreeNodeFlags_DefaultOpen:0)){m_themeAxis=axisIndex;m_themeDirty|=ImGui::InputText("Axis 名稱",&axis.displayName);for(auto& value:axis.values){ImGui::PushID(value.id.ToString().c_str());if(ImGui::TreeNode(value.displayName.c_str())){m_themeDirty|=ImGui::InputText("Value 名稱",&value.displayName);m_themeDirty|=renderProperties(value.properties,"variant-"+value.id.ToString());ImGui::TreePop();}ImGui::PopID();}if(ImGui::Button("＋ Variant Value")){ui::VariantValueDefinition value;value.id=Uuid::Random();value.displayName="New Value";axis.values.push_back(value);if(axis.defaultValue.Empty())axis.defaultValue=value.id;m_themeDirty=true;}ImGui::TreePop();}ImGui::PopID();}if(ImGui::Button("＋ Variant Axis")){ui::VariantAxisDefinition axis;axis.id=Uuid::Random();axis.displayName="New Axis";ui::VariantValueDefinition value;value.id=Uuid::Random();value.displayName="Default";axis.defaultValue=value.id;axis.values.push_back(value);theme.variantAxes.push_back(std::move(axis));m_themeDirty=true;}ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Resolved Trace")){ImGui::InputText("Control type",&m_themeTraceControl);const char* preview=m_themeTraceStyle>=0&&m_themeTraceStyle<static_cast<int>(theme.styles.size())?theme.styles[static_cast<std::size_t>(m_themeTraceStyle)].displayName.c_str():"(none)";if(ImGui::BeginCombo("Base style",preview)){if(ImGui::Selectable("(none)",m_themeTraceStyle<0))m_themeTraceStyle=-1;for(int index=0;index<static_cast<int>(theme.styles.size());++index)if(ImGui::Selectable(theme.styles[static_cast<std::size_t>(index)].displayName.c_str(),m_themeTraceStyle==index))m_themeTraceStyle=index;ImGui::EndCombo();}ui::StyleResolveRequest request{.controlType=m_themeTraceControl};if(m_themeTraceStyle>=0&&m_themeTraceStyle<static_cast<int>(theme.styles.size()))request.binding.baseStyle=theme.styles[static_cast<std::size_t>(m_themeTraceStyle)].id;const auto resolved=ui::StyleResolver{}.Resolve(theme,request,propertyRegistry);if(resolved&&ImGui::BeginTable("##trace",4,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)){ImGui::TableSetupColumn("Property");ImGui::TableSetupColumn("Resolved source");ImGui::TableSetupColumn("Overrides");ImGui::TableSetupColumn("Token chain");ImGui::TableHeadersRow();for(const auto& [id,value]:resolved.Value().properties){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(id.c_str());ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(value.source.label.c_str());ImGui::TableSetColumnIndex(2);ImGui::Text("%zu",value.overriddenSources.size());ImGui::TableSetColumnIndex(3);ImGui::Text("%zu",value.tokenChain.size());}ImGui::EndTable();}else if(!resolved)for(const auto& item:resolved.Diagnostics())ImGui::TextColored({1,.4f,.35f,1},"%s %s",item.code.c_str(),item.message.c_str());ImGui::EndTabItem();}
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void EditorApp::RenderStoryLibrary() {
    if (ImGui::Begin("Story Library")) m_storyLibrary.Render();
    ImGui::End();
}

void EditorApp::RenderStoryPreview() {
    if (!ImGui::Begin("Story Preview")) { ImGui::End(); return; }
    NodeGraphEditor* document = ActiveDocPtr();
    if (!m_preview || !document) {
        ImGui::TextDisabled("開啟一個 Scenario 後即可直接預覽。");
        ImGui::End(); return;
    }
    if (m_storyPreviewCatalogRevision != m_storyLibrary.Revision()) {
        m_preview->SetGameCatalog(m_storyLibrary.Catalog());
        m_storyPreviewCatalogRevision = m_storyLibrary.Revision();
    }
    const bool pathChanged = m_storyPreviewPath != document->CurrentRuntimePath();
    const bool contentChanged = pathChanged || m_storyPreviewGraphRevision != document->Revision();
    const auto runDocument = [&] {
        const std::string compiled = document->Compile();
        if (compiled.empty()) {
            Log("Story Preview: Scenario 驗證失敗，請先修正 Problems。");
            return false;
        }
        m_preview->SetGameCatalog(m_storyLibrary.Catalog());
        if (!m_preview->LoadVnText(compiled, document->CurrentRuntimePath())) return false;
        m_storyPreviewText = compiled;
        m_storyPreviewPath = document->CurrentRuntimePath();
        m_storyPreviewGraphRevision = document->Revision();
        return true;
    };
    if (ImGui::Button(m_storyPreviewText.empty() || pathChanged ? "▶ 執行目前 Scenario" : "↻ 重新開始"))
        runDocument();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_storyPreviewText.empty() || pathChanged);
    if (ImGui::Button("下一句")) m_preview->VMRef().OnAdvance();
    ImGui::SameLine();
    if (ImGui::Button("Step")) m_preview->VMRef().DebugStep();
    ImGui::SameLine();
    if (ImGui::Button("Continue")) m_preview->VMRef().DebugContinue();
    ImGui::EndDisabled();
    if (contentChanged && !m_storyPreviewText.empty() && !pathChanged) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f,.72f,.25f,1.0f}, "內容已更新");
    }
    const auto stateName=[](vn::VMState state){switch(state){case vn::VMState::Idle:return "Idle";case vn::VMState::Running:return "Running";case vn::VMState::WaitingClick:return "Waiting Click";case vn::VMState::WaitingChoice:return "Waiting Choice";case vn::VMState::WaitingTimer:return "Waiting Timer";case vn::VMState::WaitingVideo:return "Waiting Video";case vn::VMState::WaitingExternal:return "Waiting External";case vn::VMState::Paused:return "Paused";case vn::VMState::Finished:return "Finished";}return "Unknown";};
    ImGui::SameLine(); ImGui::TextDisabled("%s · PC %d", stateName(m_preview->VMRef().State()),
                                           m_preview->VMRef().ProgramCounter());
    ImGui::Separator();
    if (m_storyPreviewText.empty() || pathChanged) {
        ImGui::TextWrapped("預覽會直接編譯目前尚未儲存的節點內容，不會覆寫 Scenario 檔案。");
        ImGui::End(); return;
    }
    const float logicalWidth=static_cast<float>(m_preview->Width());
    const float logicalHeight=static_cast<float>(m_preview->Height());
    const ImVec2 available=ImGui::GetContentRegionAvail();
    const float scale=std::max(.05f,std::min(available.x/logicalWidth,
        std::max(80.0f,available.y)/logicalHeight));
    const ImVec2 display{logicalWidth*scale,logicalHeight*scale};
    const ImVec2 topLeft=ImGui::GetCursorScreenPos();
    const ImVec2 mouse=ImGui::GetMousePos();
    const bool hovered=ImGui::IsMouseHoveringRect(topLeft,{topLeft.x+display.x,topLeft.y+display.y});
    const bool click=hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    m_preview->SetDisplayScale(scale);
    m_preview->Tick(ImGui::GetIO().DeltaTime,SDL_GetTicks(),hovered,
        hovered?(mouse.x-topLeft.x)/scale:-1.0f,
        hovered?(mouse.y-topLeft.y)/scale:-1.0f,click);
    if (m_preview->Target()) ImGui::Image(reinterpret_cast<ImTextureID>(m_preview->Target()),display);
    ImGui::End();
}

void EditorApp::RenderRecoveryCenter() {
    if (!m_showRecoveryCenter) return;
    if (!ImGui::Begin("Recovery Center", &m_showRecoveryCenter)) { ImGui::End(); return; }
    ImGui::TextWrapped("Recovery snapshots never overwrite a source file. Restore creates a new .recovered copy so you can compare it before saving.");
    ImGui::Separator();
    auto snapshots = m_recovery.ListSnapshots();
    if (!snapshots) {
        for (const auto& diagnostic : snapshots.Diagnostics()) diag::Emit(diagnostic);
        ImGui::TextColored({1,.35f,.35f,1}, "Recovery storage could not be read. See Problems.");
        ImGui::End(); return;
    }
    if (snapshots.Value().empty()) ImGui::TextDisabled("No recovery snapshots for this project.");
    for (std::size_t i = 0; i < snapshots.Value().size(); ++i) {
        const RecoverySnapshot& snapshot = snapshots.Value()[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%s", snapshot.sourcePath.filename().string().c_str());
        ImGui::SameLine(); ImGui::TextDisabled("%zu bytes • %llu", snapshot.contentSize,
                                               static_cast<unsigned long long>(snapshot.timestamp));
        if (ImGui::Button("Restore as copy")) {
            auto content = m_recovery.LoadContent(snapshot);
            if (!content) {
                for (const auto& diagnostic : content.Diagnostics()) diag::Emit(diagnostic);
            } else {
                const auto restored = snapshot.sourcePath.parent_path() /
                    (snapshot.sourcePath.stem().string() + ".recovered-" + std::to_string(snapshot.timestamp) + snapshot.sourcePath.extension().string());
                const Status written = io::AtomicFile::WriteText(restored, content.Value());
                if (!written) for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
                else { const Status activated=ActivateUIDocument(restored);if(!activated)for(const auto& diagnostic:activated.Diagnostics())diag::Emit(diagnostic);else Log("Recovered UI document as " + restored.string()); }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            const Status discarded = m_recovery.Discard(snapshot);
            if (!discarded) for (const auto& diagnostic : discarded.Diagnostics()) diag::Emit(diagnostic);
        }
        ImGui::Separator(); ImGui::PopID();
    }
    ImGui::End();
}

void EditorApp::RenderAssetIdentityResolver() {
    if (!m_showAssetIdentity) return;
    if (!ImGui::Begin("Asset Identity Resolver", &m_showAssetIdentity)) { ImGui::End(); return; }
    const bool valid = m_assetRegistry.Valid();
    ImGui::TextColored(valid ? ImVec4(.4f,.85f,.55f,1) : ImVec4(1,.35f,.35f,1),
                       valid ? "Asset identities are valid; Build is allowed." : "Build is blocked until every identity conflict is resolved.");
    ImGui::TextWrapped("GUIDs are never regenerated silently. Choose exactly which copy receives a new identity after a branch merge or duplicated file.");
    if (ImGui::Button("Rescan Content")) m_assetRegistry.Scan(m_project.Context().root);
    ImGui::Separator();

    std::unordered_map<Uuid, std::vector<const resource::AssetEntry*>, UuidHash> groups;
    for (const auto& entry : m_assetRegistry.Entries()) groups[entry.id].push_back(&entry);
    bool foundDuplicate = false;
    for (const auto& [id, entries] : groups) {
        if (entries.size() < 2) continue; foundDuplicate = true;
        ImGui::PushID(id.ToString().c_str());
        ImGui::TextColored({1,.5f,.3f,1}, "Duplicate GUID %s", id.ToString().c_str());
        for (std::size_t i=0;i<entries.size();++i) {
            ImGui::BulletText("%s", entries[i]->sourcePath.string().c_str()); ImGui::SameLine();
            if (ImGui::SmallButton(("Assign new GUID##"+std::to_string(i)).c_str())) {
                auto reassigned=m_assetRegistry.ReassignIdentity(entries[i]->sourcePath);
                if(!reassigned)for(const auto& diagnostic:reassigned.Diagnostics())diag::Emit(diagnostic);
                m_assetRegistry.Scan(m_project.Context().root); break;
            }
        }
        ImGui::Separator(); ImGui::PopID();
    }
    for (const auto& diagnostic : m_assetRegistry.Diagnostics()) {
        if (diagnostic.code != "PXASSET-E1002") continue;
        ImGui::TextWrapped("Missing identity: %s", diagnostic.source.path.c_str()); ImGui::SameLine();
        ImGui::PushID(diagnostic.source.path.c_str());
        if (ImGui::SmallButton("Register asset")) {
            auto registered=m_assetRegistry.RegisterAsset(m_project.Context().root,diagnostic.source.path);
            if(!registered)for(const auto& d:registered.Diagnostics())diag::Emit(d);
            m_assetRegistry.Scan(m_project.Context().root);
        }
        ImGui::PopID();
    }
    if (!foundDuplicate && valid) ImGui::TextDisabled("No duplicate GUIDs.");
    ImGui::End();
}

void EditorApp::LocScanScripts() {
    if (!m_project.Context().IsOpen()) {
        return;
    }
    // Keep existing translations while re-collecting source lines.
    std::unordered_map<std::string, std::string> existing;
    for (const auto& entry : m_locEntries) {
        if (!entry.translation.empty()) existing[entry.id] = entry.translation;
    }
    m_locEntries.clear();
    std::unordered_set<std::string> seen;
    const auto add = [&](const std::string& id,const std::string& source) {
        if (id.empty() || !seen.insert(id).second) {
            return;
        }
        const auto it = existing.find(id);
        m_locEntries.push_back({id,source,it != existing.end() ? it->second : std::string{}});
    };

    const fs::path scriptDir = m_project.Context().root / "Content" / "Scenario";
    std::error_code ec;
    for (fs::directory_iterator it(scriptDir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".pxscenario") continue;
        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        auto parsed = px::vn::scenario::ParseScenario(ss.str(), it->path().generic_string());
        if (!parsed) continue;
        for (const auto& node : parsed.Value().nodes) {
            const char* field = node.command == "choice" ? "text" :
                                (node.command == "say" || node.command == "text") ? "value" : nullptr;
            if (!field) continue;
            const auto found = node.parameters.find(field),id=node.parameters.find("textId");
            if (found != node.parameters.end()&&id!=node.parameters.end()) if (const auto* text = found->second.TryGet<std::string>()) if(const auto* textId=id->second.TryGet<std::string>()) add(*textId,*text);
        }
    }
    Log("Localization: scanned " + std::to_string(m_locEntries.size()) + " source line(s).");
}

void EditorApp::LocLoad() {
    if (!m_project.Context().IsOpen()) {
        return;
    }
    const fs::path path =
        m_project.Context().root / "Content" / "Localization" /
        (std::string(m_locLang) + ".json");
    std::unordered_map<std::string, std::string> table;
    if (std::ifstream in(path, std::ios::binary); in) {
        const Json j = Json::parse(in, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_object()&&it.value().contains("translation")&&it.value()["translation"].is_string()) table[it.key()] = it.value()["translation"].get<std::string>();
            }
        }
    }
    LocScanScripts();
    std::unordered_set<std::string> seen;
    for (auto& entry : m_locEntries) {
        seen.insert(entry.id);
        if (const auto it = table.find(entry.id); it != table.end()) {
            entry.translation = it->second;
        }
    }
    // Keep stale entries from the file (sources no longer in any script).
    for (const auto& [id, translation] : table) {
        if (seen.insert(id).second) {
            m_locEntries.push_back({id,"(source removed)",translation});
        }
    }
    m_locDirty = false;
    Log("Localization: loaded " + std::to_string(table.size()) + " translation(s) for '" +
        m_locLang + "'.");
}

void EditorApp::LocSave() {
    if (!m_project.Context().IsOpen()) {
        return;
    }
    Json j = Json::object();
    int count = 0;
    for (const auto& entry : m_locEntries) {
        if (!entry.translation.empty()) {
            j[entry.id] = Json{{"source",entry.source},{"translation",entry.translation}};
            ++count;
        }
    }
    const fs::path dir = m_project.Context().root / "Content" / "Localization";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (std::string(m_locLang) + ".json");
    const Status written = io::AtomicFile::WriteText(path, j.dump(2));
    if (!written) {
        for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
        return;
    }
    if (!fs::exists(resource::AssetRegistry::MetaPath(path))) {
        auto registered =
            m_assetRegistry.RegisterAsset(m_project.Context().root, path, "localization");
        if (!registered) {
            for (const auto& diagnostic : registered.Diagnostics()) diag::Emit(diagnostic);
        }
    }
    m_locDirty = false;
    m_assets.Scan(m_project.Context());
    Log("Localization: saved " + std::to_string(count) + " translation(s) to Content/Localization/" +
        m_locLang + ".json");
}

void EditorApp::LocExportCsv() {
    if (!m_project.Context().IsOpen()) {
        return;
    }
    const auto quote = [](const std::string& value) {
        std::string out = "\"";
        for (char c : value) {
            if (c == '"') out += "\"\"";
            else out.push_back(c);
        }
        out += "\"";
        return out;
    };
    const fs::path dir = m_project.Context().root / "Content" / "Localization";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (std::string(m_locLang) + ".csv");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "\xEF\xBB\xBF";  // UTF-8 BOM so spreadsheet apps decode correctly
    out << "text_id,source,translation\n";
    for (const auto& entry : m_locEntries) {
        out << quote(entry.id) << ',' << quote(entry.source) << ',' << quote(entry.translation) << '\n';
    }
    Log("Localization: exported " + path.generic_string());
}

void EditorApp::RenderProjectTrash(){
    if(!m_showProjectTrash)return;
    if(ImGui::Begin("專案垃圾桶",&m_showProjectTrash,ImGuiWindowFlags_AlwaysAutoResize)){
        const auto trash=m_project.Context().root/".prismatix"/"Trash";std::error_code error;
        std::vector<fs::path> transactions;
        if(fs::exists(trash,error))for(fs::directory_iterator iterator(trash,error),end;iterator!=end&&!error;iterator.increment(error))if(iterator->is_directory(error))transactions.push_back(iterator->path());
        ImGui::Text("%zu 筆可復原的刪除交易",transactions.size());
        ImGui::TextDisabled("一般刪除可用專案 Undo 還原；清空後無法復原。");
        ImGui::Separator();
        for(const auto& transaction:transactions){ImGui::BulletText("%s",transaction.filename().string().c_str());const auto manifest=transaction/"TrashRecord.pxres";if(fs::exists(manifest))ImGui::SameLine(),ImGui::TextDisabled("含 reference snapshot");}
        ImGui::BeginDisabled(transactions.empty());
        if(ImGui::Button("清空垃圾桶…"))ImGui::OpenPopup("永久清空垃圾桶");
        ImGui::EndDisabled();
        if(ImGui::BeginPopupModal("永久清空垃圾桶",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextColored(ImVec4(1,.38f,.35f,1),"這會永久刪除所有垃圾桶內容，且無法 Undo。");
            if(ImGui::Button("永久刪除",ImVec2(130,0))){fs::remove_all(trash,error);if(error){diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXASSETTRASH9303",.category="Editor.Trash",.message="無法清空專案垃圾桶",.details=error.message()};diagnostic.source.path=trash.generic_string();diag::Emit(std::move(diagnostic));}else m_projectHistory.Clear();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("取消",ImVec2(100,0)))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void EditorApp::RenderLocalization() {
    if(!m_showLocalizationWindow)return;
    if (ImGui::Begin("Localization",&m_showLocalizationWindow)) {
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##loclang", "lang", m_locLang, sizeof(m_locLang));
        ImGui::SameLine();
        if (ImGui::SmallButton("Load")) LocLoad();
        ImGui::SameLine();
        if (ImGui::SmallButton("Scan Scripts")) {
            LocScanScripts();
            m_locDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(m_locDirty ? "Save*" : "Save")) LocSave();
        ImGui::SameLine();
        if (ImGui::SmallButton("Export CSV")) LocExportCsv();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##locfilter", "filter...", m_locFilter, sizeof(m_locFilter));
        ImGui::SameLine();
        int translated = 0;
        for (const auto& entry : m_locEntries) {
            if (!entry.translation.empty()) ++translated;
        }
        ImGui::TextDisabled("%d / %d translated", translated,
                            static_cast<int>(m_locEntries.size()));
        ImGui::Separator();

        if (m_locEntries.empty()) {
            ImGui::TextDisabled("Press \"Load\" (or \"Scan Scripts\") to collect dialogue lines. "
                                "The player reads Content/Localization/<language>.json at startup.");
        } else if (ImGui::BeginTable("##loctable", 2,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Translation");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            const std::string filter = m_locFilter;
            for (int i = 0; i < static_cast<int>(m_locEntries.size()); ++i) {
                auto& entry = m_locEntries[static_cast<std::size_t>(i)];
                if (!filter.empty() && entry.source.find(filter) == std::string::npos && entry.id.find(filter)==std::string::npos&&
                    entry.translation.find(filter) == std::string::npos) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                ImGui::TextDisabled("%s",entry.id.c_str());ImGui::TextWrapped("%s", entry.source.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##tr", &entry.translation)) {
                    m_locDirty = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorApp::RenderFlow() {
    if (ImGui::Begin("Flow")) {
        if (m_flowStale && !ImGui::IsAnyItemActive() && m_project.Context().IsOpen()) {
            m_flow.SetEntryScript(m_project.Context().manifest.startScript);
            m_flow.Rebuild(ScriptFileNames(), m_project.Context().root);
            m_flowStale = false;
        }
        if (ImGui::SmallButton("Rebuild") && m_project.Context().IsOpen()) {
            m_flow.SetEntryScript(m_project.Context().manifest.startScript);
            m_flow.Rebuild(ScriptFileNames(), m_project.Context().root);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Run")) {
            RunDev();
        }
        ImGui::SameLine();
        const std::string& entry = m_project.Context().manifest.startScript;
        ImGui::TextDisabled("Entry: %s — drag from START or right-click a chapter to change it.",
                            entry.empty() ? "(none)" : entry.c_str());
        ImGui::Separator();
        m_flow.SetAvailableScripts(ScriptFileNames());
        m_flow.Render();
    }
    ImGui::End();
}

void EditorApp::RenderBuild() {
    if(!m_showBuildWindow)return;
    if (ImGui::Begin("Build",&m_showBuildWindow)) {
        ProjectManifest& m = m_project.Context().manifest;

        ImGui::SeparatorText("Project");
        bool manifestChanged = false;
        bool entryChanged = false;
        ImGui::SetNextItemWidth(260);
        manifestChanged |= ImGui::InputText("Title", &m.name, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(120);
        manifestChanged |= ImGui::InputInt("Width", &m.gameWidth, 0, 0,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        manifestChanged |= ImGui::InputInt("Height", &m.gameHeight, 0, 0,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Start Route", m.startRoute.c_str())) {
            for (const auto& route : m.routes) {
                if (ImGui::Selectable(route.id.c_str(), route.id == m.startRoute)) {
                    m.startRoute = route.id;
                    manifestChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Entry Script", m.startScript.c_str())) {
            for (const AssetRecord& a : m_assets.Filter("", "script")) {
                if (ImGui::Selectable(a.runtimePath.c_str(), a.runtimePath == m.startScript)) {
                    m.startScript = a.runtimePath;
                    manifestChanged = true;
                    entryChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(also set from the Flow panel's START node)");
        if (manifestChanged) {
            m_project.SaveManifest();
        }
        if (entryChanged) {
            m_flow.SetEntryScript(m.startScript);
            m_flow.Rebuild(ScriptFileNames(), m_project.Context().root);
        }

        ImGui::SeparatorText("Play");
        if (ImGui::Button("Run (Dev)", ImVec2(150, 34))) {
            RunDev();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Saves all, then runs the player from the project folder.");

        ImGui::SeparatorText("Build profile");
        const char* profiles[] = { "Debug (unencrypted)", "Release (encrypted)" };
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("##profile", &m_buildProfile, profiles, 2)) {
            m.encrypt = (m_buildProfile == 1);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Encrypt", &m.encrypt);
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Key", &m.encryptKey);

        ImGui::SeparatorText("Publish");
        if (ImGui::Button("Build", ImVec2(110, 34))) {
            RunBuild();
        }
        ImGui::SameLine();
        if (ImGui::Button("Build & Run", ImVec2(130, 34))) {
            RunBuild();
            RunPackaged();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Output", ImVec2(130, 34))) {
            std::error_code ec;
            std::filesystem::create_directories(m_project.Context().ExportRoot(), ec);
            OpenInExplorer(m_project.Context().ExportRoot());
        }
        ImGui::TextDisabled("-> %s", m_project.Context().ExportRoot().string().c_str());
        ImGui::TextWrapped(
            "Packs Content/ into Content.pdx after validating every .pxmeta identity, bundles "
            "the player + DLLs, and writes game.pxpackage. White-box key (deters casual "
            "extraction)."
        );
    }
    ImGui::End();
}


}  // namespace px::editor
