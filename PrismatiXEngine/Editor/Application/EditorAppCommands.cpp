#include "Editor/Application/EditorApp.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace px::editor {
void EditorApp::BuildCommands() {
    const auto focus = [](const char* name) { ImGui::SetWindowFocus(name); };
    m_commands = {
        { "Save All", "Ctrl+S", [this] { SaveAll(); } },
        { "Run (Dev)", "F5", [this] { RunDev(); } },
        { "Build", "Ctrl+B", [this] { RunBuild(); } },
        { "Build & Run",
          "",
          [this] {
              RunBuild();
              RunPackaged();
          } },
        { "Open Output Folder",
          "",
          [this] {
              std::error_code ec;
              std::filesystem::create_directories(m_project.Context().ExportRoot(), ec);
              OpenInExplorer(m_project.Context().ExportRoot());
          } },
        { "Reload Preview",
          "",
          [this] {
              if (m_preview) m_preview->Reload();
          } },
        { "Rescan Assets", "", [this] { m_assets.Scan(m_project.Context()); } },
        { "Refresh Problems", "", [this] { RefreshProblems(); } },
        { "Reset Layout", "", [this] { m_buildLayout = true; } },
        { "Back to Welcome", "", [this] { m_screen = Screen::Welcome; } },
        { "Undo", "Ctrl+Z", [this] { m_undo.Undo(); } },
        { "Redo", "Ctrl+Y", [this] { m_undo.Redo(); } },
        { "Go to: Preview", "", [focus] { focus("Preview"); } },
        { "Go to: Story (Node Editor)", "", [focus] { focus("Node Editor"); } },
        { "Go to: Flow", "", [focus] { focus("Flow"); } },
        { "Go to: Scripting", "", [focus] { focus("Scripting"); } },
        { "Go to: Database", "", [focus] { focus("Database"); } },
        { "Go to: Animation", "", [focus] { focus("Animation"); } },
        { "Go to: Build", "", [focus] { focus("Build"); } },
        { "Go to: Assets", "", [focus] { focus("Assets"); } },
        { "Go to: Problems", "", [focus] { focus("Problems"); } },
    };
}

void EditorApp::HandleShortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal)) {
        m_paletteOpen = true;
        m_paletteFocus = true;
        m_paletteFilter[0] = 0;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        SaveAll();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal)) {
        RunBuild();
    }
    if (ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal)) {
        RunDev();
    }
}

void EditorApp::RenderCommandPalette() {
    if (m_paletteOpen) {
        ImGui::OpenPopup("##cmdPalette");
        m_paletteOpen = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->Pos.y + 120), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##cmdPalette")) {
        if (m_paletteFocus) {
            ImGui::SetKeyboardFocusHere();
            m_paletteFocus = false;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##cmdfilter", "Type a command...", m_paletteFilter, sizeof(m_paletteFilter));
        ImGui::Separator();

        std::string filter = m_paletteFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto matches = [&](const std::string& label) {
            if (filter.empty()) return true;
            std::string low = label;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return low.find(filter) != std::string::npos;
        };

        bool runFirst = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        bool ranOne = false;
        if (ImGui::BeginChild("##cmdlist", ImVec2(0, 300))) {
            for (const PaletteCommand& cmd : m_commands) {
                if (!matches(cmd.label)) continue;
                const std::string row = cmd.shortcut.empty() ? cmd.label : (cmd.label + "\t" + cmd.shortcut);
                const bool clicked = ImGui::Selectable(row.c_str());
                if (clicked || (runFirst && !ranOne)) {
                    if (cmd.run) cmd.run();
                    ranOne = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


}  // namespace px::editor
