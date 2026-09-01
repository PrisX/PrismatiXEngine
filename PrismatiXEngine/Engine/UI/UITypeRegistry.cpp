#include "Engine/UI/UITypeRegistry.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"

#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace px::ui {
namespace {
Status TypeError(std::string message) {
    return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error, .code = "PXUI2501",
        .category = "UI.TypeRegistry", .message = std::move(message)});
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
        {"IconButton", DesignerMetadata("Icon Button", "Compact button intended for an icon.", "Input",
                                        "ui.icon-button", {52, 52}, false, {{"text", std::string{}}}, {"image"})},
        {"TextureRect", DesignerMetadata("Image", "Displays an image asset.", "Display", "ui.image",
                                         {320, 180}, false,
                                         {{"scaleMode", std::string("Fit")}, {"lockAspectRatio", true}}, {"image"})},
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
        type.sourceId = "PrismatiX.Builtin.UI";
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
    auto labelH=StringProperty("horizontalAlignment","Content",[](const Object& o){switch(As<Label>(o)->HorizontalAlignment()){case HorizontalTextAlignment::Center:return std::string("Center");case HorizontalTextAlignment::Right:return std::string("Right");default:return std::string("Left");}},[](Object& o,std::string v){if(v=="Left")As<Label>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Left);else if(v=="Center")As<Label>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Center);else if(v=="Right")As<Label>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Right);else return TypeError("horizontalAlignment expects Left, Center, or Right");return Status::Ok();});labelH.editor.displayName="Horizontal alignment";labelH.editor.enumChoices={"Left","Center","Right"};label.properties.push_back(std::move(labelH));
    auto labelV=StringProperty("verticalAlignment","Content",[](const Object& o){switch(As<Label>(o)->VerticalAlignment()){case VerticalTextAlignment::Center:return std::string("Center");case VerticalTextAlignment::Bottom:return std::string("Bottom");default:return std::string("Top");}},[](Object& o,std::string v){if(v=="Top")As<Label>(o)->SetVerticalAlignment(VerticalTextAlignment::Top);else if(v=="Center")As<Label>(o)->SetVerticalAlignment(VerticalTextAlignment::Center);else if(v=="Bottom")As<Label>(o)->SetVerticalAlignment(VerticalTextAlignment::Bottom);else return TypeError("verticalAlignment expects Top, Center, or Bottom");return Status::Ok();});labelV.editor.displayName="Vertical alignment";labelV.editor.enumChoices={"Top","Center","Bottom"};label.properties.push_back(std::move(labelV));
    add(std::move(label));

    TypeInfo button{"Button", "Control", "UI/Input", [] { return std::make_unique<Button>(); }};
    button.properties.push_back(StringProperty("text", "Content", [](const Object& o) { return As<Button>(o)->Text(); },
        [](Object& o, std::string v) { As<Button>(o)->SetText(std::move(v)); return Status::Ok(); }));
    auto buttonH=StringProperty("horizontalAlignment","Content",[](const Object& o){switch(As<Button>(o)->HorizontalAlignment()){case HorizontalTextAlignment::Left:return std::string("Left");case HorizontalTextAlignment::Right:return std::string("Right");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Left")As<Button>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Left);else if(v=="Center")As<Button>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Center);else if(v=="Right")As<Button>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Right);else return TypeError("horizontalAlignment expects Left, Center, or Right");return Status::Ok();});buttonH.editor.displayName="Horizontal alignment";buttonH.editor.enumChoices={"Left","Center","Right"};button.properties.push_back(std::move(buttonH));
    auto buttonV=StringProperty("verticalAlignment","Content",[](const Object& o){switch(As<Button>(o)->VerticalAlignment()){case VerticalTextAlignment::Top:return std::string("Top");case VerticalTextAlignment::Bottom:return std::string("Bottom");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Top")As<Button>(o)->SetVerticalAlignment(VerticalTextAlignment::Top);else if(v=="Center")As<Button>(o)->SetVerticalAlignment(VerticalTextAlignment::Center);else if(v=="Bottom")As<Button>(o)->SetVerticalAlignment(VerticalTextAlignment::Bottom);else return TypeError("verticalAlignment expects Top, Center, or Bottom");return Status::Ok();});buttonV.editor.displayName="Vertical alignment";buttonV.editor.enumChoices={"Top","Center","Bottom"};button.properties.push_back(std::move(buttonV));
    button.signals.push_back({"activated", "啟用", "Button was activated by pointer or keyboard.", {}}); add(std::move(button));
    add({"IconButton", "Button", "UI/Input", [] { return std::make_unique<IconButton>(); }});

    TypeInfo texture{"TextureRect", "Control", "UI/Display", [] { return std::make_unique<TextureRect>(); }};
    auto texturePath = StringProperty("path", "Content", [](const Object& o) { return As<TextureRect>(o)->Path(); },
        [](Object& o, std::string v) { As<TextureRect>(o)->SetPath(std::move(v)); return Status::Ok(); });
    texturePath.flags = texturePath.flags | PropertyFlags::ResourcePath;
    texturePath.editor.displayName="Texture";texturePath.editor.resourceFilter="image";texturePath.editor.description="Image asset used by this TextureRect.";
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
            As<TextureRect>(o)->SetTexture(*reference);
            return Status::Ok();
        }};
    textureReference.editor.displayName = "Texture asset";
    textureReference.editor.description = "UUID-authoritative image resource.";
    textureReference.editor.resourceFilter = "image";
    texture.properties.push_back(std::move(textureReference));
    auto scaleMode=StringProperty("scaleMode","Display",[](const Object& o){switch(As<TextureRect>(o)->ScaleMode()){case TextureScaleMode::Fit:return std::string("Fit");case TextureScaleMode::Fill:return std::string("Fill");case TextureScaleMode::Original:return std::string("Original");default:return std::string("Stretch");}},[](Object& o,std::string v){if(v=="Stretch")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Stretch);else if(v=="Fit")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Fit);else if(v=="Fill")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Fill);else if(v=="Original")As<TextureRect>(o)->SetScaleMode(TextureScaleMode::Original);else return TypeError("scaleMode expects Stretch, Fit, Fill, or Original");return Status::Ok();});scaleMode.editor.displayName="Scale mode";scaleMode.editor.enumChoices={"Stretch","Fit","Fill","Original"};texture.properties.push_back(std::move(scaleMode));
    texture.properties.push_back(BoolProperty("lockAspectRatio","Layout",[](const Object& o){return As<TextureRect>(o)->LockAspectRatio();},[](Object& o,bool v){As<TextureRect>(o)->SetLockAspectRatio(v);return Status::Ok();}));
    auto textureH=StringProperty("horizontalAlignment","Display",[](const Object& o){switch(As<TextureRect>(o)->HorizontalAlignment()){case HorizontalTextAlignment::Left:return std::string("Left");case HorizontalTextAlignment::Right:return std::string("Right");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Left")As<TextureRect>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Left);else if(v=="Center")As<TextureRect>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Center);else if(v=="Right")As<TextureRect>(o)->SetHorizontalAlignment(HorizontalTextAlignment::Right);else return TypeError("horizontalAlignment expects Left, Center, or Right");return Status::Ok();});textureH.editor.displayName="Horizontal alignment";textureH.editor.enumChoices={"Left","Center","Right"};texture.properties.push_back(std::move(textureH));
    auto textureV=StringProperty("verticalAlignment","Display",[](const Object& o){switch(As<TextureRect>(o)->VerticalAlignment()){case VerticalTextAlignment::Top:return std::string("Top");case VerticalTextAlignment::Bottom:return std::string("Bottom");default:return std::string("Center");}},[](Object& o,std::string v){if(v=="Top")As<TextureRect>(o)->SetVerticalAlignment(VerticalTextAlignment::Top);else if(v=="Center")As<TextureRect>(o)->SetVerticalAlignment(VerticalTextAlignment::Center);else if(v=="Bottom")As<TextureRect>(o)->SetVerticalAlignment(VerticalTextAlignment::Bottom);else return TypeError("verticalAlignment expects Top, Center, or Bottom");return Status::Ok();});textureV.editor.displayName="Vertical alignment";textureV.editor.enumChoices={"Top","Center","Bottom"};texture.properties.push_back(std::move(textureV));
    auto opacity=NumberProperty("opacity","Display",[](const Object& o){return As<TextureRect>(o)->Opacity();},[](Object& o,double v){As<TextureRect>(o)->SetOpacity(static_cast<float>(v));return Status::Ok();});opacity.editor.hasRange=true;opacity.editor.minimum=0;opacity.editor.maximum=1;opacity.editor.step=.01;opacity.editor.tokenBindable=true;texture.properties.push_back(std::move(opacity));
    add(std::move(texture));
    TypeInfo progress{"ProgressBar", "Control", "UI/Display", [] { return std::make_unique<ProgressBar>(); }};
    progress.properties.push_back(NumberProperty("value", "Value", [](const Object& o) { return As<ProgressBar>(o)->Value(); },
        [](Object& o, double v) { As<ProgressBar>(o)->SetValue(v); return Status::Ok(); }));
    add(std::move(progress));
    TypeInfo virtualized{"VirtualizedView","Control","UI/Data",[]{return std::make_unique<VirtualizedView>();}};
    virtualized.properties.push_back(NumberProperty("columns","Data",[](const Object& o){return static_cast<double>(As<VirtualizedView>(o)->GridColumns());},[](Object& o,double v){As<VirtualizedView>(o)->SetGridColumns(static_cast<std::size_t>(std::max(1.0,v)));return Status::Ok();}));
    virtualized.properties.push_back(Vec2Property("itemExtent","Data",[](const Object& o){return As<VirtualizedView>(o)->ItemExtent();},[](Object& o,Vec2 v){As<VirtualizedView>(o)->SetItemExtent(v);return Status::Ok();}));
    virtualized.properties.push_back(Vec2Property("gap","Data",[](const Object& o){return As<VirtualizedView>(o)->Gap();},[](Object& o,Vec2 v){As<VirtualizedView>(o)->SetGap(v);return Status::Ok();}));
    virtualized.properties.push_back(NumberProperty("overscan","Data",[](const Object& o){return static_cast<double>(As<VirtualizedView>(o)->Overscan());},[](Object& o,double v){As<VirtualizedView>(o)->SetOverscan(static_cast<std::size_t>(std::max(0.0,v)));return Status::Ok();}));add(std::move(virtualized));
    add({"ListView", "VirtualizedView", "UI/Data", [] { return std::make_unique<ListView>(); }});
    add({"GridView", "VirtualizedView", "UI/Data", [] { return std::make_unique<GridView>(); }});
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
    slider.signals.push_back({"valueChanged", "數值變更", "Slider value changed.", {{"value", VariantType::Number}}});add(std::move(slider));
    TypeInfo lineEdit{"LineEdit","Control","UI/Input",[]{return std::make_unique<LineEdit>();}};
    lineEdit.properties.push_back(StringProperty("text","Content",[](const Object& o){return As<LineEdit>(o)->Text();},[](Object& o,std::string v){As<LineEdit>(o)->SetText(std::move(v));return Status::Ok();}));
    lineEdit.properties.push_back(StringProperty("placeholder","Content",[](const Object& o){return As<LineEdit>(o)->Placeholder();},[](Object& o,std::string v){As<LineEdit>(o)->SetPlaceholder(std::move(v));return Status::Ok();}));
    lineEdit.signals.push_back({"textChanged", "文字變更", "Text content changed.", {{"text", VariantType::String}}});
    lineEdit.signals.push_back({"submitted", "送出", "Text was submitted.", {{"text", VariantType::String}}});add(std::move(lineEdit));
    TypeInfo rich{"RichTextLabel","Label","UI/Display",[]{return std::make_unique<RichTextLabel>();}};
    rich.properties.push_back(StringProperty("markup","Content",[](const Object& o){return As<RichTextLabel>(o)->Markup();},[](Object& o,std::string v){As<RichTextLabel>(o)->SetMarkup(std::move(v));return Status::Ok();}));
    rich.properties.push_back(BoolProperty("vertical","Content",[](const Object& o){return As<RichTextLabel>(o)->Vertical();},[](Object& o,bool v){As<RichTextLabel>(o)->SetVertical(v);return Status::Ok();}));
    auto verticalRows=NumberProperty("verticalRows","Content",[](const Object& o){return static_cast<double>(As<RichTextLabel>(o)->VerticalRows());},[](Object& o,double v){As<RichTextLabel>(o)->SetVerticalRows(static_cast<std::size_t>(std::max(0.0,v)));return Status::Ok();});verticalRows.editor.hasRange=true;verticalRows.editor.minimum=0;verticalRows.editor.maximum=4096;verticalRows.editor.step=1;rich.properties.push_back(std::move(verticalRows));add(std::move(rich));
    TypeInfo ninePatch{"NinePatchRect","Control","UI/Display",[]{return std::make_unique<NinePatchRect>();}};
    auto ninePatchPath=StringProperty("path","Content",[](const Object& o){return As<NinePatchRect>(o)->Path();},[](Object& o,std::string v){As<NinePatchRect>(o)->SetPath(std::move(v));return Status::Ok();});
    ninePatchPath.flags=ninePatchPath.flags|PropertyFlags::ResourcePath;ninePatchPath.editor.displayName="圖片";ninePatchPath.editor.resourceFilter="image";ninePatch.properties.push_back(std::move(ninePatchPath));
    ninePatch.properties.push_back({"patchMargins","Display",VariantType::Rect,PropertyFlags::Editable|PropertyFlags::Serializable|PropertyFlags::Bindable,Variant(Rect{16,16,16,16}),
        [](const Object& o){return Variant(As<NinePatchRect>(o)->PatchMargins());},[](Object& o,const Variant& v){const auto* margins=v.TryGet<Rect>();if(!margins)return TypeError("patchMargins expects Rect");As<NinePatchRect>(o)->SetPatchMargins(*margins);return Status::Ok();}});
    ninePatch.properties.push_back(BoolProperty("drawCenter","Display",[](const Object& o){return As<NinePatchRect>(o)->DrawCenter();},[](Object& o,bool v){As<NinePatchRect>(o)->SetDrawCenter(v);return Status::Ok();}));add(std::move(ninePatch));

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
