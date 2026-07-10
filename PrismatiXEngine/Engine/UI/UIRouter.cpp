#include "Engine/UI/UIRouter.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::ui {
namespace {
Status RouteFailure(std::string code, std::string message) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error, .code = std::move(code),
                                .category = "UI.Navigation", .message = std::move(message)};
    diag::Emit(diagnostic); return Status::Fail(std::move(diagnostic));
}
}

Status UIRouter::Register(std::string route, ScreenFactory factory) {
    if (route.empty() || !factory) return RouteFailure("PXUI2301", "Invalid UI route registration");
    if (m_factories.contains(route)) return RouteFailure("PXUI2302", "Duplicate UI route: " + route);
    m_factories.emplace(std::move(route), std::move(factory)); return Status::Ok();
}

Status UIRouter::Navigate(std::string_view route, bool replace) {
    const auto it = m_factories.find(std::string(route));
    if (it == m_factories.end()) return RouteFailure("PXUI2303", "Unknown UI route: " + std::string(route));
    auto created = it->second();
    if (!created) return Status::Fail(created.Diagnostics());
    if (!created.Value()) return RouteFailure("PXUI2304", "UI route factory returned null: " + std::string(route));
    if (replace && !m_stack.empty()) m_stack.pop_back();
    m_stack.push_back({std::string(route), std::move(created.Value())});
    return Status::Ok();
}

Status UIRouter::Back() {
    if (m_modal) return CloseModal();
    if (m_stack.size() <= 1) return RouteFailure("PXUI2305", "Cannot navigate back from the root route");
    m_stack.pop_back(); return Status::Ok();
}

Status UIRouter::ShowModal(std::string_view route) {
    const auto it = m_factories.find(std::string(route));
    if (it == m_factories.end()) return RouteFailure("PXUI2303", "Unknown UI route: " + std::string(route));
    auto created = it->second(); if (!created) return Status::Fail(created.Diagnostics());
    m_modal = std::move(created.Value());
    if (!m_modal) return RouteFailure("PXUI2304", "UI modal factory returned null: " + std::string(route));
    return Status::Ok();
}

Status UIRouter::CloseModal() {
    if (!m_modal) return RouteFailure("PXUI2306", "No modal UI is open");
    m_modal.reset(); return Status::Ok();
}

Control* UIRouter::Current() const { return m_stack.empty() ? nullptr : m_stack.back().screen.get(); }
std::string_view UIRouter::CurrentRoute() const { return m_stack.empty() ? std::string_view{} : m_stack.back().route; }

}  // namespace px::ui
