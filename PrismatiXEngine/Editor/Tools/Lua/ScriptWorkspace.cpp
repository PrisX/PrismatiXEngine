#include "Editor/Tools/Lua/ScriptWorkspace.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <array>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace px::editor {

namespace {

std::vector<std::pair<std::string, std::string>> ParseKeyValues(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const size_t keyStart = i;
        while (i < s.size() && s[i] != '=' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size() || s[i] != '=') {
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            continue;
        }
        std::string key = s.substr(keyStart, i - keyStart);
        ++i;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            ++i;
            const size_t vs = i;
            while (i < s.size() && s[i] != '"') ++i;
            value = s.substr(vs, i - vs);
            if (i < s.size()) ++i;
        } else {
            const size_t vs = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            value = s.substr(vs, i - vs);
        }
        if (!key.empty()) out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

std::string FindString(const std::vector<std::pair<std::string, std::string>>& kv,
                       const std::string& key) {
    for (const auto& [k, v] : kv) {
        if (k == key) return v;
    }
    return {};
}

std::string ExtractRegisteredName(const std::string& line) {
    const size_t call = line.find("RegisterCommand");
    if (call == std::string::npos) return {};
    size_t i = line.find('(', call);
    if (i == std::string::npos) return {};
    ++i;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size() || (line[i] != '"' && line[i] != '\'')) return {};
    const char quote = line[i++];
    const size_t start = i;
    while (i < line.size() && line[i] != quote) ++i;
    return line.substr(start, i - start);
}

}

void ScriptWorkspace::SetProject(const ProjectContext* context) {
    m_project = context;
    m_currentFile.clear();
    m_buffer.clear();
    m_dirty = false;
    Rescan();
}

void ScriptWorkspace::Rescan() {
    RefreshFiles();
    ScanCommands();
}

void ScriptWorkspace::RefreshFiles() {
    m_files.clear();
    if (!m_project || !m_project->IsOpen()) return;
    const fs::path root = m_project->root;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() == ".lua") {
            m_files.push_back(fs::relative(it->path(), root, ec).generic_string());
        }
    }
}

void ScriptWorkspace::ScanCommands() {
    m_commands.clear();
    if (!m_project || !m_project->IsOpen()) return;
    const fs::path root = m_project->root;

    for (const std::string& rel : m_files) {
        std::ifstream in(root / rel, std::ios::binary);
        if (!in) continue;

        CustomCommandDef pending;
        bool hasAnnotation = false;
        std::string line;
        while (std::getline(in, line)) {
            const size_t at = line.find("--@");
            if (at != std::string::npos) {
                const std::string rest = line.substr(at + 3);
                if (rest.rfind("command", 0) == 0) {
                    const auto kv = ParseKeyValues(rest.substr(7));
                    pending.name = FindString(kv, "name");
                    pending.category = FindString(kv, "category");
                    pending.description = FindString(kv, "desc");
                    if (pending.category.empty()) pending.category = "Custom";
                    hasAnnotation = true;
                } else if (rest.rfind("param", 0) == 0) {
                    const auto kv = ParseKeyValues(rest.substr(5));
                    CustomCommandParam p;
                    p.key = FindString(kv, "key");
                    if (p.key.empty()) p.key = FindString(kv, "name");
                    p.defaultValue = FindString(kv, "default");
                    if (!p.key.empty()) pending.params.push_back(std::move(p));
                    hasAnnotation = true;
                }
                continue;
            }
            const std::string registered = ExtractRegisteredName(line);
            if (!registered.empty()) {
                CustomCommandDef def = pending;
                if (def.name.empty()) def.name = registered;
                if (def.category.empty()) def.category = "Custom";
                def.sourceFile = rel;
                m_commands.push_back(std::move(def));
                pending = CustomCommandDef{};
                hasAnnotation = false;
            } else if (hasAnnotation && line.find_first_not_of(" \t\r\n") == std::string::npos) {
                pending = CustomCommandDef{};
                hasAnnotation = false;
            }
        }
    }

    Log("Scripting: discovered " + std::to_string(m_commands.size()) + " custom command(s).");
    if (m_onCommands) m_onCommands(m_commands);
}

