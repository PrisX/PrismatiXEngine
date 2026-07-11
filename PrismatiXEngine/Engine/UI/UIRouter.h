#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/ResourceRef.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::ui {

using RouteId = std::string;
struct UISceneResourceTag {};

enum class RouteCachePolicy : std::uint8_t { Recreate, KeepAlive };

struct RouteTransition {
    std::string preset;
    float durationSeconds = 0.0f;
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

    Status Register(std::string route, ScreenFactory factory);
    Status Navigate(std::string_view route, bool replace = false);
    Status Push(std::string_view route, RouteTransition transition = {});
    Status Replace(std::string_view route, RouteTransition transition = {});
    Status Back();
    Status ShowModal(std::string_view route, RouteTransition transition = {});
    Status CloseModal();
    [[nodiscard]] Control* Current() const;
    [[nodiscard]] Control* Modal() const;
    [[nodiscard]] std::string_view CurrentRoute() const;
    [[nodiscard]] std::string_view CurrentModalRoute() const;
    [[nodiscard]] RouteState CaptureState() const;
    Status RestoreState(const RouteState& state);
    void Clear();

private:
    struct Entry { std::string route; std::unique_ptr<Control> screen; };
    Result<Entry> CreateEntry(std::string_view route) const;
    std::unordered_map<std::string, ScreenFactory> m_factories;
    std::vector<Entry> m_stack;
    std::vector<Entry> m_modals;
};

}  // namespace px::ui
