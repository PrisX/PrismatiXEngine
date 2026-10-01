#include "Engine/Progression/ProfileRules.h"
#include "Engine/UI/VirtualizedView.h"
#include "Tests/TestSupport/TestHarness.h"

#include <map>
#include <string>

namespace {

using px::Variant;
using namespace px::progress;

struct Flags {
    std::map<std::string, Variant> values;
    [[nodiscard]] ProfileFlagReader Reader() const {
        return [this](const std::string& name) -> const Variant* {
            const auto found = values.find(name);
            return found == values.end() ? nullptr : &found->second;
        };
    }
};

}  // namespace

int main() {
    px::test::Suite suite("ProfileRules");

    suite.Run("ConditionsCombineFlags", [&] {
        const auto condition = ParseProfileCondition(nlohmann::json::parse(
            R"({"all": ["clear_a", {"not": "clear_b"}, {"any": ["x", "y"]}]})"));
        suite.Require(condition.has_value(), "all/not/any parse");
        Flags flags;
        flags.values["clear_a"] = Variant(static_cast<std::int64_t>(1));
        flags.values["y"] = Variant(true);
        suite.Expect(EvaluateProfileCondition(*condition, flags.Reader()), "a, not b, any of x/y");
        flags.values["clear_b"] = Variant(true);
        suite.Expect(!EvaluateProfileCondition(*condition, flags.Reader()), "b set breaks not");
        flags.values["clear_a"] = Variant(static_cast<std::int64_t>(2));
        suite.Expect(!EvaluateProfileCondition(*ParseProfileCondition("clear_a"), flags.Reader()),
                     "only true or the integer 1 holds");
        for (const char* bad : {R"({"all": []})", R"({"every": ["a"]})", R"({"not": 3})", R"("")",
                                R"({"all": ["a"], "any": ["b"]})"})
            suite.Expect(!ParseProfileCondition(nlohmann::json::parse(bad)), bad);
    });

    suite.Run("CasesPickTheFirstMatch", [&] {
        const auto datum = ParseProfileUiDatum("title.ending", nlohmann::json::parse(
            R"({"cases": [{"when": "b", "value": 2}, {"when": "a", "value": 1}], "default": 0})"));
        suite.Require(datum.has_value(), "cases parse");
        Flags flags;
        suite.Expect(datum->Evaluate(flags.Reader()) == 0, "default when nothing holds");
        flags.values["a"] = Variant(true);
        suite.Expect(datum->Evaluate(flags.Reader()) == 1, "a alone");
        flags.values["b"] = Variant(true);
        suite.Expect(datum->Evaluate(flags.Reader()) == 2, "the first case wins");
        suite.Expect(datum->Values() == std::vector<std::int64_t>{0, 1, 2}, "every value is listed");
        suite.Expect(!ParseProfileUiDatum("x", nlohmann::json::parse(
            R"({"cases": [{"when": "a"}]})")), "a case needs a value");
    });

    suite.Run("TitleMusicRules", [&] {
        std::vector<ProfileChoice> rules;
        rules.push_back({ParseProfileCondition("clear"), "after.ogg"});
        rules.push_back({std::nullopt, "title.ogg"});
        Flags flags;
        suite.Equal("title.ogg", *FirstMatchingChoice(rules, flags.Reader()), "fallback rule");
        flags.values["clear"] = Variant(true);
        suite.Equal("after.ogg", *FirstMatchingChoice(rules, flags.Reader()), "flagged rule");
    });

    suite.Run("ChoiceRows", [&] {
        const auto rows = px::ui::ParseChoiceRows("2:-1,1;3:-1.5,0,1.5");
        suite.Require(rows.has_value(), "rows parse");
        suite.Expect(rows->at(3) == std::vector<double>{-1.5, 0.0, 1.5}, "offsets keep their order");
        for (const char* bad : {"2:-1", "0:", "2:a,b", "2:1,2;2:1,2", "x"})
            suite.Expect(!px::ui::ParseChoiceRows(bad), bad);
        suite.Expect(px::ui::ParseChoiceRows("")->empty(), "empty means no rows");
    });

    return suite.Finish();
}
