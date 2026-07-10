#include "Editor/Application/EditorApp.h"

#include "Engine/VN/PDS/Parser.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace px::editor {

namespace {
const std::array<const char*, 8> kAssetTypes = { "all", "image", "audio", "script", "ui", "font", "lua", "other" };
constexpr const char* kResourcePayload = "PX_RESOURCE_PATH";
namespace fs = std::filesystem;

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
}
void EditorApp::BuildDockLayout(unsigned int dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Assets", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Database", right);
    ImGui::DockBuilderDockWindow("Node Editor", center);
    ImGui::DockBuilderDockWindow("Flow", center);
    ImGui::DockBuilderDockWindow("Scripting", center);
    ImGui::DockBuilderDockWindow("Preview", center);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Problems", bottom);
    ImGui::DockBuilderDockWindow("PDS Text", bottom);
    ImGui::DockBuilderDockWindow("Localization", bottom);
    ImGui::DockBuilderDockWindow("Build", bottom);
    ImGui::DockBuilderDockWindow("Animation", bottom);
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
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
    if (m_buildLayout || dockNode == nullptr || dockNode->IsLeafNode()) {
        BuildDockLayout(dockId);
        m_buildLayout = false;
    }
    HandleShortcuts();
    SyncDesigner();
    RenderMenuBar();
    RenderCommandPalette();
    RenderShortcutsWindow();
    RenderHierarchy();
    RenderInspector();
    RenderAssets();
    RenderConsole();
    RenderPreview();
    RenderNodeEditor();
    RenderPDSText();
    RenderBuild();
    RenderDatabase();
    RenderAnimation();
    RenderFlow();
    RenderScripting();
    RenderLocalization();
    RenderProblems();

    // One undo entry per edit gesture (Database fields, Flow node drags):
    // commit when both the widget and the mouse are released.
    if (m_dbEditPending && !ImGui::IsAnyItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        RecordDatabaseUndo("Database edit", m_dbBaseline, m_database.Serialize());
        m_dbBaseline = m_database.Serialize();
        m_dbEditPending = false;
    }
}

void EditorApp::SyncDesigner() {
    if (!m_preview) {
        return;
    }
    const bool uiMode = m_previewMode == 0;
    m_preview->UIStageRef().SetEditMode(uiMode && !m_previewAnims);
    if (!uiMode) {
        return;
    }
    const std::string abs = (m_project.Context().root / m_preview->CurrentUIPath()).string();
    if (m_designerPath != abs) {
        m_designer.SetScene(&m_preview->UIStageRef().Scene(), abs);
        m_designer.SetOnStructureChange([this] { m_preview->UIStageRef().OnSceneEdited(); });
        m_designerPath = abs;
    }
}

void EditorApp::RenderMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Current Folder")) {
            OpenProject(std::filesystem::current_path());
        }
        if (ImGui::MenuItem("Rescan Assets")) {
            m_assets.Scan(m_project.Context());
        }
        if (ImGui::MenuItem("Import Clipboard Files", "Ctrl+V")) {
            ImportClipboardAssets();
        }
        if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
            if (m_project.SaveManifest()) Log("Manifest saved.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            m_running = false;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem(("Undo " + m_undo.NextUndoLabel()).c_str(), "Ctrl+Z", false, m_undo.CanUndo())) {
            m_undo.Undo();
        }
        if (ImGui::MenuItem(("Redo " + m_undo.NextRedoLabel()).c_str(), "Ctrl+Y", false, m_undo.CanRedo())) {
            m_undo.Redo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Keyboard Shortcuts", "F1", m_showShortcuts)) {
            m_showShortcuts = !m_showShortcuts;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Command Palette", "Ctrl+P")) {
            m_paletteOpen = true;
            m_paletteFocus = true;
            m_paletteFilter[0] = 0;
        }
        if (ImGui::MenuItem("Reset Layout")) {
            m_buildLayout = true;
        }
        if (ImGui::MenuItem("Back to Welcome")) {
            m_screen = Screen::Welcome;
        }
        ImGui::EndMenu();
    }

    ImGui::TextDisabled("   |   ");
    if (ImGui::SmallButton("Save All")) {
        SaveAll();
    }
    if (ImGui::SmallButton("Reload Preview") && m_preview) {
        m_preview->Reload();
    }
    if (ImGui::SmallButton("Run")) {
        RunDev();
    }
    if (ImGui::SmallButton("Build")) {
        RunBuild();
    }
    ImGui::EndMainMenuBar();
}

