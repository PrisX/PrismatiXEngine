#include "Engine/UI/Layout.h"

#include "Engine/Graphics/Renderer2D.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace px::ui {
namespace {
std::vector<Control*> VisibleChildren(Control& owner) {
    std::vector<Control*> result;
    for (const auto& child : owner.Children()) {
        if (auto* control = dynamic_cast<Control*>(child.get());
            control && control->GetVisibility() != Visibility::Collapsed) result.push_back(control);
    }
    return result;
}
}

Vec2 BoxContainer::MeasureOverride(Vec2 available) {
    const auto children = VisibleChildren(*this);
    Vec2 result{};
    for (auto* child : children) {
        const Vec2 desired = child->Measure(available);
        if (m_orientation == Orientation::Horizontal) {
            result.x += desired.x;
            result.y = std::max(result.y, desired.y);
        } else {
            result.x = std::max(result.x, desired.x);
            result.y += desired.y;
        }
    }
    const float gaps = children.empty() ? 0.0f : m_separation * static_cast<float>(children.size() - 1);
    if (m_orientation == Orientation::Horizontal) result.x += gaps; else result.y += gaps;
    return result;
}

void BoxContainer::ArrangeOverride(Rect area) {
    const auto children = VisibleChildren(*this);
    if (children.empty()) return;
    const bool horizontal = m_orientation == Orientation::Horizontal;
    const float total = horizontal ? area.w : area.h;
    const float gaps = m_separation * static_cast<float>(children.size() - 1);
    float fixed = gaps;
    float ratio = 0.0f;
    for (auto* child : children) {
        const bool expand = HasFlag(horizontal ? child->HorizontalSizeFlags() : child->VerticalSizeFlags(), SizeFlag::Expand);
        if (expand) ratio += child->StretchRatio();
        else fixed += horizontal ? child->DesiredSize().x : child->DesiredSize().y;
    }
    const float remaining = std::max(0.0f, total - fixed);
    float cursor = horizontal ? area.x : area.y;
    for (auto* child : children) {
        const SizeFlag mainFlags = horizontal ? child->HorizontalSizeFlags() : child->VerticalSizeFlags();
        const bool expand = HasFlag(mainFlags, SizeFlag::Expand);
        const float desiredMain = horizontal ? child->DesiredSize().x : child->DesiredSize().y;
        const float main = expand && ratio > 0.0f ? remaining * child->StretchRatio() / ratio : desiredMain;
        const float crossAvailable = horizontal ? area.h : area.w;
        const float desiredCross = horizontal ? child->DesiredSize().y : child->DesiredSize().x;
        const SizeFlag crossFlags = horizontal ? child->VerticalSizeFlags() : child->HorizontalSizeFlags();
        const float cross = HasFlag(crossFlags, SizeFlag::Fill) ? crossAvailable : std::min(crossAvailable, desiredCross);
        float crossOffset = 0.0f;
        if (HasFlag(crossFlags, SizeFlag::ShrinkCenter)) crossOffset = (crossAvailable - cross) * 0.5f;
        else if (HasFlag(crossFlags, SizeFlag::ShrinkEnd)) crossOffset = crossAvailable - cross;
        if (horizontal) child->Arrange({cursor, area.y + crossOffset, main, cross});
        else child->Arrange({area.x + crossOffset, cursor, cross, main});
        cursor += main + m_separation;
    }
}

void MarginContainer::SetMargins(float left, float top, float right, float bottom) {
    m_margins = {left, top, right, bottom};
    InvalidateLayout();
}

Vec2 MarginContainer::MeasureOverride(Vec2 available) {
    const Vec2 inner{std::max(0.0f, available.x - m_margins.x - m_margins.w),
                     std::max(0.0f, available.y - m_margins.y - m_margins.h)};
    Vec2 desired{};
    for (auto* child : VisibleChildren(*this)) {
        const Vec2 value = child->Measure(inner);
        desired.x = std::max(desired.x, value.x);
        desired.y = std::max(desired.y, value.y);
    }
    return {desired.x + m_margins.x + m_margins.w, desired.y + m_margins.y + m_margins.h};
}

void MarginContainer::ArrangeOverride(Rect area) {
    const Rect inner{area.x + m_margins.x, area.y + m_margins.y,
                     std::max(0.0f, area.w - m_margins.x - m_margins.w),
                     std::max(0.0f, area.h - m_margins.y - m_margins.h)};
    for (auto* child : VisibleChildren(*this)) child->Arrange(inner);
}

Vec2 CenterContainer::MeasureOverride(Vec2 available) {
    Vec2 result{};
    for (auto* child : VisibleChildren(*this)) {
        const Vec2 value = child->Measure(available);
        result.x = std::max(result.x, value.x); result.y = std::max(result.y, value.y);
    }
    return result;
}

void CenterContainer::ArrangeOverride(Rect area) {
    for (auto* child : VisibleChildren(*this)) {
        const Vec2 size{std::min(area.w, child->DesiredSize().x), std::min(area.h, child->DesiredSize().y)};
        child->Arrange({area.x + (area.w - size.x) * 0.5f, area.y + (area.h - size.y) * 0.5f, size.x, size.y});
    }
}

