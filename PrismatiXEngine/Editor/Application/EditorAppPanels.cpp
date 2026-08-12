#include "Editor/Application/EditorApp.h"

#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/IO/AtomicFile.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/Styles/StyleResolver.h"
#include "Engine/UI/Animation.h"

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

bool SegmentedButton(const char* label, bool selected, const ImVec2& size = {0, 0}) {
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    const bool pressed = ImGui::Button(label, size);
    if (selected) ImGui::PopStyleColor();
    return pressed;
}

bool ToolbarButton(const char* label, const char* tooltip, bool selected = false,
                   bool enabled = true, const char* disabledReason = nullptr) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = SegmentedButton(label, selected);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        if (tooltip && *tooltip) ImGui::TextUnformatted(tooltip);
        if (!enabled && disabledReason && *disabledReason) {
            ImGui::Separator();
            ImGui::TextUnformatted(disabledReason);
        }
        ImGui::EndTooltip();
    }
    return enabled && pressed;
}

void StatusChip(const char* label, bool success) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          success ? ImVec4{0.35f, 0.80f, 0.50f, 1.0f}
                                  : ImVec4{0.90f, 0.35f, 0.35f, 1.0f});
    ImGui::Text("● %s", label);
    ImGui::PopStyleColor();
}

void EmptyState(const char* title, const char* description) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", title);
    ImGui::TextDisabled("%s", description);
}

}

void EditorApp::BuildDockLayout(unsigned int dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    if(m_workspace==EditorWorkspace::UI){
        ImGuiID center=dockspaceId;ImGuiID bottom=ImGui::DockBuilderSplitNode(center,ImGuiDir_Down,.24f,nullptr,&center);
        ImGui::DockBuilderDockWindow("UI Designer",center);ImGui::DockBuilderDockWindow("Problems",bottom);ImGui::DockBuilderDockWindow("Console",bottom);ImGui::DockBuilderFinish(dockspaceId);return;
    }

    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);

    if (m_workspace == EditorWorkspace::Story)
        ImGui::DockBuilderDockWindow("Story Library", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    switch (m_workspace) {
        case EditorWorkspace::UI: break;
        case EditorWorkspace::Story:
            ImGui::DockBuilderDockWindow("Narrative", center);
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
                                   "PrismatiX.Script.Dock"};
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
    RenderMenuBar();
    RenderCommandPalette();
    RenderShortcutsWindow();
    RenderQuickOpen();
    if(m_workspace!=EditorWorkspace::UI)RenderInspector();
    switch (m_workspace) {
        case EditorWorkspace::UI:
            RenderPreview();
            RenderProblems(); RenderConsole(); break;
        case EditorWorkspace::Story:
            RenderNarrative(); RenderStoryLibrary(); RenderProblems(); RenderConsole(); break;
        case EditorWorkspace::Script:
            RenderScripting(); RenderProblems(); RenderConsole(); break;
    }
    RenderAssetIdentityResolver();
    RenderBuild();
    RenderLocalization();
    RenderStatusBar();
    RenderDiagnosticToasts();

}

void EditorApp::SetWorkspace(EditorWorkspace workspace) {
    if (m_workspace == workspace) return;
    m_workspace = workspace;
}

