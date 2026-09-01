#include "Engine/Accessibility/SemanticTree.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/InputRouter.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Widgets.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <memory>
#include <string>

namespace {

int failures = 0;
void Check(const bool value, const char* message) {
    if (value) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

const px::accessibility::SemanticNode* Find(
    const px::accessibility::SemanticNode& node, const px::Uuid& id) {
    if (node.semantics.id == id) return &node;
    for (const auto& child : node.children)
        if (const auto* found = Find(child, id)) return found;
    return nullptr;
}

}  // namespace

int main() {
    Check(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
          "built-in UI types register");

    auto root = std::make_unique<px::ui::Panel>("Root");
    root->SetId(*px::Uuid::Parse("10000000-0000-4000-8000-000000000001"));
    root->SetClipContent(true);

    auto checkbox = std::make_unique<px::ui::CheckBox>("Remember", "Remember");
    const px::Uuid checkboxId = *px::Uuid::Parse("10000000-0000-4000-8000-000000000002");
    checkbox->SetId(checkboxId);
    checkbox->SetChecked(true);
    checkbox->SetFocusMode(px::ui::FocusMode::All);
    checkbox->SetAccessibilityLabel("Remember choice");
    checkbox->SetAccessibilityFocusOrder(20);
    checkbox->SetOffsets({10, 10, 160, 40});

    auto slider = std::make_unique<px::ui::Slider>("Volume");
    const px::Uuid sliderId = *px::Uuid::Parse("10000000-0000-4000-8000-000000000003");
    slider->SetId(sliderId);
    slider->SetRange(0.0, 100.0, 1.0);
    slider->SetValue(75.0);
    slider->SetAccessibilityLabel("Voice volume");
    slider->SetAccessibilityFocusOrder(10);
    slider->SetOffsets({10, 60, 200, 32});

    auto clipped = std::make_unique<px::ui::Button>("Outside", "Outside");
    const px::Uuid clippedId = *px::Uuid::Parse("10000000-0000-4000-8000-000000000004");
    clipped->SetId(clippedId);
    clipped->SetFocusMode(px::ui::FocusMode::None);
    clipped->SetOffsets({400, 400, 100, 40});

    auto lineEdit = std::make_unique<px::ui::LineEdit>(
        "A👩‍👩‍👧‍👦B", "PlayerName");
    const px::Uuid lineEditId =
        *px::Uuid::Parse("10000000-0000-4000-8000-000000000007");
    lineEdit->SetId(lineEditId);
    lineEdit->SetAccessibilityLabel("Player name");
    lineEdit->SetAccessibilityFocusOrder(15);
    lineEdit->SetOffsets({10, 105, 150, 44});
    auto* lineEditPointer = lineEdit.get();

    auto modal = std::make_unique<px::ui::Panel>("Modal");
    modal->SetId(*px::Uuid::Parse("10000000-0000-4000-8000-000000000005"));
    modal->SetOffsets({180,20,120,120});
    auto modalButton=std::make_unique<px::ui::Button>("Confirm","Confirm");
    modalButton->SetId(
        *px::Uuid::Parse("10000000-0000-4000-8000-000000000006"));
    modalButton->SetFocusMode(px::ui::FocusMode::None);
    modalButton->SetOffsets({10,10,90,36});
    auto* modalPointer=modal.get();
    auto* modalButtonPointer=modalButton.get();
    Check(static_cast<bool>(modal->AddChild(std::move(modalButton))),
          "modal focus fixture child attaches");

    auto* checkboxPointer = checkbox.get();
    auto* sliderPointer = slider.get();
    Check(static_cast<bool>(root->AddChild(std::move(checkbox))) &&
              static_cast<bool>(root->AddChild(std::move(slider))) &&
              static_cast<bool>(root->AddChild(std::move(clipped))) &&
              static_cast<bool>(root->AddChild(std::move(lineEdit))) &&
              static_cast<bool>(root->AddChild(std::move(modal))),
          "semantic fixture children attach");

    px::ui::UIContext context;
    Check(static_cast<bool>(context.SetRoot(std::move(root))),
          "semantic fixture installs");
    auto mock = std::make_shared<px::accessibility::MockSemanticAdapter>();
    context.SetAccessibilityAdapter(mock);
    px::Input input;
    (void)context.Update(input, 320, 180, 0.0f);

    const auto tree = context.CaptureAccessibilityTree();
    const auto* checkboxNode = Find(tree.root, checkboxId);
    const auto* sliderNode = Find(tree.root, sliderId);
    const auto* clippedNode = Find(tree.root, clippedId);
    Check(checkboxNode && checkboxNode->semantics.role == "checkbox" &&
              checkboxNode->semantics.label == "Remember choice" &&
              checkboxNode->semantics.states.size() == 1 &&
              checkboxNode->semantics.states.front() == "checked" &&
              checkboxNode->semantics.actions.size() == 1 &&
              checkboxNode->semantics.actions.front() == "activate",
          "checkbox exposes role, label, state, and action");
    Check(sliderNode && sliderNode->semantics.role == "slider" &&
              sliderNode->semantics.value.starts_with("75") &&
              sliderNode->semantics.actions.size() == 3,
          "slider exposes current value and range actions");
    Check(clippedNode && clippedNode->semantics.hidden &&
              clippedNode->semantics.bounds.w == 0.0f,
          "ancestor clip removes offscreen controls from accessibility");
    Check(mock->LastTree().revision == tree.revision && tree.revision >= 1 &&
              mock->CanonicalJson().find("Remember choice") != std::string::npos,
          "mock adapter receives deterministic production semantic snapshots");
    Check(mock->InvokeAction(checkboxId, "activate") &&
              !checkboxPointer->Checked() &&
              mock->InvokeAction(sliderId, "increment") &&
              sliderPointer->Value() == 76.0 &&
              mock->InvokeAction(sliderId, "setValue", "41") &&
              sliderPointer->Value() == 41.0,
          "platform adapters invoke controls through the same semantic action bridge as CI");
    Check(!mock->InvokeAction(sliderId, "setValue", "not-a-number") &&
              mock->InvokeAction(sliderId, "focus") && sliderPointer->Focused(),
          "semantic value actions reject malformed payloads and focus the Runtime control");

    int submitted = 0;
    lineEditPointer->SetOnSubmitted(
        [&submitted](const std::string&) { ++submitted; });
    px::ui::InputRouter editRouter(*context.Root());
    editRouter.SetFocus(lineEditPointer);
    px::Input editInput;
    editInput.InjectKeyPress(SDL_SCANCODE_LEFT);
    editRouter.Update(editInput);
    const std::size_t beforeFinalAscii = lineEditPointer->Text().size() - 1;
    Check(editRouter.Focused() == lineEditPointer &&
              lineEditPointer->CaretByteOffset() == beforeFinalAscii,
          "physical arrow keys move the text caret instead of global UI focus");
    editInput.NewFrame();
    editInput.InjectKeyPress(SDL_SCANCODE_LSHIFT, true);
    editInput.InjectKeyPress(SDL_SCANCODE_LEFT);
    editRouter.Update(editInput);
    Check(lineEditPointer->SelectionStartByteOffset() == 1 &&
              lineEditPointer->SelectionEndByteOffset() == beforeFinalAscii,
          "shift-arrow selects one complete emoji ZWJ grapheme");

    editInput.NewFrame();
    SDL_Event space{};
    space.type = SDL_EVENT_KEY_DOWN;
    space.key.scancode = SDL_SCANCODE_SPACE;
    space.key.repeat = false;
    editInput.Process(space);
    SDL_Event textInput{};
    textInput.type = SDL_EVENT_TEXT_INPUT;
    textInput.text.text = " ";
    editInput.Process(textInput);
    editRouter.Update(editInput);
    Check(submitted == 0 && lineEditPointer->Text() == "A B",
          "Space replaces the selected grapheme as text and never triggers Accept");

    const auto textTree = context.CaptureAccessibilityTree();
    const auto* lineEditNode = Find(textTree.root, lineEditId);
    Check(lineEditNode && lineEditNode->semantics.text &&
              lineEditNode->semantics.text->layout.Text() == "A B" &&
              lineEditNode->semantics.text->caretByteOffset == 2 &&
              lineEditNode->semantics.text->selectionStartByteOffset == 2 &&
              lineEditNode->semantics.text->selectionEndByteOffset == 2,
          "semantic snapshot exposes the same text, grapheme caret, and selection used by drawing");
    Check(mock->InvokeAction(lineEditId, "setSelection", "0:1") &&
              lineEditPointer->SelectionStartByteOffset() == 0 &&
              lineEditPointer->SelectionEndByteOffset() == 1,
          "platform selection actions update the Runtime text selection transactionally");

    px::ui::InputRouter router(*context.Root());
    router.SetFocus(checkboxPointer);
    router.FocusNext();
    Check(router.Focused() == sliderPointer,
          "keyboard focus follows authored accessibility focus order");

    px::Input directionalInput;
    directionalInput.InjectAction(px::InputAction::NavigateUp);
    router.Update(directionalInput);
    Check(router.Focused()==checkboxPointer,
          "directional navigation uses spatial control positions");
    modalButtonPointer->SetFocusMode(px::ui::FocusMode::All);
    Check(router.PushFocusScope(*modalPointer,true) &&
              router.Focused()==modalButtonPointer &&
              router.TargetAt({20,20})==modalPointer,
          "modal focus scope traps keyboard focus and captures outside pointer input");
    directionalInput.NewFrame();
    directionalInput.InjectAction(px::InputAction::NavigateLeft);
    router.Update(directionalInput);
    Check(router.Focused()==modalButtonPointer && router.PopFocusScope() &&
              router.Focused()==checkboxPointer,
          "directional focus cannot escape a modal and closing it restores prior focus");

    px::Input remapped;
    remapped.ClearBindings(px::InputAction::Accept);
    remapped.BindKey(px::InputAction::Accept,SDL_SCANCODE_F2);
    SDL_Event keyboard{};
    keyboard.type=SDL_EVENT_KEY_DOWN;
    keyboard.key.scancode=SDL_SCANCODE_RETURN;
    keyboard.key.repeat=false;
    remapped.Process(keyboard);
    Check(!remapped.ActionPressed(px::InputAction::Accept),
          "clearing an operation removes its default keyboard binding");
    remapped.NewFrame();
    keyboard.key.scancode=SDL_SCANCODE_F2;
    remapped.Process(keyboard);
    Check(remapped.ActionPressed(px::InputAction::Accept),
          "remappable UI operations use the authored replacement binding");

    px::Input gamepad;
    SDL_Event gamepadButton{};
    gamepadButton.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    gamepadButton.gbutton.button=SDL_GAMEPAD_BUTTON_SOUTH;
    gamepad.Process(gamepadButton);
    Check(gamepad.ActionPressed(px::InputAction::Accept),
          "gamepad accept enters the same operation map as keyboard input");

    px::Input touch;
    SDL_Event finger{};
    finger.type=SDL_EVENT_FINGER_DOWN;
    finger.tfinger.fingerID=42;
    finger.tfinger.x=.5f;
    finger.tfinger.y=.25f;
    touch.Process(finger);
    finger.type=SDL_EVENT_FINGER_UP;
    touch.Process(finger);
    Check(touch.LeftClick()&&touch.LeftReleased()&&!touch.LeftDown(),
          "primary touch uses the same captured pointer lifecycle as mouse input");

    if (failures == 0)
        std::cout << "PASS: UI semantic accessibility tree\n";
    return failures == 0 ? 0 : 1;
}
