#include "Engine/Accessibility/PlatformSemanticAdapter.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <UIAutomationClient.h>

#include <iostream>
#include <string>

namespace {
int failures = 0;
void Check(const bool value, const char* message) {
    if (value) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}

int main() {
    Check(SDL_Init(SDL_INIT_VIDEO), "SDL video initializes");
    SDL_Window* window = SDL_CreateWindow(
        "PrismatiX UIA acceptance", 480, 270, SDL_WINDOW_HIDDEN);
    Check(window != nullptr, "native test window is created");
    auto adapter =
        px::accessibility::CreatePlatformSemanticAdapter(window);
    Check(adapter != nullptr &&
              px::accessibility::PlatformAccessibilityBackend() ==
                  "Windows UI Automation",
          "Windows UI Automation bridge attaches to the SDL HWND");

    const px::Uuid rootId =
        *px::Uuid::Parse("90000000-0000-4000-8000-000000000001");
    const px::Uuid buttonId =
        *px::Uuid::Parse("90000000-0000-4000-8000-000000000002");
    const px::Uuid textId =
        *px::Uuid::Parse("90000000-0000-4000-8000-000000000003");
    int invoked = 0;
    std::string selectedRange;
    if (adapter)
        adapter->SetActionHandler(
            [&](const px::Uuid& id, const std::string_view action,
                const std::string_view value) {
                if (id == buttonId && action == "activate") {
                    ++invoked;
                    return true;
                }
                if (id == textId && action == "setSelection") {
                    selectedRange = std::string(value);
                    return true;
                }
                return false;
            });
    px::accessibility::SemanticTree tree;
    tree.revision = 1;
    tree.root.semantics.id = rootId;
    tree.root.semantics.role = "window";
    tree.root.semantics.label = "PrismatiX test root";
    tree.root.semantics.bounds = {0, 0, 480, 270};
    px::accessibility::SemanticNode button;
    button.semantics.id = buttonId;
    button.semantics.role = "button";
    button.semantics.label = "Start story";
    button.semantics.actions = {"activate"};
    button.semantics.focusable = true;
    button.semantics.bounds = {20, 20, 180, 48};
    tree.root.children.push_back(std::move(button));
    const std::string textValue = "A👩‍👩‍👧‍👦B";
    const std::size_t familyEnd = textValue.rfind('B');
    px::accessibility::SemanticNode text;
    text.semantics.id = textId;
    text.semantics.role = "textbox";
    text.semantics.label = "Player name";
    text.semantics.value = textValue;
    text.semantics.focusable = true;
    text.semantics.bounds = {20, 90, 300, 48};
    text.semantics.text = px::ui::AccessibilityTextSemantics{
        .layout = px::text::TextLayout(
            textValue, "und", {240, 30},
            {{0, 1, 0, {0, 0, 20, 30}, false, false},
             {1, familyEnd - 1, 0, {20, 0, 80, 30}, false, false},
             {familyEnd, 1, 0, {100, 0, 20, 30}, false, false}}),
        .origin = {10, 8},
        .caretByteOffset = familyEnd,
        .selectionStartByteOffset = 1,
        .selectionEndByteOffset = familyEnd,
        .editable = true,
    };
    tree.root.children.push_back(std::move(text));
    if (adapter) adapter->Publish(tree);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation* automation = nullptr;
    Check(SUCCEEDED(CoCreateInstance(
              CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
              IID_PPV_ARGS(&automation))),
          "UI Automation client initializes");
    IUIAutomationElement* root = nullptr;
    auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
    Check(automation && hwnd &&
              SUCCEEDED(automation->ElementFromHandle(hwnd, &root)) && root,
          "UI Automation discovers the custom root through WM_GETOBJECT");

    IUIAutomationCondition* condition = nullptr;
    VARIANT expected;
    VariantInit(&expected);
    expected.vt = VT_BSTR;
    const std::string buttonIdText = buttonId.ToString();
    const std::wstring buttonAutomationId(buttonIdText.begin(),
                                          buttonIdText.end());
    expected.bstrVal = SysAllocString(buttonAutomationId.c_str());
    Check(automation && SUCCEEDED(automation->CreatePropertyCondition(
              UIA_AutomationIdPropertyId, expected, &condition)),
          "UIA child condition is created");
    VariantClear(&expected);
    IUIAutomationElement* child = nullptr;
    Check(root && condition &&
              SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition,
                                        &child)) && child,
          "UIA traverses the published semantic child");
    BSTR name = nullptr;
    Check(child && SUCCEEDED(child->get_CurrentName(&name)) && name &&
              std::wstring(name) == L"Start story",
          "UIA exposes the authored accessible name");
    SysFreeString(name);
    IUIAutomationInvokePattern* invoke = nullptr;
    Check(child && SUCCEEDED(child->GetCurrentPatternAs(
              UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke &&
              SUCCEEDED(invoke->Invoke()) && invoked == 1,
          "UIA Invoke reaches the engine semantic action handler");

    IUIAutomationCondition* textCondition = nullptr;
    VariantInit(&expected);
    expected.vt = VT_BSTR;
    const std::string textIdText = textId.ToString();
    const std::wstring textAutomationId(textIdText.begin(), textIdText.end());
    expected.bstrVal = SysAllocString(textAutomationId.c_str());
    Check(automation && SUCCEEDED(automation->CreatePropertyCondition(
              UIA_AutomationIdPropertyId, expected, &textCondition)),
          "UIA text condition is created");
    VariantClear(&expected);
    IUIAutomationElement* textElement = nullptr;
    Check(root && textCondition &&
              SUCCEEDED(root->FindFirst(TreeScope_Descendants, textCondition,
                                        &textElement)) && textElement,
          "UIA discovers the semantic text control");
    IUIAutomationTextPattern* textPattern = nullptr;
    Check(textElement && SUCCEEDED(textElement->GetCurrentPatternAs(
              UIA_TextPatternId, IID_PPV_ARGS(&textPattern))) && textPattern,
          "UIA exposes TextPattern for semantic text");
    IUIAutomationTextRange* documentRange = nullptr;
    BSTR documentText = nullptr;
    Check(textPattern && SUCCEEDED(textPattern->get_DocumentRange(
              &documentRange)) && documentRange &&
              SUCCEEDED(documentRange->GetText(-1, &documentText)) &&
              documentText && std::wstring(documentText) ==
                                  L"A👩‍👩‍👧‍👦B",
          "UIA document range returns the complete Unicode text");
    SysFreeString(documentText);
    IUIAutomationTextRangeArray* selections = nullptr;
    IUIAutomationTextRange* selected = nullptr;
    BSTR selectedText = nullptr;
    int selectionCount = 0;
    if (textPattern && SUCCEEDED(textPattern->GetSelection(&selections)) &&
        selections && SUCCEEDED(selections->get_Length(&selectionCount)) &&
        selectionCount == 1) {
        (void)selections->GetElement(0, &selected);
    }
    Check(selected && SUCCEEDED(selected->GetText(-1, &selectedText)) &&
              selectedText && std::wstring(selectedText) == L"👩‍👩‍👧‍👦" &&
              SUCCEEDED(selected->Select()) &&
              selectedRange == "1:" + std::to_string(familyEnd),
          "UIA selection preserves one emoji grapheme and selects through the engine bridge");
    SysFreeString(selectedText);

    if (selected) selected->Release();
    if (selections) selections->Release();
    if (documentRange) documentRange->Release();
    if (textPattern) textPattern->Release();
    if (textElement) textElement->Release();
    if (textCondition) textCondition->Release();
    if (invoke) invoke->Release();
    if (child) child->Release();
    if (condition) condition->Release();
    if (root) root->Release();
    if (automation) automation->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    adapter.reset();
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    if (!failures) std::cout << "PASS: Windows UI Automation bridge\n";
    return failures ? 1 : 0;
}

#else

#include <iostream>
#include <string>

int main() {
    int failures = 0;
    const auto check = [&](const bool value, const char* message) {
        if (value) return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    };
    check(SDL_Init(SDL_INIT_VIDEO), "SDL video initializes");
    SDL_Window* window = SDL_CreateWindow(
        "PrismatiX native accessibility acceptance", 480, 270,
        SDL_WINDOW_HIDDEN);
    check(window != nullptr, "native test window is created");
    auto adapter =
        px::accessibility::CreatePlatformSemanticAdapter(window);
    check(adapter != nullptr, "native semantic adapter is created");
#if defined(__APPLE__)
    check(px::accessibility::PlatformAccessibilityBackend() ==
              "macOS NSAccessibility",
          "NSAccessibility backend is selected");
#elif defined(__linux__)
    check(px::accessibility::PlatformAccessibilityBackend() ==
              "Linux AT-SPI",
          "AT-SPI backend is selected");
#endif
    px::accessibility::SemanticTree tree;
    tree.revision = 1;
    tree.root.semantics.id =
        *px::Uuid::Parse("90000000-0000-4000-8000-000000000001");
    tree.root.semantics.role = "window";
    tree.root.semantics.label = "PrismatiX test root";
    tree.root.semantics.bounds = {0, 0, 480, 270};
    if (adapter) adapter->Publish(tree);
    adapter.reset();
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return failures ? 1 : 0;
}
#endif
