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

    modalPointer->SetModalCapture(true);
    directionalInput.NewFrame();
    router.Update(directionalInput);
    Check(router.ModalCaptureActive() && router.Focused() == modalButtonPointer &&
              router.TargetAt({20, 20}) == modalPointer,
          "authored modal capture traps focus and pointer input without a host focus-scope call");
    router.SetFocus(checkboxPointer);
    Check(router.Focused() == modalButtonPointer,
          "focus cannot move behind a visible authored modal");
    Check(router.PushFocusScope(*modalButtonPointer, true) &&
              router.TargetAt({20, 20}) == modalButtonPointer &&
              router.PopFocusScope(),
          "a nested host modal scope can tighten authored modal pointer capture");
    modalPointer->SetVisibility(px::ui::Visibility::Hidden);
    directionalInput.NewFrame();
    router.Update(directionalInput);
    Check(!router.ModalCaptureActive() && router.Focused() == checkboxPointer &&
              router.TargetAt({20, 20}) != modalPointer,
          "hiding an authored modal restores the previous focus and hit testing");

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

    px::Input capture;
    Check(capture.BeginKeyCapture() && capture.KeyCaptureActive() &&
              !capture.BeginKeyCapture(),
          "keyboard capture starts once without replacing a pending request");
    SDL_Event capturedKey{};
    capturedKey.type = SDL_EVENT_KEY_DOWN;
    capturedKey.key.scancode = SDL_SCANCODE_RETURN;
    capturedKey.key.repeat = false;
    capture.Process(capturedKey);
    SDL_Event capturedText{};
    capturedText.type = SDL_EVENT_TEXT_INPUT;
    capturedText.text.text = "x";
    capture.Process(capturedText);
    Check(!capture.KeyCaptureActive() &&
              !capture.ActionPressed(px::InputAction::Accept) &&
              !capture.KeyPressed(SDL_SCANCODE_RETURN) &&
              capture.TextInput().empty(),
          "captured key cannot activate the old action or leak text input");
    capture.NewFrame();
    Check(capture.TakeCapturedKey() == SDL_SCANCODE_RETURN &&
              !capture.TakeCapturedKey(),
          "captured key survives the frame boundary and is taken once");
    Check(capture.BeginKeyCapture(), "capture can restart after consumption");
    capture.InjectKeyPress(SDL_SCANCODE_F1);
    Check(capture.TakeCapturedKey() == SDL_SCANCODE_F1 &&
              !capture.KeyPressed(SDL_SCANCODE_F1),
          "deterministic key injection follows the same capture path");
    Check(capture.BeginKeyCapture(), "capture can restart for cancellation");
    capture.CancelKeyCapture();
    Check(!capture.KeyCaptureActive() && !capture.TakeCapturedKey(),
          "cancelled capture does not produce a stale shortcut");

    px::Input rebindInput;
    Check(rebindInput.RegisterAction("photo-mode") &&
              rebindInput.BindKey("photo-mode", SDL_SCANCODE_P) &&
              rebindInput.RegisterAction("another-action") &&
              rebindInput.BindKey("another-action", SDL_SCANCODE_F4),
          "key binding button fixture registers named actions");
    int bindingSaves = 0;
    px::ui::UIContext bindingContext;
    bindingContext.SetInputBindingService(&rebindInput,
                                          [&bindingSaves] { ++bindingSaves; });
    auto bindingButton = std::make_unique<px::ui::TextureButton>();
    bindingButton->SetBindingAction("photo-mode");
    auto* mountedBindingButton = bindingButton.get();
    Check(bindingContext.SetRoot(std::move(bindingButton)).IsOk() &&
              mountedBindingButton->Text() == "P",
          "UI context injects input service and displays current key");
    mountedBindingButton->Activate();
    Check(mountedBindingButton->CapturingBinding() &&
              mountedBindingButton->Text() == "Press a key",
          "key binding button starts capture without an authored click action");
    rebindInput.InjectKeyPress(SDL_SCANCODE_F3);
    (void)bindingContext.Update(rebindInput, 800, 600, 0.016f);
    Check(rebindInput.KeyBindings("photo-mode") ==
              std::vector<int>{SDL_SCANCODE_F3} &&
              mountedBindingButton->Text() == "F3" && bindingSaves == 1,
          "captured key updates the action, caption, and persistence callback");
    mountedBindingButton->Activate();
    rebindInput.InjectKeyPress(SDL_SCANCODE_F4);
    (void)bindingContext.Update(rebindInput, 800, 600, 0.016f);
    Check(mountedBindingButton->Text() == "Already assigned" &&
              rebindInput.KeyBindings("photo-mode") ==
                  std::vector<int>{SDL_SCANCODE_F3} && bindingSaves == 1,
          "conflicting key is rejected without persisting");
    (void)bindingContext.Update(rebindInput, 800, 600, 2.0f);
    Check(mountedBindingButton->Text() == "F3",
          "conflict feedback returns to the current binding");
    mountedBindingButton->Activate();
    rebindInput.InjectKeyPress(SDL_SCANCODE_ESCAPE);
    (void)bindingContext.Update(rebindInput, 800, 600, 0.016f);
    Check(mountedBindingButton->Text() == "F3" && bindingSaves == 1,
          "Escape cancels key capture without changing the binding");
    mountedBindingButton->SetBindingAction("misspelled-action");
    Check(mountedBindingButton->Text() == "Unknown action",
          "unregistered binding actions are visible before a click");
    mountedBindingButton->SetBindingAction("photo-mode");
    mountedBindingButton->Activate();
    Check(bindingContext.SetRoot(std::make_unique<px::ui::Control>()).IsOk() &&
              !rebindInput.KeyCaptureActive(),
          "unmounting a binding button cancels its pending physical capture");

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
