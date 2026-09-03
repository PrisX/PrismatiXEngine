#include "Engine/UI/UIRouter.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <cmath>
#include <utility>

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
    return replace ? Replace(route) : Push(route);
}

Status UIRouter::Push(const std::string_view route, RouteTransition transition) {
    auto created = CreateEntry(route);
    if (!created) return Status::Fail(created.Diagnostics());
    const std::string outgoing = std::string(CurrentModalRoute().empty()
                                                 ? CurrentRoute()
                                                 : CurrentModalRoute());
    auto started = StartTransition(outgoing, route, transition, false);
    if (!started) return Status::Fail(started.Diagnostics());
    m_stack.push_back(created.TakeValue());
    Present(route, "push");
    return Status::Ok();
}

Status UIRouter::Replace(const std::string_view route, RouteTransition transition) {
    auto created = CreateEntry(route);
    if (!created) return Status::Fail(created.Diagnostics());
    const std::string outgoing = std::string(CurrentModalRoute().empty()
                                                 ? CurrentRoute()
                                                 : CurrentModalRoute());
    auto started = StartTransition(outgoing, route, transition, false);
    if (!started) return Status::Fail(started.Diagnostics());
    if (!m_stack.empty()) m_stack.pop_back();
    m_stack.push_back(created.TakeValue());
    Present(route, "replace");
    return Status::Ok();
}

Status UIRouter::Back(RouteTransition transition) {
    if (!m_modals.empty()) return CloseModal(std::move(transition));
    if (m_stack.size() <= 1) return RouteFailure("PXUI2305", "Cannot navigate back from the root route");
    const std::string outgoing = m_stack.back().route;
    const std::string incoming = m_stack[m_stack.size() - 2].route;
    auto started = StartTransition(outgoing, incoming, transition, false);
    if (!started) return Status::Fail(started.Diagnostics());
    m_stack.pop_back();
    Present(incoming, "back");
    return Status::Ok();
}

Status UIRouter::ShowModal(std::string_view route, RouteTransition transition) {
    auto created = CreateEntry(route);
    if (!created) return Status::Fail(created.Diagnostics());
    const std::string outgoing = std::string(CurrentModalRoute().empty()
                                                 ? CurrentRoute()
                                                 : CurrentModalRoute());
    auto started = StartTransition(outgoing, route, transition, true);
    if (!started) return Status::Fail(started.Diagnostics());
    m_modals.push_back(created.TakeValue());
    Present(route, "modal");
    return Status::Ok();
}

Status UIRouter::CloseModal(RouteTransition transition) {
    if (m_modals.empty()) return RouteFailure("PXUI2306", "No modal UI is open");
    const std::string outgoing = m_modals.back().route;
    const std::string incoming = m_modals.size() > 1
                                     ? m_modals[m_modals.size() - 2].route
                                     : std::string(CurrentRoute());
    auto started = StartTransition(outgoing, incoming, transition, true);
    if (!started) return Status::Fail(started.Diagnostics());
    m_modals.pop_back();
    Present(incoming, "back");
    return Status::Ok();
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
    if (m_activeTransition && m_transitionDriver.cancel)
        (void)m_transitionDriver.cancel(m_activeTransition->handle);
    m_activeTransition.reset();
    return Status::Ok();
}

Result<RouteTransitionHandle> UIRouter::StartTransition(
    const std::string_view outgoing, const std::string_view incoming,
    const RouteTransition& transition, const bool modal) {
    RouteTransition normalized = transition;
    if (normalized.preset.empty()) {
        if (const auto fallback = DefaultTransition(outgoing, incoming))
            normalized = *fallback;
    }
    if (normalized.preset.empty()) normalized.preset = "none";
    const std::string& preset = normalized.preset;
    if (!std::isfinite(normalized.durationSeconds) ||
        normalized.durationSeconds < 0.0f) {
        return Result<RouteTransitionHandle>::Failure(
            RouteFailure("PXUI2307", "Route transition duration is invalid")
                .Diagnostics());
    }
    if (m_activeTransition && m_transitionDriver.cancel)
        (void)m_transitionDriver.cancel(m_activeTransition->handle);
    m_activeTransition.reset();
    if (preset == "none" || normalized.durationSeconds <= 0.0f)
        return Result<RouteTransitionHandle>::Success(0);
    if (!m_transitionDriver.play) {
        return Result<RouteTransitionHandle>::Failure(
            RouteFailure("PXUI2308", "Route transition runtime is unavailable")
                .Diagnostics());
    }
    const auto handle = m_transitionDriver.play(normalized);
    if (!handle) {
        return Result<RouteTransitionHandle>::Failure(
            RouteFailure("PXUI2309", "Unknown or unavailable route transition: " + preset)
                .Diagnostics());
    }
    m_activeTransition = ActiveRouteTransition{
        handle, std::string(outgoing), std::string(incoming),
        std::move(normalized), modal};
    return Result<RouteTransitionHandle>::Success(handle);
}

Status UIRouter::SetDefaultTransition(const std::string_view outgoing,
                                      const std::string_view incoming,
                                      RouteTransition transition) {
    if (outgoing.empty() || incoming.empty() || transition.preset.empty() ||
        !std::isfinite(transition.durationSeconds) ||
        transition.durationSeconds < 0.0f)
        return RouteFailure("PXUI2312", "Default route transition is invalid");
    m_defaultTransitions.insert_or_assign(
        std::string(outgoing) + "\n" + std::string(incoming),
        std::move(transition));
    return Status::Ok();
}

std::optional<RouteTransition> UIRouter::DefaultTransition(
    const std::string_view outgoing, const std::string_view incoming) const {
    const auto found = m_defaultTransitions.find(
        std::string(outgoing) + "\n" + std::string(incoming));
    return found == m_defaultTransitions.end()
               ? std::nullopt
               : std::optional<RouteTransition>{found->second};
}

void UIRouter::Present(const std::string_view route,
                       const std::string_view operation) const {
    if (m_presentation) m_presentation(route, operation);
}

void UIRouter::UpdateTransition() {
    if (!m_activeTransition || !m_transitionDriver.playing ||
        m_transitionDriver.playing(m_activeTransition->handle))
        return;
    m_activeTransition.reset();
}

bool UIRouter::StopTransition() {
    if (!m_activeTransition || !m_transitionDriver.stop) return false;
    const bool stopped = m_transitionDriver.stop(m_activeTransition->handle);
    if (stopped) m_activeTransition.reset();
    return stopped;
}

bool UIRouter::CancelTransition() {
    if (!m_activeTransition || !m_transitionDriver.cancel) return false;
    const bool cancelled = m_transitionDriver.cancel(m_activeTransition->handle);
    if (cancelled) m_activeTransition.reset();
    return cancelled;
}

void UIRouter::Clear() {
    (void)CancelTransition();
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
