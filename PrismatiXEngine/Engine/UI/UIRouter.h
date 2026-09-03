#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/ResourceRef.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::ui {

using RouteId = std::string;
struct UISceneResourceTag {};

enum class RouteCachePolicy : std::uint8_t { Recreate, KeepAlive };

struct RouteTransition {
    // Empty selects a registered route-pair default; "none" explicitly
    // disables it.
    std::string preset;
    float durationSeconds = 0.0f;
};

using RouteTransitionHandle = std::uint64_t;

struct ActiveRouteTransition {
    RouteTransitionHandle handle = 0;
    RouteId outgoingRoute;
    RouteId incomingRoute;
    RouteTransition transition;
    bool modal = false;
};

struct RouteDescriptor {
    RouteId id;
    resource::ResourceRef<UISceneResourceTag> scene;
    RouteCachePolicy cachePolicy = RouteCachePolicy::Recreate;
};

struct RouteState {
    std::vector<RouteId> stack;
    std::vector<RouteId> modals;
};

class RouteTable {
public:
    Status Register(RouteDescriptor descriptor);
    [[nodiscard]] const RouteDescriptor* Find(std::string_view id) const;
    [[nodiscard]] const std::vector<RouteDescriptor>& Routes() const { return m_routes; }

private:
    std::vector<RouteDescriptor> m_routes;
    std::unordered_map<RouteId, std::size_t> m_byId;
};

class UIRouter {
public:
    using ScreenFactory = std::function<Result<std::unique_ptr<Control>>() >;
    using PresentationHandler =
        std::function<void(std::string_view route, std::string_view operation)>;
    struct TransitionDriver {
        std::function<RouteTransitionHandle(const RouteTransition&)> play;
        std::function<bool(RouteTransitionHandle)> playing;
        std::function<bool(RouteTransitionHandle)> stop;
        std::function<bool(RouteTransitionHandle)> cancel;
    };

    Status Register(std::string route, ScreenFactory factory);
    Status Navigate(std::string_view route, bool replace = false);
    Status Push(std::string_view route, RouteTransition transition = {});
    Status Replace(std::string_view route, RouteTransition transition = {});
    Status Back(RouteTransition transition = {});
    Status ShowModal(std::string_view route, RouteTransition transition = {});
    Status CloseModal(RouteTransition transition = {});
    [[nodiscard]] Control* Current() const;
    [[nodiscard]] Control* Modal() const;
    [[nodiscard]] std::string_view CurrentRoute() const;
    [[nodiscard]] std::string_view CurrentModalRoute() const;
    [[nodiscard]] RouteState CaptureState() const;
    [[nodiscard]] Status ValidateState(const RouteState& state) const;
    Status RestoreState(const RouteState& state);
    void SetPresentationHandler(PresentationHandler handler) {
        m_presentation = std::move(handler);
    }
    void SetTransitionDriver(TransitionDriver driver) {
        m_transitionDriver = std::move(driver);
    }
    Status SetDefaultTransition(std::string_view outgoing,
                                std::string_view incoming,
                                RouteTransition transition);
    [[nodiscard]] std::optional<RouteTransition> DefaultTransition(
        std::string_view outgoing, std::string_view incoming) const;
    void UpdateTransition();
    [[nodiscard]] const std::optional<ActiveRouteTransition>& ActiveTransition() const {
        return m_activeTransition;
    }
    [[nodiscard]] bool StopTransition();
    [[nodiscard]] bool CancelTransition();
    void Clear();

private:
    struct Entry { std::string route; std::unique_ptr<Control> screen; };
    Result<Entry> CreateEntry(std::string_view route) const;
    Result<RouteTransitionHandle> StartTransition(
        std::string_view outgoing, std::string_view incoming,
        const RouteTransition& transition, bool modal);
    void Present(std::string_view route, std::string_view operation) const;
    std::unordered_map<std::string, ScreenFactory> m_factories;
    std::vector<Entry> m_stack;
    std::vector<Entry> m_modals;
    PresentationHandler m_presentation;
    TransitionDriver m_transitionDriver;
    std::optional<ActiveRouteTransition> m_activeTransition;
    std::unordered_map<std::string, RouteTransition> m_defaultTransitions;
};

}  // namespace px::ui
