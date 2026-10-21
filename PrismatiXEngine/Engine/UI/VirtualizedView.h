#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Control.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace px::ui {

class IVirtualItemSource {
public:
    virtual ~IVirtualItemSource() = default;
    [[nodiscard]] virtual std::size_t Count() const = 0;
    // Unique within a source revision. Visible controls keep their identity
    // when the same key moves to another index; increment Revision on edits.
    [[nodiscard]] virtual std::string StableKey(std::size_t index) const = 0;
    virtual Status Bind(std::size_t index, Control& item) = 0;
    /** Local rectangle for a PlacedItemsView item; absent for grid/list sources. */
    [[nodiscard]] virtual std::optional<Rect> Placement(std::size_t index) const {
        (void)index; return std::nullopt;
    }
    // Release item-scoped subscriptions or actions before a Control enters
    // the reuse pool. The key remains valid even after indexes are reordered.
    virtual void Unbind(std::string_view key, Control& item) {
        (void)key; (void)item;
    }
    [[nodiscard]] virtual std::uint64_t Revision() const = 0;
};

class VirtualizedView : public Control {
public:
    using Factory = std::function<std::unique_ptr<Control>()>;
    explicit VirtualizedView(std::string name = "VirtualizedView") : Control(std::move(name)) {}
    ~VirtualizedView() override;
    [[nodiscard]] std::string_view TypeName() const override { return "VirtualizedView"; }
    [[nodiscard]] ChildLayoutPolicy ChildPolicy() const override {
        return ChildLayoutPolicy::RuntimeManaged;
    }

    Status SetSource(std::shared_ptr<IVirtualItemSource> source, Factory factory);
    // Called once for each newly created item subtree, including authored cells.
    void SetItemCreatedCallback(std::function<void(Control&)> callback) {
        m_itemCreated = std::move(callback);
    }
    void SetGridColumns(std::size_t columns);
    void SetItemExtent(Vec2 extent);
    void SetGap(Vec2 gap);
    void SetOverscan(std::size_t rows) { m_overscan = rows; }
    // The show transition each item plays when the view fills (on open or a
    // new source); items realized later by scrolling appear at once.
    [[nodiscard]] const std::optional<ShowTransitionSpec>& ItemShowTransition() const { return m_itemShow; }
    ShowTransitionSpec& EditItemShowTransition() {
        if (!m_itemShow) m_itemShow.emplace();
        return *m_itemShow;
    }
    void SetItemHoverScale(float value) { m_itemHoverScale = std::isfinite(value) ? std::clamp(value, 0.1f, 4.0f) : 1.0f; }
    [[nodiscard]] float ItemHoverScale() const { return m_itemHoverScale; }
    [[nodiscard]] std::size_t GridColumns() const { return m_columns; }
    [[nodiscard]] Vec2 ItemExtent() const { return m_itemExtent; }
    [[nodiscard]] Vec2 Gap() const { return m_gap; }
    [[nodiscard]] std::size_t Overscan() const { return m_overscan; }
    void ScrollTo(float offset);
    void ScrollToIndex(std::size_t index);
    // Scrolls by whole rows; negative values move towards the first item.
    void ScrollByRows(int rows);
    // Shows the last row at the bottom edge, as a log view opens.
    void ScrollToEnd();
    [[nodiscard]] float ScrollOffset() const { return m_scroll; }
    [[nodiscard]] std::size_t RealizedCount() const { return m_realized.size(); }
    [[nodiscard]] std::size_t PooledCount() const { return m_pool.size(); }
    [[nodiscard]] bool HasSource() const { return static_cast<bool>(m_source); }
    void HandleEvent(UIEvent& event) override;
    void Draw(graphics::Renderer2D& renderer,const Theme& theme) override;

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;
    void EnablePlacedItems() { m_placedItems = true; }

private:
    static Status ValidateKeys(const IVirtualItemSource& source);
    Status Synchronize(Rect viewport);
    std::unique_ptr<Control> Acquire(const std::string& key);
    void Recycle(const std::string& key);

    std::shared_ptr<IVirtualItemSource> m_source;
    Factory m_factory;
    std::function<void(Control&)> m_itemCreated;
    // A visible item's Runtime identity follows its source key, not its
    // current index. Reordering data must not recreate focused controls.
    std::unordered_map<std::string, Uuid> m_realized;
    std::vector<std::unique_ptr<Control>> m_pool;
    void PresentItem(Control& item);
    std::optional<ShowTransitionSpec> m_itemShow;
    float m_itemHoverScale = 1.0f;
    // Items realized before the first full fill play the item transition.
    bool m_revealItems = true;
    std::size_t m_columns = 1;
    std::size_t m_overscan = 2;
    Vec2 m_itemExtent{320, 72};
    Vec2 m_gap{8, 8};
    float m_scroll = 0.0f;
    std::uint64_t m_seenRevision = 0;
    bool m_placedItems = false;
};

class ListView final : public VirtualizedView {
public:
    explicit ListView(std::string name = "ListView") : VirtualizedView(std::move(name)) { SetGridColumns(1); }
    [[nodiscard]] std::string_view TypeName() const override { return "ListView"; }
};

class GridView final : public VirtualizedView {
public:
    explicit GridView(std::size_t columns = 4, std::string name = "GridView") : VirtualizedView(std::move(name)) { SetGridColumns(columns); }
    [[nodiscard]] std::string_view TypeName() const override { return "GridView"; }
};

/** Data-driven absolute item placement, retaining stable-key item identities. */
class PlacedItemsView final : public VirtualizedView {
public:
    explicit PlacedItemsView(std::string name = "PlacedItemsView")
        : VirtualizedView(std::move(name)) { EnablePlacedItems(); }
    [[nodiscard]] std::string_view TypeName() const override { return "PlacedItemsView"; }

    // Where the stock Player places story choices when this view lists
    // `dialogue.choices`: each choice is a `choiceArea`-sized bar moved down
    // by `choicePitch` times its offset. `choiceRows` holds one row of
    // offsets per choice count, "2:-1,1;3:-1.5,0,1.5"; a count without a row
    // centres the choices one pitch apart.
    [[nodiscard]] const Rect& ChoiceArea() const { return m_choiceArea; }
    void SetChoiceArea(const Rect& area) { m_choiceArea = area; }
    [[nodiscard]] float ChoicePitch() const { return m_choicePitch; }
    void SetChoicePitch(const float pitch) { m_choicePitch = pitch; }
    [[nodiscard]] const std::string& ChoiceRows() const { return m_choiceRows; }
    void SetChoiceRows(std::string rows) { m_choiceRows = std::move(rows); }
    [[nodiscard]] bool HasChoiceLayout() const { return m_choiceArea.w > 0 && m_choiceArea.h > 0; }

private:
    Rect m_choiceArea{};
    float m_choicePitch = 0.0f;
    std::string m_choiceRows;
};

/** Parses `choiceRows` ("2:-1,1;3:-1.5,0,1.5"); nullopt when malformed. */
[[nodiscard]] std::optional<std::map<std::size_t, std::vector<double>>>
ParseChoiceRows(std::string_view rows);

}  // namespace px::ui
