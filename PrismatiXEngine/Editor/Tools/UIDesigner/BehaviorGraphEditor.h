#pragma once

#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
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

    bool Render(UISceneDocument& document, DesignerCommandService& commands,
                DesignerBehaviorGraphState& state,
                const Uuid& selectedControl);
    bool RenderInspector(UISceneDocument& document, DesignerCommandService& commands,
                         DesignerBehaviorGraphState& state,
                         const Uuid& selectedControl);
    void FrameSelection();
    void FocusNode(DesignerBehaviorGraphState& state, const Uuid& node);
    void ClearSelection(DesignerBehaviorGraphState& state);
    void SetDebugState(ui::BehaviorRuntimeState state){m_debugState=std::move(state);}
    [[nodiscard]] static const Uuid& SelectedNode(const DesignerBehaviorGraphState& state) {
        return state.selectedNode;
    }

private:
    void EnsureContext();

    ax::NodeEditor::EditorContext* m_context = nullptr;
    std::unordered_set<Uuid, UuidHash> m_initializedNodes;
    std::unordered_set<Uuid, UuidHash> m_initializedGroups;
    ui::BehaviorGraph m_clipboard;
    char m_filter[96] = {0};
    std::uintptr_t m_pendingCreatePin = 0;
    float m_pendingCreateX = 0.0f;
    float m_pendingCreateY = 0.0f;
    bool m_frameSelection = false;
    ui::BehaviorRuntimeState m_debugState;
};

}  // namespace px::editor
