#include "Engine/Accessibility/PlatformSemanticAdapter.h"

#if defined(__APPLE__)

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace px::accessibility {
class MacSemanticAdapter;
}

@interface PXSemanticElement : NSAccessibilityElement {
@public
    px::accessibility::MacSemanticAdapter* _pxOwner;
    px::Uuid _pxId;
    NSString* _pxValue;
    BOOL _pxReadOnly;
    BOOL _pxHasText;
    NSRange _pxSelection;
}
- (BOOL)isAccessibilityElement;
- (id)accessibilityValue;
- (void)setAccessibilityValue:(id)value;
- (NSInteger)accessibilityNumberOfCharacters;
- (NSRange)accessibilitySelectedTextRange;
- (void)setAccessibilitySelectedTextRange:(NSRange)range;
- (NSString*)accessibilitySelectedText;
- (NSRange)accessibilityVisibleCharacterRange;
- (NSString*)accessibilityStringForRange:(NSRange)range;
- (BOOL)accessibilityPerformPress;
- (BOOL)accessibilityPerformIncrement;
- (BOOL)accessibilityPerformDecrement;
- (BOOL)accessibilityPerformShowMenu;
@end

namespace px::accessibility {
namespace {

bool Has(const std::vector<std::string>& values, const std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

NSString* Text(const std::string_view value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding];
}

NSRange Utf16Range(const std::string_view value, const std::size_t byteStart,
                   const std::size_t byteEnd) {
    const std::size_t start = std::min(byteStart, value.size());
    const std::size_t end = std::clamp(byteEnd, start, value.size());
    NSString* prefix = Text(value.substr(0, start));
    NSString* selected = Text(value.substr(start, end - start));
    return NSMakeRange(prefix.length, selected.length);
}

NSAccessibilityRole NativeRole(const std::string_view role) {
    if (role == "button") return NSAccessibilityButtonRole;
    if (role == "checkbox") return NSAccessibilityCheckBoxRole;
    if (role == "radio") return NSAccessibilityRadioButtonRole;
    if (role == "slider") return NSAccessibilitySliderRole;
    if (role == "combobox") return NSAccessibilityComboBoxRole;
    if (role == "textbox") return NSAccessibilityTextFieldRole;
    if (role == "text") return NSAccessibilityStaticTextRole;
    if (role == "heading") return NSAccessibilityHeadingRole;
    if (role == "image") return NSAccessibilityImageRole;
    if (role == "progressbar") return NSAccessibilityProgressIndicatorRole;
    if (role == "list" || role == "listbox" || role == "log")
        return NSAccessibilityListRole;
    if (role == "dialog") return NSAccessibilityWindowRole;
    if (role == "window") return NSAccessibilityWindowRole;
    return NSAccessibilityGroupRole;
}

}  // namespace

class MacSemanticAdapter final : public SemanticAdapter {
public:
    static std::shared_ptr<MacSemanticAdapter> Create(SDL_Window* window) {
        if (!window || ![NSThread isMainThread]) return {};
        NSWindow* nativeWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        NSView* view = nativeWindow.contentView;
        if (!nativeWindow || !view) return {};
        return std::shared_ptr<MacSemanticAdapter>(
            new MacSemanticAdapter(nativeWindow, view));
    }

    ~MacSemanticAdapter() override {
        const auto detach = [this] {
            if (m_view) [m_view setAccessibilityChildren:m_originalChildren];
        };
        if ([NSThread isMainThread]) detach();
        else dispatch_sync(dispatch_get_main_queue(), ^{ detach(); });
    }

    void Publish(const SemanticTree& tree) override {
        SemanticTree snapshot = tree;
        const auto publish = [this, snapshot = std::move(snapshot)] {
            NSMutableArray<PXSemanticElement*>* roots =
                [[NSMutableArray alloc] initWithCapacity:1];
            NSMutableDictionary<NSString*, PXSemanticElement*>* elements =
                [[NSMutableDictionary alloc] init];
            PXSemanticElement* root = Build(snapshot.root, nil, elements);
            if (root) [roots addObject:root];
            [m_view setAccessibilityChildren:roots];
            NSAccessibilityPostNotification(
                m_view, NSAccessibilityLayoutChangedNotification);
            PXSemanticElement* focused = FindFocused(snapshot.root, elements);
            if (focused && focused != m_lastFocus) {
                NSAccessibilityPostNotification(
                    focused, NSAccessibilityFocusedUIElementChangedNotification);
            }
            m_lastFocus = focused;
        };
        if ([NSThread isMainThread]) publish();
        else dispatch_sync(dispatch_get_main_queue(), ^{ publish(); });
    }

    bool Dispatch(const Uuid& id, const std::string_view action,
                  const std::string_view value = {}) {
        return InvokeAction(id, action, value);
    }

private:
    MacSemanticAdapter(NSWindow* window, NSView* view)
        : m_window(window), m_view(view),
          m_originalChildren([view accessibilityChildren]) {}

