#include "Engine/Preview/ProgressionPreview.h"

#include <cassert>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using Json = nlohmann::json;

Json Document() {
    return {
        {"format", "PrismatiXProgression"},
        {"schemaRevision", 1},
        {"id", "project-01"},
        {"revision", 12},
        {"nodes", Json::array({
            {{"id", "start-01"}, {"kind", "start"}, {"name", "Start"},
             {"targetId", nullptr}, {"x", 80}, {"y", 120}},
            {{"id", "ending-01"}, {"kind", "ending"}, {"name", "True ending"},
             {"targetId", "true_ending"}, {"x", 340}, {"y", 120}},
        })},
        {"edges", Json::array({
            {{"id", "edge-01"}, {"from", "start-01"}, {"to", "ending-01"},
             {"requirements", Json::array({
                 {{"id", "requirement-route"}, {"kind", "routeCleared"},
                  {"key", "route-a"}, {"operator", "is"}, {"value", true}},
                 {{"id", "requirement-count"}, {"kind", "clearCount"},
                  {"key", "clearCount"}, {"operator", "gte"}, {"value", 2}},
             })}},
        })},
        {"variables", Json::array()},
        {"carryOverPolicy", {{"variables", Json::array()},
                              {"seenText", true}, {"seenChoices", true},
                              {"unlockedScenes", true},
                              {"unlockedGallery", true}, {"clearCount", true}}},
    };
}

std::string Request(Json document, Json state) {
    return Json{{"document", std::move(document)},
                {"state", std::move(state)}}.dump();
}

Json State(Json routes = Json::array(), int clearCount = 0) {
    return {{"clearedRouteIds", std::move(routes)},
            {"flags", Json::array()},
            {"clearCount", clearCount},
            {"variables", Json::object()}};
}

bool ContainsCode(const std::vector<px::diag::Diagnostic>& diagnostics,
                  const std::string& code) {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

}  // namespace

int main() {
    const auto locked = px::preview::SimulateProgressionPreview(
        Request(Document(), State()));
    assert(locked);
    assert(locked.Value().revision == 12);
    assert(locked.Value().nodes.size() == 2);
    assert(locked.Value().nodes[0].unlocked);
    assert(!locked.Value().nodes[1].unlocked);
    assert(locked.Value().nodes[1].unmetRequirementIds.size() == 2);

    const auto unlocked = px::preview::SimulateProgressionPreview(
        Request(Document(), State(Json::array({"route-a"}), 2)));
    assert(unlocked);
    assert(unlocked.Value().nodes[1].unlocked);
    assert(unlocked.Value().nodes[1].unmetRequirementIds.empty());

    auto cyclic = Document();
    cyclic["edges"].push_back({{"id", "edge-02"},
                                {"from", "ending-01"},
                                {"to", "start-01"},
                                {"requirements", Json::array()}});
    const auto cycle = px::preview::SimulateProgressionPreview(
        Request(std::move(cyclic), State()));
    assert(!cycle);
    assert(ContainsCode(cycle.Diagnostics(),
                        "PXWASM-PROGRESSION-CYCLE-001"));

    auto invalid = Document();
    invalid["edges"][0]["requirements"][0]["operator"] = "gte";
    const auto invalidRequirement =
        px::preview::SimulateProgressionPreview(
            Request(std::move(invalid), State()));
    assert(!invalidRequirement);
    assert(ContainsCode(invalidRequirement.Diagnostics(),
                        "PXWASM-PROGRESSION-REQUIREMENT-003"));

    auto undefinedVariable = Document();
    undefinedVariable["edges"][0]["requirements"].push_back(
        {{"id", "requirement-variable"}, {"kind", "variable"},
         {"key", "profile.affinity"}, {"operator", "gte"}, {"value", 4}});
    const auto missingDeclaration =
        px::preview::SimulateProgressionPreview(
            Request(std::move(undefinedVariable), State()));
    assert(!missingDeclaration);
    assert(ContainsCode(missingDeclaration.Diagnostics(),
                        "PXWASM-PROGRESSION-VARIABLE-003"));

    auto invalidTarget = Document();
    invalidTarget["nodes"][1]["targetId"] = "not an identifier";
    const auto rejectedTarget = px::preview::SimulateProgressionPreview(
        Request(std::move(invalidTarget), State()));
    assert(!rejectedTarget);
    assert(ContainsCode(rejectedTarget.Diagnostics(),
                        "PXWASM-PROGRESSION-NODE-005"));
    return 0;
}
