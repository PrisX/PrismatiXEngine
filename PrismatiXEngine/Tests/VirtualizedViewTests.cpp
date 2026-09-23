#include "Engine/UI/VirtualizedView.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

class Rows final : public px::ui::IVirtualItemSource {
public:
    explicit Rows(std::vector<std::string> values) : keys(std::move(values)) {}

    [[nodiscard]] std::size_t Count() const override { return keys.size(); }
    [[nodiscard]] std::string StableKey(std::size_t index) const override {
        return keys.at(index);
    }
    px::Status Bind(std::size_t index, px::ui::Control& item) override {
        item.SetAccessibilityLabel(keys.at(index));
        return px::Status::Ok();
    }
    void Unbind(std::string_view key, px::ui::Control& item) override {
        unbound.emplace_back(key);
        item.SetAccessibilityLabel({});
    }
    [[nodiscard]] std::uint64_t Revision() const override { return revision; }

    void Replace(std::vector<std::string> values) {
        keys = std::move(values);
        ++revision;
    }

    std::vector<std::string> keys;
    std::vector<std::string> unbound;
    std::uint64_t revision = 1;
};

std::unique_ptr<px::ui::Control> MakeItem() {
    auto item = std::make_unique<px::ui::Control>("Item");
    (void)item->AddChild(std::make_unique<px::ui::Control>("Nested"));
    return item;
}

using Ids = std::pair<px::Uuid, px::Uuid>;

std::map<std::string, Ids> Snapshot(const px::ui::VirtualizedView& view) {
    std::map<std::string, Ids> result;
    for (const auto& child : view.Children()) {
        const auto* control = dynamic_cast<const px::ui::Control*>(child.get());
        Check(control && control->Child(0), "a virtual item must retain its nested control");
        result.emplace(control->AccessibilityLabel(),
                       Ids{control->Id(), control->Child(0)->Id()});
    }
    return result;
}

std::vector<std::string> ChildOrder(const px::ui::VirtualizedView& view) {
    std::vector<std::string> values;
    for (const auto& child : view.Children()) {
        const auto* control = dynamic_cast<const px::ui::Control*>(child.get());
        Check(control != nullptr, "virtual child must be a Control");
        values.push_back(control->AccessibilityLabel());
    }
    return values;
}

}  // namespace

int main() {
    px::ui::GridView view(2);
    view.SetItemExtent({100.0f, 50.0f});
    view.SetGap({0.0f, 0.0f});
    const auto source = std::make_shared<Rows>(
        std::vector<std::string>{"A", "B", "C"});
    Check(static_cast<bool>(view.SetSource(source, MakeItem)),
          "a keyed virtual source must install");
    view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
    const auto original = Snapshot(view);
    Check(original.size() == 3 && original.at("A") != original.at("B") &&
              original.at("A").second != original.at("C").second,
          "item and nested node identities must be unique per stable key");

    px::ui::GridView slashKeys(2);
    slashKeys.SetItemExtent({100.0f, 50.0f});
    const auto slashSource = std::make_shared<Rows>(
        std::vector<std::string>{"a", "a/0"});
    Check(static_cast<bool>(slashKeys.SetSource(slashSource, MakeItem)),
          "keys containing path separators must remain valid data keys");
    slashKeys.Arrange({0.0f, 0.0f, 200.0f, 100.0f});
    const auto slashIds = Snapshot(slashKeys);
    Check(slashIds.at("a").second != slashIds.at("a/0").first,
          "length-prefixed keys must not collide with descendant id paths");

    source->Replace({"C", "A", "B"});
    view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
    Check(ChildOrder(view) == std::vector<std::string>{"C", "A", "B"} &&
              Snapshot(view) == original && view.PooledCount() == 0,
          "reordering visible keys must retain controls and update draw/input order");

    source->Replace({"D", "C", "A", "B"});
    view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
    const auto inserted = Snapshot(view);
    Check(inserted.size() == 4 && inserted.at("A") == original.at("A") &&
              inserted.at("B") == original.at("B") &&
              inserted.at("C") == original.at("C") &&
              inserted.at("D") != original.at("A"),
          "inserting a key must not recreate existing virtual items");

    std::vector<std::string> permutation{"A", "B", "C", "D"};
    do {
        source->Replace(permutation);
        view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
        Check(ChildOrder(view) == permutation && Snapshot(view) == inserted,
              "every visible key permutation must preserve identity and order");
    } while (std::next_permutation(permutation.begin(), permutation.end()));

    source->Replace({"D", "C", "A"});
    view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
    Check(view.RealizedCount() == 3 && view.PooledCount() == 1 &&
              !Snapshot(view).contains("B") && source->unbound.back() == "B",
          "removing a key must recycle only that item");

    const auto invalid = std::make_shared<Rows>(
        std::vector<std::string>{"duplicate", "duplicate"});
    const auto unbindCount = source->unbound.size();
    Check(!view.SetSource(invalid, MakeItem) && view.RealizedCount() == 3 &&
              source->unbound.size() == unbindCount,
          "an invalid replacement source must preserve the live view");

    source->Replace({"E", "D", "C", "A"});
    view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
    const auto reused = Snapshot(view);
    Check(reused.at("E") != original.at("B") &&
              reused.at("E").second != original.at("B").second &&
              reused.at("A") == original.at("A"),
          "a pooled subtree assigned to a different key needs new per-key ids");

    source->Replace({"B", "D", "C", "A"});
    view.Arrange({0.0f, 0.0f, 200.0f, 150.0f});
    Check(Snapshot(view).at("B") == original.at("B"),
          "a key returning after recycling must recover its stable node identity");

    px::ui::ListView longList;
    longList.SetItemExtent({100.0f, 20.0f});
    longList.SetGap({0.0f, 0.0f});
    longList.SetOverscan(0);
    const auto longSource = std::make_shared<Rows>(
        std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"});
    Check(static_cast<bool>(longList.SetSource(longSource, MakeItem)),
          "a long keyed list must install");
    longList.Arrange({0.0f, 0.0f, 100.0f, 20.0f});
    const auto firstWindow = Snapshot(longList);
    Check(firstWindow.size() == 2 && firstWindow.contains("0"),
          "virtualization must realize only the visible range");
    longList.ScrollToIndex(8);
    longList.Arrange({0.0f, 0.0f, 100.0f, 20.0f});
    Check(Snapshot(longList).contains("8") && !Snapshot(longList).contains("0") &&
              longSource->unbound.size() == 2,
          "scrolling must recycle offscreen items");
    longList.ScrollToIndex(0);
    longList.Arrange({0.0f, 0.0f, 100.0f, 20.0f});
    Check(Snapshot(longList) == firstWindow,
          "offscreen items returning through a pool must recover per-key subtree ids");

    const auto destructionSource = std::make_shared<Rows>(
        std::vector<std::string>{"one", "two"});
    {
        px::ui::ListView temporary;
        temporary.SetItemExtent({100.0f, 20.0f});
        Check(static_cast<bool>(temporary.SetSource(destructionSource, MakeItem)),
              "temporary view must install");
        temporary.Arrange({0.0f, 0.0f, 100.0f, 80.0f});
    }
    Check(destructionSource->unbound.size() == 2,
          "destroying a virtual view must unbind all remaining live items");
    return 0;
}
