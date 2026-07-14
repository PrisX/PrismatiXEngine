#pragma once

#include "Engine/UI/Actions/ActionDispatcher.h"

#include <functional>
#include <unordered_map>

namespace px::ui {

class BuiltInActionProvider final : public IActionProvider {
public:
    using Handler = std::function<Status(const ActionInvocation&)>;

    [[nodiscard]] std::string_view ProviderId() const override { return "builtin"; }
    [[nodiscard]] ActionOrigin Origin() const override { return ActionOrigin::BuiltIn; }
    [[nodiscard]] bool CanInvoke(std::string_view action) const override;
    Status Invoke(const ActionInvocation& invocation) override;

    Status Register(std::string action, Handler handler);
    void SetFallback(Handler handler) { m_fallback = std::move(handler); }

private:
    std::unordered_map<std::string, Handler> m_handlers;
    Handler m_fallback;
};

[[nodiscard]] std::vector<ActionDescriptor> BuiltInActionDescriptors();
Status RegisterBuiltInActionDescriptors(ActionCatalog& catalog);

}  // namespace px::ui
