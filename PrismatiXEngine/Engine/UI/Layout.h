#pragma once

#include "Engine/UI/Control.h"

#include <cstddef>

namespace px::ui {

enum class Orientation { Horizontal, Vertical };
enum class RevealEdge { Top, Bottom, Left, Right };
enum class RevealTrigger { Hover, Manual };

class EdgeRevealContainer final : public Container {
public:
    explicit EdgeRevealContainer(std::string name="EdgeReveal") : Container(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override{return "EdgeRevealContainer";}
    void SetEdge(RevealEdge value){m_edge=value;InvalidateLayout();}
    [[nodiscard]] RevealEdge Edge() const{return m_edge;}
    void SetSpeed(float value){m_speed=std::max(.1f,value);}
    [[nodiscard]] float Speed() const{return m_speed;}
    void SetTriggerSize(float value){m_triggerSize=std::max(1.0f,value);InvalidateLayout();}
    [[nodiscard]] float TriggerSize() const{return m_triggerSize;}
    void SetPinned(bool value){m_pinned=value;}
    [[nodiscard]] bool Pinned() const{return m_pinned;}
    void TogglePinned(){SetPinned(!m_pinned);}
    void SetTrigger(RevealTrigger value){m_trigger=value;}
    [[nodiscard]] RevealTrigger Trigger() const{return m_trigger;}
    void RevealFor(float seconds){m_manualVisible=true;m_holdRemaining=std::max(0.0f,seconds);}
    void Hide(){m_manualVisible=false;m_holdRemaining=0.0f;}
    [[nodiscard]] bool HitTest(Vec2 point) const override;
    void HandleEvent(UIEvent& event) override;
    void Update(float deltaSeconds) override;
protected:void ArrangeOverride(Rect finalRect) override;
private:
    RevealEdge m_edge=RevealEdge::Top;
    float m_speed=12.0f,m_triggerSize=10.0f,m_progress=0.0f;
    bool m_pinned=false,m_pointerInside=false,m_manualVisible=false;
    float m_holdRemaining=0.0f;
    RevealTrigger m_trigger=RevealTrigger::Hover;
};

class BoxContainer : public Container {
public:
    explicit BoxContainer(Orientation orientation, std::string name = "BoxContainer")
        : Container(std::move(name)), m_orientation(orientation) {}
    [[nodiscard]] std::string_view TypeName() const override { return "BoxContainer"; }
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override {
        return m_orientation == Orientation::Horizontal ? ChildLayoutPolicy::LinearX
                                                        : ChildLayoutPolicy::LinearY;
    }
    void SetSeparation(float value) { m_separation = value; InvalidateLayout(); }
    [[nodiscard]] float Separation() const { return m_separation; }

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;

private:
    Orientation m_orientation;
    float m_separation = 8.0f;
};

class HBoxContainer final : public BoxContainer {
public:
    explicit HBoxContainer(std::string name = "HBox") : BoxContainer(Orientation::Horizontal, std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "HBoxContainer"; }
};

class VBoxContainer final : public BoxContainer {
public:
    explicit VBoxContainer(std::string name = "VBox") : BoxContainer(Orientation::Vertical, std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "VBoxContainer"; }
};

class MarginContainer final : public Container {
public:
    explicit MarginContainer(std::string name = "Margin") : Container(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "MarginContainer"; }
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override { return ChildLayoutPolicy::SingleSlot; }
    void SetMargins(float left, float top, float right, float bottom);
    [[nodiscard]] Rect Margins() const { return m_margins; }

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;

private:
    Rect m_margins{};
};

class CenterContainer final : public Container {
public:
    explicit CenterContainer(std::string name = "Center") : Container(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "CenterContainer"; }
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override { return ChildLayoutPolicy::SingleSlot; }
protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;
};

class GridContainer final : public Container {
public:
    explicit GridContainer(std::size_t columns = 1, std::string name = "Grid")
        : Container(std::move(name)), m_columns(columns ? columns : 1) {}
    [[nodiscard]] std::string_view TypeName() const override { return "GridContainer"; }
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override { return ChildLayoutPolicy::Grid; }
    void SetColumns(std::size_t value) { m_columns = value ? value : 1; InvalidateLayout(); }
    [[nodiscard]] std::size_t Columns() const { return m_columns; }
    void SetGaps(Vec2 value) { m_gaps = value; InvalidateLayout(); }
    [[nodiscard]] Vec2 Gaps() const { return m_gaps; }

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;

private:
    std::size_t m_columns = 1;
    Vec2 m_gaps{8, 8};
    Vec2 m_cell{};
};

class StackContainer final : public Container {
public:
    explicit StackContainer(std::string name = "Stack") : Container(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "StackContainer"; }
};

class ScrollContainer final : public Container {
public:
    explicit ScrollContainer(std::string name = "Scroll") : Container(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "ScrollContainer"; }
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override { return ChildLayoutPolicy::SingleSlot; }
    void SetScrollOffset(Vec2 value);
    [[nodiscard]] Vec2 ScrollOffset() const { return m_offset; }
    void HandleEvent(UIEvent& event) override;
    void Draw(graphics::Renderer2D& renderer, const Theme& theme) override;

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;

private:
    Vec2 m_offset{};
    Vec2 m_contentSize{};
};

class AspectRatioContainer final : public Container {
public:
    explicit AspectRatioContainer(float ratio=16.0f/9.0f,std::string name="AspectRatio") : Container(std::move(name)),m_ratio(ratio){}
    [[nodiscard]] std::string_view TypeName() const override{return "AspectRatioContainer";}
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override{return ChildLayoutPolicy::SingleSlot;}
    void SetRatio(float value){m_ratio=std::max(.001f,value);InvalidateLayout();}
    [[nodiscard]] float Ratio() const{return m_ratio;}
protected:Vec2 MeasureOverride(Vec2 available) override;void ArrangeOverride(Rect finalRect) override;
private:float m_ratio;
};

class FlowContainer final : public Container {
public:
    explicit FlowContainer(std::string name="Flow") : Container(std::move(name)){}
    [[nodiscard]] std::string_view TypeName() const override{return "FlowContainer";}
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override{return ChildLayoutPolicy::Flow;}
    void SetGaps(Vec2 value){m_gaps=value;InvalidateLayout();}
    [[nodiscard]] Vec2 Gaps() const{return m_gaps;}
protected:Vec2 MeasureOverride(Vec2 available) override;void ArrangeOverride(Rect finalRect) override;
private:Vec2 m_gaps{8,8};
};

class TabContainer final : public Container {
public:
    explicit TabContainer(std::string name="Tabs") : Container(std::move(name)){}
    [[nodiscard]] std::string_view TypeName() const override{return "TabContainer";}
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override{return ChildLayoutPolicy::Pages;}
    void SetCurrent(std::size_t value){m_current=value;InvalidateLayout();}
    [[nodiscard]] std::size_t Current() const{return m_current;}
protected:Vec2 MeasureOverride(Vec2 available) override;void ArrangeOverride(Rect finalRect) override;
private:std::size_t m_current=0;
};

}  // namespace px::ui
