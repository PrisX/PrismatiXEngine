#define IMGUI_DEFINE_MATH_OPERATORS

#include "EditorApp.h"

#include <SDL2/SDL.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

void EditorApp::RenderWorkspace() {
    ImGui::Begin("Workspace");
    if (ImGui::BeginTabBar("WorkspaceTabs")) {
        if (ImGui::BeginTabItem("Entrypoint")) {
            m_activeTab = WorkspaceTab::Entrypoint;
            m_lastDocumentTab = WorkspaceTab::Entrypoint;
            m_entrypointEditor.Render();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scene Graph")) {
            m_activeTab = WorkspaceTab::SceneScript;
            m_lastDocumentTab = WorkspaceTab::SceneScript;
            m_sceneEditor.Render();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("UI Editor")) {
            m_activeTab = WorkspaceTab::UIDesign;
            m_lastDocumentTab = WorkspaceTab::UIDesign;
            m_uiDesigner.Render(0.0f, [this](const ImRect&, float, int mouseX, int mouseY, bool leftClick, bool rightClick) {
                return RenderRuntimeScene(
                    m_uiDesigner.GeneratedSceneScriptPath(),
                    m_uiDesigner.SceneWidth(),
                    m_uiDesigner.SceneHeight(),
                    mouseX,
                    mouseY,
                    leftClick,
                    rightClick);
            });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game Preview")) {
            m_activeTab = WorkspaceTab::Preview;
            RenderGamePreviewTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Export")) {
            m_activeTab = WorkspaceTab::Export;
            RenderExportManager();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

UIDesigner::RuntimeCanvasResult EditorApp::RenderRuntimeScene(
    const std::string& scenePath,
    int logicalWidth,
    int logicalHeight,
    int mouseX,
    int mouseY,
    bool leftClick,
    bool rightClick) {
    SyncUIDesignerExports();

    m_gamePreview.SetWorkspaceRoot(m_workspaceRoot);
    m_gamePreview.SetProjectRoot(m_projectRoot);
    m_gamePreview.SetScenePath(scenePath);
    m_gamePreview.SetStoryScriptPath(m_sceneEditor.CurrentDocumentRuntimePath().empty() ? "Script/chapter1.pds" : m_sceneEditor.CurrentDocumentRuntimePath());
    m_gamePreview.SetViewportSize(logicalWidth, logicalHeight);

    UIDesigner::RuntimeCanvasResult result;
    result.rendered = m_gamePreview.RenderFrame(m_window, m_renderer, mouseX, mouseY, leftClick, rightClick) && m_gamePreview.GetTexture();
    result.texture = m_gamePreview.GetTexture();
    result.status = m_gamePreview.Status();
    return result;
}

void EditorApp::RenderResources() {
    ImGui::Begin("Resources");
    if (HasProjectLoaded()) {
        const std::string projectLabel = "Project: " + m_projectName;
        ImGui::TextUnformatted(projectLabel.c_str());
        ImGui::TextDisabled("%s", m_projectRoot.string().c_str());
        ImGui::Separator();
    }
    ImGui::InputTextWithHint("##resource-filter", "Search assets or scripts...", &m_resourceFilter);
    ImGui::Separator();

    const std::string filterLower = [&]() {
        std::string lowered = m_resourceFilter;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return lowered;
    }();

    std::function<bool(const fs::path&)> hasMatch = [&](const fs::path& path) -> bool {
        if (filterLower.empty()) {
            return true;
        }

        std::string lowered = path.filename().string();
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lowered.find(filterLower) != std::string::npos) {
            return true;
        }

        if (fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (hasMatch(entry.path())) {
                    return true;
                }
            }
        }
        return false;
    };

    std::function<void(const fs::path&)> drawTree = [&](const fs::path& root) {
        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (hasMatch(entry.path())) {
                entries.push_back(entry);
            }
        }

        std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& left, const fs::directory_entry& right) {
            if (left.is_directory() != right.is_directory()) {
                return left.is_directory() > right.is_directory();
            }
            return left.path().filename().string() < right.path().filename().string();
        });

        for (const auto& entry : entries) {
            const fs::path path = entry.path();
            if (entry.is_directory()) {
                if (ImGui::TreeNodeEx(path.filename().string().c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow)) {
                    drawTree(path);
                    ImGui::TreePop();
                }
            } else {
                const bool selected = m_selectedAsset == path;
                if (ImGui::Selectable(path.filename().string().c_str(), selected)) {
                    m_selectedAsset = path;
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    const std::string runtimePath = ToRuntimePath(path);
                    ImGui::SetDragDropPayload("PX_RESOURCE_PATH", runtimePath.c_str(), runtimePath.size() + 1);
                    ImGui::TextUnformatted(runtimePath.c_str());
                    ImGui::EndDragDropSource();
                }
            }
        }
    };

    const auto roots = ExplorerRoots();
    for (const auto& root : roots) {
        if (!fs::exists(root)) {
            continue;
        }
        ImGui::SeparatorText(root.filename().string().c_str());
        drawTree(root);
    }

    ImGui::Separator();
    if (!m_selectedAsset.empty()) {
        const WorkspaceTab documentTab = CurrentDocumentTab();
        const std::string runtimePath = ToRuntimePath(m_selectedAsset);
        ImGui::TextWrapped("%s", runtimePath.c_str());
        if (documentTab == WorkspaceTab::UIDesign || documentTab == WorkspaceTab::Preview || documentTab == WorkspaceTab::Export) {
            if (ImGui::Button("Apply To UI Editor")) {
                m_uiDesigner.ApplyAssetToSelection(runtimePath);
            }
        } else {
            const std::string buttonLabel = documentTab == WorkspaceTab::SceneScript
                ? "Copy Path For Scene Graph"
                : ("Apply To " + WorkspaceTabLabel(documentTab));
            if (ImGui::Button(buttonLabel.c_str())) {
                if (documentTab == WorkspaceTab::Entrypoint) {
                    m_entrypointEditor.ApplyAssetToSelection(runtimePath);
                } else if (documentTab == WorkspaceTab::SceneScript) {
                    m_sceneEditor.ApplyAssetToSelection(runtimePath);
                }
            }
        }
    } else {
        ImGui::TextDisabled("Select a file to preview or drag into the editor.");
    }

    ImGui::SeparatorText("Asset Preview");
    RenderAssetPreview();
    ImGui::End();
}