    PXSemanticElement* Build(
        const SemanticNode& node, PXSemanticElement* parent,
        NSMutableDictionary<NSString*, PXSemanticElement*>* elements) {
        const auto& semantics = node.semantics;
        PXSemanticElement* element = [[PXSemanticElement alloc] init];
        element->_pxOwner = this;
        element->_pxId = semantics.id;
        element->_pxHasText = semantics.text.has_value();
        const std::string_view accessibleText = semantics.text
                                                    ? semantics.text->layout.Text()
                                                    : semantics.value;
        element->_pxValue = Text(accessibleText);
        element->_pxReadOnly = semantics.readOnly;
        if (semantics.text)
            element->_pxSelection = Utf16Range(
                accessibleText, semantics.text->selectionStartByteOffset,
                semantics.text->selectionEndByteOffset);
        [element setAccessibilityIdentifier:Text(semantics.id.ToString())];
        [element setAccessibilityRole:
            semantics.text && semantics.text->multiline
                ? NSAccessibilityTextAreaRole
                : NativeRole(semantics.role)];
        if (semantics.role == "dialog")
            [element setAccessibilitySubrole:NSAccessibilityDialogSubrole];
        [element setAccessibilityLabel:Text(semantics.label)];
        [element setAccessibilityHelp:Text(semantics.description)];
        [element setAccessibilityEnabled:
            !Has(semantics.states, "disabled")];
        [element setAccessibilityFocused:
            Has(semantics.states, "focused")];
        [element setAccessibilityHidden:semantics.hidden];
        [element setAccessibilityParent:parent ? (id)parent : (id)m_view];

        const NSRect local = NSMakeRect(
            semantics.bounds.x,
            m_view.bounds.size.height - semantics.bounds.y -
                semantics.bounds.h,
            semantics.hidden ? 0.0 : semantics.bounds.w,
            semantics.hidden ? 0.0 : semantics.bounds.h);
        const NSRect windowRect = [m_view convertRect:local toView:nil];
        [element setAccessibilityFrame:[m_window convertRectToScreen:windowRect]];
        if (semantics.hasRange) {
            [element setAccessibilityMinValue:@(semantics.minimum)];
            [element setAccessibilityMaxValue:@(semantics.maximum)];
        }

        NSMutableArray<PXSemanticElement*>* children =
            [[NSMutableArray alloc] initWithCapacity:node.children.size()];
        for (const auto& child : node.children) {
            if (PXSemanticElement* built = Build(child, element, elements))
                [children addObject:built];
        }
        [element setAccessibilityChildren:children];
        elements[Text(semantics.id.ToString())] = element;
        return element;
    }

    static PXSemanticElement* FindFocused(
        const SemanticNode& node,
        NSDictionary<NSString*, PXSemanticElement*>* elements) {
        if (Has(node.semantics.states, "focused"))
            return elements[Text(node.semantics.id.ToString())];
        for (const auto& child : node.children)
            if (PXSemanticElement* found = FindFocused(child, elements))
                return found;
        return nil;
    }

    NSWindow* __weak m_window = nil;
    NSView* __weak m_view = nil;
    NSArray* m_originalChildren = nil;
    PXSemanticElement* __weak m_lastFocus = nil;
};

std::shared_ptr<SemanticAdapter> CreatePlatformSemanticAdapter(
    SDL_Window* window) {
    auto adapter = MacSemanticAdapter::Create(window);
    if (!adapter)
        PX_LOG_ERROR("Could not attach macOS NSAccessibility provider");
    return adapter;
}

std::string_view PlatformAccessibilityBackend() {
    return "macOS NSAccessibility";
}

}  // namespace px::accessibility

@implementation PXSemanticElement
- (BOOL)isAccessibilityElement { return YES; }
- (id)accessibilityValue { return _pxValue; }
- (void)setAccessibilityValue:(id)value {
    if (_pxReadOnly || !_pxOwner) return;
    NSString* text = [value isKindOfClass:[NSString class]]
                         ? (NSString*)value
                         : [value description];
    const char* utf8 = text.UTF8String;
    if (_pxOwner->Dispatch(_pxId, "setValue", utf8 ? utf8 : ""))
        _pxValue = [text copy];
}
- (NSInteger)accessibilityNumberOfCharacters {
    return _pxHasText ? static_cast<NSInteger>(_pxValue.length) : 0;
}
- (NSRange)accessibilitySelectedTextRange {
    return _pxHasText ? _pxSelection : NSMakeRange(NSNotFound, 0);
}
- (void)setAccessibilitySelectedTextRange:(NSRange)range {
    if (!_pxHasText || !_pxOwner || NSMaxRange(range) > _pxValue.length) return;
    NSString* prefix = [_pxValue substringToIndex:range.location];
    NSString* throughSelection = [_pxValue substringToIndex:NSMaxRange(range)];
    NSData* prefixBytes = [prefix dataUsingEncoding:NSUTF8StringEncoding];
    NSData* selectionBytes =
        [throughSelection dataUsingEncoding:NSUTF8StringEncoding];
    const std::string encoded =
        std::to_string(prefixBytes.length) + ":" +
        std::to_string(selectionBytes.length);
    if (_pxOwner->Dispatch(_pxId, "setSelection", encoded))
        _pxSelection = range;
}
- (NSString*)accessibilitySelectedText {
    if (!_pxHasText || _pxSelection.location == NSNotFound ||
        NSMaxRange(_pxSelection) > _pxValue.length) return @"";
    return [_pxValue substringWithRange:_pxSelection];
}
- (NSRange)accessibilityVisibleCharacterRange {
    return _pxHasText ? NSMakeRange(0, _pxValue.length)
                      : NSMakeRange(NSNotFound, 0);
}
- (NSString*)accessibilityStringForRange:(NSRange)range {
    if (!_pxHasText || NSMaxRange(range) > _pxValue.length) return nil;
    return [_pxValue substringWithRange:range];
}
- (BOOL)accessibilityPerformPress {
    return _pxOwner && _pxOwner->Dispatch(_pxId, "activate");
}
- (BOOL)accessibilityPerformIncrement {
    return _pxOwner && _pxOwner->Dispatch(_pxId, "increment");
}
- (BOOL)accessibilityPerformDecrement {
    return _pxOwner && _pxOwner->Dispatch(_pxId, "decrement");
}
- (BOOL)accessibilityPerformShowMenu {
    return _pxOwner && _pxOwner->Dispatch(_pxId, "showMenu");
}
@end

#endif