void EditorApp::RenderWorkspaceSwitcher() {
    const char* labels[]{"介面", "劇情", "腳本"};
    const float totalWidth = 3.0f * 64.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 12.0f,
                                  ImGui::GetWindowWidth() * 0.5f - totalWidth * 0.5f));
    for (int index = 0; index < 3; ++index) {
        if (index) ImGui::SameLine();
        const bool active = static_cast<int>(m_workspace) == index;
        if (SegmentedButton(labels[index], active, ImVec2(64, 0)))
            SetWorkspace(static_cast<EditorWorkspace>(index));
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
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("執行")) {
        if (ImGui::MenuItem("執行專案", "F5")) RunDev();
        if (ImGui::MenuItem("停止", "F8", false, false)) {}
        if (ImGui::MenuItem("Build", "Ctrl+B")) RunBuild();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("工具")) {
        if(ImGui::MenuItem("Localization…"))m_showLocalizationWindow=true;
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
    if (ImGui::SmallButton("執行")) RunDev();
    ImGui::SameLine();ToolbarButton("停止","停止目前執行中的專案",false,false,"目前尚未連接可停止的執行程序");
    ImGui::SameLine();if(ToolbarButton("Build","建置專案 · Ctrl+B"))RunBuild();
    const auto diagnostics = diag::Global().Snapshot();
    const auto errors = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item) { return item.severity >= diag::Severity::Error; });
    ImGui::SameLine();
    if(errors)StatusChip(std::to_string(errors).c_str(),false);else StatusChip("正常",true);
    ImGui::EndMainMenuBar();
}

void EditorApp::RenderQuickOpen() {
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
        for (const resource::AssetEntry& asset : m_assetRegistry.Entries()) {
            const std::string type=ClassifyAsset(asset.sourcePath);
            const std::string runtimePath=AssetRuntimePath(asset);
            if (type != "ui" && type != "script" && type != "lua") continue;
            if (!filter.empty() && runtimePath.find(filter) == std::string::npos &&
                type.find(filter) == std::string::npos) continue;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AssetTypeColor(type)),
                               "%s", type.c_str());
            ImGui::SameLine();
            if (ImGui::Selectable(runtimePath.c_str())) {
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
        ImGui::TextDisabled("工作區"); ImGui::SameLine();
        const char* workspaceLabel=m_workspace==EditorWorkspace::UI?"UI":m_workspace==EditorWorkspace::Story?"Story":"Script";
        ImGui::TextUnformatted(workspaceLabel);
        const auto diagnostics = diag::Global().Snapshot();
        const auto errors = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item){ return item.severity >= diag::Severity::Error; });
        const auto warnings = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& item){ return item.severity == diag::Severity::Warning; });
        const std::string right = "錯誤 " + std::to_string(errors) + "  警告 " + std::to_string(warnings);
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

void EditorApp::RenderInspector() {
    if (ImGui::Begin("Inspector")) {
        ImGui::TextDisabled("UI, Narrative and Script authoring have moved to PrismatiXStudio.");
    }
    ImGui::End();
}

void EditorApp::OpenAssetByType(const resource::AssetEntry& entry) {
    const std::string runtimePath = AssetRuntimePath(entry);
    const std::string type = ClassifyAsset(entry.sourcePath);
    m_selectedAsset = runtimePath;
    if (type == "script") {
        SetWorkspace(EditorWorkspace::Story);
        Log("Narrative authoring has moved to PrismatiXStudio: " + runtimePath);
    } else if (type == "ui") {
        SetWorkspace(EditorWorkspace::UI);
        Log("UI authoring has moved to PrismatiXStudio: " + runtimePath);
    } else if (type == "lua") {
        SetWorkspace(EditorWorkspace::Script);
        Log("Script authoring has moved to PrismatiXStudio: " + runtimePath);
    } else {
        OpenInExplorer(entry.sourcePath);
    }
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
    if (ImGui::Begin("UI Designer")) {
        EmptyState("UI authoring moved",
                   "Use PrismatiXStudio for UI documents, hierarchy, canvas commands and History.");
    }
    ImGui::End();
}

