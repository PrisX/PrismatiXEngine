#include "Editor/Application/EditorApp.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>

namespace px::editor {

namespace {
const std::array<const char*, 8> kAssetTypes = { "all", "image", "audio", "script", "ui", "font", "lua", "other" };
constexpr const char* kResourcePayload = "PX_RESOURCE_PATH";
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
    ImGui::DockBuilderDockWindow("Build", bottom);
    ImGui::DockBuilderDockWindow("Animation", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorApp::RenderWelcome() {
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
        if (ImGui::BeginTabItem("Open")) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextDisabled("Project folder");
            ImGui::SetNextItemWidth(panel.x - 24);
            ImGui::InputText("##path", m_openPath, sizeof(m_openPath));
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
            ImGui::SetNextItemWidth(panel.x - 24);
            ImGui::InputText("##newpath", m_newPath, sizeof(m_newPath));
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
    RenderProblems();
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
            m_nodeEditor.RenderInspector();
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

void EditorApp::RenderAssets() {
    if (ImGui::Begin("Assets")) {
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##filter", "filter...", m_assetFilter, sizeof(m_assetFilter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##type", kAssetTypes[m_assetTypeIndex])) {
            for (int i = 0; i < static_cast<int>(kAssetTypes.size()); ++i) {
                if (ImGui::Selectable(kAssetTypes[i], m_assetTypeIndex == i)) m_assetTypeIndex = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Paste")) {
            ImportClipboardAssets();
        }
        ImGui::Separator();

        const bool isPds = m_selectedAsset.size() > 4 && m_selectedAsset.substr(m_selectedAsset.size() - 4) == ".pds";
        if (isPds && ImGui::Button("Open in Node Editor")) {
            m_nodeEditor.OpenDocument(m_selectedAsset);
        }

        if (!m_selectedAsset.empty() && m_project.Context().IsOpen()) {
            const std::filesystem::path abs = m_project.Context().root / m_selectedAsset;
            if (m_metaAsset != m_selectedAsset) {
                m_meta = LoadAssetMeta(abs);
                m_metaAsset = m_selectedAsset;
            }
            if (ImGui::CollapsingHeader("Import settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool changed = ImGui::Checkbox("Include in build", &m_meta.includeInBuild);
                changed |= ImGui::InputText("Note", &m_meta.note);
                if (changed) SaveAssetMeta(abs, m_meta);
            }
        }
        ImGui::Separator();

        const auto filtered = m_assets.Filter(m_assetFilter, kAssetTypes[m_assetTypeIndex]);
        ImGui::Text("%zu assets", filtered.size());
        if (ImGui::BeginChild("##assetlist")) {
            for (const AssetRecord& rec : filtered) {
                ImGui::PushID(rec.runtimePath.c_str());
                ImGui::BeginGroup();
                if (rec.type == "image" && m_textures) {
                    if (ImTextureID thumb = m_textures->LoadId(rec.absolutePath.string())) {
                        ImGui::Image(thumb, ImVec2(40, 24));
                        ImGui::SameLine();
                    }
                }
                const bool selected = rec.runtimePath == m_selectedAsset;
                if (ImGui::Selectable(rec.runtimePath.c_str(), selected)) {
                    m_selectedAsset = rec.runtimePath;
                }
                ImGui::EndGroup();
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload(kResourcePayload, rec.runtimePath.c_str(),
                                              rec.runtimePath.size() + 1);
                    ImGui::TextUnformatted(rec.runtimePath.c_str());
                    ImGui::TextDisabled("%s", rec.type.c_str());
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
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

        const ImVec2 avail = ImGui::GetContentRegionAvail();
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
        m_nodeEditor.Render();
    }
    ImGui::End();
}

void EditorApp::RenderPDSText() {
    if (ImGui::Begin("PDS Text")) {
        std::string text = m_nodeEditor.Compile();
        ImGui::InputTextMultiline("##pds", &text, ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
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
            for (const std::string& p : m_problems) {
                ImGui::BulletText("%s", p.c_str());
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::RenderFlow() {
    if (ImGui::Begin("Flow")) {
        if (ImGui::SmallButton("Rebuild") && m_project.Context().IsOpen()) {
            m_flow.Rebuild(m_database, m_project.Context().root);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Story map — chapters from the Database + cross-script jumps.");
        ImGui::Separator();
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
        if (m_dbPanel.Render(m_database)) {
            m_dbDirty = true;
        }
    }
    ImGui::End();
}

void EditorApp::RenderBuild() {
    if (ImGui::Begin("Build")) {
        ProjectManifest& m = m_project.Context().manifest;

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
