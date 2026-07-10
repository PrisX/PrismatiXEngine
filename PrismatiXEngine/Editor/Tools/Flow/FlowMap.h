#pragma once

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
    // Fired when the user designates a chapter as the game entry point.
    void SetEntryChangedCallback(std::function<void(const std::string& script)> cb) {
        m_entryChanged = std::move(cb);
    }
    // Fired when a node is moved; script is empty for the Start node.
    void SetLayoutChangedCallback(std::function<void(const std::string& script, ImVec2 position)> cb) {
        m_layoutChanged = std::move(cb);
    }
    void SetEntryScript(const std::string& script) { m_entryScript = script; }
    [[nodiscard]] const std::string& EntryScript() const { return m_entryScript; }
    // Script paths under Content/Script drive the graph; no central database is involved.
    void SetAvailableScripts(std::vector<std::string> scripts) {
        m_availableScripts = std::move(scripts);
    }
    // Fired when a chapter's script assignment changes (via the node's script picker).
    void SetScriptChangedCallback(
        std::function<void(const std::string& chapterId, const std::string& newScript)> cb) {
        m_scriptChanged = std::move(cb);
    }
    // Fired when the user asks to create a missing script file on disk.
    void SetCreateScriptCallback(std::function<void(const std::string& script)> cb) {
        m_createScript = std::move(cb);
    }
    // Fired when a chapter is renamed from the flow node.
    void SetTitleChangedCallback(
        std::function<void(const std::string& chapterId, const std::string& newTitle)> cb) {
        m_titleChanged = std::move(cb);
    }
    // Fired when the user draws/deletes a chapter→chapter link (not during Rebuild).
    void SetLinkAddedCallback(
        std::function<void(const std::string& fromScript, const std::string& toScript)> cb) {
        m_linkAdded = std::move(cb);
    }
    void SetLinkRemovedCallback(
        std::function<void(const std::string& fromScript, const std::string& toScript)> cb) {
        m_linkRemoved = std::move(cb);
    }
    // Fired when a chapter node is removed from the flow.
    void SetChapterRemovedCallback(std::function<void(const std::string& chapterId)> cb) {
        m_chapterRemoved = std::move(cb);
    }
    void SetHeaderTexture(ImTextureID texture, int width, int height);
    void SetNodePositionByScript(const std::string& script, ImVec2 position);
    void Rebuild(const std::vector<std::string>& scripts, const std::filesystem::path& projectRoot);
    void Render();

private:
    struct FNode {
        int id = 0;
        int pinIn = 0;
        int pinOut = 0;
        std::string chapterId;
        std::string title;
        std::string script;
        ImVec2 pos;
        bool posSet = false;
        bool isEntry = false;
        bool scriptMissing = false;
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

    void RenderEntryNode(FNode& node);
    void SyncMovedNodes();

    ax::NodeEditor::EditorContext* m_ctx = nullptr;
    std::vector<FNode> m_nodes;
    std::vector<FLink> m_links;
    std::function<void(const std::string&)> m_open;
    std::function<void(ImVec2)> m_createChapter;
    std::function<void(const std::string&)> m_entryChanged;
    std::function<void(const std::string&, ImVec2)> m_layoutChanged;
    std::function<void(const std::string&, const std::string&)> m_scriptChanged;
    std::function<void(const std::string&)> m_createScript;
    std::function<void(const std::string&, const std::string&)> m_titleChanged;
    std::function<void(const std::string&, const std::string&)> m_linkAdded;
    std::function<void(const std::string&, const std::string&)> m_linkRemoved;
    std::function<void(const std::string&)> m_chapterRemoved;
    bool m_rebuilding = false;
    std::vector<std::string> m_availableScripts;
    char m_renameBuffer[128] = { 0 };
    char m_newScriptBuffer[128] = { 0 };
    int m_scriptPickerNodeId = 0;
    bool m_scriptPickerOpen = false;
    std::string m_entryScript;
    int m_entryNodeId = 0;
    int m_entryLinkId = 0;
    int m_nextId = 1;
    int m_nextLinkId = 100000;
    int m_contextNodeId = 0;
    int m_contextLinkId = 0;
    ImTextureID m_headerTexture = ImTextureID{};
    int m_headerTextureWidth = 0;
    int m_headerTextureHeight = 0;
};

}
