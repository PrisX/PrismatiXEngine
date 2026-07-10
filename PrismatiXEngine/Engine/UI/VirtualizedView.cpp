#include "Engine/UI/VirtualizedView.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace px::ui {
namespace {
Status VirtualFailure(std::string code, std::string message) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error, .code = std::move(code),
                                .category = "UI.Virtualization", .message = std::move(message)};
    diag::Emit(diagnostic); return Status::Fail(std::move(diagnostic));
}
}

Status VirtualizedView::SetSource(std::shared_ptr<IVirtualItemSource> source, Factory factory) {
    while (!m_realized.empty()) Recycle(m_realized.begin()->first);
    m_pool.clear(); m_source = std::move(source); m_factory = std::move(factory); m_seenRevision = 0; m_scroll = 0;
    if (!m_source) return VirtualFailure("PXUI2201", "Virtualized view requires an item source");
    if (!m_factory) return VirtualFailure("PXUI2202", "Virtualized view requires an item factory");
    const auto status = ValidateKeys(); if (!status) return status;
    m_seenRevision = m_source->Revision(); InvalidateLayout(); return Status::Ok();
}

void VirtualizedView::SetGridColumns(std::size_t columns) { m_columns = std::max<std::size_t>(1, columns); InvalidateLayout(); }
void VirtualizedView::SetItemExtent(Vec2 extent) { m_itemExtent = {std::max(1.0f, extent.x), std::max(1.0f, extent.y)}; InvalidateLayout(); }
void VirtualizedView::SetGap(Vec2 gap) { m_gap = {std::max(0.0f, gap.x), std::max(0.0f, gap.y)}; InvalidateLayout(); }
void VirtualizedView::ScrollTo(float offset) { m_scroll = std::max(0.0f, offset); InvalidateLayout(); }
void VirtualizedView::ScrollToIndex(std::size_t index) { ScrollTo(static_cast<float>(index / m_columns) * (m_itemExtent.y + m_gap.y)); }

Status VirtualizedView::ValidateKeys() const {
    std::unordered_set<std::string> keys;
    for (std::size_t index = 0; index < m_source->Count(); ++index) {
        const std::string key = m_source->StableKey(index);
        if (key.empty()) return VirtualFailure("PXUI2203", "Virtualized item has an empty stable key at index " + std::to_string(index));
        if (!keys.insert(key).second) return VirtualFailure("PXUI2204", "Duplicate virtualized item key: " + key);
    }
    return Status::Ok();
}

Vec2 VirtualizedView::MeasureOverride(Vec2 available) {
    return {std::min(available.x, m_itemExtent.x * static_cast<float>(m_columns) + m_gap.x * static_cast<float>(m_columns - 1)),
            std::min(available.y, m_itemExtent.y * 6.0f + m_gap.y * 5.0f)};
}

std::unique_ptr<Control> VirtualizedView::Acquire() {
    if (!m_pool.empty()) { auto value = std::move(m_pool.back()); m_pool.pop_back(); return value; }
    return m_factory ? m_factory() : nullptr;
}

void VirtualizedView::Recycle(std::size_t index) {
    const auto it = m_realized.find(index); if (it == m_realized.end()) return;
    auto node = RemoveChild(it->second); m_realized.erase(it);
    if (node) if (auto* control = dynamic_cast<Control*>(node.get())) {
        node.release(); m_pool.emplace_back(control);
    }
}

Status VirtualizedView::Synchronize(Rect viewport) {
    if (!m_source || !m_factory) return Status::Ok();
    if (m_seenRevision != m_source->Revision()) {
        const auto status = ValidateKeys(); if (!status) return status;
        while (!m_realized.empty()) Recycle(m_realized.begin()->first);
        m_seenRevision = m_source->Revision();
    }
    const std::size_t count = m_source->Count();
    const std::size_t rows = (count + m_columns - 1) / m_columns;
    const float rowExtent = m_itemExtent.y + m_gap.y;
    const float content = rows ? static_cast<float>(rows) * rowExtent - m_gap.y : 0.0f;
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, content - viewport.h));
    const std::size_t firstRow = std::min(rows, static_cast<std::size_t>(m_scroll / rowExtent));
    const std::size_t visibleRows = static_cast<std::size_t>(std::ceil(viewport.h / rowExtent)) + 1;
    const std::size_t beginRow = firstRow > m_overscan ? firstRow - m_overscan : 0;
    const std::size_t endRow = std::min(rows, firstRow + visibleRows + m_overscan);
    const std::size_t begin = std::min(count, beginRow * m_columns);
    const std::size_t end = std::min(count, endRow * m_columns);

    std::vector<std::size_t> recycle;
    for (const auto& [index, id] : m_realized) if (index < begin || index >= end) recycle.push_back(index);
    for (std::size_t index : recycle) Recycle(index);

    const float cellWidth = m_columns == 1 ? viewport.w :
        (viewport.w - m_gap.x * static_cast<float>(m_columns - 1)) / static_cast<float>(m_columns);
    for (std::size_t index = begin; index < end; ++index) {
        Control* item = nullptr;
        if (const auto found = m_realized.find(index); found != m_realized.end()) item = dynamic_cast<Control*>(Find(found->second));
        if (!item) {
            auto created = Acquire();
            if (!created) return VirtualFailure("PXUI2205", "Virtual item factory returned null");
            item = created.get();
            const Uuid id = item->Id();
            const Status add = AddChild(std::move(created)); if (!add) return add;
            m_realized[index] = id;
        }
        const Status bind = m_source->Bind(index, *item); if (!bind) return bind;
        const std::size_t row = index / m_columns, column = index % m_columns;
        (void)item->Measure({cellWidth, m_itemExtent.y});
        item->Arrange({viewport.x + static_cast<float>(column) * (cellWidth + m_gap.x),
                       viewport.y + static_cast<float>(row) * rowExtent - m_scroll,
                       cellWidth, m_itemExtent.y});
    }
    return Status::Ok();
}

void VirtualizedView::ArrangeOverride(Rect finalRect) { const Status status=Synchronize(finalRect);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic); }

void VirtualizedView::HandleEvent(UIEvent& event) {
    if (event.type == UIEventType::Scroll) { ScrollTo(m_scroll - event.wheel * (m_itemExtent.y + m_gap.y)); event.handled = true; }
}

void VirtualizedView::Draw(graphics::Renderer2D& renderer,const Theme& theme){if(GetVisibility()!=Visibility::Visible)return;renderer.PushClip(LayoutRect());Control::Draw(renderer,theme);renderer.PopClip();}

}  // namespace px::ui
