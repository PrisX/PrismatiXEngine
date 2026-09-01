#pragma once

#include "Engine/Core/Types.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace px::accessibility {

struct SemanticNode {
    ui::AccessibilitySemantics semantics;
    std::vector<SemanticNode> children;
};

struct SemanticTree {
    std::uint64_t revision = 0;
    SemanticNode root;
};

class SemanticTreeBuilder {
public:
    [[nodiscard]] static SemanticTree Build(const ui::Control& root,
                                            std::uint64_t revision = 0);
};

// Platform accessibility bridges consume immutable semantic snapshots. The
// mock adapter is intentionally production-shaped and lets CI assert exactly
// what UIA, NSAccessibility, and AT-SPI adapters are expected to expose.
class SemanticAdapter {
public:
    using ActionHandler = std::function<bool(
        const Uuid&, std::string_view action, std::string_view value)>;
    virtual ~SemanticAdapter() = default;
    virtual void Publish(const SemanticTree& tree) = 0;
    void SetActionHandler(ActionHandler handler) {
        m_actionHandler = std::move(handler);
    }
    [[nodiscard]] bool InvokeAction(
        const Uuid& id, std::string_view action,
        std::string_view value = {}) const {
        return m_actionHandler && m_actionHandler(id, action, value);
    }

private:
    ActionHandler m_actionHandler;
};

class MockSemanticAdapter final : public SemanticAdapter {
public:
    void Publish(const SemanticTree& tree) override;
    [[nodiscard]] const SemanticTree& LastTree() const { return m_tree; }
    [[nodiscard]] const std::string& CanonicalJson() const { return m_json; }

private:
    SemanticTree m_tree;
    std::string m_json;
};

}  // namespace px::accessibility
