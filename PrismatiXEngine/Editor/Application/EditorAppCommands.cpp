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
        { "Reload Preview",
          "",
          [this] {
              if (m_preview) m_preview->Reload();
          } },
        { "Import Clipboard Files", "Ctrl+V", [this] { ImportClipboardAssets(); } },
        { "Rescan Assets", "", [this] { m_assets.Scan(m_project.Context()); } },
        { "Refresh Problems", "", [this] { RefreshProblems(); } },
        { "Reset Layout", "", [this] { m_buildLayout = true; } },
        { "Back to Welcome", "", [this] { m_screen = Screen::Welcome; } },
        { "Undo", "Ctrl+Z", [this] { if (m_previewMode == 0 && m_designer.Document()) m_designer.Undo(); else if(auto* graph=ActiveDocPtr())graph->Undo(); } },
        { "Redo", "Ctrl+Y", [this] { if (m_previewMode == 0 && m_designer.Document()) m_designer.Redo(); else if(auto* graph=ActiveDocPtr())graph->Redo(); } },
        { "Go to: Preview", "", [focus] { focus("Preview"); } },
        { "Go to: Story (Node Editor)", "", [focus] { focus("Node Editor"); } },
        { "Go to: Flow", "", [focus] { focus("Flow"); } },
        { "Go to: Scripting", "", [focus] { focus("Scripting"); } },
        { "Go to: Animation", "", [focus] { focus("Animation"); } },
        { "Go to: Build", "", [focus] { focus("Build"); } },
        { "Go to: Assets", "", [focus] { focus("Assets"); } },
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
    if (!ImGui::GetIO().WantTextInput &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Tab, ImGuiInputFlags_RouteGlobal)) {
        const auto recent = m_docs.RecentlyUsed();
        if (recent.size() > 1) {
            const auto& session = m_docs.Documents()[recent[1]];
            (void)m_docs.Activate(session.id);
            std::error_code error;
            const auto runtime = std::filesystem::relative(session.id.canonicalPath,
                m_project.Context().root, error).generic_string();
            if (!error && session.type == DocumentType::Scenario) OpenDocTab(runtime);
            else if (!error && session.type == DocumentType::UIScene && m_preview) {
                SetWorkspace(EditorWorkspace::UI); m_preview->LoadUI(runtime); SyncDesigner();
            } else if(!error&&session.type==DocumentType::Lua){SetWorkspace(EditorWorkspace::Script);m_scripts.OpenFile(runtime);}
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        SaveAll();
    }
    // Ctrl+V pastes nodes when the graph is focused; clipboard import otherwise.
    if (!ImGui::GetIO().WantTextInput && !m_nodeEditorFocused &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal)) {
        ImportClipboardAssets();
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
    // Ctrl+Z/Y route to the active per-document EditHistory.
    if (!ImGui::GetIO().WantTextInput &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        NodeGraphEditor* doc = ActiveDocPtr();
        if (m_assetsFocused && m_projectHistory.CanUndo()) {
            m_projectHistory.Undo();
        } else if (m_nodeEditorFocused && doc && doc->CanUndo()) {
            doc->Undo();
        } else if (m_previewMode == 0 && m_designer.CanUndo()) {
            m_designer.Undo();
        }
    }
    if (!ImGui::GetIO().WantTextInput &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal)) {
        NodeGraphEditor* doc = ActiveDocPtr();
        if (m_assetsFocused && m_projectHistory.CanRedo()) {
            m_projectHistory.Redo();
        } else if (m_nodeEditorFocused && doc && doc->CanRedo()) {
            doc->Redo();
        } else if (m_previewMode == 0 && m_designer.CanRedo()) {
            m_designer.Redo();
        }
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
            row("Ctrl+Z / Ctrl+Y", "Undo / Redo");
            row("Ctrl+V", "Import copied files into Assets");
            row("F1", "Toggle this cheat sheet");
            ImGui::EndTable();
        }
        if (beginSection("UI Designer (canvas)", "sc-ui")) {
            row("Drag node", "Move; snaps to guides / grid");
            row("Drag handles", "Resize from any of the 8 edges/corners");
            row("Delete / Backspace", "Delete selected UI node");
            row("Alt + drag", "Move/resize freely (disable snapping)");
            row("G", "Toggle snap grid (center-snaps to cells)");
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
        if (beginSection("Flow", "sc-flow")) {
            row("Drag pin -> pin", "Link chapters in the flow map");
            row("Delete / Backspace", "Delete selected flow node or link");
            row("Double-click node", "Open chapter in Story");
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
