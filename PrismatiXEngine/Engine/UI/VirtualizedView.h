#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Control.h"

#include <functional>
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
    void SetGridColumns(std::size_t columns);
    void SetItemExtent(Vec2 extent);
    void SetGap(Vec2 gap);
    void SetOverscan(std::size_t rows) { m_overscan = rows; }
    [[nodiscard]] std::size_t GridColumns() const { return m_columns; }
    [[nodiscard]] Vec2 ItemExtent() const { return m_itemExtent; }
    [[nodiscard]] Vec2 Gap() const { return m_gap; }
    [[nodiscard]] std::size_t Overscan() const { return m_overscan; }
    void ScrollTo(float offset);
    void ScrollToIndex(std::size_t index);
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
    // A visible item's Runtime identity follows its source key, not its
    // current index. Reordering data must not recreate focused controls.
    std::unordered_map<std::string, Uuid> m_realized;
    std::vector<std::unique_ptr<Control>> m_pool;
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
};

}  // namespace px::ui