void EditorApp::RenderNarrative() {
    if (ImGui::Begin("Narrative")) {
        ImGui::TextDisabled("Narrative authoring and debugging have moved to PrismatiXStudio.");
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
                        for(int i=0;i<static_cast<int>(presets.size());++i){const auto& preset=presets[static_cast<std::size_t>(i)];if(ImGui::Selectable((preset.name+"##preset-"+std::to_string(i)).c_str(),i==presetIndex))presetIndex=i;}
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button("Create Editable Copy")) {
                        auto clip=presets[static_cast<std::size_t>(presetIndex)];clip.id=Uuid::Random();
                        std::string file=clip.name;std::replace(file.begin(),file.end(),'/','-');
                        m_timelinePath="Content/Animations/"+file+".pxanim";
                        const Status written=io::AtomicFile::WriteText(m_project.Context().root/m_timelinePath,animation::WriteAnimationClip(clip));
                        if(written){m_timelineClip=std::move(clip);m_selectedAsset=m_timelinePath;auto registered=m_assetRegistry.RegisterAsset(m_project.Context().root,m_project.Context().root/m_timelinePath,"resource");if(!registered)for(const auto& d:registered.Diagnostics())diag::Emit(d);}
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
                ImGui::TextDisabled("UI animation authoring has moved to PrismatiXStudio.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void EditorApp::RenderScripting() {
    if (ImGui::Begin("Scripting")) {
        ImGui::TextDisabled("Lua document, typed Action and debugger workflows have moved to PrismatiXStudio.");
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
                Log("Narrative diagnostic ownership has moved to PrismatiXStudio: " +
                    path.generic_string());
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
    if(!m_themeData||!m_themeDocument){ImGui::TextDisabled("選取 .pxtheme 資源以開啟外部 Theme 文件編輯器。");ImGui::SeparatorText("Scene local overrides");ImGui::TextDisabled("Scene-local UI style authoring has moved to PrismatiXStudio.");ImGui::End();return;}
    ImGui::Text("%s%s",m_themePath.c_str(),m_themeDirty?"  ●":"");ImGui::SameLine();
    if(ImGui::Button("儲存 Theme")){const ui::StylePropertyRegistry registry;const Status valid=ui::StyleResolver{}.ValidateTheme(*m_themeData,registry);if(valid){m_themeDocument->properties["styleSystem"]=ui::WriteStyleTheme(*m_themeData);const Status written=io::AtomicFile::WriteText(m_project.Context().root/m_themePath,resource::WriteTypedDocument(*m_themeDocument));if(written){m_themeDirty=false;(void)EnsureAssetIdentity(m_project.Context().root/m_themePath);}else for(const auto& item:written.Diagnostics())diag::Emit(item);}else for(const auto& item:valid.Diagnostics())diag::Emit(item);}
    ImGui::SameLine();if(ImGui::Button("重新載入")){if(!m_themeDirty)loadTheme(m_themePath);else ImGui::OpenPopup("Theme 尚未儲存");}
    if(ImGui::BeginPopupModal("Theme 尚未儲存",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::Text("重新載入會捨棄目前 Theme 變更。");if(ImGui::Button("捨棄並重新載入")){const auto path=m_themePath;loadTheme(path);ImGui::CloseCurrentPopup();}ImGui::SameLine();if(ImGui::Button("取消"))ImGui::CloseCurrentPopup();ImGui::EndPopup();}
    auto& theme=*m_themeData;const ui::StylePropertyRegistry propertyRegistry;
    const auto defaultValue=[](const VariantType type)->Variant{switch(type){case VariantType::Bool:return false;case VariantType::Integer:return std::int64_t{0};case VariantType::Number:return 0.0;case VariantType::String:return std::string{};case VariantType::Vec2:return Vec2{};case VariantType::Color:return Color{255,255,255,255};default:return Variant{};}};
    const auto editVariant=[](const char* label,Variant& value)->bool{if(auto* boolean=value.TryGet<bool>())return ImGui::Checkbox(label,boolean);if(auto* integer=value.TryGet<std::int64_t>())return ImGui::DragScalar(label,ImGuiDataType_S64,integer);if(auto* number=value.TryGet<double>())return ImGui::DragScalar(label,ImGuiDataType_Double,number);if(auto* text=value.TryGet<std::string>())return ImGui::InputText(label,text);if(auto* vector=value.TryGet<Vec2>()){float values[2]{vector->x,vector->y};if(ImGui::DragFloat2(label,values)){*vector={values[0],values[1]};return true;}return false;}if(auto* color=value.TryGet<Color>()){float values[4]{color->r/255.f,color->g/255.f,color->b/255.f,color->a/255.f};if(ImGui::ColorEdit4(label,values)){*color={static_cast<std::uint8_t>(values[0]*255),static_cast<std::uint8_t>(values[1]*255),static_cast<std::uint8_t>(values[2]*255),static_cast<std::uint8_t>(values[3]*255)};return true;}return false;}ImGui::TextDisabled("%s：無可用編輯器",label);return false;};
    const auto editStyleValue=[&](const char* label,ui::StyleValue& styleValue,const VariantType expected)->bool{bool changed=false;int mode=styleValue.IsTokenReference()?1:0;ImGui::SetNextItemWidth(92);if(ImGui::Combo((std::string("##mode-")+label).c_str(),&mode,"Literal\0Token\0")){if(mode==0)styleValue=ui::StyleValue::Literal(defaultValue(expected));else{const auto found=std::find_if(theme.tokens.begin(),theme.tokens.end(),[&](const auto& token){return token.type==expected;});if(found!=theme.tokens.end())styleValue=ui::StyleValue::Token(found->id,found->displayName);}changed=true;}ImGui::SameLine();if(styleValue.IsTokenReference()){const auto* token=theme.FindToken(styleValue.TokenReference());const char* preview=token?token->displayName.c_str():"Missing Token";ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo(label,preview)){for(const auto& candidate:theme.tokens)if(candidate.type==expected&&ImGui::Selectable((candidate.displayName+"##"+candidate.id.ToString()).c_str(),candidate.id==styleValue.TokenReference())){styleValue=ui::StyleValue::Token(candidate.id,candidate.displayName);changed=true;}ImGui::EndCombo();}}else{Variant literal=styleValue.IsLiteral()?styleValue.LiteralValue().Clone():defaultValue(expected);if(editVariant(label,literal)){styleValue=ui::StyleValue::Literal(std::move(literal));changed=true;}}return changed;};
    const auto renderProperties=[&](ui::StylePropertyMap& properties,const std::string& scope)->bool{bool changed=false;std::string remove;for(auto& [id,value]:properties){ImGui::PushID((scope+id).c_str());const auto* descriptor=propertyRegistry.Find(id);if(descriptor){if(editStyleValue(descriptor->displayName.c_str(),value,descriptor->valueType))changed=true;}else ImGui::TextColored({1,.4f,.35f,1},"Unknown property: %s",id.c_str());ImGui::SameLine();if(ImGui::SmallButton("×"))remove=id;ImGui::PopID();}if(!remove.empty()){properties.erase(remove);changed=true;}if(ImGui::BeginCombo(("新增屬性##"+scope).c_str(),"選擇 Style Property")){for(const auto* descriptor:propertyRegistry.Descriptors())if(!properties.contains(descriptor->id)&&ImGui::Selectable((descriptor->category+" / "+descriptor->displayName+"##"+descriptor->id).c_str())){properties[descriptor->id]=ui::StyleValue::Literal(defaultValue(descriptor->valueType));changed=true;}ImGui::EndCombo();}return changed;};
    if(ImGui::BeginTabBar("##external-theme-tabs")){
        if(ImGui::BeginTabItem("Tokens")){
            if(ImGui::BeginChild("##token-list",{220,0},ImGuiChildFlags_Borders)){
                for(int index=0;index<static_cast<int>(theme.tokens.size());++index){const auto& token=theme.tokens[static_cast<std::size_t>(index)];if(ImGui::Selectable((token.displayName+"##token-"+token.id.ToString()).c_str(),m_themeToken==index))m_themeToken=index;}
                if(ImGui::Button("＋ Token")){ui::TokenDefinition token{.id=Uuid::Random(),.displayName="New Token",.type=VariantType::Color,.value=ui::StyleValue::Literal(Color{255,255,255,255})};theme.tokens.push_back(std::move(token));m_themeToken=static_cast<int>(theme.tokens.size())-1;m_themeDirty=true;}
            }
            ImGui::EndChild();ImGui::SameLine();
            if(ImGui::BeginChild("##token-editor",{0,0},ImGuiChildFlags_Borders)){
                if(m_themeToken>=0&&m_themeToken<static_cast<int>(theme.tokens.size())){auto& token=theme.tokens[static_cast<std::size_t>(m_themeToken)];m_themeDirty|=ImGui::InputText("名稱",&token.displayName);const char* types[]{"Bool","Integer","Number","String","Color"};const VariantType values[]{VariantType::Bool,VariantType::Integer,VariantType::Number,VariantType::String,VariantType::Color};int selected=0;for(int index=0;index<5;++index)if(token.type==values[index])selected=index;if(ImGui::Combo("型別",&selected,types,5)){token.type=values[selected];token.value=ui::StyleValue::Literal(defaultValue(token.type));m_themeDirty=true;}m_themeDirty|=editStyleValue("值",token.value,token.type);if(ImGui::Button("刪除 Token")){const Status removed=theme.RemoveToken(token.id);if(removed){m_themeToken=-1;m_themeDirty=true;}else for(const auto& item:removed.Diagnostics())diag::Emit(item);}}
            }
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Styles / States")){
            if(ImGui::BeginChild("##style-list",{230,0},ImGuiChildFlags_Borders)){
                for(int index=0;index<static_cast<int>(theme.styles.size());++index){const auto& style=theme.styles[static_cast<std::size_t>(index)];if(ImGui::Selectable((style.displayName+"##style-"+style.id.ToString()).c_str(),m_themeStyle==index))m_themeStyle=index;}
                if(ImGui::Button("＋ Style")){ui::StyleDefinition style;style.id=Uuid::Random();style.displayName="New Style";style.category="Custom";theme.styles.push_back(std::move(style));m_themeStyle=static_cast<int>(theme.styles.size())-1;m_themeDirty=true;}
            }
            ImGui::EndChild();ImGui::SameLine();
            if(ImGui::BeginChild("##style-editor",{0,0},ImGuiChildFlags_Borders)){
                if(m_themeStyle>=0&&m_themeStyle<static_cast<int>(theme.styles.size())){auto& style=theme.styles[static_cast<std::size_t>(m_themeStyle)];m_themeDirty|=ImGui::InputText("名稱",&style.displayName);m_themeDirty|=ImGui::InputText("分類",&style.category);ImGui::SeparatorText("Normal");m_themeDirty|=renderProperties(style.properties,"style-normal");for(const auto [state,name]:std::array<std::pair<ui::StyleStateMask,const char*>,5>{{{ui::StateMask(ui::StyleState::Hover),"Hover"},{ui::StateMask(ui::StyleState::Pressed),"Pressed"},{ui::StateMask(ui::StyleState::Focused),"Focused"},{ui::StateMask(ui::StyleState::Checked),"Checked"},{ui::StateMask(ui::StyleState::Disabled),"Disabled"}}}){auto found=style.stateOverrides.find(state);if(found!=style.stateOverrides.end()){if(ImGui::TreeNode(name)){m_themeDirty|=renderProperties(found->second,"state-"+std::to_string(state));if(ImGui::Button((std::string("刪除 ")+name).c_str())){style.stateOverrides.erase(found);m_themeDirty=true;}ImGui::TreePop();}}else if(ImGui::SmallButton((std::string("＋ ")+name).c_str())){style.stateOverrides[state]={};m_themeDirty=true;}ImGui::SameLine();}ImGui::NewLine();if(ImGui::Button("刪除 Style")){const Status removed=theme.RemoveStyle(style.id);if(removed){m_themeStyle=-1;m_themeDirty=true;}else for(const auto& item:removed.Diagnostics())diag::Emit(item);}}
            }
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Variants")){for(int axisIndex=0;axisIndex<static_cast<int>(theme.variantAxes.size());++axisIndex){auto& axis=theme.variantAxes[static_cast<std::size_t>(axisIndex)];ImGui::PushID(axisIndex);if(ImGui::TreeNodeEx(axis.displayName.c_str(),axisIndex==m_themeAxis?ImGuiTreeNodeFlags_DefaultOpen:0)){m_themeAxis=axisIndex;m_themeDirty|=ImGui::InputText("Axis 名稱",&axis.displayName);for(auto& value:axis.values){ImGui::PushID(value.id.ToString().c_str());if(ImGui::TreeNode(value.displayName.c_str())){m_themeDirty|=ImGui::InputText("Value 名稱",&value.displayName);m_themeDirty|=renderProperties(value.properties,"variant-"+value.id.ToString());ImGui::TreePop();}ImGui::PopID();}if(ImGui::Button("＋ Variant Value")){ui::VariantValueDefinition value;value.id=Uuid::Random();value.displayName="New Value";axis.values.push_back(value);if(axis.defaultValue.Empty())axis.defaultValue=value.id;m_themeDirty=true;}ImGui::TreePop();}ImGui::PopID();}if(ImGui::Button("＋ Variant Axis")){ui::VariantAxisDefinition axis;axis.id=Uuid::Random();axis.displayName="New Axis";ui::VariantValueDefinition value;value.id=Uuid::Random();value.displayName="Default";axis.defaultValue=value.id;axis.values.push_back(value);theme.variantAxes.push_back(std::move(axis));m_themeDirty=true;}ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Resolved Trace")){
            ImGui::InputText("Control type",&m_themeTraceControl);
            const char* preview=m_themeTraceStyle>=0&&m_themeTraceStyle<static_cast<int>(theme.styles.size())?theme.styles[static_cast<std::size_t>(m_themeTraceStyle)].displayName.c_str():"(none)";
            if(ImGui::BeginCombo("Base style",preview)){
                if(ImGui::Selectable("(none)",m_themeTraceStyle<0))m_themeTraceStyle=-1;
                for(int index=0;index<static_cast<int>(theme.styles.size());++index){const auto& style=theme.styles[static_cast<std::size_t>(index)];if(ImGui::Selectable((style.displayName+"##trace-style-"+style.id.ToString()).c_str(),m_themeTraceStyle==index))m_themeTraceStyle=index;}
                ImGui::EndCombo();
            }
            ui::StyleResolveRequest request{.controlType=m_themeTraceControl};if(m_themeTraceStyle>=0&&m_themeTraceStyle<static_cast<int>(theme.styles.size()))request.binding.baseStyle=theme.styles[static_cast<std::size_t>(m_themeTraceStyle)].id;
            const auto resolved=ui::StyleResolver{}.Resolve(theme,request,propertyRegistry);
            if(resolved&&ImGui::BeginTable("##trace",4,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)){ImGui::TableSetupColumn("Property");ImGui::TableSetupColumn("Resolved source");ImGui::TableSetupColumn("Overrides");ImGui::TableSetupColumn("Token chain");ImGui::TableHeadersRow();for(const auto& [id,value]:resolved.Value().properties){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(id.c_str());ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(value.source.label.c_str());ImGui::TableSetColumnIndex(2);ImGui::Text("%zu",value.overriddenSources.size());ImGui::TableSetColumnIndex(3);ImGui::Text("%zu",value.tokenChain.size());}ImGui::EndTable();}
            else if(!resolved)for(const auto& item:resolved.Diagnostics())ImGui::TextColored({1,.4f,.35f,1},"%s %s",item.code.c_str(),item.message.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void EditorApp::RenderStoryLibrary() {
    if (ImGui::Begin("Story Library")) m_storyLibrary.Render();
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

bool EditorApp::RenderSplashSettings(ProjectManifest& manifest) {
    auto& splashes = manifest.splashes;
    bool changed = false;
    if (m_splashSelection >= static_cast<int>(splashes.size()))
        m_splashSelection = static_cast<int>(splashes.size()) - 1;

    std::optional<std::pair<int, int>> pendingMove;
    if (splashes.empty())
        ImGui::TextDisabled("No splash screens — Player goes directly to Title.");
    else {
        if (ImGui::BeginChild("##splash-list", ImVec2(0, 150),
                              ImGuiChildFlags_Borders)) {
            for (int index = 0; index < static_cast<int>(splashes.size()); ++index) {
                auto& entry = splashes[static_cast<std::size_t>(index)];
                ImGui::PushID(index);
                const std::string label = "☰  " +
                    fs::path(entry.scene.lastKnownPath).filename().string();
                if (ImGui::Selectable(label.c_str(), m_splashSelection == index))
                    m_splashSelection = index;
                ImGui::SameLine(330);
                const std::string audio = entry.audio
                    ? " · " + fs::path(entry.audio->lastKnownPath).filename().string()
                    : std::string{};
                ImGui::TextDisabled("%.1fs · %s%s", entry.minimumDuration,
                                    entry.skippable ? "Skippable" : "Not skippable",
                                    audio.c_str());
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("PX_SPLASH_INDEX", &index, sizeof(index));
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* payload =
                            ImGui::AcceptDragDropPayload("PX_SPLASH_INDEX")) {
                        const int from = *static_cast<const int*>(payload->Data);
                        if (from != index) pendingMove = std::pair{from, index};
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    if (pendingMove) {
        const auto [from, to] = *pendingMove;
        if (from < to)
            std::rotate(splashes.begin() + from, splashes.begin() + from + 1,
                        splashes.begin() + to + 1);
        else
            std::rotate(splashes.begin() + to, splashes.begin() + from,
                        splashes.begin() + from + 1);
        m_splashSelection = to;
        changed = true;
    }

    const bool selectedScene = fs::path(m_selectedAsset).extension() == ".pxscene";
    ImGui::BeginDisabled(!selectedScene);
    if (ImGui::Button("Add Existing Scene")) {
        if (const auto* asset = m_assetRegistry.FindPath(
                m_project.Context().root / m_selectedAsset)) {
            ui::startup::SplashScreenEntry entry;
            entry.scene = {asset->id, m_selectedAsset};
            entry.enterAnimation.clear();
            entry.exitAnimation.clear();
            splashes.push_back(std::move(entry));
            m_splashSelection = static_cast<int>(splashes.size()) - 1;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Create Splash Scene")) {
        fs::path runtime = "Content/UI/Splash/Splash.pxscene";
        for (int suffix = 2;
             fs::exists(m_project.Context().root / runtime) && suffix < 10000;
             ++suffix)
            runtime = fs::path("Content/UI/Splash") /
                      ("Splash_" + std::to_string(suffix) + ".pxscene");
        if (CreateAssetWithHistory(m_project.Context().root / runtime, 2)) {
            if (const auto* asset = m_assetRegistry.FindPath(
                    m_project.Context().root / runtime)) {
                ui::startup::SplashScreenEntry entry;
                entry.scene = {asset->id, runtime.generic_string()};
                entry.enterAnimation.clear();
                entry.exitAnimation.clear();
                splashes.push_back(std::move(entry));
                m_splashSelection = static_cast<int>(splashes.size()) - 1;
                changed = true;
            }
        }
    }
    const bool hasSelection = m_splashSelection >= 0 &&
                              m_splashSelection < static_cast<int>(splashes.size());
    ImGui::BeginDisabled(!hasSelection);
    ImGui::SameLine();
    if (ImGui::Button("Duplicate Entry")) {
        splashes.insert(splashes.begin() + m_splashSelection + 1,
                        splashes[static_cast<std::size_t>(m_splashSelection)]);
        ++m_splashSelection;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        splashes.erase(splashes.begin() + m_splashSelection);
        m_splashSelection = std::min(m_splashSelection,
                                     static_cast<int>(splashes.size()) - 1);
        changed = true;
    }
    ImGui::EndDisabled();

    if (m_splashSelection < 0 ||
        m_splashSelection >= static_cast<int>(splashes.size()))
        return changed;
    auto& entry = splashes[static_cast<std::size_t>(m_splashSelection)];
    ImGui::SeparatorText("Selected Splash");
    std::string scenePath = entry.scene.lastKnownPath;
    if (ImGui::InputText("Scene", &scenePath, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const auto* asset = m_assetRegistry.FindPath(m_project.Context().root / scenePath);
        entry.scene = {asset ? asset->id : Uuid{}, scenePath};
        changed = true;
    }
    std::string audioPath = entry.audio ? entry.audio->lastKnownPath : "";
    if (ImGui::InputText("Audio", &audioPath, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (audioPath.empty()) entry.audio.reset();
        else {
            const auto* asset = m_assetRegistry.FindPath(
                m_project.Context().root / audioPath);
            entry.audio = ResourceRefValue{asset ? asset->id : Uuid{}, audioPath};
        }
        changed = true;
    }
    changed |= ImGui::InputFloat("Minimum Duration", &entry.minimumDuration,
                                 0.1f, 0.5f, "%.2f s");
    changed |= ImGui::Checkbox("Skippable", &entry.skippable);
    ImGui::BeginDisabled(!entry.skippable);
    changed |= ImGui::InputFloat("Skip Allowed After", &entry.skipAllowedAfter,
                                 0.1f, 0.5f, "%.2f s");
    ImGui::EndDisabled();
    changed |= ImGui::InputText("Enter Animation", &entry.enterAnimation);
    changed |= ImGui::InputText("Exit Animation", &entry.exitAnimation);

    const Status valid = ui::startup::ValidateSplashEntry(
        entry, static_cast<std::size_t>(m_splashSelection),
        m_project.Context().ManifestPath().generic_string());
    if (!valid)
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                           valid.Diagnostics().front().message.c_str());
    else {
        std::ifstream stream(m_project.Context().root / entry.scene.lastKnownPath,
                             std::ios::binary);
        std::ostringstream text; text << stream.rdbuf();
        const auto document = resource::ParseTypedDocument(
            text.str(), entry.scene.lastKnownPath);
        if (document) {
            const auto found = document.Value().properties.find("animations");
            if (found != document.Value().properties.end()) {
                const auto animations = ui::ParseUIAnimationLibrary(
                    found->second, entry.scene.lastKnownPath);
                for (const auto& name : {entry.enterAnimation, entry.exitAnimation})
                    if (!name.empty() &&
                        (!animations || !animations.Value().machine.FindState(name)))
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                                           "Animation state not found: %s",
                                           name.c_str());
            } else if (!entry.enterAnimation.empty() || !entry.exitAnimation.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                                   "Scene has no animation library.");
        }
    }

    if (ImGui::Button("Open in UI Designer")) {
        SetWorkspace(EditorWorkspace::UI);
        Log("UI authoring has moved to PrismatiXStudio: " + entry.scene.lastKnownPath);
    }
    return changed;
}

void EditorApp::RenderBuild() {
    if(!m_showBuildWindow)return;
    if (ImGui::Begin("Build",&m_showBuildWindow)) {
        ProjectManifest& m = m_project.Context().manifest;

        ImGui::SeparatorText("Project");
        bool manifestChanged = false;
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
            for (const resource::AssetEntry& a : m_assetRegistry.Entries()) {
                if(ClassifyAsset(a.sourcePath)!="script")continue;
                const std::string runtimePath=AssetRuntimePath(a);
                if (ImGui::Selectable(runtimePath.c_str(), runtimePath == m.startScript)) {
                    m.startScript = runtimePath;
                    manifestChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        if (manifestChanged) {
            m_project.SaveManifest();
        }

        ImGui::SeparatorText("Startup / Splash Screens");
        if (RenderSplashSettings(m)) m_project.SaveManifest();

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