void EditorApp::RenderHierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        if (m_previewMode == 0) {
            m_designer.RenderHierarchy();
        }
        else {
            ImGui::TextDisabled("Switch Preview to UI Scene mode to edit a .pxui.");
        }
    }
    ImGui::End();
}

void EditorApp::RenderInspector() {
    if (ImGui::Begin("Inspector")) {
        if (m_previewMode == 0) {
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
                    const std::string abs = (m_project.Context().root / m_selectedAsset).string();
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
        OpenDocTab(rec.runtimePath);
    } else if (rec.type == "ui" && m_preview) {
        m_previewMode = 0;
        m_preview->LoadUI(rec.runtimePath);
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
    fs::rename(src, dst, ec);
    if (ec) {
        Log("Move failed: " + runtimePath);
        return;
    }
    const std::string newRel = fs::relative(dst, m_project.Context().root, ec).generic_string();
    const int updated = UpdateAssetReferences(runtimePath, newRel);
    Log("Moved " + runtimePath + " -> " + newRel + " (" + std::to_string(updated) +
        " referencing file(s) updated)");
    if (m_selectedAsset == runtimePath) m_selectedAsset = newRel;
    m_assets.Scan(m_project.Context());
    m_flowStale = true;
    RefreshProblems();
}

void EditorApp::RenderAssetTree(const std::filesystem::path& dir, const std::filesystem::path& root) {
    std::vector<fs::path> subdirs;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) subdirs.push_back(it->path());
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
            m_assetDir = rel;
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
            RenderAssetTree(sub, root);
            ImGui::TreePop();
        }
    }
}

