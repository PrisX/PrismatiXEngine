#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Tests/TestSupport/TestHarness.h"

int main() {
    px::test::Suite suite("PreviewPlanning");
    suite.Run("OneHundredPaintUpdates_NeverRequestSceneOrLayoutRebuild", [&] {
        const auto document = px::Uuid::FromName("PrismatiX.StructuralPerf.Document");
        const auto node = px::Uuid::FromName("PrismatiX.StructuralPerf.Node");
        std::size_t propertyPatches = 0;
        std::size_t relayouts = 0;
        std::size_t sceneRebuilds = 0;
        for (int update = 0; update < 100; ++update) {
            const auto plan = px::editor::PlanPreviewUpdate(
                px::editor::DocumentChangeSet::Property(
                    node, "text", px::editor::DesignerDirtyFlags::Paint),
                document);
            propertyPatches += px::editor::HasPreviewUpdate(
                plan, px::editor::PreviewUpdate::PatchProperties);
            relayouts += px::editor::HasPreviewUpdate(
                plan, px::editor::PreviewUpdate::Relayout);
            sceneRebuilds += px::editor::HasPreviewUpdate(
                plan, px::editor::PreviewUpdate::RebuildScene);
        }
        suite.Expect(propertyPatches == 100 && relayouts == 0 && sceneRebuilds == 0,
                     "paint traffic is O(property patches), not O(scene rebuilds)");
    });

    suite.Run("InvalidationDomains_RequestOnlyTheirRequiredStructuralWork", [&] {
        const auto document = px::Uuid::FromName("PrismatiX.StructuralPerf.Document");
        const auto node = px::Uuid::FromName("PrismatiX.StructuralPerf.Node");
        const auto layout = px::editor::PlanPreviewUpdate(
            px::editor::DocumentChangeSet::Property(
                node, "offsets", px::editor::DesignerDirtyFlags::Layout),
            document);
        const auto structure = px::editor::PlanPreviewUpdate(
            px::editor::DocumentChangeSet::Structure(node), document);
        suite.Expect(px::editor::HasPreviewUpdate(
                         layout, px::editor::PreviewUpdate::Relayout) &&
                         !px::editor::HasPreviewUpdate(
                             layout, px::editor::PreviewUpdate::RebuildScene) &&
                         px::editor::HasPreviewUpdate(
                             structure, px::editor::PreviewUpdate::RebuildScene),
                     "layout and structure retain distinct bounded work plans");
    });
    return suite.Finish();
}
