#include "Applications/Player/PlayerNavigation.h"
#include "Tests/TestSupport/TestHarness.h"

#include <set>
#include <string>

namespace {

using px::player::NavigationTarget;
using px::player::ParseNavigationSettings;
using Kind = NavigationTarget::Kind;

const std::set<std::string, std::less<>> kRoutes = {
    "title", "save", "load", "settings", "backlog", "gallery", "credits", "menu", "hud"};

bool IsRoute(const std::string_view route) {
    return kRoutes.contains(route);
}

px::player::NavigationParseResult Parse(const char* json) {
    return ParseNavigationSettings(nlohmann::json::parse(json), IsRoute);
}

}  // namespace

int main() {
    px::test::Suite suite("PlayerNavigation");

    suite.Run("LeftOutFieldsKeepThePlayerDefaults", [&] {
        const auto parsed = Parse("{}");
        suite.Require(parsed.settings.has_value(), "an empty object is valid", parsed.error);
        suite.Expect(parsed.settings->afterStoryEnd.kind == Kind::Default, "afterStoryEnd default");
        suite.Expect(parsed.settings->StaysAfterStoryEnd(), "the last line stays by default");
        suite.Expect(parsed.settings->RightClickHidesHud(), "a right click hides the HUD by default");
        // The defaults spelled out mean the same.
        const auto spelled = Parse(R"({"afterStoryEnd": "stay", "gameRightClick": "hideHud"})");
        suite.Require(spelled.settings.has_value(), "the default words parse", spelled.error);
        suite.Expect(spelled.settings->StaysAfterStoryEnd(), "stay");
        suite.Expect(spelled.settings->RightClickHidesHud(), "hideHud");
    });

    suite.Run("KeywordsAndRoutes", [&] {
        const auto title = Parse(R"({"afterStoryEnd": "title", "gameRightClick": "none"})");
        suite.Require(title.settings.has_value(), "title and none parse", title.error);
        suite.Expect(title.settings->afterStoryEnd.kind == Kind::Title, "title");
        suite.Expect(title.settings->gameRightClick.kind == Kind::None, "none");
        suite.Expect(!title.settings->RightClickHidesHud(), "none does not hide the HUD");
        const auto quit = Parse(R"({"afterStoryEnd": "quit"})");
        suite.Require(quit.settings.has_value(), "quit parses", quit.error);
        suite.Expect(quit.settings->afterStoryEnd.kind == Kind::Quit, "quit");
        const auto routes = Parse(R"({"afterStoryEnd": "credits", "gameRightClick": "menu"})");
        suite.Require(routes.settings.has_value(), "routes parse", routes.error);
        suite.Expect(routes.settings->afterStoryEnd == NavigationTarget{Kind::Route, "credits"},
                     "afterStoryEnd shows a route");
        suite.Expect(routes.settings->gameRightClick == NavigationTarget{Kind::Route, "menu"},
                     "gameRightClick opens a route");
        suite.Expect(!routes.settings->StaysAfterStoryEnd(), "a route leaves the last line");
    });

    suite.Run("RejectsWhatThePlayerCannotShow", [&] {
        for (const char* bad : {
                 R"("title")",
                 R"({"afterStoryEnd": "missing"})",
                 R"({"afterStoryEnd": "hud"})",
                 R"({"afterStoryEnd": ""})",
                 R"({"afterStoryEnd": 3})",
                 R"({"gameRightClick": "title"})",
                 R"({"gameRightClick": "video"})",
                 R"({"onEnd": "title"})"}) {
            const auto parsed = Parse(bad);
            suite.Expect(!parsed.settings.has_value() && !parsed.error.empty(), bad);
        }
    });

    return suite.Finish();
}