void EditorApp::RenderInspector() {
    ImGui::Begin("Inspector");
    ImGui::PushFont(m_headingFont ? m_headingFont : ImGui::GetFont());
    ImGui::TextUnformatted("Inspector");
    ImGui::PopFont();
    ImGui::Separator();

    const WorkspaceTab documentTab = CurrentDocumentTab();
    if (m_activeTab == WorkspaceTab::Preview || m_activeTab == WorkspaceTab::Export) {
        const std::string contextLabel = "Editing context: " + WorkspaceTabLabel(documentTab);
        ImGui::TextDisabled("%s", contextLabel.c_str());
        ImGui::Separator();
    }

    if (documentTab == WorkspaceTab::Entrypoint) {
        ImGui::TextDisabled("%s", m_entrypointEditor.GetSelectionSummary().c_str());
        ImGui::Separator();
        m_entrypointEditor.RenderInspector();
    } else if (documentTab == WorkspaceTab::SceneScript) {
        ImGui::TextDisabled("%s", m_sceneEditor.GetSelectionSummary().c_str());
        ImGui::Separator();
        m_sceneEditor.RenderInspector();
    } else {
        ImGui::TextDisabled("%s", m_uiDesigner.GetSelectionSummary().c_str());
        ImGui::Separator();
        m_uiDesigner.RenderInspector();
    }

    if (!m_selectedAsset.empty()) {
        ImGui::SeparatorText("Selected Asset");
        ImGui::TextWrapped("%s", ToRuntimePath(m_selectedAsset).c_str());
    }
    ImGui::End();
}

