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
    void SetCreateChapterCallback(std::function<void(ImVec2 canvasPosition)> cb) {
        m_createChapter = std::move(cb);
    }
    void SetHeaderTexture(ImTextureID texture, int width, int height);
    void SetNodePositionByScript(const std::string& script, ImVec2 position);
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

    [[nodiscard]] FNode* FindNode(int id);
    [[nodiscard]] const FNode* FindNode(int id) const;
    [[nodiscard]] const FNode* FindNodeForPin(int pinId) const;
    [[nodiscard]] const FLink* FindLink(int id) const;
    [[nodiscard]] bool IsInputPin(int pinId) const;
    [[nodiscard]] bool IsOutputPin(int pinId) const;
    [[nodiscard]] bool IsPinLinked(int pinId) const;
    [[nodiscard]] bool CanLink(int fromPin, int toPin) const;
    void AddLink(int fromPin, int toPin);
    void RemoveLink(int id);
    void RemoveNode(int id);
    void DeleteSelection();
    void HandleInteractions();
    void RenderContextMenus(std::string& pendingOpen, bool& pendingCreateChapter,
                             ImVec2& pendingCreatePosition);

    ax::NodeEditor::EditorContext* m_ctx = nullptr;
    std::vector<FNode> m_nodes;
    std::vector<FLink> m_links;
    std::function<void(const std::string&)> m_open;
    std::function<void(ImVec2)> m_createChapter;
    int m_nextId = 1;
    int m_nextLinkId = 100000;
    int m_contextNodeId = 0;
    int m_contextLinkId = 0;
    ImTextureID m_headerTexture = ImTextureID{};
    int m_headerTextureWidth = 0;
    int m_headerTextureHeight = 0;
};

}
