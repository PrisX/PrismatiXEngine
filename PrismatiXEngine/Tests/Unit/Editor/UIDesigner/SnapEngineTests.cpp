#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Tests/TestSupport/DesignerFixture.h"

#include <cmath>
#include <vector>

namespace {

bool Near(float left, float right) {
    return std::abs(left - right) < 0.001f;
}

}  // namespace

int main() {
    px::test::Suite suite("SnapEngine");

    suite.Run("GridMoveAndResize_SnapOnlyEnabledEdges", [&] {
        px::test::DesignerFixture fixture(suite);
        const std::vector<px::Uuid> ignored{fixture.parent};
        const auto moved = px::editor::SnapEngine{}.Snap(
            {.movingRect = {7, 9, 31, 27},
             .mode = px::editor::SnapMode::Move,
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600},
             .alignmentEnabled = false,
             .gridEnabled = true,
             .gridSize = 16},
            fixture.session.DocumentView());
        suite.Expect(moved.rect == px::Rect{0, 16, 31, 27},
                     "grid move rounds the origin while preserving size");
        const auto resized = px::editor::SnapEngine{}.Snap(
            {.movingRect = {3, 5, 31, 27},
             .mode = px::editor::SnapMode::Resize,
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600},
             .alignmentEnabled = false,
             .gridEnabled = true,
             .gridSize = 16,
             .snapLeft = false,
             .snapRight = true,
             .snapTop = false,
             .snapBottom = false},
            fixture.session.DocumentView());
        suite.Expect(resized.rect == px::Rect{3, 5, 29, 27} && resized.guides.empty(),
                     "grid resize changes only the active right edge");
    });

    suite.Run("AlignmentCandidates_SnapEdgesCentersAndUserGuides", [&] {
        px::test::DesignerFixture fixture(suite);
        const std::vector<px::Uuid> ignored{fixture.parent};
        const auto siblingEdge = px::editor::SnapEngine{}.Snap(
            {.movingRect = {194, 200, 40, 30},
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600}},
            fixture.session.DocumentView());
        suite.Expect(Near(siblingEdge.rect.x, 190) && !siblingEdge.guides.empty() &&
                         siblingEdge.guides.front().kind ==
                             px::editor::SnapGuideKind::Sibling,
                     "sibling right edge snaps the moving left edge");

        const auto center = px::editor::SnapEngine{}.Snap(
            {.movingRect = {376, 286, 50, 30},
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600}},
            fixture.session.DocumentView());
        suite.Expect(Near(center.rect.x, 375) && Near(center.rect.y, 285),
                     "canvas center candidates snap both axes");

        const std::vector<px::editor::UserSnapGuide> guides{
            {px::editor::SnapGuideOrientation::Vertical, 250, false}};
        const auto user = px::editor::SnapEngine{}.Snap(
            {.movingRect = {247, 300, 20, 20},
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600},
             .userGuides = guides},
            fixture.session.DocumentView());
        suite.Expect(Near(user.rect.x, 250) && !user.guides.empty() &&
                         user.guides.front().kind == px::editor::SnapGuideKind::User,
                     "user guide is a first-class alignment candidate");
    });

    suite.Run("ThresholdZoomDisabledAndPriority_AreDeterministic", [&] {
        px::test::DesignerFixture fixture(suite);
        const std::vector<px::Uuid> ignored{fixture.parent};
        const auto outside = px::editor::SnapEngine{}.Snap(
            {.movingRect = {195, 300, 20, 20},
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600}},
            fixture.session.DocumentView());
        const auto zoomed = px::editor::SnapEngine{}.Snap(
            {.movingRect = {195, 300, 20, 20},
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .zoom = 2,
             .canvasRect = {0, 0, 800, 600}},
            fixture.session.DocumentView());
        suite.Expect(Near(outside.rect.x, 190) && Near(zoomed.rect.x, 195),
                     "six-screen-pixel threshold scales inversely with zoom");

        const auto disabled = px::editor::SnapEngine{}.Snap(
            {.movingRect = {2, 3, 20, 20},
             .parent = fixture.root,
             .ignoredNodes = ignored,
             .canvasRect = {0, 0, 800, 600},
             .alignmentEnabled = false},
            fixture.session.DocumentView());
        suite.Expect(disabled.rect == px::Rect{2, 3, 20, 20} &&
                         disabled.guides.empty(),
                     "disabled snapping preserves the exact request");

        const std::vector<px::editor::UserSnapGuide> tied{
            {px::editor::SnapGuideOrientation::Vertical, 30, false}};
        const std::vector<px::Uuid> ignoreOnlyParent{fixture.parent};
        const auto priority = px::editor::SnapEngine{}.Snap(
            {.movingRect = {28, 200, 20, 20},
             .parent = fixture.root,
             .ignoredNodes = ignoreOnlyParent,
             .canvasRect = {0, 0, 800, 600},
             .userGuides = tied},
            fixture.session.DocumentView());
        suite.Expect(!priority.guides.empty() &&
                         priority.guides.front().kind == px::editor::SnapGuideKind::User,
                     "equal-distance tie priority is User, Sibling, Parent, then Canvas");
    });

    suite.Run("NormalizedAnchors_UseCanonicalTargetsAndClamp", [&] {
        px::editor::SnapEngine engine;
        suite.Expect(Near(engine.SnapNormalized(0.02f), 0.0f) &&
                         Near(engine.SnapNormalized(0.52f), 0.5f) &&
                         Near(engine.SnapNormalized(0.98f), 1.0f) &&
                         Near(engine.SnapNormalized(-0.4f), 0.0f) &&
                         Near(engine.SnapNormalized(1.4f), 1.0f),
                     "normalized snap targets 0/0.5/1 and clamps out-of-range values");
    });

    return suite.Finish();
}
