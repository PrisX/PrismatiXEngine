#include "Engine/UI/UITypeRegistry.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"

namespace px::ui {
namespace {
Status TypeError(std::string message) {
    return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error, .code = "PXUI2501",
        .category = "UI.TypeRegistry", .message = std::move(message)});
}
template <typename T> T* As(Object& object) { return dynamic_cast<T*>(&object); }
template <typename T> const T* As(const Object& object) { return dynamic_cast<const T*>(&object); }

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
}

Status RegisterBuiltinUITypes() {
    auto& registry = TypeRegistry::Global();
    if (registry.Find("Control")) return Status::Ok();
    Status combined;
    auto add = [&combined, &registry](TypeInfo type) {
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
    control.properties.push_back(StringProperty("visibility", "Behavior", [](const Object& o) {
        switch (As<Control>(o)->GetVisibility()) { case Visibility::Visible: return std::string("Visible"); case Visibility::Hidden: return std::string("Hidden"); default: return std::string("Collapsed"); }
    }, [](Object& o, std::string v) {
        if (v == "Visible") As<Control>(o)->SetVisibility(Visibility::Visible);
        else if (v == "Hidden") As<Control>(o)->SetVisibility(Visibility::Hidden);
        else if (v == "Collapsed") As<Control>(o)->SetVisibility(Visibility::Collapsed);
        else return TypeError("visibility expects Visible, Hidden, or Collapsed"); return Status::Ok();
    }));
    control.properties.push_back(StringProperty("themeVariant", "Theme", [](const Object& o) { return As<Control>(o)->ThemeVariant(); },
        [](Object& o, std::string v) { As<Control>(o)->SetThemeVariant(std::move(v)); return Status::Ok(); }));
    control.properties.push_back(Vec2Property("minimumSize", "Layout", [](const Object& o) { return As<Control>(o)->CustomMinimumSize(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetCustomMinimumSize(v); return Status::Ok(); }));
    control.properties.push_back(Vec2Property("maximumSize", "Layout", [](const Object& o) { return As<Control>(o)->MaximumSize(); },
        [](Object& o, Vec2 v) { As<Control>(o)->SetMaximumSize(v); return Status::Ok(); }));
    control.properties.push_back(NumberProperty("stretchRatio", "Layout", [](const Object& o) { return As<Control>(o)->StretchRatio(); },
        [](Object& o, double v) { As<Control>(o)->SetStretchRatio(static_cast<float>(v)); return Status::Ok(); }));
    control.properties.push_back(StringProperty("tooltip", "Interaction", [](const Object& o) { return As<Control>(o)->Tooltip(); },
        [](Object& o, std::string v) { As<Control>(o)->SetTooltip(std::move(v)); return Status::Ok(); }));
    add(std::move(control));

    add({"Container", "Control", "UI/Layout", [] { return std::make_unique<Container>(); }});
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
    add(std::move(label));

    TypeInfo button{"Button", "Control", "UI/Input", [] { return std::make_unique<Button>(); }};
    button.properties.push_back(StringProperty("text", "Content", [](const Object& o) { return As<Button>(o)->Text(); },
        [](Object& o, std::string v) { As<Button>(o)->SetText(std::move(v)); return Status::Ok(); }));
    button.properties.push_back(StringProperty("command", "Behavior", [](const Object& o) { return As<Button>(o)->Command(); },
        [](Object& o, std::string v) { As<Button>(o)->SetCommand(std::move(v)); return Status::Ok(); }));
    button.signals.push_back({"activated", {}}); add(std::move(button));

    TypeInfo texture{"TextureRect", "Control", "UI/Display", [] { return std::make_unique<TextureRect>(); }};
    auto texturePath = StringProperty("path", "Content", [](const Object& o) { return As<TextureRect>(o)->Path(); },
        [](Object& o, std::string v) { As<TextureRect>(o)->SetPath(std::move(v)); return Status::Ok(); });
    texturePath.flags = texturePath.flags | PropertyFlags::ResourcePath;
    texture.properties.push_back(std::move(texturePath));
    texture.properties.push_back(NumberProperty("opacity","Display",[](const Object& o){return As<TextureRect>(o)->Opacity();},[](Object& o,double v){As<TextureRect>(o)->SetOpacity(static_cast<float>(v));return Status::Ok();}));
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
    checkbox.properties.push_back(BoolProperty("checked","Value",[](const Object& o){return As<CheckBox>(o)->Checked();},[](Object& o,bool v){As<CheckBox>(o)->SetChecked(v);return Status::Ok();}));add(std::move(checkbox));
    TypeInfo slider{"Slider","Control","UI/Input",[]{return std::make_unique<Slider>();}};
    slider.properties.push_back(NumberProperty("minimum","Value",[](const Object& o){return As<Slider>(o)->Minimum();},[](Object& o,double v){auto* s=As<Slider>(o);s->SetRange(v,s->Maximum(),s->Step());return Status::Ok();}));
    slider.properties.push_back(NumberProperty("maximum","Value",[](const Object& o){return As<Slider>(o)->Maximum();},[](Object& o,double v){auto* s=As<Slider>(o);s->SetRange(s->Minimum(),v,s->Step());return Status::Ok();}));
    slider.properties.push_back(NumberProperty("step","Value",[](const Object& o){return As<Slider>(o)->Step();},[](Object& o,double v){auto* s=As<Slider>(o);s->SetRange(s->Minimum(),s->Maximum(),v);return Status::Ok();}));
    slider.properties.push_back(NumberProperty("value","Value",[](const Object& o){return As<Slider>(o)->Value();},[](Object& o,double v){As<Slider>(o)->SetValue(v);return Status::Ok();}));add(std::move(slider));
    TypeInfo lineEdit{"LineEdit","Control","UI/Input",[]{return std::make_unique<LineEdit>();}};
    lineEdit.properties.push_back(StringProperty("text","Content",[](const Object& o){return As<LineEdit>(o)->Text();},[](Object& o,std::string v){As<LineEdit>(o)->SetText(std::move(v));return Status::Ok();}));
    lineEdit.properties.push_back(StringProperty("placeholder","Content",[](const Object& o){return As<LineEdit>(o)->Placeholder();},[](Object& o,std::string v){As<LineEdit>(o)->SetPlaceholder(std::move(v));return Status::Ok();}));add(std::move(lineEdit));
    TypeInfo rich{"RichTextLabel","Label","UI/Display",[]{return std::make_unique<RichTextLabel>();}};
    rich.properties.push_back(StringProperty("markup","Content",[](const Object& o){return As<RichTextLabel>(o)->Markup();},[](Object& o,std::string v){As<RichTextLabel>(o)->SetMarkup(std::move(v));return Status::Ok();}));add(std::move(rich));
    return combined;
}

}  // namespace px::ui
