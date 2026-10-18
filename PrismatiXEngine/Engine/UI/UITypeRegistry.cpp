#include "Engine/UI/UITypeRegistry.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"
#include "Engine/Text/Typography.h"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace px::ui {
namespace {
Status TypeError(std::string message) {
    return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error, .code = "PXUI2501",
        .category = "UI.TypeRegistry", .message = std::move(message)});
}
bool SafeSpritePath(const std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string_view::npos
            ? path.size() - start : end - start);
        if (segment == "..") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}
std::optional<double> GlyphNumber(const VariantObject& object,
                                  const std::string_view name) {
    const auto found = object.find(std::string(name));
    if (found == object.end()) return std::nullopt;
    if (const auto* value = found->second.TryGet<double>()) return *value;
    if (const auto* value = found->second.TryGet<std::int64_t>())
        return static_cast<double>(*value);
    return std::nullopt;
}
template <typename T> T* As(Object& object) { return dynamic_cast<T*>(&object); }
template <typename T> const T* As(const Object& object) { return dynamic_cast<const T*>(&object); }

DesignerTypeMetadata DesignerMetadata(std::string displayName, std::string description,
                                      std::string category, std::string iconId,
                                      Vec2 defaultSize, bool canHaveChildren,
                                      VariantObject defaults = {},
                                      std::vector<std::string> acceptedAssets = {},
                                      bool paletteVisible = true) {
    defaults.try_emplace("anchors", Rect{});
    return DesignerTypeMetadata{
        .displayName = std::move(displayName),
        .description = std::move(description),
        .category = std::move(category),
        .iconId = std::move(iconId),
        .defaultSize = defaultSize,
        .defaultProperties = std::move(defaults),
        .acceptedAssetTypes = std::move(acceptedAssets),
        .canHaveChildren = canHaveChildren,
        .paletteVisible = paletteVisible,
    };
}

const DesignerTypeMetadata* BuiltinDesignerMetadata(std::string_view type) {
    // This is an explicit descriptor table, not a name/category heuristic. New
    // controls must opt in here (or provide metadata from their own registrar).
    static const std::unordered_map<std::string, DesignerTypeMetadata> metadata{
        {"Control", DesignerMetadata("Control", "Base UI control.", "Custom", "ui.control",
                                     {180, 52}, true, {}, {}, false)},
        {"Container", DesignerMetadata("Container", "Base managed layout container.", "Layout",
                                       "ui.container", {320, 180}, true, {}, {}, false)},
        {"EdgeRevealContainer", DesignerMetadata("Edge Reveal", "Container revealed from a screen edge.", "Layout",
                                                 "ui.layout.edge-reveal", {320, 180}, true)},
        {"BoxContainer", DesignerMetadata("Box Container", "Base linear layout container.", "Layout",
                                          "ui.layout.box", {320, 180}, true, {}, {}, false)},
        {"HBoxContainer", DesignerMetadata("Horizontal Box", "Arranges children from left to right.", "Layout",
                                           "ui.layout.horizontal", {420, 120}, true)},
        {"VBoxContainer", DesignerMetadata("Vertical Box", "Arranges children from top to bottom.", "Layout",
                                           "ui.layout.vertical", {280, 320}, true)},
        {"MarginContainer", DesignerMetadata("Margin Container", "Adds margins around a child.", "Layout",
                                             "ui.layout.margin", {320, 180}, true)},
        {"CenterContainer", DesignerMetadata("Center Container", "Centers its child in the available area.", "Layout",
                                             "ui.layout.center", {320, 180}, true)},
        {"GridContainer", DesignerMetadata("Grid", "Arranges children in a configurable grid.", "Layout",
                                           "ui.layout.grid", {420, 280}, true)},
        {"StackContainer", DesignerMetadata("Stack", "Stacks children in sibling drawing order.", "Layout",
                                            "ui.layout.stack", {320, 180}, true)},
        {"ScrollContainer", DesignerMetadata("Scroll Container", "Provides a scrollable child viewport.", "Layout",
                                             "ui.layout.scroll", {360, 280}, true)},
        {"AspectRatioContainer", DesignerMetadata("Aspect Ratio", "Keeps its child at a fixed aspect ratio.", "Layout",
                                                  "ui.layout.aspect", {320, 180}, true)},
        {"FlowContainer", DesignerMetadata("Flow", "Wraps children across rows.", "Layout",
                                           "ui.layout.flow", {420, 280}, true)},
        {"TabContainer", DesignerMetadata("Tabs", "Displays one child page at a time.", "Layout",
                                          "ui.layout.tabs", {480, 320}, true)},
        {"Panel", DesignerMetadata("Panel", "Visual grouping surface that can own children.", "Display",
                                   "ui.panel", {320, 180}, true)},
        {"Label", DesignerMetadata("Label", "Single or multiline text label.", "Display", "ui.label",
                                   {200, 40}, false, {{"text", std::string("Text")}}, {"font"})},
        {"Button", DesignerMetadata("Button", "Clickable text button.", "Input", "ui.button",
                                    {180, 52}, false, {{"text", std::string("Button")}}, {"font"})},
        {"TextureButton", DesignerMetadata("Texture Button", "Button with normal, hover, pressed, and disabled images.", "Input",
                                           "ui.texture-button", {180, 52}, false, {}, {"image"})},
        {"IconButton", DesignerMetadata("Icon Button", "Compact button intended for an icon.", "Input",
                                        "ui.icon-button", {52, 52}, false, {{"text", std::string{}}}, {"image"})},
        {"TextureRect", DesignerMetadata("Image", "Displays an image asset.", "Display", "ui.image",
                                         {320, 180}, false,
                                         {{"scaleMode", std::string("Fit")}, {"lockAspectRatio", true}}, {"image"})},
        {"MotionRect", DesignerMetadata("Layered Motion", "Displays a .pxmotion image-layer clip.", "Display",
                                        "ui.motion", {320, 180}, false, {}, {"motion", "image"})},
        {"SpriteText", DesignerMetadata("Sprite Text", "Displays text from a reusable sprite glyph map.", "Display",
                                        "ui.sprite-text", {320, 80}, false, {}, {"image", "text"})},
        {"NinePatchRect", DesignerMetadata("九宮格圖片", "以可伸縮九宮格邊界顯示圖片資源。", "Display", "ui.nine-patch",
                                            {320, 180}, false, {}, {"image"})},
        {"ProgressBar", DesignerMetadata("Progress Bar", "Displays a normalized progress value.", "Display",
                                         "ui.progress", {240, 24}, false)},
        {"VirtualizedView", DesignerMetadata("Virtualized View", "Base large-data view.", "Data",
                                             "ui.data.virtualized", {360, 280}, false, {}, {}, false)},
        {"ListView", DesignerMetadata("List View", "Virtualized vertical data list.", "Data",
                                      "ui.data.list", {360, 280}, false)},
        {"GridView", DesignerMetadata("Grid View", "Virtualized data grid.", "Data",
                                      "ui.data.grid", {420, 320}, false)},
        {"PlacedItemsView", DesignerMetadata("Placed Items", "Data-driven local item rectangles.", "Data",
                                             "ui.data.placed", {420, 320}, false)},
        {"ColorRect", DesignerMetadata("Color Rectangle", "Solid color display rectangle.", "Display",
                                       "ui.color-rect", {240, 160}, false)},
        {"CheckBox", DesignerMetadata("Check Box", "Toggleable checked input.", "Input",
                                      "ui.checkbox", {180, 40}, false, {{"text", std::string("Check Box")}})},
        {"Slider", DesignerMetadata("Slider", "Numeric range input.", "Input", "ui.slider",
                                    {240, 32}, false)},
        {"LineEdit", DesignerMetadata("Line Edit", "Single-line text input.", "Input", "ui.line-edit",
                                      {240, 44}, false)},
        {"TextEdit", DesignerMetadata("多行文字編輯", "可編輯多行文字輸入。", "Input", "ui.text-edit",
                                      {320, 180}, false)},
        {"OptionButton", DesignerMetadata("選項按鈕", "從一組選項中選取單一項目。", "Input", "ui.option-button",
                                          {220, 48}, false)},
        {"SpinBox", DesignerMetadata("數值輸入", "使用鍵盤或增減按鈕編輯數值。", "Input", "ui.spin-box",
                                     {160, 48}, false)},
        {"RadioButton", DesignerMetadata("單選按鈕", "同群組只能選取一項。", "Input", "ui.radio-button",
                                         {180, 40}, false, {{"text",std::string("Radio Button")}})},
        {"Separator", DesignerMetadata("分隔線", "水平或垂直的視覺分隔線。", "Display", "ui.separator",
                                       {180, 8}, false)},
        {"ScrollBar", DesignerMetadata("捲動條", "可控制連續捲動位置。", "Input", "ui.scroll-bar",
                                       {18, 180}, false)},
        {"VideoRect", DesignerMetadata("影片", "在 UI 版面中顯示影片內容。", "Display", "ui.video",
                                       {320, 180}, false, {}, {"video","image"})},
        {"RichTextLabel", DesignerMetadata("Rich Text", "Text label with rich markup and ruby support.", "Display",
                                           "ui.rich-text", {320, 120}, false,
                                           {{"text", std::string("Rich Text")}}, {"font"})},
        {"ComponentInstance", DesignerMetadata("Component", "Instance of a reusable UI component.", "Custom",
                                               "ui.component", {320, 180}, false, {}, {"component"})},
    };
    const auto found = metadata.find(std::string(type));
    return found == metadata.end() ? nullptr : &found->second;
}

PropertyInfo StringProperty(std::string name, std::string category,
                            std::function<std::string(const Object&)> get,
                            std::function<Status(Object&, std::string)> set) {
    return {name, category, VariantType::String,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable, Variant(std::string{}),
        [get = std::move(get)](const Object& object) { return Variant(get(object)); },
        [set = std::move(set), name](Object& object, const Variant& value) {
            const auto* text = value.TryGet<std::string>(); return text ? set(object, *text) : TypeError(name + " expects String");
        }};
}
PropertyInfo BoolProperty(std::string name, std::string category,
                          std::function<bool(const Object&)> get,
                          std::function<Status(Object&, bool)> set) {
    return {name, category, VariantType::Bool,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable, Variant(false),
        [get = std::move(get)](const Object& object) { return Variant(get(object)); },
        [set = std::move(set), name](Object& object, const Variant& value) {
            const auto* boolean = value.TryGet<bool>(); return boolean ? set(object, *boolean) : TypeError(name + " expects Bool");
        }};
}
PropertyInfo NumberProperty(std::string name, std::string category,
                            std::function<double(const Object&)> get,
                            std::function<Status(Object&, double)> set) {
    return {name, category, VariantType::Number,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable, Variant(0.0),
        [get = std::move(get)](const Object& object) { return Variant(get(object)); },
        [set = std::move(set), name](Object& object, const Variant& value) {
            if (const auto* number = value.TryGet<double>()) return set(object, *number);
            if (const auto* integer = value.TryGet<std::int64_t>()) return set(object, static_cast<double>(*integer));
            return TypeError(name + " expects Number");
        }};
}
PropertyInfo IntegerProperty(std::string name, std::string category,
                             std::function<std::int64_t(const Object&)> get,
                             std::function<Status(Object&, std::int64_t)> set) {
    return {name, category, VariantType::Integer,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable, Variant(std::int64_t{}),
        [get = std::move(get)](const Object& object) { return Variant(get(object)); },
        [set = std::move(set), name](Object& object, const Variant& value) {
            if (const auto* integer = value.TryGet<std::int64_t>()) return set(object, *integer);
            return TypeError(name + " expects Integer");
        }};
}
PropertyInfo Vec2Property(std::string name, std::string category,
                          std::function<Vec2(const Object&)> get,
                          std::function<Status(Object&, Vec2)> set) {
    return {name, category, VariantType::Vec2,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable,
        Variant(Vec2{}),
        [get = std::move(get)](const Object& object) { return Variant(get(object)); },
        [set = std::move(set), name](Object& object, const Variant& value) {
            const auto* vector = value.TryGet<Vec2>();
            return vector ? set(object, *vector) : TypeError(name + " expects Vec2");
        }};
}
PropertyInfo ColorProperty(std::string name, std::string category,
                           std::function<Color(const Object&)> get,
                           std::function<Status(Object&, Color)> set) {
    return {name, category, VariantType::Color,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable, Variant(Color{255,255,255,255}),
        [get = std::move(get)](const Object& object) { return Variant(get(object)); },
        [set = std::move(set), name](Object& object, const Variant& value) {
            const auto* color = value.TryGet<Color>();
            return color ? set(object, *color) : TypeError(name + " expects Color");
        }};
}

std::string SizeFlagName(const SizeFlag value) {
    if (HasFlag(value, SizeFlag::Expand) && HasFlag(value, SizeFlag::Fill)) return "ExpandFill";
    if (HasFlag(value, SizeFlag::ShrinkCenter)) return "ShrinkCenter";
    if (HasFlag(value, SizeFlag::ShrinkEnd)) return "ShrinkEnd";
    if (HasFlag(value, SizeFlag::Fill)) return "Fill";
    return "ShrinkBegin";
}

