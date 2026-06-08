#pragma once

#include "Engine/Project/Database.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace px::editor {

class FlowMap {
public:
    FlowMap();
    ~FlowMap();

    void SetOpenCallback(std::function<void(const std::string& script)> cb) {
        m_open = std::move(cb);
    }
    void Rebuild(const px::project::Database& db, const std::filesystem::path& projectRoot);
    void Render();

private:
    struct FNode {
        int id = 0;
        int pinIn = 0;
        int pinOut = 0;
        std::string title;
        std::string script;
        ImVec2 pos;
        bool posSet = false;
    };
    struct FLink {
        int id = 0;
        int fromPin = 0;
        int toPin = 0;
    };

    ax::NodeEditor::EditorContext* m_ctx = nullptr;
    std::vector<FNode> m_nodes;
    std::vector<FLink> m_links;
    std::function<void(const std::string&)> m_open;
};

}
