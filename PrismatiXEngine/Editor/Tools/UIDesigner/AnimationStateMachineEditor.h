#pragma once

#include "Editor/Tools/UIDesigner/UISceneDocument.h"
#include "Engine/UI/Animation.h"

#include <cstdint>
#include <functional>
#include <unordered_set>

namespace ax::NodeEditor { struct EditorContext; }

namespace px::editor {

class AnimationStateMachineEditor {
public:
    AnimationStateMachineEditor();
    ~AnimationStateMachineEditor();
    AnimationStateMachineEditor(const AnimationStateMachineEditor&) = delete;
    AnimationStateMachineEditor& operator=(const AnimationStateMachineEditor&) = delete;

    bool Render(UISceneDocument& document);
    bool RenderNavigator(UISceneDocument& document);
    bool RenderInspector(UISceneDocument& document);
    void SetDebugState(ui::UIAnimationRuntimeState state) { m_debugState = std::move(state); }
    void SetParameterTester(std::function<Status(std::string_view, const Variant&)> tester) { m_parameterTester = std::move(tester); }

private:
    void EnsureContext();

    ax::NodeEditor::EditorContext* m_context = nullptr;
    std::unordered_set<Uuid, UuidHash> m_initializedNodes;
    Uuid m_selectedState;
    Uuid m_selectedTransition;
    ui::UIAnimationRuntimeState m_debugState;
    std::function<Status(std::string_view, const Variant&)> m_parameterTester;
};

}  // namespace px::editor
