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
        if (ImGui::BeginTabItem("Scene Script")) {
            m_activeTab = WorkspaceTab::SceneScript;
            m_lastDocumentTab = WorkspaceTab::SceneScript;
            m_sceneEditor.Render();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("UI Designer")) {
            m_activeTab = WorkspaceTab::UIDesign;
            m_lastDocumentTab = WorkspaceTab::UIDesign;
            m_uiDesigner.Render(0.0f);
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

        const std::string fileName = path.filename().string();
        std::string lowered = fileName;
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

        std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& lhs, const fs::directory_entry& rhs) {
            if (lhs.is_directory() != rhs.is_directory()) {
                return lhs.is_directory() > rhs.is_directory();
            }
            return lhs.path().filename().string() < rhs.path().filename().string();
        });

        for (const auto& entry : entries) {
            const fs::path path = entry.path();
            if (entry.is_directory()) {
                const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                if (ImGui::TreeNodeEx(path.filename().string().c_str(), flags)) {
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

    if (roots.empty()) {
        ImGui::TextDisabled("This project does not have any resource folders yet.");
    }

    ImGui::Separator();
    if (!m_selectedAsset.empty()) {
        const WorkspaceTab documentTab = CurrentDocumentTab();
        const std::string runtimePath = ToRuntimePath(m_selectedAsset);
        const std::string buttonLabel = "Apply To " + WorkspaceTabLabel(documentTab);
        ImGui::TextWrapped("%s", runtimePath.c_str());
        if (ImGui::Button(buttonLabel.c_str())) {
            if (documentTab == WorkspaceTab::Entrypoint) {
                m_entrypointEditor.ApplyAssetToSelection(runtimePath);
            } else if (documentTab == WorkspaceTab::SceneScript) {
                m_sceneEditor.ApplyAssetToSelection(runtimePath);
            } else {
                m_uiDesigner.ApplyAssetToSelection(runtimePath);
            }
        }
    } else {
        ImGui::TextDisabled("Select a file to preview or apply it.");
    }

    ImGui::End();
}

void EditorApp::RenderInspector() {
    ImGui::Begin("Inspector");
    ImGui::PushFont(m_headingFont ? m_headingFont : ImGui::GetFont());
    ImGui::TextUnformatted("Selection");
    ImGui::PopFont();
    ImGui::Separator();

    const WorkspaceTab documentTab = CurrentDocumentTab();
    if (m_activeTab == WorkspaceTab::Export) {
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
        ImGui::SeparatorText("Explorer Asset");
        ImGui::TextWrapped("%s", ToRuntimePath(m_selectedAsset).c_str());
        ImGui::TextDisabled(IsImageAsset(m_selectedAsset) ? "Image asset" : "Script or generic file");
    }
    ImGui::End();
}

void EditorApp::RenderPreview() {
    ImGui::Begin("Preview");

    const WorkspaceTab documentTab = CurrentDocumentTab();
    if (m_activeTab == WorkspaceTab::Export) {
        const std::string previewLabel = "Previewing " + WorkspaceTabLabel(documentTab) + " while Export tab is active.";
        ImGui::TextDisabled("%s", previewLabel.c_str());
        ImGui::Separator();
    }

    if (documentTab == WorkspaceTab::UIDesign) {
        m_uiDesigner.RenderPreview();
    } else {
        ImGui::BeginChild("graph-preview", ImVec2(0.0f, 320.0f), true);
        const auto lines = ActivePreviewLines();
        if (documentTab == WorkspaceTab::Entrypoint) {
            RenderEntrypointPreview(lines);
        } else {
            RenderScenePreview(lines);
        }
        ImGui::EndChild();
    }

    ImGui::SeparatorText("Asset Preview");
    RenderAssetPreview();
    ImGui::End();
}

void EditorApp::RenderEntrypointPreview(const std::vector<std::string>& lines) {
    const ImRect rect(ImGui::GetWindowContentRegionMin() + ImGui::GetWindowPos(), ImGui::GetWindowContentRegionMax() + ImGui::GetWindowPos());
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(rect.Min, rect.Max, IM_COL32(11, 16, 26, 255), 16.0f);
    drawList->AddRect(rect.Min, rect.Max, IM_COL32(94, 132, 176, 160), 16.0f, ImDrawFlags_RoundCornersAll, 1.2f);

    drawList->AddText(ImVec2(rect.Min.x + 18.0f, rect.Min.y + 18.0f), IM_COL32(241, 245, 249, 255), "Entrypoint Loop Preview");
    drawList->AddText(ImVec2(rect.Min.x + 18.0f, rect.Min.y + 40.0f), IM_COL32(164, 178, 196, 255), "Boot chain and frame loop exported from the visual graph.");

    float y = rect.Min.y + 82.0f;
    for (size_t index = 0; index < lines.size(); ++index) {
        const ImVec2 min(rect.Min.x + 18.0f, y);
        const ImVec2 max(rect.Max.x - 18.0f, y + 34.0f);
        drawList->AddRectFilled(min, max, index % 2 == 0 ? IM_COL32(26, 34, 50, 215) : IM_COL32(18, 26, 39, 215), 9.0f);
        drawList->AddRect(min, max, IM_COL32(104, 136, 176, 120), 9.0f);
        drawList->AddText(ImVec2(min.x + 12.0f, min.y + 8.0f), IM_COL32(232, 237, 243, 255), lines[index].c_str());
        y += 40.0f;
    }
}

void EditorApp::RenderScenePreview(const std::vector<std::string>& lines) {
    std::string background = "Scene preview stage";
    std::string speaker = "Speaker";
    std::string line = "Use the graph to assemble beats, transitions and branching dialogue.";
    std::string accent = "Transition: dissolve";

    for (const auto& entry : lines) {
        if (entry.rfind("Background:", 0) == 0) {
            background = entry.substr(std::string("Background: ").size());
        } else if (entry.rfind("Speaker:", 0) == 0) {
            speaker = entry.substr(std::string("Speaker: ").size());
        } else if (entry.rfind("Line:", 0) == 0) {
            line = entry.substr(std::string("Line: ").size());
        } else if (entry.rfind("Transition:", 0) == 0) {
            accent = entry;
        }
    }

    const ImRect rect(ImGui::GetWindowContentRegionMin() + ImGui::GetWindowPos(), ImGui::GetWindowContentRegionMax() + ImGui::GetWindowPos());
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilledMultiColor(rect.Min, rect.Max, IM_COL32(18, 24, 38, 255), IM_COL32(30, 36, 58, 255), IM_COL32(9, 12, 20, 255), IM_COL32(14, 18, 28, 255));
    drawList->AddText(ImVec2(rect.Min.x + 18.0f, rect.Min.y + 16.0f), IM_COL32(241, 245, 249, 255), background.c_str());
    drawList->AddText(ImVec2(rect.Min.x + 18.0f, rect.Min.y + 40.0f), IM_COL32(164, 178, 196, 255), accent.c_str());

    const ImVec2 panelMin(rect.Min.x + 32.0f, rect.Max.y - 120.0f);
    const ImVec2 panelMax(rect.Max.x - 32.0f, rect.Max.y - 28.0f);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(10, 14, 22, 228), 16.0f);
    drawList->AddRect(panelMin, panelMax, IM_COL32(255, 214, 143, 110), 16.0f, ImDrawFlags_RoundCornersAll, 1.4f);
    drawList->AddText(ImVec2(panelMin.x + 18.0f, panelMin.y + 16.0f), IM_COL32(255, 214, 143, 255), speaker.c_str());
    drawList->AddText(ImVec2(panelMin.x + 18.0f, panelMin.y + 44.0f), IM_COL32(238, 242, 248, 255), line.c_str());
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
    ImGui::TextDisabled("Recent editor activity and export history.");
    ImGui::Separator();
    RenderActivityLog();
    ImGui::End();
}

void EditorApp::RenderExportManager() {
    const WorkspaceTab documentTab = CurrentDocumentTab();
    const auto artifacts = BuildExportArtifacts();

    ImGui::PushFont(m_headingFont ? m_headingFont : ImGui::GetFont());
    ImGui::TextUnformatted("Export Manager");
    ImGui::PopFont();
    const std::string exportRoot = HasProjectLoaded() ? (m_projectRoot / "Scripts" / "Generated").string() : std::string{};
    ImGui::TextDisabled("Generated files follow the active project and can be exported individually or all at once.");
    if (!exportRoot.empty()) {
        ImGui::TextDisabled("%s", exportRoot.c_str());
    }
    ImGui::Spacing();

    if (ImGui::Button("Export Active")) {
        ExportActive();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export All")) {
        ExportAll();
    }
    ImGui::SameLine();
    const std::string activeLabel = "Active authoring tab: " + WorkspaceTabLabel(documentTab);
    ImGui::TextDisabled("%s", activeLabel.c_str());

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float cardWidth = std::max(220.0f, (availableWidth - spacing * 2.0f) / 3.0f);

    ImGui::Spacing();
    for (size_t index = 0; index < artifacts.size(); ++index) {
        const ExportArtifact& artifact = artifacts[index];
        if (index > 0) {
            ImGui::SameLine();
        }

        const std::string childId = "export-card-" + std::to_string(index);
        ImGui::BeginChild(childId.c_str(), ImVec2(cardWidth, 126.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextUnformatted(artifact.label.c_str());
        if (artifact.tab == documentTab) {
            ImGui::SameLine();
            ImGui::TextDisabled("(active)");
        }

        const std::string outputPath = ToRuntimePath(artifact.path);
        ImGui::TextDisabled("%s", artifact.path.filename().string().c_str());
        ImGui::TextWrapped("%s", outputPath.c_str());
        ImGui::TextDisabled("%zu bytes", artifact.content.size());

        const std::string exportButton = "Export##export-" + std::to_string(index);
        if (ImGui::Button(exportButton.c_str())) {
            ExportDocument(artifact.path, artifact.label, artifact.content);
        }

        ImGui::SameLine();
        const std::string openButton = "Open##open-" + std::to_string(index);
        if (ImGui::SmallButton(openButton.c_str())) {
            m_activeTab = artifact.tab;
            m_lastDocumentTab = artifact.tab;
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Generated Preview");
    if (ImGui::BeginTabBar("ExportPreviewTabs")) {
        for (size_t index = 0; index < artifacts.size(); ++index) {
            const ExportArtifact& artifact = artifacts[index];
            const std::string tabLabel = artifact.label + "###export-preview-" + std::to_string(index);
            if (ImGui::BeginTabItem(tabLabel.c_str())) {
                std::string content = artifact.content;
                const std::string inputId = "##export-preview-buffer-" + std::to_string(index);
                ImGui::InputTextMultiline(inputId.c_str(), &content, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
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
    ExportDocument(ExportPathFor(documentTab), WorkspaceTabLabel(documentTab), GenerateDocumentFor(documentTab));
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

std::string EditorApp::ActiveLuaPreview() const {
    return GenerateDocumentFor(CurrentDocumentTab());
}

std::vector<std::string> EditorApp::ActivePreviewLines() const {
    const WorkspaceTab documentTab = CurrentDocumentTab();
    if (documentTab == WorkspaceTab::Entrypoint) {
        return m_entrypointEditor.BuildPreviewLines();
    }
    if (documentTab == WorkspaceTab::SceneScript) {
        return m_sceneEditor.BuildPreviewLines();
    }
    return m_uiDesigner.BuildPreviewLines();
}

}  // namespace PrismatiX::Editor