void ScriptWorkspace::LoadFile(const std::string& runtimePath) {
    if (!m_project) return;
    if (m_dirty) SaveCurrent();
    std::ifstream in(m_project->root / runtimePath, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    m_buffer = ss.str();
    m_currentFile = runtimePath;
    m_dirty = false;
}

void ScriptWorkspace::SaveCurrent() {
    if (!m_project || m_currentFile.empty()) return;
    std::ofstream out(m_project->root / m_currentFile, std::ios::binary);
    out << m_buffer;
    m_dirty = false;
    Log("Saved " + m_currentFile);
    ScanCommands();
}

void ScriptWorkspace::Render() {
    const float listW = 260.0f;
    ImGui::BeginChild("scriptsLeft", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    RenderFileList();
    ImGui::Separator();
    RenderCommandList();
    ImGui::Separator();
    RenderApiReference();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("scriptsRight", ImVec2(0, 0));
    RenderEditor();
    ImGui::EndChild();
}

void ScriptWorkspace::RenderFileList() {
    ImGui::TextDisabled("Lua Scripts");
    if (ImGui::SmallButton("Rescan")) Rescan();
    ImGui::SameLine();
    if (ImGui::SmallButton("New")) ImGui::OpenPopup("newLua");
    if (ImGui::BeginPopup("newLua")) {
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("name", m_newName, sizeof(m_newName));
        ImGui::SameLine();
        if (ImGui::Button("Create") && m_project && m_project->IsOpen()) {
            const fs::path rel = fs::path("Data") / "Scripts" / (std::string(m_newName) + ".lua");
            const fs::path abs = m_project->root / rel;
            std::error_code ec;
            fs::create_directories(abs.parent_path(), ec);
            if (!fs::exists(abs)) {
                std::ofstream out(abs);
                out << "-- " << m_newName << ".lua  (PrismatiX extension script)\n"
                       "-- Declare a custom VN command (it appears as a node in the Story editor):\n"
                       "--@command name=\"shake\" category=\"Custom\" desc=\"Shake the screen\"\n"
                       "--@param key=\"power\" default=\"8\"\n"
                       "Engine.RegisterCommand(\"shake\", function(args)\n"
                       "    Engine.log(\"shake power=\" .. tostring(args.power))\n"
                       "end)\n";
            }
            Rescan();
            LoadFile(rel.generic_string());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    if (m_files.empty()) {
        ImGui::TextDisabled("(no .lua files)");
    }
    for (const std::string& f : m_files) {
        if (ImGui::Selectable(f.c_str(), f == m_currentFile)) {
            LoadFile(f);
        }
    }
}

void ScriptWorkspace::RenderCommandList() {
    if (!ImGui::CollapsingHeader("Custom Commands", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (m_commands.empty()) {
        ImGui::TextDisabled("None. Use Engine.RegisterCommand");
        ImGui::TextDisabled("+ --@command annotations.");
        return;
    }
    for (const CustomCommandDef& c : m_commands) {
        ImGui::BulletText("%s", c.name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("[%s] category=%s\n%s\nfrom %s\nparams: %zu", c.name.c_str(),
                              c.category.c_str(), c.description.c_str(), c.sourceFile.c_str(),
                              c.params.size());
        }
    }
}

void ScriptWorkspace::RenderApiReference() {
    if (!ImGui::CollapsingHeader("API Reference")) return;
    struct Entry {
        const char* sig;
        const char* desc;
    };
    struct Group {
        const char* name;
        std::vector<Entry> entries;
    };
    static const std::vector<Group> kApi = {
        { "Core", {
            { "Engine.log(msg)", "Write to the engine log." },
            { "Engine.RegisterCommand(name, fn)", "Register a custom [name ...] VN command." },
            { "Engine.On(event, fn)", "Subscribe to an engine event." },
            { "Engine.Emit(event, payload?)", "Emit an event to subscribers." },
        } },
        { "Progress (read-only)", {
            { "Engine.HasSeen(key) / MarkSeen(key)", "Read/record a seen flag." },
            { "Engine.ClearCount()", "NG+ clear count." },
            { "Engine.CGUnlocked(id) / UnlockCG(id)", "Query/unlock a CG." },
            { "Engine.SceneUnlocked(id) / UnlockScene(id)", "Query/unlock a scene." },
            { "Engine.PersistentVar(name, value?)", "Get/set a persistent variable." },
        } },
        { "Audio", {
            { "Engine.PlaySE(path)", "Play a sound effect." },
            { "Engine.PlayBGM(path, loop?, fade?)", "Play background music." },
            { "Engine.StopBGM(fade?)", "Stop background music." },
            { "Engine.SetBGMVolume/SetSEVolume/SetVoiceVolume(v)", "Set a bus volume (0-100)." },
        } },
        { "Input", {
            { "Engine.GetMouseX() / GetMouseY()", "Cursor position." },
            { "Engine.GetLeftClick() / GetRightClick()", "Mouse button edge." },
        } },
        { "Draw", {
            { "Engine.GetLogicalSize()", "Returns w, h of the logical canvas." },
            { "Engine.DrawImage(path, x, y, w, h)", "Draw a texture." },
            { "Engine.DrawAuto(path, mode, alpha?)", "Draw using a DisplayMode." },
            { "Engine.DrawRect(x,y,w,h, r,g,b,a)", "Draw a filled rectangle." },
            { "Engine.DrawRoundedRect(x,y,w,h,radius, ...)", "Draw a rounded rectangle." },
            { "Engine.DrawText(text, x, y, ...)", "Draw text." },
            { "Engine.MeasureText(text, font, size)", "Returns {w, h}." },
            { "DisplayMode.{TopLeft,Center,Fit,Fill,Bottom,FitWidthBottom}", "Draw modes." },
        } },
    };
    for (const Group& g : kApi) {
        if (ImGui::TreeNode(g.name)) {
            for (const Entry& e : g.entries) {
                ImGui::TextWrapped("%s", e.sig);
                ImGui::TextDisabled("   %s", e.desc);
            }
            ImGui::TreePop();
        }
    }
}

void ScriptWorkspace::RenderEditor() {
    if (m_currentFile.empty()) {
        ImGui::TextDisabled("Select a .lua file on the left, or create one.");
        ImGui::TextWrapped(
            "Declare custom commands with Engine.RegisterCommand(\"name\", fn). Add "
            "--@command / --@param annotation comments above the call so the command "
            "appears as a node (with parameters) in the Story editor.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "The player loads Data/Scripts/extensions.lua at startup — put custom "
            "commands there (or have it require other files) so they run in-game.");
        return;
    }

    ImGui::Text("%s%s", m_currentFile.c_str(), m_dirty ? " *" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save")) SaveCurrent();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) LoadFile(m_currentFile);
    ImGui::Separator();

    if (ImGui::InputTextMultiline("##luacode", &m_buffer, ImGui::GetContentRegionAvail(),
                                  ImGuiInputTextFlags_AllowTabInput)) {
        m_dirty = true;
    }
}

}
