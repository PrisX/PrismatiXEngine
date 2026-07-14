#pragma once

#include "Editor/Tools/UIDesigner/UISceneDocument.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <cstdint>
#include <memory>
#include <unordered_set>

namespace ax::NodeEditor { struct EditorContext; }

namespace px::editor {

class BehaviorGraphEditor {
public:
    BehaviorGraphEditor();
    ~BehaviorGraphEditor();
    BehaviorGraphEditor(const BehaviorGraphEditor&) = delete;
    BehaviorGraphEditor& operator=(const BehaviorGraphEditor&) = delete;

    bool Render(UISceneDocument& document, const Uuid& selectedControl);
    bool RenderInspector(UISceneDocument& document, const Uuid& selectedControl);
    void FrameSelection();
    void FocusNode(const Uuid& node);
    void ClearSelection();
    void SetDebugState(ui::BehaviorRuntimeState state){m_debugState=std::move(state);}
    [[nodiscard]] const Uuid& SelectedNode() const { return m_selectedNode; }

private:
    void EnsureContext();

    ax::NodeEditor::EditorContext* m_context = nullptr;
    std::unordered_set<Uuid, UuidHash> m_initializedNodes;
    std::unordered_set<Uuid, UuidHash> m_initializedGroups;
    ui::BehaviorGraph m_clipboard;
    Uuid m_selectedNode;
    Uuid m_selectedGroup;
    char m_filter[96] = {0};
    std::uintptr_t m_pendingCreatePin = 0;
    float m_pendingCreateX = 0.0f;
    float m_pendingCreateY = 0.0f;
    bool m_frameSelection = false;
    Uuid m_focusNode;
    ui::BehaviorRuntimeState m_debugState;
};

}  // namespace px::editor