bool ParseSizeFlag(const std::string_view text, SizeFlag& output) {
    if (text == "ExpandFill") output = SizeFlag::Expand | SizeFlag::Fill;
    else if (text == "Fill") output = SizeFlag::Fill;
    else if (text == "ShrinkCenter") output = SizeFlag::ShrinkCenter;
    else if (text == "ShrinkEnd") output = SizeFlag::ShrinkEnd;
    else if (text == "ShrinkBegin") output = SizeFlag::ShrinkBegin;
    else return false;
    return true;
}
}

Status RegisterBuiltinUITypes() {
    static std::mutex registrationMutex;
    const std::lock_guard registrationLock(registrationMutex);
    auto& registry = TypeRegistry::Global();
    if (registry.Find("Button") &&
        registry.FindProperty("Button", "opacity"))
        return Status::Ok();
    Status combined;
    auto add = [&combined, &registry](TypeInfo type) {
        if (!type.designer) {
            if (const auto* metadata = BuiltinDesignerMetadata(type.name))
                type.designer = *metadata;
        }
        for (auto& property : type.properties) {
            property.bindable = HasFlag(property.flags, PropertyFlags::Bindable);
            property.animatable = property.set &&
                (property.type == VariantType::Bool || property.type == VariantType::Integer ||
                 property.type == VariantType::Number || property.type == VariantType::String ||
                 property.type == VariantType::Vec2 || property.type == VariantType::Rect ||
                 property.type == VariantType::Color);
            if (property.category == "Layout" || property.name == "visibility")
                property.impact = PropertyImpact::Layout | PropertyImpact::Paint;
            else if (property.category == "Theme")
                property.impact = PropertyImpact::Theme | PropertyImpact::Paint;
            else
                property.impact = PropertyImpact::Paint;
            if (property.name == "offsets")
                property.ownership = PropertyOwnership::ParentLayout;
            property.advanced = property.category == "Transform" ||
                                property.category == "Behavior";
        }
        auto status = registry.Register(std::move(type));
        for (const auto& d : status.Diagnostics()) combined.Add(d);
    };

    add({"Node", "", "Core", [] { return std::make_unique<scene::Node>(); }});
    TypeInfo control{"Control", "Node", "UI/Core", [] { return std::make_unique<Control>(); }};
    control.properties.push_back({"anchors", "Layout", VariantType::Rect,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(Rect{}),
        [](const Object& o) { return Variant(As<Control>(o)->Anchors()); },
        [](Object& o, const Variant& v) { if (auto* p = v.TryGet<Rect>()) { As<Control>(o)->SetAnchors(*p); return Status::Ok(); } return TypeError("anchors expects Rect"); }});
    control.properties.push_back({"offsets", "Layout", VariantType::Rect,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(Rect{}),
        [](const Object& o) { return Variant(As<Control>(o)->Offsets()); },
        [](Object& o, const Variant& v) { if (auto* p = v.TryGet<Rect>()) { As<Control>(o)->SetOffsets(*p); return Status::Ok(); } return TypeError("offsets expects Rect"); }});
    control.properties.push_back(BoolProperty("enabled", "Interaction", [](const Object& o) { return As<Control>(o)->Enabled(); },
        [](Object& o, bool v) { As<Control>(o)->SetEnabled(v); return Status::Ok(); }));
    // JSX exposes `visible` as a bool. Make the same property bindable at
    // Runtime, so authored modal surfaces can react to ViewModel state.
    control.properties.push_back(BoolProperty("visible", "Behavior", [](const Object& o) {
        return As<Control>(o)->GetVisibility() == Visibility::Visible;
    }, [](Object& o, bool v) {
        As<Control>(o)->SetVisibility(v ? Visibility::Visible : Visibility::Hidden);
        return Status::Ok();
    }));
    auto showTransition = NumberProperty("showTransitionSeconds", "Display", [](const Object& o) {
        return As<Control>(o)->ShowTransitionSeconds();
    }, [](Object& o, double v) {
        if (!std::isfinite(v) || v < 0.0 || v > 2.0)
            return TypeError("showTransitionSeconds must be between 0 and 2");
        As<Control>(o)->SetShowTransitionSeconds(static_cast<float>(v));
        return Status::Ok();
    });
    showTransition.editor.hasRange = true;
    showTransition.editor.minimum = 0;
    showTransition.editor.maximum = 2;
    showTransition.editor.step = 0.01;
    control.properties.push_back(std::move(showTransition));
    auto hideTransition = NumberProperty("hideTransitionSeconds", "Display", [](const Object& o) {
        return As<Control>(o)->HideTransitionSeconds();
    }, [](Object& o, double v) {
        if (!std::isfinite(v) || v < 0.0 || v > 2.0)
            return TypeError("hideTransitionSeconds must be between 0 and 2");
        As<Control>(o)->SetHideTransitionSeconds(static_cast<float>(v));
        return Status::Ok();
    });
    hideTransition.editor.hasRange = true;
    hideTransition.editor.minimum = 0;
    hideTransition.editor.maximum = 2;
    hideTransition.editor.step = 0.01;
    hideTransition.editor.description = "Keeps drawing the control while it fades out after it is hidden.";
    control.properties.push_back(std::move(hideTransition));
    for (const bool vertical : {true, false}) {
        for (const bool showing : {true, false}) {
            const std::string name = std::string(showing ? "show" : "hide") +
                                     (vertical ? "OffsetY" : "OffsetX");
            auto offset = NumberProperty(name, "Display",
                [showing, vertical](const Object& o) {
                    const auto* control = As<Control>(o);
                    return static_cast<double>(vertical
                        ? (showing ? control->ShowOffsetY() : control->HideOffsetY())
                        : (showing ? control->ShowOffsetX() : control->HideOffsetX()));
                }, [showing, vertical](Object& o, double v) {
                    if (!std::isfinite(v) || v < -2000.0 || v > 2000.0)
                        return TypeError("transition offsets must be between -2000 and 2000");
                    auto* control = As<Control>(o);
                    const float value = static_cast<float>(v);
                    if (vertical) {
                        if (showing) control->SetShowOffsetY(value); else control->SetHideOffsetY(value);
                    } else {
                        if (showing) control->SetShowOffsetX(value); else control->SetHideOffsetX(value);
                    }
                    return Status::Ok();
                });
            offset.editor.description = showing
                ? "Distance the control starts from its place while it appears."
                : "Distance the control travels while it disappears.";
            control.properties.push_back(std::move(offset));
        }
    }
    auto linearTransition = BoolProperty("linearTransition", "Display", [](const Object& o) {
        return As<Control>(o)->LinearTransition();
    }, [](Object& o, bool v) { As<Control>(o)->SetLinearTransition(v); return Status::Ok(); });
    linearTransition.editor.description = "Use linear instead of squared progress for show and hide transitions.";
    control.properties.push_back(std::move(linearTransition));
    for (const bool show : {true, false}) {
        const auto name = [](TransitionEase e) {
            switch (e) {
                case TransitionEase::Linear: return std::string("linear");
                case TransitionEase::Cubic: return std::string("cubic");
                case TransitionEase::Back: return std::string("back");
                default: return std::string("quad");
            }
        };
        auto ease = StringProperty(show ? "showTransitionEase" : "hideTransitionEase", "Display",
            [show, name](const Object& o) {
                const auto* c = As<Control>(o);
                return name(show ? c->ShowEase() : c->HideEase());
            },
            [show](Object& o, std::string v) {
                TransitionEase e;
                if (v == "quad") e = TransitionEase::Quad;
                else if (v == "linear") e = TransitionEase::Linear;
                else if (v == "cubic") e = TransitionEase::Cubic;
                else if (v == "back") e = TransitionEase::Back;
                else return TypeError("transition ease expects quad, linear, cubic or back");
                if (show) As<Control>(o)->SetShowEase(e); else As<Control>(o)->SetHideEase(e);
                return Status::Ok();
            });
        ease.editor.enumChoices = {"quad", "linear", "cubic", "back"};
        ease.editor.description = show
            ? "Curve of the show slide; back overshoots and settles."
            : "Curve of the hide slide; back pulls back before leaving.";
        control.properties.push_back(std::move(ease));
    }
    // Delay, wipe, scale, fade, stagger and hover: the rest of an authored
    // show/hide animation.
    const auto ranged = [](PropertyInfo info, double minimum, double maximum, double step,
                           std::string description) {
        info.editor.hasRange = true;
        info.editor.minimum = minimum;
        info.editor.maximum = maximum;
        info.editor.step = step;
        info.editor.description = std::move(description);
        return info;
    };
    control.properties.push_back(ranged(NumberProperty("showDelaySeconds", "Display",
        [](const Object& o) { return static_cast<double>(As<Control>(o)->ShowDelaySeconds()); },
        [](Object& o, double v) {
            if (!std::isfinite(v) || v < 0.0 || v > 10.0) return TypeError("showDelaySeconds must be between 0 and 10");
            As<Control>(o)->SetShowDelaySeconds(static_cast<float>(v));
            return Status::Ok();
        }), 0, 10, 0.01, "Seconds the control waits, unseen, before its show transition starts."));
    control.properties.push_back(ranged(NumberProperty("staggerSeconds", "Display",
        [](const Object& o) { return static_cast<double>(As<Control>(o)->StaggerSeconds()); },
        [](Object& o, double v) {
            if (!std::isfinite(v) || v < 0.0 || v > 2.0) return TypeError("staggerSeconds must be between 0 and 2");
            As<Control>(o)->SetStaggerSeconds(static_cast<float>(v));
            return Status::Ok();
        }), 0, 2, 0.01, "Each visible child (list or grid item) starts its show transition this much after the previous one."));
    control.properties.push_back(ranged(NumberProperty("hoverScale", "Display",
        [](const Object& o) { return static_cast<double>(As<Control>(o)->HoverScale()); },
        [](Object& o, double v) {
            if (!std::isfinite(v) || v < 0.1 || v > 4.0) return TypeError("hoverScale must be between 0.1 and 4");
            As<Control>(o)->SetHoverScale(static_cast<float>(v));
            return Status::Ok();
        }), 0.1, 4, 0.01, "Scale the control eases to while the pointer is over it; 1 is none."));
    control.properties.push_back(ranged(NumberProperty("hoverTransitionSeconds", "Display",
        [](const Object& o) { return static_cast<double>(As<Control>(o)->HoverTransitionSeconds()); },
        [](Object& o, double v) {
            if (!std::isfinite(v) || v < 0.0 || v > 2.0) return TypeError("hoverTransitionSeconds must be between 0 and 2");
            As<Control>(o)->SetHoverTransitionSeconds(static_cast<float>(v));
            return Status::Ok();
        }), 0, 2, 0.01, "Seconds the hover scale takes to settle."));
    for (const bool show : {true, false}) {
        auto wipe = StringProperty(show ? "showWipe" : "hideWipe", "Display",
            [show](const Object& o) {
                const auto* c = As<Control>(o);
                return std::string(TransitionWipeName(show ? c->ShowWipe() : c->HideWipe()));
            },
            [show](Object& o, std::string v) {
                const auto wipe = ParseTransitionWipe(v);
                if (!wipe) return TypeError("wipe expects none, left, right, up or down");
                if (show) As<Control>(o)->SetShowWipe(*wipe); else As<Control>(o)->SetHideWipe(*wipe);
                return Status::Ok();
            });
        wipe.editor.enumChoices = {"none", "left", "right", "up", "down"};
        wipe.editor.description = show
            ? "Reveals the control from this edge as it appears (left: left to right)."
            : "Takes the control away towards this edge as it disappears.";
        control.properties.push_back(std::move(wipe));
        control.properties.push_back(ranged(NumberProperty(show ? "showScale" : "hideScale", "Display",
            [show](const Object& o) {
                const auto* c = As<Control>(o);
                return static_cast<double>(show ? c->ShowScale() : c->HideScale());
            },
            [show](Object& o, double v) {
                if (!std::isfinite(v) || v < 0.0 || v > 10.0) return TypeError("transition scale must be between 0 and 10");
                if (show) As<Control>(o)->SetShowScale(static_cast<float>(v));
                else As<Control>(o)->SetHideScale(static_cast<float>(v));
                return Status::Ok();
            }), 0, 10, 0.01, show ? "Scale the control grows from as it appears; 1 is none."
                                  : "Scale the control ends at as it disappears; 1 is none."));
        auto fade = BoolProperty(show ? "showFade" : "hideFade", "Display",
            [show](const Object& o) {
                const auto* c = As<Control>(o);
                return show ? c->ShowFade() : c->HideFade();
            },
            [show](Object& o, bool v) {
                if (show) As<Control>(o)->SetShowFade(v); else As<Control>(o)->SetHideFade(v);
                return Status::Ok();
            });
        fade.editor.description = "Whether the transition fades the opacity (on by default).";
        control.properties.push_back(std::move(fade));
    }
    auto visibilityProperty=StringProperty("visibility", "Behavior", [](const Object& o) {
        switch (As<Control>(o)->GetVisibility()) { case Visibility::Visible: return std::string("Visible"); case Visibility::Hidden: return std::string("Hidden"); default: return std::string("Collapsed"); }
    }, [](Object& o, std::string v) {
        if (v == "Visible") As<Control>(o)->SetVisibility(Visibility::Visible);
        else if (v == "Hidden") As<Control>(o)->SetVisibility(Visibility::Hidden);
        else if (v == "Collapsed") As<Control>(o)->SetVisibility(Visibility::Collapsed);
        else return TypeError("visibility expects Visible, Hidden, or Collapsed"); return Status::Ok();
    });
    visibilityProperty.editor.displayName="Visibility";visibilityProperty.editor.enumChoices={"Visible","Hidden","Collapsed"};visibilityProperty.editor.description="Hidden keeps layout space; Collapsed removes it from layout.";control.properties.push_back(std::move(visibilityProperty));
    control.properties.push_back(Vec2Property("minimumSize", "Layout", [](const Object& o) { return As<Control>(o)->CustomMinimumSize(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetCustomMinimumSize(v); return Status::Ok(); }));
    control.properties.push_back(Vec2Property("maximumSize", "Layout", [](const Object& o) { return As<Control>(o)->MaximumSize(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetMaximumSize(v); return Status::Ok(); }));
    control.properties.push_back(NumberProperty("stretchRatio", "Layout", [](const Object& o) { return As<Control>(o)->StretchRatio(); },
        [](Object& o, double v) { As<Control>(o)->SetStretchRatio(static_cast<float>(v)); return Status::Ok(); }));
    auto horizontalFlags = StringProperty("horizontalSizeFlags", "Layout", [](const Object& o) {
        return SizeFlagName(As<Control>(o)->HorizontalSizeFlags());
    }, [](Object& o, std::string value) {
        SizeFlag parsed{}; if (!ParseSizeFlag(value, parsed)) return TypeError("horizontalSizeFlags has an unknown value");
        auto* controlObject = As<Control>(o); controlObject->SetSizeFlags(parsed, controlObject->VerticalSizeFlags()); return Status::Ok();
    });
    horizontalFlags.editor.displayName = "水平尺寸旗標";
    horizontalFlags.editor.enumChoices = {"ShrinkBegin", "Fill", "ExpandFill", "ShrinkCenter", "ShrinkEnd"};
    horizontalFlags.editor.description = "決定此元件在受管理容器中的水平配置方式。";
    control.properties.push_back(std::move(horizontalFlags));
    auto verticalFlags = StringProperty("verticalSizeFlags", "Layout", [](const Object& o) {
        return SizeFlagName(As<Control>(o)->VerticalSizeFlags());
    }, [](Object& o, std::string value) {
        SizeFlag parsed{}; if (!ParseSizeFlag(value, parsed)) return TypeError("verticalSizeFlags has an unknown value");
        auto* controlObject = As<Control>(o); controlObject->SetSizeFlags(controlObject->HorizontalSizeFlags(), parsed); return Status::Ok();
    });
    verticalFlags.editor.displayName = "垂直尺寸旗標";
    verticalFlags.editor.enumChoices = {"ShrinkBegin", "Fill", "ExpandFill", "ShrinkCenter", "ShrinkEnd"};
    verticalFlags.editor.description = "決定此元件在受管理容器中的垂直配置方式。";
    control.properties.push_back(std::move(verticalFlags));
    auto mouseFilter = StringProperty("mouseFilter", "Interaction", [](const Object& o) {
        switch (As<Control>(o)->GetMouseFilter()) { case MouseFilter::Stop: return std::string("Stop"); case MouseFilter::Pass: return std::string("Pass"); default: return std::string("Ignore"); }
    }, [](Object& o, std::string value) {
        if (value == "Stop") As<Control>(o)->SetMouseFilter(MouseFilter::Stop);
        else if (value == "Pass") As<Control>(o)->SetMouseFilter(MouseFilter::Pass);
        else if (value == "Ignore") As<Control>(o)->SetMouseFilter(MouseFilter::Ignore);
        else return TypeError("mouseFilter expects Stop, Pass, or Ignore"); return Status::Ok();
    });
    mouseFilter.editor.displayName = "滑鼠過濾"; mouseFilter.editor.enumChoices = {"Stop", "Pass", "Ignore"};
    mouseFilter.editor.description = "控制滑鼠事件是否停止、向父層傳遞，或完全忽略此元件。";
    control.properties.push_back(std::move(mouseFilter));
    control.properties.push_back(BoolProperty("modalCapture", "Interaction",
        [](const Object& o) { return As<Control>(o)->ModalCapture(); },
        [](Object& o, bool value) {
            As<Control>(o)->SetModalCapture(value);
            return Status::Ok();
        }));
    auto focusMode = StringProperty("focusMode", "Interaction", [](const Object& o) {
        switch (As<Control>(o)->GetFocusMode()) { case FocusMode::None: return std::string("None"); case FocusMode::Click: return std::string("Click"); default: return std::string("All"); }
    }, [](Object& o, std::string value) {
        if (value == "None") As<Control>(o)->SetFocusMode(FocusMode::None);
        else if (value == "Click") As<Control>(o)->SetFocusMode(FocusMode::Click);
        else if (value == "All") As<Control>(o)->SetFocusMode(FocusMode::All);
        else return TypeError("focusMode expects None, Click, or All"); return Status::Ok();
    });
    focusMode.editor.displayName = "焦點模式"; focusMode.editor.enumChoices = {"None", "Click", "All"};
    focusMode.editor.description = "控制滑鼠點擊與 Tab 導覽是否能讓此元件取得鍵盤焦點。";
    control.properties.push_back(std::move(focusMode));
    control.properties.push_back(StringProperty("tooltip", "Interaction", [](const Object& o) { return As<Control>(o)->Tooltip(); },
        [](Object& o, std::string v) { As<Control>(o)->SetTooltip(std::move(v)); return Status::Ok(); }));
    control.properties.push_back(StringProperty("hoverSoundPath", "Audio", [](const Object& o) { return As<Control>(o)->HoverSoundPath(); },
        [](Object& o, std::string v) { As<Control>(o)->SetHoverSoundPath(std::move(v)); return Status::Ok(); }));
    control.properties.push_back(StringProperty("activateSoundPath", "Audio", [](const Object& o) { return As<Control>(o)->ActivateSoundPath(); },
        [](Object& o, std::string v) { As<Control>(o)->SetActivateSoundPath(std::move(v)); return Status::Ok(); }));
    // A sound as the control appears (show) or starts to hide.
    for (const bool show : {true, false}) {
        const std::string prefix = show ? "showSound" : "hideSound";
        const auto sound = [show](Object& o) -> Control::Sound& {
            return show ? As<Control>(o)->EditShowSound() : As<Control>(o)->EditHideSound();
        };
        const auto read = [show](const Object& o) -> const Control::Sound& {
            return show ? As<Control>(o)->ShowSound() : As<Control>(o)->HideSound();
        };
        auto path = StringProperty(prefix + "Path", "Audio", [read](const Object& o) { return read(o).path; },
            [sound](Object& o, std::string v) { sound(o).path = std::move(v); return Status::Ok(); });
        path.editor.description = show
            ? "Sound played as the control appears: when its show transition starts, after its delay and any stagger."
            : "Sound played as the control starts to hide.";
        control.properties.push_back(std::move(path));
        control.properties.push_back({prefix + "Variants", "Audio", VariantType::Array,
            PropertyFlags::Editable | PropertyFlags::Serializable, Variant(VariantArray{}),
            [read](const Object& o) {
                VariantArray values;
                for (const auto& variant : read(o).variants) values.emplace_back(variant);
                return Variant(std::move(values));
            },
            [sound, prefix](Object& o, const Variant& v) {
                const auto* array = v.AsArray();
                if (!array) return TypeError(prefix + "Variants expects Array");
                std::vector<std::string> values;
                for (const auto& item : *array) {
                    const auto* text = item.TryGet<std::string>();
                    if (!text || text->empty()) return TypeError(prefix + "Variants entries must be non-empty String");
                    values.push_back(*text);
                }
                sound(o).variants = std::move(values);
                return Status::Ok();
            }});
        auto volume = NumberProperty(prefix + "Volume", "Audio",
            [read](const Object& o) { return static_cast<double>(read(o).volume); },
            [sound, prefix](Object& o, double v) {
                if (!std::isfinite(v) || v < 0.0 || v > 1.0) return TypeError(prefix + "Volume must be between 0 and 1");
                sound(o).volume = static_cast<float>(v);
                return Status::Ok();
            });
        volume.editor.hasRange = true;
        volume.editor.minimum = 0;
        volume.editor.maximum = 1;
        volume.editor.step = 0.05;
        volume.editor.description = "Volume of the sound before the player's SE volume (1 is full).";
        control.properties.push_back(std::move(volume));
    }
    {
        auto delay = NumberProperty("showSoundDelaySeconds", "Audio",
            [](const Object& o) { return static_cast<double>(As<Control>(o)->ShowSound().delaySeconds); },
            [](Object& o, double v) {
                if (!std::isfinite(v) || v < 0.0 || v > 10.0) return TypeError("showSoundDelaySeconds must be between 0 and 10");
                As<Control>(o)->EditShowSound().delaySeconds = static_cast<float>(v);
                return Status::Ok();
            });
        delay.editor.hasRange = true;
        delay.editor.minimum = 0;
        delay.editor.maximum = 10;
        delay.editor.step = 0.05;
        delay.editor.description = "Seconds the show sound waits after the control begins to show; on a screen's root, after the screen opens.";
        control.properties.push_back(std::move(delay));
    }
    control.properties.push_back(StringProperty("accessibilityLabel", "Accessibility", [](const Object& o) { return As<Control>(o)->AccessibilityLabel(); },
        [](Object& o, std::string v) { As<Control>(o)->SetAccessibilityLabel(std::move(v)); return Status::Ok(); }));
    control.properties.push_back(StringProperty("accessibilityRole", "Accessibility", [](const Object& o) { return As<Control>(o)->AccessibilityRole(); },
        [](Object& o, std::string v) { As<Control>(o)->SetAccessibilityRole(std::move(v)); return Status::Ok(); }));
    control.properties.push_back(StringProperty("accessibilityDescription", "Accessibility", [](const Object& o) { return As<Control>(o)->AccessibilityDescription(); },
        [](Object& o, std::string v) { As<Control>(o)->SetAccessibilityDescription(std::move(v)); return Status::Ok(); }));
    control.properties.push_back(IntegerProperty("accessibilityFocusOrder", "Accessibility", [](const Object& o) { return static_cast<std::int64_t>(As<Control>(o)->AccessibilityFocusOrder()); },
        [](Object& o, std::int64_t v) {
            if (v < -1000000 || v > 1000000) return TypeError("accessibilityFocusOrder is out of range");
            As<Control>(o)->SetAccessibilityFocusOrder(static_cast<std::int32_t>(v)); return Status::Ok();
        }));
    control.properties.push_back(Vec2Property("pivot", "Transform", [](const Object& o) { return As<Control>(o)->Pivot(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetPivot(v); return Status::Ok(); }));
    control.properties.push_back(Vec2Property("scale", "Transform", [](const Object& o) { return As<Control>(o)->Scale(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetScale(v); return Status::Ok(); }));
    control.properties.push_back(Vec2Property("translation", "Transform", [](const Object& o) { return As<Control>(o)->Translation(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetTranslation(v); return Status::Ok(); }));
    auto rotation = NumberProperty("rotation", "Transform", [](const Object& o) { return As<Control>(o)->Rotation(); },
        [](Object& o, double v) { As<Control>(o)->SetRotation(static_cast<float>(v)); return Status::Ok(); });
    rotation.editor.displayName = "旋轉"; rotation.editor.description = "以度數表示，繞 pivot 旋轉。";
    rotation.editor.hasRange = true; rotation.editor.minimum = -360.0; rotation.editor.maximum = 360.0; rotation.editor.step = 0.1;
    control.properties.push_back(std::move(rotation));
    auto controlOpacity = NumberProperty("opacity", "Display", [](const Object& o) { return As<Control>(o)->Opacity(); },
        [](Object& o, double v) { As<Control>(o)->SetOpacity(static_cast<float>(v)); return Status::Ok(); });
    controlOpacity.editor.displayName = "不透明度"; controlOpacity.editor.hasRange = true; controlOpacity.editor.minimum = 0.0; controlOpacity.editor.maximum = 1.0; controlOpacity.editor.step = 0.01;
    controlOpacity.editor.tokenBindable = true; control.properties.push_back(std::move(controlOpacity));
    auto modulate = ColorProperty("modulate", "Display", [](const Object& o) { return As<Control>(o)->Modulate(); },
        [](Object& o, Color v) { As<Control>(o)->SetModulate(v); return Status::Ok(); });
    modulate.editor.displayName = "調變色"; modulate.editor.description = "與元件及其子元件的最終顏色相乘。"; modulate.editor.tokenBindable = true;
    control.properties.push_back(std::move(modulate));
    PropertyInfo styleToken{
        "styleToken", "Theme", VariantType::TokenRef,
        PropertyFlags::Editable | PropertyFlags::Serializable,
        Variant(TokenRefValue{}),
        [](const Object& o) { return Variant(As<Control>(o)->StyleToken()); },
        [](Object& o, const Variant& value) {
            const auto* token = value.TryGet<TokenRefValue>();
            if (!token || token->name.empty())
                return TypeError("styleToken expects a non-empty TokenRef");
            As<Control>(o)->SetStyleToken(*token);
            return Status::Ok();
        }};
    styleToken.editor.displayName = "Style token";
    styleToken.editor.description = "Stable theme token identity used by this control.";
    control.properties.push_back(std::move(styleToken));
    auto clipping = BoolProperty("clipContent", "Display", [](const Object& o) { return As<Control>(o)->ClipContent(); },
        [](Object& o, bool v) { As<Control>(o)->SetClipContent(v); return Status::Ok(); });
    clipping.editor.displayName = "裁切內容"; clipping.editor.description = "將子元件繪製結果限制在此元件的邊界內。";
    control.properties.push_back(std::move(clipping));
    control.signals.push_back({"pointerEntered", "滑鼠進入", "Pointer entered this Control.", {}});
    control.signals.push_back({"pointerExited", "滑鼠離開", "Pointer exited this Control.", {}});
    control.signals.push_back({"pointerDown", "按下", "Primary pointer was pressed.", {{"position", VariantType::Vec2}}});
    control.signals.push_back({"pointerUp", "放開", "Primary pointer was released.", {{"position", VariantType::Vec2}}});
    control.signals.push_back({"clicked", "點擊", "Primary pointer clicked this Control.", {{"position", VariantType::Vec2}}});
    control.signals.push_back({"scrolled", "捲動", "Pointer wheel changed over this Control.", {{"value", VariantType::Number}}});
    control.signals.push_back({"focusEntered", "取得焦點", "Keyboard focus entered this Control.", {}});
    control.signals.push_back({"focusExited", "失去焦點", "Keyboard focus exited this Control.", {}});
    add(std::move(control));

    // ComponentInstance is expanded before runtime control construction.  It
    // is still registered as an editor-visible typed record so palettes and
    // property registries never need a hard-coded pseudo type.
    TypeInfo componentInstance{"ComponentInstance", "Control", "UI/Custom",
                               [] { return std::make_unique<Control>(); }};
    componentInstance.properties.push_back({
        "component", "Data", VariantType::ResourceRef,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(ResourceRefValue{})});
    componentInstance.properties.push_back({
        "overrides", "Data", VariantType::Object,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(VariantObject{})});
    componentInstance.properties.push_back({
        "componentProperties", "Data", VariantType::Object,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(VariantObject{})});
    componentInstance.properties.push_back({
        "componentEvents", "Events", VariantType::Object,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(VariantObject{})});
    add(std::move(componentInstance));

    add({"Container", "Control", "UI/Layout", [] { return std::make_unique<Container>(); }});
    TypeInfo edgeReveal{"EdgeRevealContainer","Container","UI/Layout",[]{return std::make_unique<EdgeRevealContainer>();}};
    auto edge=StringProperty("edge","Behavior",[](const Object& o){switch(As<EdgeRevealContainer>(o)->Edge()){case RevealEdge::Bottom:return std::string("Bottom");case RevealEdge::Left:return std::string("Left");case RevealEdge::Right:return std::string("Right");default:return std::string("Top");}},[](Object& o,std::string v){if(v=="Top")As<EdgeRevealContainer>(o)->SetEdge(RevealEdge::Top);else if(v=="Bottom")As<EdgeRevealContainer>(o)->SetEdge(RevealEdge::Bottom);else if(v=="Left")As<EdgeRevealContainer>(o)->SetEdge(RevealEdge::Left);else if(v=="Right")As<EdgeRevealContainer>(o)->SetEdge(RevealEdge::Right);else return TypeError("edge expects Top, Bottom, Left, or Right");return Status::Ok();});edge.editor.displayName="Reveal edge";edge.editor.enumChoices={"Top","Bottom","Left","Right"};edgeReveal.properties.push_back(std::move(edge));
    auto revealSpeed=NumberProperty("revealSpeed","Behavior",[](const Object& o){return static_cast<double>(As<EdgeRevealContainer>(o)->Speed());},[](Object& o,double v){As<EdgeRevealContainer>(o)->SetSpeed(static_cast<float>(v));return Status::Ok();});revealSpeed.editor.hasRange=true;revealSpeed.editor.minimum=.1;revealSpeed.editor.maximum=30;revealSpeed.editor.step=.1;edgeReveal.properties.push_back(std::move(revealSpeed));
    auto triggerSize=NumberProperty("triggerSize","Behavior",[](const Object& o){return static_cast<double>(As<EdgeRevealContainer>(o)->TriggerSize());},[](Object& o,double v){As<EdgeRevealContainer>(o)->SetTriggerSize(static_cast<float>(v));return Status::Ok();});triggerSize.editor.hasRange=true;triggerSize.editor.minimum=1;triggerSize.editor.maximum=64;triggerSize.editor.step=1;edgeReveal.properties.push_back(std::move(triggerSize));
    auto revealDistance=NumberProperty("revealDistance","Behavior",[](const Object& o){return static_cast<double>(As<EdgeRevealContainer>(o)->RevealDistance());},[](Object& o,double v){As<EdgeRevealContainer>(o)->SetRevealDistance(static_cast<float>(v));return Status::Ok();});revealDistance.editor.hasRange=true;revealDistance.editor.minimum=0;revealDistance.editor.maximum=1920;revealDistance.editor.step=1;edgeReveal.properties.push_back(std::move(revealDistance));
    edgeReveal.properties.push_back(BoolProperty("fadeWithReveal","Behavior",[](const Object& o){return As<EdgeRevealContainer>(o)->FadeWithReveal();},[](Object& o,bool v){As<EdgeRevealContainer>(o)->SetFadeWithReveal(v);return Status::Ok();}));
    auto revealTrigger=StringProperty("revealTrigger","Behavior",[](const Object& o){return As<EdgeRevealContainer>(o)->Trigger()==RevealTrigger::Manual?std::string("Manual"):std::string("Hover");},[](Object& o,std::string v){if(v=="Hover")As<EdgeRevealContainer>(o)->SetTrigger(RevealTrigger::Hover);else if(v=="Manual")As<EdgeRevealContainer>(o)->SetTrigger(RevealTrigger::Manual);else return TypeError("revealTrigger expects Hover or Manual");return Status::Ok();});revealTrigger.editor.displayName="Reveal trigger";revealTrigger.editor.enumChoices={"Hover","Manual"};edgeReveal.properties.push_back(std::move(revealTrigger));
    auto revealEasing=StringProperty("revealEasing","Behavior",[](const Object& o){switch(As<EdgeRevealContainer>(o)->Easing()){case RevealEasing::Linear:return std::string("Linear");case RevealEasing::EaseInOut:return std::string("EaseInOut");default:return std::string("EaseOut");}},[](Object& o,std::string v){if(v=="Linear")As<EdgeRevealContainer>(o)->SetEasing(RevealEasing::Linear);else if(v=="EaseOut")As<EdgeRevealContainer>(o)->SetEasing(RevealEasing::EaseOut);else if(v=="EaseInOut")As<EdgeRevealContainer>(o)->SetEasing(RevealEasing::EaseInOut);else return TypeError("revealEasing expects Linear, EaseOut, or EaseInOut");return Status::Ok();});revealEasing.editor.displayName="Reveal easing";revealEasing.editor.enumChoices={"Linear","EaseOut","EaseInOut"};edgeReveal.properties.push_back(std::move(revealEasing));
    edgeReveal.properties.push_back(BoolProperty("pinned","Behavior",[](const Object& o){return As<EdgeRevealContainer>(o)->Pinned();},[](Object& o,bool v){As<EdgeRevealContainer>(o)->SetPinned(v);return Status::Ok();}));add(std::move(edgeReveal));
    TypeInfo box{"BoxContainer", "Container", "UI/Layout", [] { return std::make_unique<BoxContainer>(Orientation::Horizontal); }};
    box.properties.push_back(NumberProperty("separation", "Layout", [](const Object& o) { return As<BoxContainer>(o)->Separation(); },
        [](Object& o, double v) { As<BoxContainer>(o)->SetSeparation(static_cast<float>(v)); return Status::Ok(); })); add(std::move(box));
    add({"HBoxContainer", "BoxContainer", "UI/Layout", [] { return std::make_unique<HBoxContainer>(); }});
    add({"VBoxContainer", "BoxContainer", "UI/Layout", [] { return std::make_unique<VBoxContainer>(); }});
    TypeInfo margin{"MarginContainer", "Container", "UI/Layout", [] { return std::make_unique<MarginContainer>(); }};
    margin.properties.push_back({"margins","Layout",VariantType::Rect,PropertyFlags::Editable|PropertyFlags::Serializable,Variant(Rect{}),
        [](const Object& o){return Variant(As<MarginContainer>(o)->Margins());},[](Object& o,const Variant& v){if(const auto* r=v.TryGet<Rect>()){As<MarginContainer>(o)->SetMargins(r->x,r->y,r->w,r->h);return Status::Ok();}return TypeError("margins expects Rect");}}); add(std::move(margin));
    add({"CenterContainer", "Container", "UI/Layout", [] { return std::make_unique<CenterContainer>(); }});
    TypeInfo grid{"GridContainer", "Container", "UI/Layout", [] { return std::make_unique<GridContainer>(); }};
    grid.properties.push_back(NumberProperty("columns","Layout",[](const Object& o){return static_cast<double>(As<GridContainer>(o)->Columns());},[](Object& o,double v){As<GridContainer>(o)->SetColumns(static_cast<std::size_t>(std::max(1.0,v)));return Status::Ok();}));
    grid.properties.push_back(Vec2Property("gaps","Layout",[](const Object& o){return As<GridContainer>(o)->Gaps();},[](Object& o,Vec2 v){As<GridContainer>(o)->SetGaps(v);return Status::Ok();}));add(std::move(grid));
    add({"StackContainer", "Container", "UI/Layout", [] { return std::make_unique<StackContainer>(); }});
    add({"ScrollContainer", "Container", "UI/Layout", [] { return std::make_unique<ScrollContainer>(); }});
    TypeInfo aspect{"AspectRatioContainer","Container","UI/Layout",[]{return std::make_unique<AspectRatioContainer>();}};
    aspect.properties.push_back(NumberProperty("ratio","Layout",[](const Object& o){return As<AspectRatioContainer>(o)->Ratio();},[](Object& o,double v){As<AspectRatioContainer>(o)->SetRatio(static_cast<float>(v));return Status::Ok();}));add(std::move(aspect));
    TypeInfo flow{"FlowContainer","Container","UI/Layout",[]{return std::make_unique<FlowContainer>();}};
    flow.properties.push_back(Vec2Property("gaps","Layout",[](const Object& o){return As<FlowContainer>(o)->Gaps();},[](Object& o,Vec2 v){As<FlowContainer>(o)->SetGaps(v);return Status::Ok();}));add(std::move(flow));
    TypeInfo tabs{"TabContainer","Container","UI/Layout",[]{return std::make_unique<TabContainer>();}};
    tabs.properties.push_back(NumberProperty("current","Behavior",[](const Object& o){return static_cast<double>(As<TabContainer>(o)->Current());},[](Object& o,double v){As<TabContainer>(o)->SetCurrent(static_cast<std::size_t>(std::max(0.0,v)));return Status::Ok();}));add(std::move(tabs));
    add({"Panel", "Control", "UI/Display", [] { return std::make_unique<Panel>(); }});

    TypeInfo label{"Label", "Control", "UI/Display", [] { return std::make_unique<Label>(); }};
    label.properties.push_back(StringProperty("text", "Content", [](const Object& o) { return As<Label>(o)->Text(); },
        [](Object& o, std::string v) { As<Label>(o)->SetText(std::move(v)); return Status::Ok(); }));
    label.properties.push_back(BoolProperty("wrap","Content",[](const Object& o){return As<Label>(o)->Wrap();},[](Object& o,bool v){As<Label>(o)->SetWrap(v);return Status::Ok();}));
    label.properties.push_back(NumberProperty("fontSize","Theme",[](const Object& o){return As<Label>(o)->FontSize();},[](Object& o,double v){As<Label>(o)->SetFontSize(static_cast<int>(v));return Status::Ok();}));
    label.properties.push_back(NumberProperty("letterSpacing","Theme",[](const Object& o){return static_cast<double>(As<Label>(o)->LetterSpacing());},[](Object& o,double v){As<Label>(o)->SetLetterSpacing(static_cast<float>(v));return Status::Ok();}));
    label.properties.push_back(StringProperty("fontPath","Theme",[](const Object& o){return As<Label>(o)->FontPath();},[](Object& o,std::string v){As<Label>(o)->SetFontPath(std::move(v));return Status::Ok();}));
    auto labelOutlineSize=NumberProperty("outlineSize","Theme",[](const Object& o){return static_cast<double>(As<Label>(o)->OutlineSize());},[](Object& o,double v){As<Label>(o)->SetOutlineSize(static_cast<int>(v));return Status::Ok();});labelOutlineSize.editor.hasRange=true;labelOutlineSize.editor.minimum=0;labelOutlineSize.editor.maximum=8;labelOutlineSize.editor.step=1;label.properties.push_back(std::move(labelOutlineSize));
    label.properties.push_back(ColorProperty("outlineColor","Theme",[](const Object& o){return As<Label>(o)->OutlineColor();},[](Object& o,Color v){As<Label>(o)->SetOutlineColor(v);return Status::Ok();}));
    auto labelH=StringProperty("horizontalAlignment","Content",[](const Object& o){switch(As<Label>(o)->HorizontalAlignment()){case HorizontalTextAlignment::Center:return std::string("Center");case HorizontalTextAlignment::Right:return std::string("Right");default:return std::string("Left");}},[](Object& o,std::string v){if(v=="Left")As<Label>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Left);else if(v=="Center")As<Label>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Center);else if(v=="Right")As<Label>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Right);else return TypeError("horizontalAlignment expects Left, Center, or Right");return Status::Ok();});labelH.editor.displayName="Horizontal alignment";labelH.editor.enumChoices={"Left","Center","Right"};label.properties.push_back(std::move(labelH));
    auto labelV=StringProperty("verticalAlignment","Content",[](const Object& o){switch(As<Label>(o)->VerticalAlignment()){case VerticalTextAlignment::Center:return std::string("Center");case VerticalTextAlignment::Bottom:return std::string("Bottom");default:return std::string("Top");}},[](Object& o,std::string v){if(v=="Top")As<Label>(o)->SetVerticalAlignment(VerticalTextAlignment::Top);else if(v=="Center")As<Label>(o)->SetVerticalAlignment(VerticalTextAlignment::Center);else if(v=="Bottom")As<Label>(o)->SetVerticalAlignment(VerticalTextAlignment::Bottom);else return TypeError("verticalAlignment expects Top, Center, or Bottom");return Status::Ok();});labelV.editor.displayName="Vertical alignment";labelV.editor.enumChoices={"Top","Center","Bottom"};label.properties.push_back(std::move(labelV));
    add(std::move(label));

    TypeInfo button{"Button", "Control", "UI/Input", [] { return std::make_unique<Button>(); }};
    button.properties.push_back(StringProperty("fontPath","Theme",[](const Object& o){return As<Button>(o)->FontPath();},[](Object& o,std::string v){As<Button>(o)->SetFontPath(std::move(v));return Status::Ok();}));
    button.properties.push_back(StringProperty("text", "Content", [](const Object& o) { return As<Button>(o)->Text(); },
        [](Object& o, std::string v) { As<Button>(o)->SetText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("bindingAction", "Input",
        [](const Object& o) { return As<Button>(o)->BindingAction(); },
        [](Object& o, std::string v) { As<Button>(o)->SetBindingAction(std::move(v)); return Status::Ok(); }));
    auto bindingDevice = StringProperty("bindingDevice", "Input",
        [](const Object& o) { return As<Button>(o)->GetBindingDevice() == BindingDevice::Wheel
            ? std::string("wheel") : std::string("keyboard"); },
        [](Object& o, std::string v) {
            if (v == "keyboard") As<Button>(o)->SetBindingDevice(BindingDevice::Keyboard);
            else if (v == "wheel") As<Button>(o)->SetBindingDevice(BindingDevice::Wheel);
            else return TypeError("bindingDevice expects keyboard or wheel");
            return Status::Ok();
        });
    bindingDevice.editor.enumChoices = {"keyboard", "wheel"};
    button.properties.push_back(std::move(bindingDevice));
    auto wheelThreshold = NumberProperty("wheelThreshold", "Input",
        [](const Object& o) { return static_cast<double>(As<Button>(o)->WheelThreshold()); },
        [](Object& o, double v) {
            if (!std::isfinite(v) || v < 0.0 || v > 100.0 ||
                !As<Button>(o)->SetWheelThreshold(static_cast<float>(v)))
                return TypeError("wheelThreshold expects a finite number from 0 to 100");
            return Status::Ok();
        });
    wheelThreshold.editor.hasRange = true;
    wheelThreshold.editor.minimum = 0;
    wheelThreshold.editor.maximum = 100;
    wheelThreshold.editor.step = 0.1;
    button.properties.push_back(std::move(wheelThreshold));
    button.properties.push_back(StringProperty("wheelUpText", "Input",
        [](const Object& o) { return As<Button>(o)->WheelUpText(); },
        [](Object& o, std::string v) { As<Button>(o)->SetWheelUpText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("wheelDownText", "Input",
        [](const Object& o) { return As<Button>(o)->WheelDownText(); },
        [](Object& o, std::string v) { As<Button>(o)->SetWheelDownText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("bindingPrompt", "Input",
        [](const Object& o) { return As<Button>(o)->BindingPrompt(); },
        [](Object& o, std::string v) { As<Button>(o)->SetBindingPrompt(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("unboundText", "Input",
        [](const Object& o) { return As<Button>(o)->UnboundText(); },
        [](Object& o, std::string v) { As<Button>(o)->SetUnboundText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("bindingConflictText", "Input",
        [](const Object& o) { return As<Button>(o)->BindingConflictText(); },
        [](Object& o, std::string v) { As<Button>(o)->SetBindingConflictText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("bindingConfirmText", "Input",
        [](const Object& o) { return As<Button>(o)->BindingConfirmText(); },
        [](Object& o, std::string v) { As<Button>(o)->SetBindingConfirmText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("bindingLimitText", "Input",
        [](const Object& o) { return As<Button>(o)->BindingLimitText(); },
        [](Object& o, std::string v) { As<Button>(o)->SetBindingLimitText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(BoolProperty("escapeCancelsBinding", "Input",
        [](const Object& o) { return As<Button>(o)->EscapeCancelsBinding(); },
        [](Object& o, bool v) { As<Button>(o)->SetEscapeCancelsBinding(v); return Status::Ok(); }));
    button.properties.push_back(BoolProperty("allowSharedBinding", "Input",
        [](const Object& o) { return As<Button>(o)->AllowSharedBinding(); },
        [](Object& o, bool v) { As<Button>(o)->SetAllowSharedBinding(v); return Status::Ok(); }));
    button.properties.push_back(BoolProperty("confirmSharedBinding", "Input",
        [](const Object& o) { return As<Button>(o)->ConfirmSharedBinding(); },
        [](Object& o, bool v) { As<Button>(o)->SetConfirmSharedBinding(v); return Status::Ok(); }));
    button.properties.push_back(BoolProperty("deleteClearsBinding", "Input",
        [](const Object& o) { return As<Button>(o)->DeleteClearsBinding(); },
        [](Object& o, bool v) { As<Button>(o)->SetDeleteClearsBinding(v); return Status::Ok(); }));
    button.properties.push_back(BoolProperty("addCapturedBinding", "Input",
        [](const Object& o) { return As<Button>(o)->AddCapturedBinding(); },
        [](Object& o, bool v) { As<Button>(o)->SetAddCapturedBinding(v); return Status::Ok(); }));
    button.properties.push_back(BoolProperty("selectableBindingKeys", "Input",
        [](const Object& o) { return As<Button>(o)->SelectableBindingKeys(); },
        [](Object& o, bool v) { As<Button>(o)->SetSelectableBindingKeys(v); return Status::Ok(); }));
    auto buttonFontSize = NumberProperty("fontSize", "Theme", [](const Object& o) { return As<Button>(o)->FontSize(); },
        [](Object& o, double v) { As<Button>(o)->SetFontSize(static_cast<int>(v)); return Status::Ok(); });
    buttonFontSize.editor.displayName = "Font size";
    buttonFontSize.editor.hasRange = true;
    buttonFontSize.editor.minimum = 0.0;
    buttonFontSize.editor.maximum = 512.0;
    buttonFontSize.editor.step = 1.0;
    button.properties.push_back(std::move(buttonFontSize));
    auto buttonTextColor = ColorProperty("textColor", "Theme", [](const Object& o) { return As<Button>(o)->GetTextColor(); },
        [](Object& o, Color v) { As<Button>(o)->SetTextColor(v); return Status::Ok(); });
    buttonTextColor.editor.displayName = "Text colour";
    buttonTextColor.editor.description = "Optional explicit caption colour. Authored UI appearance.textColor initializes this value.";
    buttonTextColor.editor.tokenBindable = true;
    button.properties.push_back(std::move(buttonTextColor));
    auto buttonH=StringProperty("horizontalAlignment","Content",[](const Object& o){switch(As<Button>(o)->HorizontalAlignment()){case HorizontalTextAlignment::Left:return std::string("Left");case HorizontalTextAlignment::Right:return std::string("Right");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Left")As<Button>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Left);else if(v=="Center")As<Button>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Center);else if(v=="Right")As<Button>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Right);else return TypeError("horizontalAlignment expects Left, Center, or Right");return Status::Ok();});buttonH.editor.displayName="Horizontal alignment";buttonH.editor.enumChoices={"Left","Center","Right"};button.properties.push_back(std::move(buttonH));
    auto buttonV=StringProperty("verticalAlignment","Content",[](const Object& o){switch(As<Button>(o)->VerticalAlignment()){case VerticalTextAlignment::Top:return std::string("Top");case VerticalTextAlignment::Bottom:return std::string("Bottom");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Top")As<Button>(o)->SetVerticalAlignment(VerticalTextAlignment::Top);else if(v=="Center")As<Button>(o)->SetVerticalAlignment(VerticalTextAlignment::Center);else if(v=="Bottom")As<Button>(o)->SetVerticalAlignment(VerticalTextAlignment::Bottom);else return TypeError("verticalAlignment expects Top, Center, or Bottom");return Status::Ok();});buttonV.editor.displayName="Vertical alignment";buttonV.editor.enumChoices={"Top","Center","Bottom"};button.properties.push_back(std::move(buttonV));
    button.signals.push_back({"activated", "啟用", "Button was activated by pointer or keyboard.", {}}); add(std::move(button));
    add({"IconButton", "Button", "UI/Input", [] { return std::make_unique<IconButton>(); }});
    TypeInfo textureButton{"TextureButton", "Button", "UI/Input",
                           [] { return std::make_unique<TextureButton>(); }};
    const auto addTextureButtonPath = [&textureButton](const char* name,
                                                       const char* displayName,
                                                       auto getter,
                                                       auto setter) {
        auto property = StringProperty(
            name, "Content", std::move(getter), std::move(setter));
        property.flags = property.flags | PropertyFlags::ResourcePath;
        property.editor.displayName = displayName;
        property.editor.resourceFilter = "image";
        textureButton.properties.push_back(std::move(property));
    };
    addTextureButtonPath(
        "normalPath", "Normal texture",
        [](const Object& o) { return As<TextureButton>(o)->NormalPath(); },
        [](Object& o, std::string v) {
            As<TextureButton>(o)->SetNormalPath(std::move(v));
            return Status::Ok();
        });
    addTextureButtonPath(
        "hoverPath", "Hover texture",
        [](const Object& o) { return As<TextureButton>(o)->HoverPath(); },
        [](Object& o, std::string v) {
            As<TextureButton>(o)->SetHoverPath(std::move(v));
            return Status::Ok();
        });
    addTextureButtonPath(
        "pressedPath", "Pressed texture",
        [](const Object& o) { return As<TextureButton>(o)->PressedPath(); },
        [](Object& o, std::string v) {
            As<TextureButton>(o)->SetPressedPath(std::move(v));
            return Status::Ok();
        });
    addTextureButtonPath(
        "disabledPath", "Disabled texture",
        [](const Object& o) { return As<TextureButton>(o)->DisabledPath(); },
        [](Object& o, std::string v) {
            As<TextureButton>(o)->SetDisabledPath(std::move(v));
            return Status::Ok();
        });
    auto textureButtonScale = StringProperty(
        "scaleMode", "Display",
        [](const Object& o) {
            switch (As<TextureButton>(o)->ScaleMode()) {
                case TextureScaleMode::Fit: return std::string("Fit");
                case TextureScaleMode::Fill: return std::string("Fill");
                case TextureScaleMode::Original: return std::string("Original");
                default: return std::string("Stretch");
            }
        },
        [](Object& o, std::string v) {
            auto* value = As<TextureButton>(o);
            if (v == "Stretch") value->SetScaleMode(TextureScaleMode::Stretch);
            else if (v == "Fit") value->SetScaleMode(TextureScaleMode::Fit);
            else if (v == "Fill") value->SetScaleMode(TextureScaleMode::Fill);
            else if (v == "Original") value->SetScaleMode(TextureScaleMode::Original);
            else return TypeError("scaleMode expects Stretch, Fit, Fill, or Original");
            return Status::Ok();
        });
    textureButtonScale.editor.enumChoices = {"Stretch", "Fit", "Fill", "Original"};
    textureButton.properties.push_back(std::move(textureButtonScale));
    {
        PropertyInfo margins{"patchMargins", "Display", VariantType::Rect,
            PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable,
            Variant(Rect{0, 0, 0, 0}),
            [](const Object& o) { return Variant(As<TextureButton>(o)->PatchMargins()); },
            [](Object& o, const Variant& v) {
                const auto* margins = v.TryGet<Rect>();
                if (!margins) return TypeError("patchMargins expects Rect");
                for (const float side : {margins->x, margins->y, margins->w, margins->h})
                    if (!std::isfinite(side) || side < 0.0f)
                        return TypeError("patchMargins must be four non-negative numbers");
                As<TextureButton>(o)->SetPatchMargins(*margins);
                return Status::Ok();
            }};
        margins.editor.displayName = "Nine-slice margins";
        margins.editor.description = "Image pixels kept unstretched at the left, top, right and bottom of every state image; zero draws the images by scaleMode.";
        textureButton.properties.push_back(std::move(margins));
        auto scale = NumberProperty("patchScale", "Display",
            [](const Object& o) { return static_cast<double>(As<TextureButton>(o)->PatchScale()); },
            [](Object& o, double v) {
                if (!std::isfinite(v) || v < 0.01 || v > 16.0) return TypeError("patchScale must be between 0.01 and 16");
                As<TextureButton>(o)->SetPatchScale(static_cast<float>(v));
                return Status::Ok();
            });
        scale.editor.hasRange = true;
        scale.editor.minimum = 0.01;
        scale.editor.maximum = 16;
        scale.editor.step = 0.01;
        scale.editor.description = "Size of the drawn nine-slice border relative to patchMargins in the image.";
        textureButton.properties.push_back(std::move(scale));
    }
    add(std::move(textureButton));

    TypeInfo texture{"TextureRect", "Control", "UI/Display", [] { return std::make_unique<TextureRect>(); }};
    auto texturePath = StringProperty("path", "Content", [](const Object& o) { return As<TextureRect>(o)->Path(); },
        [](Object& o, std::string v) { return As<TextureRect>(o)->SetPath(std::move(v)); });
    texturePath.flags = texturePath.flags | PropertyFlags::ResourcePath;
    texturePath.editor.displayName="Texture";texturePath.editor.resourceFilter="image";texturePath.editor.description="Image or .pxportrait descriptor used by this TextureRect.";
    texture.properties.push_back(std::move(texturePath));
    PropertyInfo textureReference{
        "texture", "Content", VariantType::ResourceRef,
        PropertyFlags::Editable | PropertyFlags::Serializable,
        Variant(ResourceRefValue{}),
        [](const Object& o) { return Variant(As<TextureRect>(o)->Texture()); },
        [](Object& o, const Variant& value) {
            const auto* reference = value.TryGet<ResourceRefValue>();
            if (!reference)
                return TypeError("texture expects a ResourceRef");
            return As<TextureRect>(o)->SetTexture(*reference);
        }};
    textureReference.editor.displayName = "Texture asset";
    textureReference.editor.description = "UUID-authoritative image resource.";
    textureReference.editor.resourceFilter = "image";
    texture.properties.push_back(std::move(textureReference));
    auto scaleMode=StringProperty("scaleMode","Display",[](const Object& o){switch(As<TextureRect>(o)->ScaleMode()){case TextureScaleMode::Fit:return std::string("Fit");case TextureScaleMode::Fill:return std::string("Fill");case TextureScaleMode::Original:return std::string("Original");default:return std::string("Stretch");}},[](Object& o,std::string v){if(v=="Stretch")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Stretch);else if(v=="Fit")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Fit);else if(v=="Fill")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Fill);else if(v=="Original")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Original);else return TypeError("scaleMode expects Stretch, Fit, Fill, or Original");return Status::Ok();});scaleMode.editor.displayName="Scale mode";scaleMode.editor.enumChoices={"Stretch","Fit","Fill","Original"};texture.properties.push_back(std::move(scaleMode));
    PropertyInfo textureRegion{"region", "Content", VariantType::Rect,
        PropertyFlags::Editable | PropertyFlags::Serializable | PropertyFlags::Bindable, Variant(Rect{}),
        [](const Object& o) { return Variant(As<TextureRect>(o)->Region()); },
        [](Object& o, const Variant& v) {
            const auto* rect = v.TryGet<Rect>();
            if (!rect || rect->x < 0 || rect->y < 0 || rect->w < 0 || rect->h < 0)
                return TypeError("region expects a non-negative Rect in image pixels");
            As<TextureRect>(o)->SetRegion(*rect);
            return Status::Ok();
        }};
    textureRegion.editor.displayName = "Region";
    textureRegion.editor.description = "Atlas cell in image pixels; empty shows the whole image.";
    texture.properties.push_back(std::move(textureRegion));
    texture.properties.push_back(BoolProperty("lockAspectRatio","Layout",[](const Object& o){return As<TextureRect>(o)->LockAspectRatio();},[](Object& o,bool v){As<TextureRect>(o)->SetLockAspectRatio(v);return Status::Ok();}));
    auto textureH=StringProperty("horizontalAlignment","Display",[](const Object& o){switch(As<TextureRect>(o)->HorizontalAlignment()){case HorizontalTextAlignment::Left:return std::string("Left");case HorizontalTextAlignment::Right:return std::string("Right");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Left")As<TextureRect>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Left);else if(v=="Center")As<TextureRect>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Center);else if(v=="Right")As<TextureRect>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Right);else return TypeError("horizontalAlignment expects Left, Center, or Right");return Status::Ok();});textureH.editor.displayName="Horizontal alignment";textureH.editor.enumChoices={"Left","Center","Right"};texture.properties.push_back(std::move(textureH));
    auto textureV=StringProperty("verticalAlignment","Display",[](const Object& o){switch(As<TextureRect>(o)->VerticalAlignment()){case VerticalTextAlignment::Top:return std::string("Top");case VerticalTextAlignment::Bottom:return std::string("Bottom");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Top")As<TextureRect>(o)->SetVerticalAlignment(VerticalTextAlignment::Top);else if(v=="Center")As<TextureRect>(o)->SetVerticalAlignment(VerticalTextAlignment::Center);else if(v=="Bottom")As<TextureRect>(o)->SetVerticalAlignment(VerticalTextAlignment::Bottom);else return TypeError("verticalAlignment expects Top, Center, or Bottom");return Status::Ok();});textureV.editor.displayName="Vertical alignment";textureV.editor.enumChoices={"Top","Center","Bottom"};texture.properties.push_back(std::move(textureV));
    auto opacity=NumberProperty("opacity","Display",[](const Object& o){return As<TextureRect>(o)->Opacity();},[](Object& o,double v){As<TextureRect>(o)->SetOpacity(static_cast<float>(v));return Status::Ok();});opacity.editor.hasRange=true;opacity.editor.minimum=0;opacity.editor.maximum=1;opacity.editor.step=.01;opacity.editor.tokenBindable=true;texture.properties.push_back(std::move(opacity));
    add(std::move(texture));
    TypeInfo motion{"MotionRect", "Control", "UI/Display",
                    [] { return std::make_unique<MotionRect>(); }};
    auto motionPath = StringProperty("path", "Content",
        [](const Object& o) { return As<MotionRect>(o)->Path(); },
        [](Object& o, std::string v) { return As<MotionRect>(o)->SetPath(std::move(v)); });
    motionPath.flags = motionPath.flags | PropertyFlags::ResourcePath;
    motionPath.editor.displayName = "Motion clip";
    motionPath.editor.resourceFilter = "motion";
    motion.properties.push_back(std::move(motionPath));
    motion.properties.push_back(BoolProperty("autoplay", "Playback",
        [](const Object& o) { return As<MotionRect>(o)->Autoplay(); },
        [](Object& o, bool v) { As<MotionRect>(o)->SetAutoplay(v); return Status::Ok(); }));
    motion.properties.push_back(BoolProperty("playing", "Playback",
        [](const Object& o) { return As<MotionRect>(o)->Playing(); },
        [](Object& o, bool v) { As<MotionRect>(o)->SetPlaying(v); return Status::Ok(); }));
    auto motionSpeed = NumberProperty("speed", "Playback",
        [](const Object& o) { return As<MotionRect>(o)->Speed(); },
        [](Object& o, double v) { return As<MotionRect>(o)->SetSpeed(v); });
    motionSpeed.editor.hasRange = true;
    motionSpeed.editor.minimum = 0.01;
    motionSpeed.editor.maximum = 16;
    motion.properties.push_back(std::move(motionSpeed));
    motion.properties.push_back(NumberProperty("position", "Playback",
        [](const Object& o) { return As<MotionRect>(o)->Position(); },
        [](Object& o, double v) { As<MotionRect>(o)->Seek(v); return Status::Ok(); }));
    motion.signals.push_back({"playbackFinished", "Playback finished",
                              "Non-looping motion reached its final frame.", {}});
    add(std::move(motion));
    TypeInfo spriteText{"SpriteText", "Control", "UI/Display",
                        [] { return std::make_unique<SpriteText>(); }};
    spriteText.properties.push_back(StringProperty("text", "Content",
        [](const Object& o) { return As<SpriteText>(o)->Text(); },
        [](Object& o, std::string value) {
            if (value.size() > 1024) return TypeError("SpriteText text exceeds 1024 bytes");
            As<SpriteText>(o)->SetText(std::move(value));
            return Status::Ok();
        }));
    spriteText.properties.push_back(NumberProperty("advance", "Layout",
        [](const Object& o) { return As<SpriteText>(o)->Advance(); },
        [](Object& o, const double value) {
            if (!std::isfinite(value) || value < 0 || value > 4096)
                return TypeError("SpriteText advance must be between 0 and 4096");
            As<SpriteText>(o)->SetAdvance(static_cast<float>(value));
            return Status::Ok();
        }));
    spriteText.properties.push_back({"glyphs", "Content", VariantType::Array,
        PropertyFlags::Editable | PropertyFlags::Serializable,
        Variant(VariantArray{}),
        [](const Object& o) {
            VariantArray values;
            for (const auto& glyph : As<SpriteText>(o)->Glyphs()) {
                values.emplace_back(VariantObject{
                    {"character", Variant(glyph.character)},
                    {"path", Variant(glyph.path)},
                    {"x", Variant(glyph.x)}, {"y", Variant(glyph.y)},
                    {"width", Variant(glyph.width)},
                    {"height", Variant(glyph.height)},
                });
            }
            return Variant(std::move(values));
        },
        [](Object& o, const Variant& value) {
            const auto* array = value.AsArray();
            if (!array || array->empty() || array->size() > 128)
                return TypeError("SpriteText glyphs must contain 1 to 128 entries");
            std::vector<SpriteGlyph> glyphs;
            std::unordered_set<std::string> seen;
            glyphs.reserve(array->size());
            for (const auto& item : *array) {
                const auto* object = item.AsObject();
                if (!object || object->size() != 6)
                    return TypeError("SpriteText glyph entry requires character, path, x, y, width and height");
                const auto characterIt = object->find("character");
                const auto pathIt = object->find("path");
                if (characterIt == object->end() || pathIt == object->end())
                    return TypeError("SpriteText glyph character and path are required");
                const auto* character = characterIt->second.TryGet<std::string>();
                const auto* path = pathIt->second.TryGet<std::string>();
                const auto x = GlyphNumber(*object, "x"), y = GlyphNumber(*object, "y");
                const auto width = GlyphNumber(*object, "width");
                const auto height = GlyphNumber(*object, "height");
                if (!character || character->empty() ||
                    text::GraphemeBoundaries(*character).size() != 2 ||
                    !seen.insert(*character).second || !path || !SafeSpritePath(*path) ||
                    !x || !y || !width || !height ||
                    !std::isfinite(*x) || !std::isfinite(*y) ||
                    !std::isfinite(*width) || !std::isfinite(*height) ||
                    std::abs(*x) > 4096 || std::abs(*y) > 4096 ||
                    *width <= 0 || *width > 4096 ||
                    *height <= 0 || *height > 4096)
                    return TypeError("SpriteText glyph entry is invalid or duplicated");
                glyphs.push_back({*character, *path, static_cast<float>(*x),
                                  static_cast<float>(*y), static_cast<float>(*width),
                                  static_cast<float>(*height)});
            }
            As<SpriteText>(o)->SetGlyphs(std::move(glyphs));
            return Status::Ok();
        }});
    add(std::move(spriteText));
    TypeInfo progress{"ProgressBar", "Control", "UI/Display", [] { return std::make_unique<ProgressBar>(); }};
    progress.properties.push_back(NumberProperty("value", "Value", [](const Object& o) { return As<ProgressBar>(o)->Value(); },
        [](Object& o, double v) { As<ProgressBar>(o)->SetValue(v); return Status::Ok(); }));
    add(std::move(progress));
    TypeInfo virtualized{"VirtualizedView","Control","UI/Data",[]{return std::make_unique<VirtualizedView>();}};
    virtualized.properties.push_back(NumberProperty("columns","Data",[](const Object& o){return static_cast<double>(As<VirtualizedView>(o)->GridColumns());},[](Object& o,double v){As<VirtualizedView>(o)->SetGridColumns(static_cast<std::size_t>(std::max(1.0,v)));return Status::Ok();}));
    virtualized.properties.push_back(Vec2Property("itemExtent","Data",[](const Object& o){return As<VirtualizedView>(o)->ItemExtent();},[](Object& o,Vec2 v){As<VirtualizedView>(o)->SetItemExtent(v);return Status::Ok();}));
    virtualized.properties.push_back(Vec2Property("gap","Data",[](const Object& o){return As<VirtualizedView>(o)->Gap();},[](Object& o,Vec2 v){As<VirtualizedView>(o)->SetGap(v);return Status::Ok();}));
    // The show transition every runtime-made item plays when the view fills;
    // `staggerSeconds` on the view spaces the items out.
    virtualized.properties.push_back(NumberProperty("itemShowTransitionSeconds","Display",[](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return static_cast<double>(s?s->seconds:0.0f);},[](Object& o,double v){if(!std::isfinite(v)||v<0.0||v>2.0)return TypeError("itemShowTransitionSeconds must be between 0 and 2");As<VirtualizedView>(o)->EditItemShowTransition().seconds=static_cast<float>(v);return Status::Ok();}));
    virtualized.properties.push_back(NumberProperty("itemShowDelaySeconds","Display",[](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return static_cast<double>(s?s->delaySeconds:0.0f);},[](Object& o,double v){if(!std::isfinite(v)||v<0.0||v>10.0)return TypeError("itemShowDelaySeconds must be between 0 and 10");As<VirtualizedView>(o)->EditItemShowTransition().delaySeconds=static_cast<float>(v);return Status::Ok();}));
    for (const bool vertical : {false, true})
        virtualized.properties.push_back(NumberProperty(vertical ? "itemShowOffsetY" : "itemShowOffsetX","Display",[vertical](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return static_cast<double>(!s?(vertical?8.0f:0.0f):vertical?s->offsetY:s->offsetX);},[vertical](Object& o,double v){if(!std::isfinite(v)||v<-2000.0||v>2000.0)return TypeError("item transition offsets must be between -2000 and 2000");auto& s=As<VirtualizedView>(o)->EditItemShowTransition();(vertical?s.offsetY:s.offsetX)=static_cast<float>(v);return Status::Ok();}));
    {
        auto ease=StringProperty("itemShowTransitionEase","Display",[](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return std::string(TransitionEaseName(s?s->ease:TransitionEase::Quad));},[](Object& o,std::string v){const auto ease=ParseTransitionEase(v);if(!ease)return TypeError("transition ease expects quad, linear, cubic or back");As<VirtualizedView>(o)->EditItemShowTransition().ease=*ease;return Status::Ok();});
        ease.editor.enumChoices={"quad","linear","cubic","back"};virtualized.properties.push_back(std::move(ease));
        auto wipe=StringProperty("itemShowWipe","Display",[](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return std::string(TransitionWipeName(s?s->wipe:TransitionWipe::None));},[](Object& o,std::string v){const auto wipe=ParseTransitionWipe(v);if(!wipe)return TypeError("wipe expects none, left, right, up or down");As<VirtualizedView>(o)->EditItemShowTransition().wipe=*wipe;return Status::Ok();});
        wipe.editor.enumChoices={"none","left","right","up","down"};virtualized.properties.push_back(std::move(wipe));
    }
    virtualized.properties.push_back(NumberProperty("itemShowScale","Display",[](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return static_cast<double>(s?s->scale:1.0f);},[](Object& o,double v){if(!std::isfinite(v)||v<0.0||v>10.0)return TypeError("itemShowScale must be between 0 and 10");As<VirtualizedView>(o)->EditItemShowTransition().scale=static_cast<float>(v);return Status::Ok();}));
    virtualized.properties.push_back(BoolProperty("itemShowFade","Display",[](const Object& o){const auto& s=As<VirtualizedView>(o)->ItemShowTransition();return s?s->fade:true;},[](Object& o,bool v){As<VirtualizedView>(o)->EditItemShowTransition().fade=v;return Status::Ok();}));
    virtualized.properties.push_back(NumberProperty("itemHoverScale","Display",[](const Object& o){return static_cast<double>(As<VirtualizedView>(o)->ItemHoverScale());},[](Object& o,double v){if(!std::isfinite(v)||v<0.1||v>4.0)return TypeError("itemHoverScale must be between 0.1 and 4");As<VirtualizedView>(o)->SetItemHoverScale(static_cast<float>(v));return Status::Ok();}));
    virtualized.properties.push_back(NumberProperty("overscan","Data",[](const Object& o){return static_cast<double>(As<VirtualizedView>(o)->Overscan());},[](Object& o,double v){As<VirtualizedView>(o)->SetOverscan(static_cast<std::size_t>(std::max(0.0,v)));return Status::Ok();}));add(std::move(virtualized));
    add({"ListView", "VirtualizedView", "UI/Data", [] { return std::make_unique<ListView>(); }});
    add({"GridView", "VirtualizedView", "UI/Data", [] { return std::make_unique<GridView>(); }});
    TypeInfo placed{"PlacedItemsView", "VirtualizedView", "UI/Data", [] { return std::make_unique<PlacedItemsView>(); }};
    PropertyInfo choiceArea{"choiceArea", "Choices", VariantType::Rect,
        PropertyFlags::Editable | PropertyFlags::Serializable, Variant(Rect{}),
        [](const Object& o) { return Variant(As<PlacedItemsView>(o)->ChoiceArea()); },
        [](Object& o, const Variant& v) {
            const auto* rect = v.TryGet<Rect>();
            if (!rect || rect->w < 0 || rect->h < 0)
                return TypeError("choiceArea expects a Rect with a non-negative size");
            As<PlacedItemsView>(o)->SetChoiceArea(*rect);
            return Status::Ok();
        }};
    choiceArea.editor.displayName = "Choice area";
    choiceArea.editor.description = "Rectangle of the middle choice bar, in this view's space.";
    placed.properties.push_back(std::move(choiceArea));
    placed.properties.push_back(NumberProperty("choicePitch", "Choices",
        [](const Object& o) { return static_cast<double>(As<PlacedItemsView>(o)->ChoicePitch()); },
        [](Object& o, double v) {
            if (!std::isfinite(v)) return TypeError("choicePitch expects a finite number");
            As<PlacedItemsView>(o)->SetChoicePitch(static_cast<float>(v));
            return Status::Ok();
        }));
    placed.properties.push_back(StringProperty("choiceRows", "Choices",
        [](const Object& o) { return As<PlacedItemsView>(o)->ChoiceRows(); },
        [](Object& o, std::string v) {
            if (!v.empty() && !ParseChoiceRows(v))
                return TypeError("choiceRows expects rows like \"2:-1,1;3:-1.5,0,1.5\"");
            As<PlacedItemsView>(o)->SetChoiceRows(std::move(v));
            return Status::Ok();
        }));
    add(std::move(placed));
    TypeInfo colorRect{"ColorRect","Control","UI/Display",[]{return std::make_unique<ColorRect>();}};
    colorRect.properties.push_back({"color","Display",VariantType::Color,PropertyFlags::Editable|PropertyFlags::Serializable|PropertyFlags::Bindable,Variant(Color{}),
        [](const Object& o){return Variant(As<ColorRect>(o)->GetColor());},[](Object& o,const Variant& v){if(auto* c=v.TryGet<Color>()){As<ColorRect>(o)->SetColor(*c);return Status::Ok();}return TypeError("color expects Color");}});add(std::move(colorRect));
    TypeInfo checkbox{"CheckBox","Button","UI/Input",[]{return std::make_unique<CheckBox>();}};
    checkbox.properties.push_back(BoolProperty("checked","Value",[](const Object& o){return As<CheckBox>(o)->Checked();},[](Object& o,bool v){As<CheckBox>(o)->SetChecked(v);return Status::Ok();}));
    checkbox.signals.push_back({"toggled", "切換", "Checked state changed.", {{"value", VariantType::Bool}}});add(std::move(checkbox));
    TypeInfo slider{"Slider","Control","UI/Input",[]{return std::make_unique<Slider>();}};
    slider.properties.push_back(NumberProperty("minimum","Value",[](const Object& o){return As<Slider>(o)->Minimum();},[](Object& o,double v){auto* s=As<Slider>(o);s->SetRange(v,s->Maximum(),s->Step());return Status::Ok();}));
    slider.properties.push_back(NumberProperty("maximum","Value",[](const Object& o){return As<Slider>(o)->Maximum();},[](Object& o,double v){auto* s=As<Slider>(o);s->SetRange(s->Minimum(),v,s->Step());return Status::Ok();}));
    slider.properties.push_back(NumberProperty("step","Value",[](const Object& o){return As<Slider>(o)->Step();},[](Object& o,double v){auto* s=As<Slider>(o);s->SetRange(s->Minimum(),s->Maximum(),v);return Status::Ok();}));
    slider.properties.push_back(NumberProperty("value","Value",[](const Object& o){return As<Slider>(o)->Value();},[](Object& o,double v){As<Slider>(o)->SetValue(v);return Status::Ok();}));
    const auto addSliderTexture=[&slider](const char* name,const char* displayName,auto getter,auto setter){auto property=StringProperty(name,"Skin",std::move(getter),std::move(setter));property.flags=property.flags|PropertyFlags::ResourcePath;property.editor.displayName=displayName;property.editor.resourceFilter="image";slider.properties.push_back(std::move(property));};
    addSliderTexture("railPath","Rail texture",[](const Object& o){return As<Slider>(o)->RailPath();},[](Object& o,std::string v){As<Slider>(o)->SetRailPath(std::move(v));return Status::Ok();});
    addSliderTexture("railHoverPath","Rail hover texture",[](const Object& o){return As<Slider>(o)->RailHoverPath();},[](Object& o,std::string v){As<Slider>(o)->SetRailHoverPath(std::move(v));return Status::Ok();});
    addSliderTexture("railPressedPath","Rail pressed texture",[](const Object& o){return As<Slider>(o)->RailPressedPath();},[](Object& o,std::string v){As<Slider>(o)->SetRailPressedPath(std::move(v));return Status::Ok();});
    addSliderTexture("knobPath","Knob texture",[](const Object& o){return As<Slider>(o)->KnobPath();},[](Object& o,std::string v){As<Slider>(o)->SetKnobPath(std::move(v));return Status::Ok();});
    addSliderTexture("knobHoverPath","Knob hover texture",[](const Object& o){return As<Slider>(o)->KnobHoverPath();},[](Object& o,std::string v){As<Slider>(o)->SetKnobHoverPath(std::move(v));return Status::Ok();});
    addSliderTexture("knobPressedPath","Knob pressed texture",[](const Object& o){return As<Slider>(o)->KnobPressedPath();},[](Object& o,std::string v){As<Slider>(o)->SetKnobPressedPath(std::move(v));return Status::Ok();});
    auto railHeight=NumberProperty("railHeight","Skin",[](const Object& o){return As<Slider>(o)->RailHeight();},[](Object& o,double v){As<Slider>(o)->SetRailHeight(static_cast<float>(v));return Status::Ok();});railHeight.editor.hasRange=true;railHeight.editor.minimum=1;railHeight.editor.maximum=128;railHeight.editor.step=1;slider.properties.push_back(std::move(railHeight));
    auto knobSize=NumberProperty("knobSize","Skin",[](const Object& o){return As<Slider>(o)->KnobSize();},[](Object& o,double v){As<Slider>(o)->SetKnobSize(static_cast<float>(v));return Status::Ok();});knobSize.editor.hasRange=true;knobSize.editor.minimum=1;knobSize.editor.maximum=256;knobSize.editor.step=1;slider.properties.push_back(std::move(knobSize));
    slider.properties.push_back(BoolProperty("fillRailToValue", "Skin", [](const Object& o){return As<Slider>(o)->FillRailToValue();}, [](Object& o,bool v){As<Slider>(o)->SetFillRailToValue(v);return Status::Ok();}));
    auto optionKey=StringProperty("optionKey","Value",[](const Object& o){return As<Slider>(o)->OptionKey();},[](Object& o,std::string v){As<Slider>(o)->SetOptionKey(std::move(v));return Status::Ok();});
    optionKey.editor.description="Name of the Player option (player.options) this slider edits.";
    slider.properties.push_back(std::move(optionKey));
    slider.signals.push_back({"valueChanged", "數值變更", "Slider value changed.", {{"value", VariantType::Number}}});add(std::move(slider));
    TypeInfo lineEdit{"LineEdit","Control","UI/Input",[]{return std::make_unique<LineEdit>();}};
    lineEdit.properties.push_back(StringProperty("text","Content",[](const Object& o){return As<LineEdit>(o)->Text();},[](Object& o,std::string v){As<LineEdit>(o)->SetText(std::move(v));return Status::Ok();}));
    lineEdit.properties.push_back(StringProperty("placeholder","Content",[](const Object& o){return As<LineEdit>(o)->Placeholder();},[](Object& o,std::string v){As<LineEdit>(o)->SetPlaceholder(std::move(v));return Status::Ok();}));
    lineEdit.properties.push_back(NumberProperty("fontSize","Theme",[](const Object& o){return static_cast<double>(As<LineEdit>(o)->FontSize());},[](Object& o,double v){As<LineEdit>(o)->SetFontSize(static_cast<int>(v));return Status::Ok();}));
    auto maxLength=NumberProperty("maxLength","Content",[](const Object& o){return static_cast<double>(As<LineEdit>(o)->MaxLength());},[](Object& o,double v){As<LineEdit>(o)->SetMaxLength(static_cast<std::size_t>(std::clamp(v,0.0,4096.0)));return Status::Ok();});maxLength.editor.hasRange=true;maxLength.editor.minimum=0;maxLength.editor.maximum=4096;maxLength.editor.step=1;maxLength.editor.description="Most characters the field takes; 0: no limit.";lineEdit.properties.push_back(std::move(maxLength));
    lineEdit.signals.push_back({"textChanged", "文字變更", "Text content changed.", {{"text", VariantType::String}}});
    lineEdit.signals.push_back({"submitted", "送出", "Text was submitted.", {{"text", VariantType::String}}});
    lineEdit.signals.push_back({"cancelled", "取消", "Escape was pressed in the field.", {}});add(std::move(lineEdit));
    TypeInfo rich{"RichTextLabel","Label","UI/Display",[]{return std::make_unique<RichTextLabel>();}};
    rich.properties.push_back(StringProperty("markup","Content",[](const Object& o){return As<RichTextLabel>(o)->Markup();},[](Object& o,std::string v){As<RichTextLabel>(o)->SetMarkup(std::move(v));return Status::Ok();}));
    rich.properties.push_back(BoolProperty("vertical","Content",[](const Object& o){return As<RichTextLabel>(o)->Vertical();},[](Object& o,bool v){As<RichTextLabel>(o)->SetVertical(v);return Status::Ok();}));
    auto verticalRows=NumberProperty("verticalRows","Content",[](const Object& o){return static_cast<double>(As<RichTextLabel>(o)->VerticalRows());},[](Object& o,double v){As<RichTextLabel>(o)->SetVerticalRows(static_cast<std::size_t>(std::max(0.0,v)));return Status::Ok();});verticalRows.editor.hasRange=true;verticalRows.editor.minimum=0;verticalRows.editor.maximum=4096;verticalRows.editor.step=1;rich.properties.push_back(std::move(verticalRows));add(std::move(rich));
    TypeInfo ninePatch{"NinePatchRect","Control","UI/Display",[]{return std::make_unique<NinePatchRect>();}};
    auto ninePatchPath=StringProperty("path","Content",[](const Object& o){return As<NinePatchRect>(o)->Path();},[](Object& o,std::string v){As<NinePatchRect>(o)->SetPath(std::move(v));return Status::Ok();});
    ninePatchPath.flags=ninePatchPath.flags|PropertyFlags::ResourcePath;ninePatchPath.editor.displayName="圖片";ninePatchPath.editor.resourceFilter="image";ninePatch.properties.push_back(std::move(ninePatchPath));
    ninePatch.properties.push_back({"patchMargins","Display",VariantType::Rect,PropertyFlags::Editable|PropertyFlags::Serializable|PropertyFlags::Bindable,Variant(Rect{16,16,16,16}),
        [](const Object& o){return Variant(As<NinePatchRect>(o)->PatchMargins());},[](Object& o,const Variant& v){const auto* margins=v.TryGet<Rect>();if(!margins)return TypeError("patchMargins expects Rect");As<NinePatchRect>(o)->SetPatchMargins(*margins);return Status::Ok();}});
    ninePatch.properties.push_back(BoolProperty("drawCenter","Display",[](const Object& o){return As<NinePatchRect>(o)->DrawCenter();},[](Object& o,bool v){As<NinePatchRect>(o)->SetDrawCenter(v);return Status::Ok();}));
    auto ninePatchScale=NumberProperty("patchScale","Display",[](const Object& o){return static_cast<double>(As<NinePatchRect>(o)->PatchScale());},[](Object& o,double v){if(!std::isfinite(v)||v<0.01||v>16.0)return TypeError("patchScale must be between 0.01 and 16");As<NinePatchRect>(o)->SetPatchScale(static_cast<float>(v));return Status::Ok();});
    ninePatchScale.editor.hasRange=true;ninePatchScale.editor.minimum=0.01;ninePatchScale.editor.maximum=16;ninePatchScale.editor.step=0.01;ninePatchScale.editor.description="Size of the drawn border relative to patchMargins in the image: 0.5 for an image made at twice the layout's resolution.";
    ninePatch.properties.push_back(std::move(ninePatchScale));add(std::move(ninePatch));

    TypeInfo textEdit{"TextEdit","Control","UI/Input",[]{return std::make_unique<TextEdit>();}};
    auto multilineText=StringProperty("text","Content",[](const Object& o){return As<TextEdit>(o)->Text();},[](Object& o,std::string v){As<TextEdit>(o)->SetText(std::move(v));return Status::Ok();});multilineText.editor.multiline=true;textEdit.properties.push_back(std::move(multilineText));
    textEdit.properties.push_back(StringProperty("placeholder","Content",[](const Object& o){return As<TextEdit>(o)->Placeholder();},[](Object& o,std::string v){As<TextEdit>(o)->SetPlaceholder(std::move(v));return Status::Ok();}));
    textEdit.properties.push_back(BoolProperty("readOnly","Interaction",[](const Object& o){return As<TextEdit>(o)->ReadOnly();},[](Object& o,bool v){As<TextEdit>(o)->SetReadOnly(v);return Status::Ok();}));
    textEdit.signals.push_back({"textChanged","文字變更","Text content changed.",{{"text",VariantType::String}}});add(std::move(textEdit));

    TypeInfo optionButton{"OptionButton","Button","UI/Input",[]{return std::make_unique<OptionButton>();}};
    optionButton.properties.push_back({"options","Content",VariantType::Array,PropertyFlags::Editable|PropertyFlags::Serializable,Variant(VariantArray{}),
        [](const Object& o){VariantArray values;for(const auto& option:As<OptionButton>(o)->Options())values.emplace_back(option);return Variant(std::move(values));},
        [](Object& o,const Variant& v){const auto* array=v.AsArray();if(!array)return TypeError("options expects Array");std::vector<std::string> values;for(const auto& item:*array){const auto* text=item.TryGet<std::string>();if(!text)return TypeError("options entries must be String");values.push_back(*text);}As<OptionButton>(o)->SetOptions(std::move(values));return Status::Ok();}});
    optionButton.properties.push_back(IntegerProperty("selected","Value",[](const Object& o){return static_cast<std::int64_t>(As<OptionButton>(o)->Selected());},[](Object& o,std::int64_t v){As<OptionButton>(o)->SetSelected(static_cast<int>(v));return Status::Ok();}));
    optionButton.signals.push_back({"itemSelected","選項變更","Selected option changed.",{{"index",VariantType::Integer},{"text",VariantType::String}}});add(std::move(optionButton));

    TypeInfo spinBox{"SpinBox","Control","UI/Input",[]{return std::make_unique<SpinBox>();}};
    spinBox.properties.push_back(NumberProperty("minimum","Value",[](const Object& o){return As<SpinBox>(o)->Minimum();},[](Object& o,double v){auto* spin=As<SpinBox>(o);spin->SetRange(v,spin->Maximum(),spin->Step());return Status::Ok();}));
    spinBox.properties.push_back(NumberProperty("maximum","Value",[](const Object& o){return As<SpinBox>(o)->Maximum();},[](Object& o,double v){auto* spin=As<SpinBox>(o);spin->SetRange(spin->Minimum(),v,spin->Step());return Status::Ok();}));
    spinBox.properties.push_back(NumberProperty("step","Value",[](const Object& o){return As<SpinBox>(o)->Step();},[](Object& o,double v){auto* spin=As<SpinBox>(o);spin->SetRange(spin->Minimum(),spin->Maximum(),v);return Status::Ok();}));
    spinBox.properties.push_back(NumberProperty("value","Value",[](const Object& o){return As<SpinBox>(o)->Value();},[](Object& o,double v){As<SpinBox>(o)->SetValue(v);return Status::Ok();}));
    spinBox.signals.push_back({"valueChanged","數值變更","SpinBox value changed.",{{"value",VariantType::Number}}});add(std::move(spinBox));

    TypeInfo radio{"RadioButton","CheckBox","UI/Input",[]{return std::make_unique<RadioButton>();}};
    radio.properties.push_back(StringProperty("group","Behavior",[](const Object& o){return As<RadioButton>(o)->Group();},[](Object& o,std::string v){As<RadioButton>(o)->SetGroup(std::move(v));return Status::Ok();}));add(std::move(radio));

    TypeInfo separator{"Separator","Control","UI/Display",[]{return std::make_unique<Separator>();}};
    auto separatorOrientation=StringProperty("orientation","Layout",[](const Object& o){return As<Separator>(o)->Orientation()==SeparatorOrientation::Horizontal?std::string("Horizontal"):std::string("Vertical");},[](Object& o,std::string v){if(v=="Horizontal")As<Separator>(o)->SetOrientation(SeparatorOrientation::Horizontal);else if(v=="Vertical")As<Separator>(o)->SetOrientation(SeparatorOrientation::Vertical);else return TypeError("orientation expects Horizontal or Vertical");return Status::Ok();});separatorOrientation.editor.enumChoices={"Horizontal","Vertical"};separator.properties.push_back(std::move(separatorOrientation));add(std::move(separator));

    TypeInfo scrollBar{"ScrollBar","Slider","UI/Input",[]{return std::make_unique<ScrollBar>();}};
    scrollBar.properties.push_back(BoolProperty("vertical","Layout",[](const Object& o){return As<ScrollBar>(o)->Vertical();},[](Object& o,bool v){As<ScrollBar>(o)->SetVertical(v);return Status::Ok();}));
    scrollBar.properties.push_back(NumberProperty("page","Value",[](const Object& o){return As<ScrollBar>(o)->Page();},[](Object& o,double v){As<ScrollBar>(o)->SetPage(v);return Status::Ok();}));add(std::move(scrollBar));

    TypeInfo videoRect{"VideoRect","Control","UI/Display",[]{return std::make_unique<VideoRect>();}};
    auto videoPath=StringProperty("path","Content",[](const Object& o){return As<VideoRect>(o)->Path();},[](Object& o,std::string v){As<VideoRect>(o)->SetPath(std::move(v));return Status::Ok();});videoPath.flags=videoPath.flags|PropertyFlags::ResourcePath;videoPath.editor.displayName="影片";videoPath.editor.resourceFilter="video";videoRect.properties.push_back(std::move(videoPath));
    auto videoPoster=StringProperty("poster","Content",[](const Object& o){return As<VideoRect>(o)->Poster();},[](Object& o,std::string v){As<VideoRect>(o)->SetPoster(std::move(v));return Status::Ok();});videoPoster.flags=videoPoster.flags|PropertyFlags::ResourcePath;videoPoster.editor.displayName="預覽圖片";videoPoster.editor.resourceFilter="image";videoRect.properties.push_back(std::move(videoPoster));
    videoRect.properties.push_back(BoolProperty("autoplay","Playback",[](const Object& o){return As<VideoRect>(o)->Autoplay();},[](Object& o,bool v){As<VideoRect>(o)->SetAutoplay(v);return Status::Ok();}));
    videoRect.properties.push_back(BoolProperty("loop","Playback",[](const Object& o){return As<VideoRect>(o)->Loop();},[](Object& o,bool v){As<VideoRect>(o)->SetLoop(v);return Status::Ok();}));
    videoRect.properties.push_back(BoolProperty("playing","Playback",[](const Object& o){return As<VideoRect>(o)->Playing();},[](Object& o,bool v){As<VideoRect>(o)->SetPlaying(v);return Status::Ok();}));
    videoRect.signals.push_back({"playbackStarted","開始播放","Video playback started.",{}});videoRect.signals.push_back({"playbackStopped","停止播放","Video playback stopped.",{}});add(std::move(videoRect));
    return combined;
}

}  // namespace px::ui