void EditorApp::RenderGamePreviewTab() {
    SyncUIDesignerExports();

    auto scenes = AvailableSceneScripts();
    if (scenes.empty()) {
        ImGui::TextDisabled("No scene scripts found under %s", RuntimeSceneRoot().string().c_str());
        return;
    }

    if (std::find(scenes.begin(), scenes.end(), m_previewScenePath) == scenes.end()) {
        m_previewScenePath = scenes.front();
    }

    if (ImGui::BeginCombo("Scene", m_previewScenePath.c_str())) {
        for (const std::string& scene : scenes) {
            const bool selected = scene == m_previewScenePath;
            if (ImGui::Selectable(scene.c_str(), selected)) {
                m_previewScenePath = scene;
                m_gamePreview.SetScenePath(scene);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Use UI Scene")) {
        m_previewScenePath = m_uiDesigner.GeneratedSceneScriptPath();
        m_gamePreview.SetScenePath(m_previewScenePath);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload Preview")) {
        m_gamePreview.RequestReload();
    }

    ImGui::Spacing();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x <= 2.0f || available.y <= 2.0f) {
        ImGui::TextDisabled("Preview area is too small.");
        return;
    }

    const ImVec2 outerMin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("preview-runtime-surface", available, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImRect outer(outerMin, outerMin + available);
    const int logicalWidth = m_previewScenePath == m_uiDesigner.GeneratedSceneScriptPath() ? m_uiDesigner.SceneWidth() : 1280;
    const int logicalHeight = m_previewScenePath == m_uiDesigner.GeneratedSceneScriptPath() ? m_uiDesigner.SceneHeight() : 720;
    const float scale = std::max(0.01f, std::min(outer.GetWidth() / static_cast<float>(logicalWidth), outer.GetHeight() / static_cast<float>(logicalHeight)));
    const ImVec2 previewSize(static_cast<float>(logicalWidth) * scale, static_cast<float>(logicalHeight) * scale);
    const ImVec2 previewMin(
        outer.Min.x + (outer.GetWidth() - previewSize.x) * 0.5f,
        outer.Min.y + (outer.GetHeight() - previewSize.y) * 0.5f);
    const ImRect previewRect(previewMin, previewMin + previewSize);

    int previewMouseX = -1000;
    int previewMouseY = -1000;
    bool leftClick = false;
    bool rightClick = false;
    if (previewRect.Contains(ImGui::GetMousePos())) {
        previewMouseX = static_cast<int>((ImGui::GetMousePos().x - previewRect.Min.x) / scale);
        previewMouseY = static_cast<int>((ImGui::GetMousePos().y - previewRect.Min.y) / scale);
        leftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        rightClick = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    }

    const UIDesigner::RuntimeCanvasResult runtimeCanvas = RenderRuntimeScene(
        m_previewScenePath,
        logicalWidth,
        logicalHeight,
        previewMouseX,
        previewMouseY,
        leftClick,
        rightClick);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(previewRect.Min, previewRect.Max, IM_COL32(0, 0, 0, 255), 18.0f);
    drawList->AddRect(previewRect.Min, previewRect.Max, IM_COL32(64, 64, 72, 255), 18.0f, ImDrawFlags_RoundCornersAll, 1.2f);
    if (runtimeCanvas.rendered && runtimeCanvas.texture) {
        drawList->AddImage(runtimeCanvas.texture, previewRect.Min, previewRect.Max);
    } else {
        drawList->AddText(previewRect.Min + ImVec2(18.0f, 18.0f), IM_COL32(180, 180, 188, 255), runtimeCanvas.status.c_str());
    }

    drawList->AddRectFilled(previewRect.Min + ImVec2(14.0f, 14.0f), previewRect.Min + ImVec2(340.0f, 42.0f), IM_COL32(0, 0, 0, 188), 8.0f);
    drawList->AddText(previewRect.Min + ImVec2(24.0f, 22.0f), IM_COL32(188, 188, 196, 255), runtimeCanvas.status.c_str());
}

void EditorApp::RenderAssetPreview() {
    if (m_selectedAsset.empty()) {
        ImGui::TextDisabled("Select an asset or script in the explorer.");
        return;
    }

    ImGui::TextWrapped("%s", ToRuntimePath(m_selectedAsset).c_str());
    if (SDL_Texture* texture = GetTexture(m_selectedAsset)) {
        int textureWidth = 0;
        int textureHeight = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &textureWidth, &textureHeight);
        const float width = static_cast<float>(textureWidth);
        const float height = static_cast<float>(textureHeight);
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float scale = std::min(availableWidth / width, 220.0f / height);
        ImGui::Image(texture, ImVec2(width * scale, height * scale));
    } else {
        ImGui::TextDisabled("No raster preview available for this file type.");
    }
}

void EditorApp::RenderOutput() {
    ImGui::Begin("Output");
    ImGui::TextDisabled("Recent editor activity, preview state and export history.");
    ImGui::Separator();
    RenderActivityLog();
    ImGui::End();
}

void EditorApp::RenderExportManager() {
    SyncUIDesignerExports();
    const auto artifacts = BuildExportArtifacts();
    const fs::path runtimeExe = FindRuntimeExecutable();
    const fs::path folderBuildRoot = RuntimeExportRoot() / "folder_build";
    const fs::path pdxBuildRoot = RuntimeExportRoot() / "pdx_build";
    const fs::path sharedScriptsRoot = WorkspaceScriptsRoot();
    const auto countFiles = [](const fs::path& root) {
        std::uintmax_t count = 0;
        if (!fs::exists(root)) {
            return count;
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file()) {
                ++count;
            }
        }
        return count;
    };

    ImGui::PushFont(m_headingFont ? m_headingFont : ImGui::GetFont());
    ImGui::TextUnformatted("Build & Export");
    ImGui::PopFont();
    ImGui::TextDisabled("Export the complete game package, including executable, story scripts, UI scripts, assets and engine content.");
    ImGui::TextDisabled("%s", RuntimeExportRoot().string().c_str());
    ImGui::Spacing();

    if (ImGui::Button("Export Folder Game", ImVec2(190.0f, 42.0f))) {
        ExportGameFolder();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export PDX Game", ImVec2(190.0f, 42.0f))) {
        ExportGamePdx();
    }

    if (runtimeExe.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.38f, 1.0f), "Executable not found yet. Build the runtime first, otherwise export will only contain data packages.");
    } else {
        ImGui::TextDisabled("Runtime executable: %s", runtimeExe.string().c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Package Contents");
    ImGui::BeginChild("export-packages", ImVec2(0.0f, 210.0f), false);
    ImGui::BeginChild("folder-package", ImVec2(0.0f, 96.0f), true);
    ImGui::TextUnformatted("Folder Game");
    ImGui::TextDisabled("%s", folderBuildRoot.string().c_str());
    ImGui::TextWrapped("Includes: `PrismatiXEngine.exe`, all sibling runtime `.dll`, project `Data/` including `Data/Script/*.pds`, plus `Engine/`.");
    ImGui::TextDisabled("Merge snapshot: %ju project data files, %ju shared runtime script files, %ju engine files", countFiles(RuntimeDataRoot()), countFiles(sharedScriptsRoot), countFiles(RuntimeEngineRoot()));
    ImGui::EndChild();

    ImGui::BeginChild("pdx-package", ImVec2(0.0f, 96.0f), true);
    ImGui::TextUnformatted("PDX Game");
    ImGui::TextDisabled("%s", pdxBuildRoot.string().c_str());
    ImGui::TextWrapped("Includes: `PrismatiXEngine.exe`, all sibling runtime `.dll`, `Data.pdx`, `Engine.pdx`.");
    ImGui::TextDisabled("Project Data, including chapter `.pds` files, is merged with shared runtime `Scripts/`, then packaged into `Data.pdx`.");
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SeparatorText("Editor Documents");
    if (ImGui::Button("Write Active Document")) {
        ExportActive();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write All Documents")) {
        ExportAll();
    }

    for (size_t index = 0; index < artifacts.size(); ++index) {
        const ExportArtifact& artifact = artifacts[index];
        const std::string childId = "export-artifact-" + std::to_string(index);
        ImGui::BeginChild(childId.c_str(), ImVec2(0.0f, 82.0f), true);
        ImGui::TextUnformatted(artifact.label.c_str());
        ImGui::TextDisabled("%s", artifact.path.string().c_str());
        ImGui::TextDisabled("%zu bytes", artifact.content.size());
        if (ImGui::Button(("Write##" + std::to_string(index)).c_str())) {
            ExportDocument(artifact.path, artifact.label, artifact.content);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(("Open Tab##" + std::to_string(index)).c_str())) {
            m_lastDocumentTab = artifact.tab;
        }
        ImGui::EndChild();
    }

    if (ImGui::BeginTabBar("GeneratedLuaPreview")) {
        for (size_t index = 0; index < artifacts.size(); ++index) {
            const ExportArtifact& artifact = artifacts[index];
            if (ImGui::BeginTabItem((artifact.label + "###generated-" + std::to_string(index)).c_str())) {
                std::string content = artifact.content;
                ImGui::InputTextMultiline(("##generated-content-" + std::to_string(index)).c_str(), &content, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void EditorApp::RenderActivityLog() {
    ImGui::BeginChild("activity-log", ImVec2(0.0f, 0.0f), false);
    for (const auto& line : m_logs) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::EndChild();
}

void EditorApp::ExportActive() {
    const WorkspaceTab documentTab = CurrentDocumentTab();
    if (documentTab == WorkspaceTab::UIDesign) {
        const fs::path path = ExportPathFor(documentTab);
        const std::string content = GenerateDocumentFor(documentTab);
        if (!content.empty()) {
            ExportDocument(path, "UI Document", content);
        }
        return;
    }
    if (documentTab == WorkspaceTab::SceneScript) {
        const fs::path path = ExportPathFor(documentTab);
        const std::string content = GenerateDocumentFor(documentTab);
        if (!content.empty()) {
            ExportDocument(path, "PDS Script", content);
        }
        return;
    }

    for (const ExportArtifact& artifact : BuildExportArtifacts()) {
        if (artifact.tab == documentTab) {
            ExportDocument(artifact.path, artifact.label, artifact.content);
        }
    }
}

void EditorApp::ExportAll() {
    for (const ExportArtifact& artifact : BuildExportArtifacts()) {
        ExportDocument(artifact.path, artifact.label, artifact.content);
    }
}

void EditorApp::ExportDocument(const fs::path& filePath, const std::string& label, const std::string& content) {
    fs::create_directories(filePath.parent_path());
    std::ofstream out(filePath, std::ios::binary);
    out << content;
    Log("Exported " + label + " to " + filePath.string());
}

}  // namespace PrismatiX::Editor