Vec2 GridContainer::MeasureOverride(Vec2 available) {
    m_cell = {};
    const auto children = VisibleChildren(*this);
    for (auto* child : children) {
        const Vec2 value = child->Measure(available);
        m_cell.x = std::max(m_cell.x, value.x); m_cell.y = std::max(m_cell.y, value.y);
    }
    const std::size_t rows = (children.size() + m_columns - 1) / m_columns;
    return {m_cell.x * static_cast<float>(std::min(m_columns, children.size())) +
                m_gaps.x * static_cast<float>(children.empty() ? 0 : std::min(m_columns, children.size()) - 1),
            m_cell.y * static_cast<float>(rows) + m_gaps.y * static_cast<float>(rows ? rows - 1 : 0)};
}

void GridContainer::ArrangeOverride(Rect area) {
    const auto children = VisibleChildren(*this);
    const float width = (area.w - m_gaps.x * static_cast<float>(m_columns - 1)) / static_cast<float>(m_columns);
    for (std::size_t i = 0; i < children.size(); ++i) {
        const std::size_t col = i % m_columns, row = i / m_columns;
        children[i]->Arrange({area.x + static_cast<float>(col) * (width + m_gaps.x),
                              area.y + static_cast<float>(row) * (m_cell.y + m_gaps.y), width, m_cell.y});
    }
}

void ScrollContainer::SetScrollOffset(Vec2 value) { m_offset = value; InvalidateLayout(); }

Vec2 ScrollContainer::MeasureOverride(Vec2 available) {
    m_contentSize = {};
    for (auto* child : VisibleChildren(*this)) {
        const Vec2 desired = child->Measure({available.x, 1000000.0f});
        m_contentSize.x = std::max(m_contentSize.x, desired.x); m_contentSize.y = std::max(m_contentSize.y, desired.y);
    }
    return {std::min(available.x, m_contentSize.x), std::min(available.y, m_contentSize.y)};
}

void ScrollContainer::ArrangeOverride(Rect area) {
    m_offset.x = std::clamp(m_offset.x, 0.0f, std::max(0.0f, m_contentSize.x - area.w));
    m_offset.y = std::clamp(m_offset.y, 0.0f, std::max(0.0f, m_contentSize.y - area.h));
    for (auto* child : VisibleChildren(*this)) child->Arrange({area.x - m_offset.x, area.y - m_offset.y,
                                                               std::max(area.w, m_contentSize.x), m_contentSize.y});
}

void ScrollContainer::HandleEvent(UIEvent& event) {
    if (event.type == UIEventType::Scroll && event.wheel != 0.0f) {
        SetScrollOffset({m_offset.x, m_offset.y - event.wheel * 48.0f});
        event.handled = true;
    }
}

void ScrollContainer::Draw(graphics::Renderer2D& renderer, const Theme& theme) {
    if (GetVisibility() != Visibility::Visible) return;
    renderer.PushClip(LayoutRect());
    Control::Draw(renderer, theme);
    renderer.PopClip();
}

Vec2 AspectRatioContainer::MeasureOverride(Vec2 available){Vec2 desired{};for(auto* child:VisibleChildren(*this)){const Vec2 size=child->Measure(available);desired.x=std::max(desired.x,size.x);desired.y=std::max(desired.y,size.y);}if(desired.y>0)desired.x=std::max(desired.x,desired.y*m_ratio);return desired;}
void AspectRatioContainer::ArrangeOverride(Rect area){float w=area.w,h=w/m_ratio;if(h>area.h){h=area.h;w=h*m_ratio;}Rect inner{area.x+(area.w-w)*.5f,area.y+(area.h-h)*.5f,w,h};for(auto* child:VisibleChildren(*this))child->Arrange(inner);}

Vec2 FlowContainer::MeasureOverride(Vec2 available){float x=0,y=0,row=0;for(auto* child:VisibleChildren(*this)){const Vec2 size=child->Measure(available);if(x>0&&x+size.x>available.x){y+=row+m_gaps.y;x=0;row=0;}x+=size.x+(x>0?m_gaps.x:0);row=std::max(row,size.y);}return{available.x,y+row};}
void FlowContainer::ArrangeOverride(Rect area){float x=0,y=0,row=0;for(auto* child:VisibleChildren(*this)){const Vec2 size=child->DesiredSize();if(x>0&&x+size.x>area.w){y+=row+m_gaps.y;x=0;row=0;}child->Arrange({area.x+x,area.y+y,size.x,size.y});x+=size.x+m_gaps.x;row=std::max(row,size.y);}}

Vec2 TabContainer::MeasureOverride(Vec2 available){Vec2 result{};auto children=VisibleChildren(*this);for(auto* child:children){const Vec2 size=child->Measure(available);result.x=std::max(result.x,size.x);result.y=std::max(result.y,size.y);}return result;}
void TabContainer::ArrangeOverride(Rect area){auto children=VisibleChildren(*this);for(std::size_t i=0;i<children.size();++i){children[i]->SetVisibility(i==std::min(m_current,children.size()-1)?Visibility::Visible:Visibility::Collapsed);if(i==std::min(m_current,children.size()-1))children[i]->Arrange(area);}}

}  // namespace px::ui
