#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace px::player {

// `player.navigation`: what the Player does at the points of a game's flow
// that no screen button decides. Every field left out keeps the Player's own
// default, so a project without the setting behaves exactly as before.
//
//   afterStoryEnd   the Story reached [end] or ran off its last line:
//                   "stay" (default: the last line stays on screen), "title"
//                   (as the title.open action), "quit", or a route id shown
//                   in place of the HUD (a credits or after-ending screen).
//   gameRightClick  a right click during the game (a two-finger tap, the
//                   Android Back button): "hideHud" (default: hide or show
//                   the message window), "none", or a route id opened as a
//                   menu, as screen.open opens it.
struct NavigationTarget {
    enum class Kind { Default, Stay, Title, Quit, HideHud, None, Route };
    Kind kind = Kind::Default;
    std::string route;

    [[nodiscard]] bool operator==(const NavigationTarget&) const = default;
};

struct NavigationSettings {
    NavigationTarget afterStoryEnd;
    NavigationTarget gameRightClick;

    [[nodiscard]] bool StaysAfterStoryEnd() const {
        return afterStoryEnd.kind == NavigationTarget::Kind::Default ||
               afterStoryEnd.kind == NavigationTarget::Kind::Stay;
    }
    [[nodiscard]] bool RightClickHidesHud() const {
        return gameRightClick.kind == NavigationTarget::Kind::Default ||
               gameRightClick.kind == NavigationTarget::Kind::HideHud;
    }
};

struct NavigationParseResult {
    std::optional<NavigationSettings> settings;
    std::string error;
};

// Routes that are the game itself and cannot be a navigation target.
inline bool IsGameplayRoute(const std::string_view route) {
    return route == "hud" || route == "game" || route == "video";
}

// Reads `player.navigation`. `isRoute` says whether an id names a route the
// Player can show (an authored uiEntryPoints route or a stock one).
inline NavigationParseResult ParseNavigationSettings(
    const nlohmann::json& value, const std::function<bool(std::string_view)>& isRoute) {
    NavigationParseResult result;
    if (!value.is_object()) {
        result.error = "player.navigation must be an object";
        return result;
    }
    NavigationSettings settings;
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (item.key() != "afterStoryEnd" && item.key() != "gameRightClick") {
            result.error = "player.navigation has no field " + item.key();
            return result;
        }
    }
    const auto read = [&](const char* field, NavigationTarget& target,
                          std::initializer_list<std::pair<std::string_view,
                                                          NavigationTarget::Kind>> words) {
        const auto found = value.find(field);
        if (found == value.end()) return true;
        if (!found->is_string() || found->get<std::string>().empty()) {
            result.error = std::string("player.navigation.") + field + " must be a non-empty string";
            return false;
        }
        const std::string text = found->get<std::string>();
        for (const auto& [word, kind] : words)
            if (text == word) {
                target.kind = kind;
                return true;
            }
        if (IsGameplayRoute(text) || !isRoute(text)) {
            result.error = std::string("player.navigation.") + field + " names no screen the Player can show: " + text;
            return false;
        }
        target.kind = NavigationTarget::Kind::Route;
        target.route = text;
        return true;
    };
    using Kind = NavigationTarget::Kind;
    if (!read("afterStoryEnd", settings.afterStoryEnd,
              {{"stay", Kind::Stay}, {"title", Kind::Title}, {"quit", Kind::Quit}}) ||
        !read("gameRightClick", settings.gameRightClick,
              {{"hideHud", Kind::HideHud}, {"none", Kind::None}}))
        return result;
    // The title is a place to return to, not a menu to open over the game.
    if (settings.gameRightClick.kind == Kind::Route && settings.gameRightClick.route == "title") {
        result.error = "player.navigation.gameRightClick cannot open the title; use a menu screen";
        return result;
    }
    result.settings = std::move(settings);
    return result;
}

}  // namespace px::player
