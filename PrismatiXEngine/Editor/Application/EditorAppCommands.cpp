#include <imgui.h>

#include <algorithm>
#include <cctype>

#include "Editor/Application/EditorApp.h"

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
        { "Rescan Asset Identities", "", [this] { const Status scanned=m_assetRegistry.Scan(m_project.Context().root);if(!scanned)m_showAssetIdentity=true; } },
        { "Refresh Problems", "", [this] { RefreshProblems(); } },
        { "Reset Layout", "", [this] { m_buildLayout = true; } },
        { "Back to Welcome", "", [this] { m_screen = Screen::Welcome; } },
        { "Go to: Narrative", "", [focus] { focus("Narrative"); } },
        { "Go to: Scripting", "", [focus] { focus("Scripting"); } },
        { "Go to: Animation", "", [focus] { focus("Animation"); } },
        { "Go to: Build", "", [focus] { focus("Build"); } },
        { "Go to: Problems", "", [focus] { focus("Problems"); } },
        { "Go to: Localization", "", [focus] { focus("Localization"); } },
    };
}

void EditorApp::HandleShortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal)) {
        m_paletteOpen = true;
        m_paletteFocus = true;
        m_paletteFilter[0] = 0;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, ImGuiInputFlags_RouteGlobal)) {
        m_quickOpenOpen = true;
        m_quickOpenFilter[0] = 0;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        SaveAll();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B, ImGuiInputFlags_RouteGlobal)) {
        RunBuild();
        RunPackaged();
    }
    else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, ImGuiInputFlags_RouteGlobal)) {
        RunBuild();
    }
    if (ImGui::Shortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal)) {
        RunDev();
    }
    if (ImGui::Shortcut(ImGuiKey_F1, ImGuiInputFlags_RouteGlobal)) {
        m_showShortcuts = !m_showShortcuts;
    }
}

void EditorApp::RenderShortcutsWindow() {
    if (!m_showShortcuts) {
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Keyboard Shortcuts", &m_showShortcuts, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        const auto row = [](const char* keys, const char* action) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(0.62f, 0.78f, 1.0f, 1.0f), "%s", keys);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(action);
        };
        const auto beginSection = [](const char* title, const char* id) {
            ImGui::SeparatorText(title);
            if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch);
                return true;
            }
            return false;
        };

        if (beginSection("General", "sc-general")) {
            row("Ctrl+P", "Command palette");
            row("Ctrl+S", "Save all");
            row("Ctrl+B", "Build");
            row("Ctrl+Shift+B", "Build & run packaged");
            row("F5", "Run (dev)");
            row("Ctrl+V", "Import copied files into Assets");
            row("F1", "Toggle this cheat sheet");
            ImGui::EndTable();
        }
        if (beginSection("Node Graph", "sc-node")) {
            row("Right-click canvas", "Create node");
            row("Drag pin -> pin", "Link nodes");
            row("Delete / Backspace", "Delete selected node or link");
            row("Right-click node/link", "Context menu");
            row("Drag from Resources", "Drop asset onto a node");
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Press F1 or close this window to dismiss.");
    }
    ImGui::End();
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
            for (std::size_t commandIndex = 0; commandIndex < m_commands.size(); ++commandIndex) {
                const PaletteCommand& cmd = m_commands[commandIndex];
                if (!matches(cmd.label)) continue;
                const std::string row = cmd.shortcut.empty() ? cmd.label : (cmd.label + "\t" + cmd.shortcut);
                const bool clicked = ImGui::Selectable(
                    (row + "##command-" + std::to_string(commandIndex)).c_str());
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