void EditorApp::RenderAssetEntry(const AssetRecord& rec, bool gridMode, float tile) {
    ImGui::PushID(rec.runtimePath.c_str());
    const bool selected = rec.runtimePath == m_selectedAsset;
    const std::string name = rec.absolutePath.filename().string();

    if (gridMode) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float labelH = ImGui::GetTextLineHeight() + 6.0f;
        if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(tile, tile + labelH))) {
            m_selectedAsset = rec.runtimePath;
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
            thumb = m_textures->LoadId(rec.absolutePath.string(), &tw, &th);
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
                                    : rec.type == "script" ? "PDS"
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
            m_selectedAsset = rec.runtimePath;
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
    if (ImGui::Begin("Assets")) {
        const fs::path root = m_project.Context().root;

        // --- Toolbar ---
        ImGui::SetNextItemWidth(150);
        ImGui::InputTextWithHint("##filter", "search...", m_assetFilter, sizeof(m_assetFilter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("##type", kAssetTypes[m_assetTypeIndex])) {
            for (int i = 0; i < static_cast<int>(kAssetTypes.size()); ++i) {
                if (ImGui::Selectable(kAssetTypes[i], m_assetTypeIndex == i)) m_assetTypeIndex = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(m_assetGridView ? "List" : "Grid")) {
            m_assetGridView = !m_assetGridView;
        }
        if (m_assetGridView) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderFloat("##thumb", &m_assetThumbSize, 48.0f, 160.0f, "");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("New")) {
            ImGui::OpenPopup("##assetNewMenu");
        }
        if (ImGui::BeginPopup("##assetNewMenu")) {
            if (ImGui::MenuItem("Folder")) { m_assetNewKind = 0; m_assetNewNameBuf[0] = 0; }
            if (ImGui::MenuItem("Script (.pds)")) { m_assetNewKind = 1; m_assetNewNameBuf[0] = 0; }
            if (ImGui::MenuItem("UI Screen (.pxui)")) { m_assetNewKind = 2; m_assetNewNameBuf[0] = 0; }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste")) {
            ImportClipboardAssets();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) {
            m_assets.Scan(m_project.Context());
        }
        ImGui::Separator();

        // --- Folder tree | content ---
        if (ImGui::BeginChild("##assettree", ImVec2(170, 0), ImGuiChildFlags_ResizeX)) {
            ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_SpanAvailWidth |
                                           ImGuiTreeNodeFlags_DefaultOpen;
            if (m_assetDir == "Data") rootFlags |= ImGuiTreeNodeFlags_Selected;
            const bool rootOpen = ImGui::TreeNodeEx("Data", rootFlags);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                m_assetDir = "Data";
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kResourcePayload)) {
                    MoveAssetTo(std::string(static_cast<const char*>(payload->Data),
                                            payload->DataSize > 0 ? payload->DataSize - 1 : 0),
                                root / "Data");
                }
                ImGui::EndDragDropTarget();
            }
            if (rootOpen) {
                std::error_code ec;
                if (fs::exists(root / "Data", ec)) {
                    RenderAssetTree(root / "Data", root);
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (ImGui::BeginChild("##assetcontent")) {
            const bool searching = m_assetFilter[0] != 0 || m_assetTypeIndex != 0;
            if (searching) {
                const auto filtered = m_assets.Filter(m_assetFilter, kAssetTypes[m_assetTypeIndex]);
                ImGui::TextDisabled("%zu result(s)", filtered.size());
                ImGui::Separator();
                for (const AssetRecord& rec : filtered) {
                    RenderAssetEntry(rec, false, 0.0f);
                }
            } else {
                // Breadcrumb
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
                    if (ImGui::SmallButton((part + "##crumb" + accum).c_str())) {
                        m_assetDir = accum;
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
                    } else if (it->path().extension() != ".meta") {
                        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
                        for (const AssetRecord& rec : m_assets.Assets()) {
                            if (rec.runtimePath == rel) {
                                files.push_back(&rec);
                                break;
                            }
                        }
                    }
                }
                std::sort(dirs.begin(), dirs.end());
                std::sort(files.begin(), files.end(),
                          [](const AssetRecord* a, const AssetRecord* b) {
                              return a->runtimePath < b->runtimePath;
                          });

                const float tile = m_assetThumbSize;
                const float cellW = tile + 10.0f;
                float x = 0.0f;
                const float maxW = ImGui::GetContentRegionAvail().x;

                for (const fs::path& d : dirs) {
                    const std::string dname = d.filename().string();
                    ImGui::PushID(dname.c_str());
                    if (m_assetGridView) {
                        if (x + cellW > maxW && x > 0.0f) x = 0.0f;
                        else if (x > 0.0f) ImGui::SameLine();
                        const ImVec2 pos = ImGui::GetCursorScreenPos();
                        const float labelH = ImGui::GetTextLineHeight() + 6.0f;
                        if (ImGui::Selectable("##dir", false, ImGuiSelectableFlags_AllowDoubleClick,
                                              ImVec2(tile, tile + labelH)) &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            m_assetDir += "/" + dname;
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
                        if (ImGui::Selectable(("[ " + dname + " ]").c_str(), false,
                                              ImGuiSelectableFlags_AllowDoubleClick) &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            m_assetDir += "/" + dname;
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

                for (const AssetRecord* rec : files) {
                    if (m_assetGridView) {
                        if (x + cellW > maxW && x > 0.0f) x = 0.0f;
                        else if (x > 0.0f) ImGui::SameLine();
                        RenderAssetEntry(*rec, true, tile);
                        x += cellW;
                    } else {
                        RenderAssetEntry(*rec, false, 0.0f);
                    }
                }
                if (dirs.empty() && files.empty()) {
                    ImGui::TextDisabled("(empty folder)");
                }
            }

            // --- Import settings for the selection ---
            if (!m_selectedAsset.empty() && m_project.Context().IsOpen()) {
                ImGui::Dummy(ImVec2(0, 6));
                const fs::path abs = root / m_selectedAsset;
                if (m_metaAsset != m_selectedAsset) {
                    m_meta = LoadAssetMeta(abs);
                    m_metaAsset = m_selectedAsset;
                }
                if (ImGui::CollapsingHeader(("Import settings: " +
                                             abs.filename().string()).c_str())) {
                    bool changed = ImGui::Checkbox("Include in build", &m_meta.includeInBuild);
                    changed |= ImGui::InputText("Note", &m_meta.note);
                    if (changed) SaveAssetMeta(abs, m_meta);
                }
            }
        }
        ImGui::EndChild();

        // --- Create popup ---
        if (m_assetNewKind >= 0) {
            ImGui::OpenPopup("Create Asset");
        }
        if (ImGui::BeginPopupModal("Create Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* kinds[] = { "Folder", "Script (.pds)", "UI Screen (.pxui)" };
            ImGui::TextDisabled("%s in %s/",
                                kinds[std::clamp(m_assetNewKind, 0, 2)], m_assetDir.c_str());
            ImGui::SetNextItemWidth(240);
            const bool submit = ImGui::InputText("##newname", m_assetNewNameBuf,
                                                 sizeof(m_assetNewNameBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if ((submit || ImGui::Button("Create")) && m_assetNewNameBuf[0] != 0) {
                std::error_code ec;
                const fs::path cur = root / m_assetDir;
                if (m_assetNewKind == 0) {
                    fs::create_directories(cur / m_assetNewNameBuf, ec);
                } else if (m_assetNewKind == 1) {
                    std::ofstream out(cur / (std::string(m_assetNewNameBuf) + ".pds"));
                    out << "# " << m_assetNewNameBuf << "\n\n[text]\nNew line\n\n";
                } else {
                    std::ofstream out(cur / (std::string(m_assetNewNameBuf) + ".pxui"));
                    out << "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
                           "  \"background\": { \"color\": [12,14,20,255] },\n  \"nodes\": []\n}\n";
                }
                m_assets.Scan(m_project.Context());
                Log(std::string("Created ") + m_assetNewNameBuf);
                m_assetNewKind = -1;
                ImGui::CloseCurrentPopup();
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
            if ((submit || ImGui::Button("Rename")) && m_assetRenameBuf[0] != 0) {
                std::error_code ec;
                const fs::path src = root / m_assetRenameFrom;
                const fs::path dst = src.parent_path() / m_assetRenameBuf;
                if (fs::exists(dst, ec)) {
                    Log("Rename failed, target exists: " + dst.filename().string());
                } else {
                    fs::rename(src, dst, ec);
                    if (ec) {
                        Log("Rename failed: " + m_assetRenameFrom);
                    } else {
                        const std::string newRel =
                            fs::relative(dst, root, ec).generic_string();
                        const int updated = UpdateAssetReferences(m_assetRenameFrom, newRel);
                        Log("Renamed to " + newRel + " (" + std::to_string(updated) +
                            " referencing file(s) updated)");
                        if (m_selectedAsset == m_assetRenameFrom) m_selectedAsset = newRel;
                        m_assets.Scan(m_project.Context());
                        m_flowStale = true;
                        RefreshProblems();
                    }
                }
                m_assetRenameFrom.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
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
            ImGui::Text("Delete \"%s\" from disk?", m_assetPendingDelete.c_str());
            ImGui::TextDisabled("This cannot be undone.");
            ImGui::Separator();
            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                std::error_code ec;
                std::filesystem::remove(m_project.Context().root / m_assetPendingDelete, ec);
                if (ec) {
                    Log("Delete failed: " + m_assetPendingDelete);
                } else {
                    Log("Deleted " + m_assetPendingDelete);
                    if (m_selectedAsset == m_assetPendingDelete) m_selectedAsset.clear();
                    m_assets.Scan(m_project.Context());
                    m_flowStale = true;
                    RefreshProblems();
                }
                m_assetPendingDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                m_assetPendingDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
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
    if (ImGui::Begin("Preview") && m_preview) {
        ImGui::SetNextItemWidth(140);
        const char* modes[] = { "UI Scene", "VN Script" };
        if (ImGui::Combo("##mode", &m_previewMode, modes, 2)) {
            if (m_previewMode == 0) {
                m_preview->LoadUI(m_project.Context().manifest.startUI);
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
                        m_preview->LoadUI(a.runtimePath);
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
                    const std::filesystem::path rel = std::filesystem::path("Data/UI") / (std::string(m_newScreenName) + ".pxui");
                    const std::filesystem::path abs = m_project.Context().root / rel;
                    std::error_code ec;
                    std::filesystem::create_directories(abs.parent_path(), ec);
                    if (!std::filesystem::exists(abs)) {
                        std::ofstream out(abs);
                        out << "{\n  \"canvas\": { \"w\": 1280, \"h\": 720 },\n"
                               "  \"background\": { \"color\": [12,14,20,255] },\n  \"nodes\": []\n}\n";
                    }
                    m_assets.Scan(m_project.Context());
                    m_preview->LoadUI(rel.generic_string());
                    Log("Created screen " + rel.generic_string());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();

        ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool vnMode = m_previewMode == 1;
        const float debugHeight = vnMode ? 230.0f : 0.0f;
        avail.y = std::max(80.0f, avail.y - debugHeight);
        const float gw = static_cast<float>(m_preview->Width());
        const float gh = static_cast<float>(m_preview->Height());
        const float scale = std::min(avail.x / gw, avail.y / gh);
        const ImVec2 disp(gw * scale, gh * scale);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        p0.x += (avail.x - disp.x) * 0.5f;
        p0.y += std::max(0.0f, (avail.y - disp.y) * 0.5f);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hovered = mouse.x >= p0.x && mouse.x <= p0.x + disp.x && mouse.y >= p0.y && mouse.y <= p0.y + disp.y;
        const float localX = scale > 0 ? (mouse.x - p0.x) / scale : 0.0f;
        const float localY = scale > 0 ? (mouse.y - p0.y) / scale : 0.0f;
        const bool click = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool uiMode = m_previewMode == 0;

        m_preview->Tick(ImGui::GetIO().DeltaTime, SDL_GetTicks(), hovered, localX, localY, uiMode ? false : click);

        if (m_preview->Target()) {
            ImGui::SetCursorScreenPos(p0);
            ImGui::Image(reinterpret_cast<ImTextureID>(m_preview->Target()), disp);
        }
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
            m_designer.CanvasInput(p0, scale, hovered, selectedImageAsset);
            m_designer.DrawOverlay(p0, scale);
            const ImRect canvasRect(p0, ImVec2(p0.x + disp.x, p0.y + disp.y));
            if (ImGui::BeginDragDropTargetCustom(canvasRect, ImGui::GetID("UIDesignerCanvasDrop"))) {
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
    }
    ImGui::End();
}

void EditorApp::RenderNodeEditor() {
    if (ImGui::Begin("Node Editor")) {
        m_nodeEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (m_scriptDocs.empty()) {
            ImGui::TextDisabled("Open a .pds from the Assets panel or the Flow map.");
        } else if (ImGui::BeginTabBar("##pds-doc-tabs",
                                      ImGuiTabBarFlags_Reorderable |
                                          ImGuiTabBarFlags_FittingPolicyScroll)) {
            int closeRequest = -1;
            for (int i = 0; i < static_cast<int>(m_scriptDocs.size()); ++i) {
                NodeGraphEditor& doc = *m_scriptDocs[static_cast<std::size_t>(i)];
                const std::string path = doc.CurrentRuntimePath();
                const std::string label = fs::path(path).filename().string() +
                                          (doc.Dirty() ? " *" : "") + "###doc-" + path;
                bool open = true;
                ImGuiTabItemFlags flags = 0;
                if (m_focusDocRequest == i) {
                    flags |= ImGuiTabItemFlags_SetSelected;
                    m_focusDocRequest = -1;
                }
                if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                    m_activeDoc = i;
                    doc.Render();
                    ImGui::EndTabItem();
                }
                if (!open) {
                    closeRequest = i;
                }
            }
            ImGui::EndTabBar();
            if (closeRequest >= 0) {
                if (m_scriptDocs[static_cast<std::size_t>(closeRequest)]->Dirty()) {
                    m_scriptDocs[static_cast<std::size_t>(closeRequest)]->Save();  // closing never loses work
                }
                m_scriptDocs.erase(m_scriptDocs.begin() + closeRequest);
                if (m_activeDoc >= static_cast<int>(m_scriptDocs.size())) {
                    m_activeDoc = static_cast<int>(m_scriptDocs.size()) - 1;
                }
            }
        }
    } else {
        m_nodeEditorFocused = false;
    }
    ImGui::End();
}

void EditorApp::RenderPDSText() {
    if (ImGui::Begin("PDS Text")) {
        NodeGraphEditor* doc = ActiveDocPtr();
        if (!doc) {
            ImGui::TextDisabled("No script open.");
            ImGui::End();
            return;
        }
        // Text-first editing: the buffer mirrors the compiled graph until the
        // user types; Apply re-imports the text back into the node graph.
        if (!m_pdsTextEditing) {
            m_pdsTextBuf = doc->Compile();
        }
        ImGui::BeginDisabled(!m_pdsTextEditing);
        if (ImGui::SmallButton("Apply to Graph")) {
            if (doc->ImportPDSText(m_pdsTextBuf)) {
                Log("PDS text applied to the node graph (unsaved).");
            } else {
                Log("PDS text apply failed.");
            }
            m_pdsTextEditing = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Revert")) {
            m_pdsTextEditing = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_pdsTextEditing
                                      ? "edited — Apply writes back into the graph"
                                      : "editable text view of the open script");
        if (ImGui::InputTextMultiline("##pds", &m_pdsTextBuf, ImGui::GetContentRegionAvail())) {
            m_pdsTextEditing = true;
        }
    }
    ImGui::End();
}


void EditorApp::RenderAnimation() {
    if (ImGui::Begin("Animation")) {
        if (m_previewMode != 0) {
            ImGui::TextDisabled("Switch Preview to UI Scene mode to edit animations.");
        }
        else {
            if (ImGui::Checkbox("Preview animations (play in canvas)", &m_previewAnims)) {
                if (m_previewAnims && m_preview) {
                    m_preview->UIStageRef().TriggerEnter();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Replay") && m_preview) {
                m_preview->UIStageRef().TriggerEnter();
            }
            ImGui::Separator();
            m_designer.RenderAnimation();
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
        if (m_problems.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.5f, 1.0f), "No problems detected.");
        }
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "%zu problem(s)", m_problems.size());
        }
        ImGui::Separator();
        if (ImGui::BeginChild("##problemlist")) {
            int idx = 0;
            for (const std::string& p : m_problems) {
                // "script.pds:NN  ..." rows open the script in the Node Editor.
                const std::size_t ext = p.find(".pds:");
                if (ext != std::string::npos && ext < 64) {
                    if (ImGui::Selectable((p + "##prob" + std::to_string(idx)).c_str())) {
                        OpenDocTab(p.substr(0, ext + 4));
                    }
                } else {
                    ImGui::BulletText("%s", p.c_str());
                }
                ++idx;
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::LocScanScripts() {
    if (!m_project.Context().IsOpen()) {
        return;
    }
    // Keep existing translations while re-collecting source lines.
    std::unordered_map<std::string, std::string> existing;
    for (const auto& [source, translation] : m_locEntries) {
        if (!translation.empty()) existing[source] = translation;
    }
    m_locEntries.clear();
    std::unordered_set<std::string> seen;
    const auto add = [&](const std::string& source) {
        if (source.empty() || !seen.insert(source).second) {
            return;
        }
        const auto it = existing.find(source);
        m_locEntries.emplace_back(source, it != existing.end() ? it->second : std::string{});
    };

    const fs::path scriptDir = m_project.Context().root / "Data" / "Script";
    std::error_code ec;
    for (fs::directory_iterator it(scriptDir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".pds") continue;
        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        const px::vn::ParsedScript parsed = px::vn::ParsePDS(ss.str());
        for (const px::vn::Command& cmd : parsed.commands) {
            if (cmd.type == "say") {
                add(cmd.Get("value", cmd.Get("text")));
            } else if (cmd.type == "text" && (cmd.Has("value") || cmd.Has("text"))) {
                add(cmd.Get("value", cmd.Get("text")));
            } else if (cmd.type == "choice") {
                add(cmd.Get("text", cmd.Get("value")));
            }
        }
    }
    Log("Localization: scanned " + std::to_string(m_locEntries.size()) + " source line(s).");
}

void EditorApp::LocLoad() {
    if (!m_project.Context().IsOpen()) {
        return;
    }
    const fs::path path =
        m_project.Context().root / "Data" / "Lang" / (std::string(m_locLang) + ".json");
    std::unordered_map<std::string, std::string> table;
    if (std::ifstream in(path, std::ios::binary); in) {
        const Json j = Json::parse(in, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) table[it.key()] = it.value().get<std::string>();
            }
        }
    }
    LocScanScripts();
    std::unordered_set<std::string> seen;
    for (auto& [source, translation] : m_locEntries) {
        seen.insert(source);
        if (const auto it = table.find(source); it != table.end()) {
            translation = it->second;
        }
    }
    // Keep stale entries from the file (sources no longer in any script).
    for (const auto& [source, translation] : table) {
        if (seen.insert(source).second) {
            m_locEntries.emplace_back(source, translation);
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
    for (const auto& [source, translation] : m_locEntries) {
        if (!translation.empty()) {
            j[source] = translation;
            ++count;
        }
    }
    const fs::path dir = m_project.Context().root / "Data" / "Lang";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / (std::string(m_locLang) + ".json"), std::ios::binary | std::ios::trunc);
    out << j.dump(2);
    m_locDirty = false;
    m_assets.Scan(m_project.Context());
    Log("Localization: saved " + std::to_string(count) + " translation(s) to Data/Lang/" +
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
    const fs::path dir = m_project.Context().root / "Data" / "Lang";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (std::string(m_locLang) + ".csv");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "\xEF\xBB\xBF";  // UTF-8 BOM so spreadsheet apps decode correctly
    out << "source,translation\n";
    for (const auto& [source, translation] : m_locEntries) {
        out << quote(source) << ',' << quote(translation) << '\n';
    }
    Log("Localization: exported " + path.generic_string());
}

void EditorApp::RenderLocalization() {
    if (ImGui::Begin("Localization")) {
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
        for (const auto& [s, t] : m_locEntries) {
            if (!t.empty()) ++translated;
        }
        ImGui::TextDisabled("%d / %d translated", translated,
                            static_cast<int>(m_locEntries.size()));
        ImGui::Separator();

        if (m_locEntries.empty()) {
            ImGui::TextDisabled("Press \"Load\" (or \"Scan Scripts\") to collect dialogue lines. "
                                "The player reads Data/Lang/<language>.json at startup.");
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
                auto& [source, translation] = m_locEntries[static_cast<std::size_t>(i)];
                if (!filter.empty() && source.find(filter) == std::string::npos &&
                    translation.find(filter) == std::string::npos) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                ImGui::TextWrapped("%s", source.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##tr", &translation)) {
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
            m_flow.Rebuild(m_database, m_project.Context().root);
            m_flowStale = false;
        }
        if (ImGui::SmallButton("Rebuild") && m_project.Context().IsOpen()) {
            m_flow.SetEntryScript(m_project.Context().manifest.startScript);
            m_flow.Rebuild(m_database, m_project.Context().root);
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

void EditorApp::RenderDatabase() {
    if (ImGui::Begin("Database")) {
        if (ImGui::Button(m_dbDirty ? "Save*" : "Save") && !m_dbPath.empty()) {
            std::ofstream out(m_dbPath);
            out << m_database.Serialize();
            m_dbDirty = false;
            Log("Saved database.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Cast / Variables / Gallery / Chapters / Input triggers");
        ImGui::Separator();
        if (m_dbPanel.Render(m_database, ScriptFileNames())) {
            m_dbDirty = true;
            m_flowStale = true;
            m_dbEditPending = true;
        }
    }
    ImGui::End();
}

void EditorApp::RenderBuild() {
    if (ImGui::Begin("Build")) {
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
        if (ImGui::BeginCombo("Start UI", m.startUI.c_str())) {
            for (const AssetRecord& a : m_assets.Filter("", "ui")) {
                if (ImGui::Selectable(a.runtimePath.c_str(), a.runtimePath == m.startUI)) {
                    m.startUI = a.runtimePath;
                    manifestChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Entry Script", m.startScript.c_str())) {
            for (const AssetRecord& a : m_assets.Filter("", "script")) {
                const std::string name = std::filesystem::path(a.runtimePath).filename().string();
                if (ImGui::Selectable(name.c_str(), name == m.startScript)) {
                    m.startScript = name;
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
            m_flow.Rebuild(m_database, m_project.Context().root);
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
            "Packs Data/ into Data.pdx (honoring per-asset 'Include in build'), bundles "
            "the player + DLLs, and writes game.prismatix. White-box key (deters casual "
            "extraction)."
        );
    }
    ImGui::End();
}


}  // namespace px::editor
