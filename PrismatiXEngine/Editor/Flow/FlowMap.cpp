#include "Editor/Flow/FlowMap.h"

#include <fstream>
#include <sstream>

namespace ed = ax::NodeEditor;

namespace px::editor {

namespace {
std::string ExtractTarget(const std::string& line) {
    const std::size_t key = line.find("target=\"");
    if (key == std::string::npos) return {};
    const std::size_t start = key + 8;
    const std::size_t end = line.find('"', start);
    if (end == std::string::npos) return {};
    return line.substr(start, end - start);
}
}

FlowMap::FlowMap() {
    ed::Config config;
    config.SettingsFile = nullptr;
    m_ctx = ed::CreateEditor(&config);
}

FlowMap::~FlowMap() {
    if (m_ctx) ed::DestroyEditor(m_ctx);
}

void FlowMap::Rebuild(const px::project::Database& db, const std::filesystem::path& projectRoot) {
    m_nodes.clear();
    m_links.clear();
    int nextId = 1;

    for (int i = 0; i < static_cast<int>(db.chapters.size()); ++i) {
        FNode n;
        n.id = nextId++;
        n.pinIn = nextId++;
        n.pinOut = nextId++;
        n.title = db.chapters[i].title.empty() ? db.chapters[i].id : db.chapters[i].title;
        n.script = db.chapters[i].script;
        n.pos = ImVec2(60.0f + (i % 4) * 260.0f, 60.0f + (i / 4) * 180.0f);
        m_nodes.push_back(std::move(n));
    }

    int linkId = 100000;
    for (const FNode& from : m_nodes) {
        if (from.script.empty()) continue;
        std::ifstream in(projectRoot / "Data" / "Script" / from.script);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("[jump") == std::string::npos && line.find("[choice") == std::string::npos) {
                continue;
            }
            const std::string target = ExtractTarget(line);
            if (target.empty() || target[0] == '*') continue;
            for (const FNode& to : m_nodes) {
                if (&to != &from && (to.script == target || to.script == target + ".pds")) {
                    m_links.push_back(FLink{ linkId++, from.pinOut, to.pinIn });
                }
            }
        }
    }
}

void FlowMap::Render() {
    if (m_nodes.empty()) {
        ImGui::TextDisabled("No chapters. Add them in the Database (Chapters tab).");
        return;
    }
    ImGui::TextDisabled("Story flow — chapters + cross-script jumps. Click a node to open it.");

    ed::SetCurrentEditor(m_ctx);
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 10.0f);
    ed::Begin("flow_map");

    for (FNode& n : m_nodes) {
        if (!n.posSet) {
            ed::SetNodePosition(ed::NodeId(n.id), n.pos);
            n.posSet = true;
        }
        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(n.id);
        ed::BeginPin(ed::PinId(n.pinIn), ed::PinKind::Input);
        ImGui::TextUnformatted("->");
        ed::EndPin();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.98f, 1.0f, 1.0f), "%s", n.title.c_str());
        ImGui::SameLine();
        ed::BeginPin(ed::PinId(n.pinOut), ed::PinKind::Output);
        ImGui::TextUnformatted("->");
        ed::EndPin();
        ImGui::TextDisabled("%s", n.script.c_str());
        if (ImGui::SmallButton("Open in Story") && m_open) {
            m_open(n.script);
        }
        ImGui::PopID();
        ed::EndNode();
    }
    for (const FLink& l : m_links) {
        ed::Link(ed::LinkId(l.id), ed::PinId(l.fromPin), ed::PinId(l.toPin));
    }

    ed::End();
    ed::PopStyleVar();
    ed::SetCurrentEditor(nullptr);
}

}
