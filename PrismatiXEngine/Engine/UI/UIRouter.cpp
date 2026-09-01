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

Status RouteTable::Register(RouteDescriptor descriptor) {
    if (descriptor.id.empty() || descriptor.scene.Empty()) {
        return RouteFailure("PXUI2310", "A route requires an id and scene ResourceId");
    }
    if (m_byId.contains(descriptor.id)) {
        return RouteFailure("PXUI2311", "Duplicate route id: " + descriptor.id);
    }
    m_byId[descriptor.id] = m_routes.size();
    m_routes.push_back(std::move(descriptor));
    return Status::Ok();
}

const RouteDescriptor* RouteTable::Find(const std::string_view id) const {
    const auto found = m_byId.find(std::string(id));
    return found == m_byId.end() ? nullptr : &m_routes[found->second];
}

Status UIRouter::Register(std::string route, ScreenFactory factory) {
    if (route.empty() || !factory) return RouteFailure("PXUI2301", "Invalid UI route registration");
    if (m_factories.contains(route)) return RouteFailure("PXUI2302", "Duplicate UI route: " + route);
    m_factories.emplace(std::move(route), std::move(factory)); return Status::Ok();
}

Status UIRouter::Navigate(std::string_view route, bool replace) {
    auto created = CreateEntry(route);
    if (!created) return Status::Fail(created.Diagnostics());
    if (replace && !m_stack.empty()) m_stack.pop_back();
    m_stack.push_back(created.TakeValue());
    return Status::Ok();
}

Status UIRouter::Push(const std::string_view route, RouteTransition) {
    return Navigate(route, false);
}

Status UIRouter::Replace(const std::string_view route, RouteTransition) {
    return Navigate(route, true);
}

Status UIRouter::Back() {
    if (!m_modals.empty()) return CloseModal();
    if (m_stack.size() <= 1) return RouteFailure("PXUI2305", "Cannot navigate back from the root route");
    m_stack.pop_back(); return Status::Ok();
}

Status UIRouter::ShowModal(std::string_view route, RouteTransition) {
    auto created = CreateEntry(route);
    if (!created) return Status::Fail(created.Diagnostics());
    m_modals.push_back(created.TakeValue());
    return Status::Ok();
}

Status UIRouter::CloseModal() {
    if (m_modals.empty()) return RouteFailure("PXUI2306", "No modal UI is open");
    m_modals.pop_back(); return Status::Ok();
}

Control* UIRouter::Current() const { return m_stack.empty() ? nullptr : m_stack.back().screen.get(); }
Control* UIRouter::Modal() const {
    return m_modals.empty() ? nullptr : m_modals.back().screen.get();
}
std::string_view UIRouter::CurrentRoute() const { return m_stack.empty() ? std::string_view{} : m_stack.back().route; }
std::string_view UIRouter::CurrentModalRoute() const {
    return m_modals.empty() ? std::string_view{} : m_modals.back().route;
}

RouteState UIRouter::CaptureState() const {
    RouteState state;
    state.stack.reserve(m_stack.size());
    state.modals.reserve(m_modals.size());
    for (const auto& entry : m_stack) state.stack.push_back(entry.route);
    for (const auto& entry : m_modals) state.modals.push_back(entry.route);
    return state;
}

Status UIRouter::ValidateState(const RouteState& state) const {
    for (const auto& route : state.stack) {
        auto created = CreateEntry(route);
        if (!created) return Status::Fail(created.Diagnostics());
    }
    for (const auto& route : state.modals) {
        auto created = CreateEntry(route);
        if (!created) return Status::Fail(created.Diagnostics());
    }
    return Status::Ok();
}

Status UIRouter::RestoreState(const RouteState& state) {
    std::vector<Entry> stack;
    std::vector<Entry> modals;
    stack.reserve(state.stack.size());
    modals.reserve(state.modals.size());
    for (const auto& route : state.stack) {
        auto created = CreateEntry(route);
        if (!created) return Status::Fail(created.Diagnostics());
        stack.push_back(created.TakeValue());
    }
    for (const auto& route : state.modals) {
        auto created = CreateEntry(route);
        if (!created) return Status::Fail(created.Diagnostics());
        modals.push_back(created.TakeValue());
    }
    m_stack = std::move(stack);
    m_modals = std::move(modals);
    return Status::Ok();
}

void UIRouter::Clear() {
    m_stack.clear();
    m_modals.clear();
}

Result<UIRouter::Entry> UIRouter::CreateEntry(const std::string_view route) const {
    const auto it = m_factories.find(std::string(route));
    if (it == m_factories.end()) {
        return Result<Entry>::Failure(
            RouteFailure("PXUI2303", "Unknown UI route: " + std::string(route)).Diagnostics());
    }
    auto created = it->second();
    if (!created) return Result<Entry>::Failure(created.Diagnostics());
    if (!created.Value()) {
        return Result<Entry>::Failure(
            RouteFailure("PXUI2304", "UI route factory returned null: " + std::string(route))
                .Diagnostics());
    }
    return Result<Entry>::Success({std::string(route), std::move(created.Value())});
}

}  // namespace px::ui
