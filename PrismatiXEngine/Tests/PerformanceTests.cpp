#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Tests/TestSupport/DesignerFixture.h"
#include "Tests/TestSupport/TestHarness.h"

#include <string>
#include <vector>

int main() {
    px::test::Suite suite("Performance");
    suite.Run("LargeScenario_10kSerializesParsesAndValidates", [&] {
        px::vn::scenario::ScenarioDocument document;
        document.id = px::Uuid::FromName("PrismatiX.Performance.10k");
        document.name = "10k acceptance";
        document.nodes.reserve(10000);
        document.edges.reserve(9999);
        for (int index = 0; index < 10000; ++index) {
            px::vn::scenario::ScenarioNode node{
                px::Uuid::FromName("PrismatiX.Performance.Node." +
                                   std::to_string(index)),
                "say",
                {{"textId", "line-" + std::to_string(index)},
                 {"value", "line " + std::to_string(index)}}};
            if (index == 0) document.entry = node.id;
            if (index > 0) {
                document.edges.push_back(
                    {px::Uuid::FromName("PrismatiX.Performance.Edge." +
                                        std::to_string(index)),
                     document.nodes.back().id, "flow", node.id, "in"});
            }
            document.nodes.push_back(std::move(node));
        }
        const auto encoded = px::vn::scenario::WriteScenario(document);
        const auto parsed =
            px::vn::scenario::ParseScenario(encoded, "large.pxscenario");
        const auto actual = parsed ?
            std::to_string(parsed.Value().nodes.size()) : "parse failure";
        suite.Expect(parsed && parsed.Value().nodes.size() == 10000 &&
                         px::vn::scenario::ValidateScenario(parsed.Value()).Valid(),
                     "10,000-line Scenario serializes, parses, and validates",
                     "actual nodes/parse result: " + actual);
    });

    suite.Run("LargeDesignerDocument_10kIndexRemainsCanonical", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& document = fixture.session.Document()->Data();
        document.nodes.reserve(10004);
        for (int index = 0; index < 10000; ++index) {
            document.nodes.push_back(
                {px::Uuid::FromName("PrismatiX.Performance.Control." +
                                    std::to_string(index)),
                 fixture.root, "Control " + std::to_string(index), "Button",
                 {{"visibility", std::string("Visible")}}});
        }
        suite.Expect(static_cast<bool>(fixture.session.DocumentView().Rebuild(
                         *fixture.session.Document())) &&
                         fixture.IndexMatchesDocument() &&
                         fixture.session.DocumentView().NodeCount() == 10004,
                     "10,000-node Designer scene rebuilds one canonical index",
                     "actual indexed nodes: " +
                         std::to_string(fixture.session.DocumentView().NodeCount()));
    });

    suite.Run("CommandHistory_256UndoRedoAndBulkEditStayExact", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& commands = fixture.session.Commands();
        bool changed = true;
        for (int index = 0; index < 256; ++index) {
            changed &= static_cast<bool>(commands.SetProperty(
                fixture.parent, "text", "revision " + std::to_string(index),
                "Performance edit"));
        }
        bool navigated = true;
        for (int index = 0; index < 256; ++index)
            navigated &= static_cast<bool>(commands.Undo());
        for (int index = 0; index < 256; ++index)
            navigated &= static_cast<bool>(commands.Redo());

        std::vector<px::Uuid> targets;
        targets.reserve(500);
        for (int index = 0; index < 500; ++index) {
            const auto id = px::Uuid::FromName(
                "PrismatiX.Performance.Bulk." + std::to_string(index));
            fixture.session.Document()->Data().nodes.push_back(
                {id, fixture.root, "Bulk " + std::to_string(index), "Label", {}});
            targets.push_back(id);
        }
        suite.Expect(static_cast<bool>(fixture.session.DocumentView().Rebuild(
                         *fixture.session.Document())),
                     "bulk fixture rebuilds before mutation");
        const auto cursor = commands.HistoryCursor();
        const bool bulk = static_cast<bool>(commands.SetProperties(
            targets, "text", std::string("bulk"), "Bulk multi-edit"));
        suite.Expect(changed && navigated && bulk &&
                         commands.HistoryCursor() == cursor + 1 &&
                         fixture.session.Document()->ReadProperty(
                             targets.back(), "text").Value() ==
                             px::Variant(std::string("bulk")),
                     "repeated history navigation and 500-target edit remain exact");
    });

    suite.Run("PreviewPlanner_100kInteractionsNeverEscalatePaint", [&] {
        const auto document = px::Uuid::FromName("PrismatiX.Performance.Preview");
        const auto node = px::Uuid::FromName("PrismatiX.Performance.Preview.Node");
        std::size_t patches = 0;
        std::size_t relayouts = 0;
        std::size_t rebuilds = 0;
        for (int index = 0; index < 100000; ++index) {
            const auto plan = px::editor::PlanPreviewUpdate(
                px::editor::DocumentChangeSet::Property(
                    node, "text", px::editor::DesignerDirtyFlags::Paint),
                document);
            patches += px::editor::HasPreviewUpdate(
                plan, px::editor::PreviewUpdate::PatchProperties);
            relayouts += px::editor::HasPreviewUpdate(
                plan, px::editor::PreviewUpdate::Relayout);
            rebuilds += px::editor::HasPreviewUpdate(
                plan, px::editor::PreviewUpdate::RebuildScene);
        }
        suite.Expect(patches == 100000 && relayouts == 0 && rebuilds == 0,
                     "long Preview paint interaction stays on patch-only path",
                     "actual: " + std::to_string(patches) + " patches, " +
                         std::to_string(relayouts) + " relayouts, " +
                         std::to_string(rebuilds) + " rebuilds");
    });
    return suite.Finish();
}
