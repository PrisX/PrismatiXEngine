#include "Tests/TestSupport/DesignerFixture.h"

#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"

#include <array>

namespace {

bool CleanState(const px::editor::UIDesignerSession& session) {
    return session.Selection().Scope().Empty() && session.hoveredNode.Empty() &&
           session.viewport.zoom == 1.0f && !session.viewport.gridVisible &&
           session.timeline.currentTime == 0.0f && !session.timeline.playing &&
           session.behaviorGraph.selectedNode.Empty() &&
           session.animationMachine.selectedState.Empty() &&
           session.canvas.gesture == px::editor::DesignerCanvasGesture::None &&
           session.canvas.snapGuides.empty() && session.clipboardSubtree.empty() &&
           session.selectedSignal.empty() && session.inspectorSearch.empty() &&
           !session.Commands().GestureActive() && session.HistoryCursor() == 0;
}

void DirtyEveryDocumentState(px::test::DesignerFixture& fixture) {
    auto& session = fixture.session;
    session.Selection().SetScope(fixture.parent, session.DocumentView());
    session.Selection().Replace(fixture.child);
    session.hoveredNode = fixture.child;
    session.viewport.zoom = 2.5f;
    session.viewport.gridVisible = true;
    session.timeline.currentTime = 12.0f;
    session.timeline.playing = true;
    session.behaviorGraph.selectedNode = fixture.child;
    session.animationMachine.selectedState = fixture.parent;
    session.canvas.gesture = px::editor::DesignerCanvasGesture::Move;
    session.canvas.snapGuides.push_back({});
    session.clipboardSubtree["name"] = std::string("clipboard");
    session.selectedSignal = "activated";
    session.inspectorSearch = "text";
}

}  // namespace

int main() {
    px::test::Suite suite("UIDesignerSession");

    suite.Run("NewDuringActiveSingleGesture_ResetsAllDocumentState", [&] {
        px::test::DesignerFixture fixture(suite);
        DirtyEveryDocumentState(fixture);
        const auto oldDocument = fixture.session.Document()->DocumentId();
        suite.Expect(static_cast<bool>(fixture.session.Commands().BeginPropertyGesture(
                         fixture.child, "offsets", "Move A",
                         px::editor::DesignerDirtyFlags::Layout)) &&
                         static_cast<bool>(fixture.session.Commands().UpdatePropertyGesture(
                             px::Rect{30, 40, 80, 40})),
                     "document A owns an active transaction before replacement");
        const auto replacement = fixture.path.parent_path() / "prismatix-session-new-b.pxscene";
        suite.Expect(static_cast<bool>(fixture.session.New(replacement, 1024, 768)) &&
                         fixture.session.Document()->DocumentId() != oldDocument &&
                         fixture.session.Selection().Primary() ==
                             fixture.session.DocumentView().Root() &&
                         CleanState(fixture.session),
                     "New cancels old references and resets selection, hover, scope, viewport, "
                     "timeline, canvas, transaction, and clipboard state");
        std::error_code error;
        std::filesystem::remove(replacement, error);
    });

    suite.Run("OpenDuringActiveMultiGesture_AdoptsCleanSavedDocument", [&] {
        px::test::DesignerFixture fixture(suite);
        const auto saved = fixture.path.parent_path() / "prismatix-session-open-b.pxscene";
        {
            px::editor::UIDesignerSession writer;
            suite.Expect(static_cast<bool>(writer.New(saved, 640, 360)) &&
                             static_cast<bool>(writer.Document()->Save()),
                         "saved document B is a valid Open target");
        }
        DirtyEveryDocumentState(fixture);
        const std::array<px::Uuid, 2> targets{fixture.parent, fixture.sibling};
        suite.Expect(static_cast<bool>(fixture.session.Commands().BeginPropertyGesture(
                         targets, "visibility", "Batch visibility",
                         px::editor::DesignerDirtyFlags::Layout)) &&
                         static_cast<bool>(fixture.session.Commands().UpdatePropertyGesture(
                             std::string("Hidden"))),
                     "document A owns an active multi-property transaction");
        suite.Expect(static_cast<bool>(fixture.session.Open(saved)) &&
                         fixture.session.Selection().Primary() ==
                             fixture.session.DocumentView().Root() &&
                         CleanState(fixture.session),
                     "Open resets every per-document state and transaction safely");
        std::error_code error;
        std::filesystem::remove(saved, error);
    });

    suite.Run("CloseDuringGestures_DestroysTransactionsBeforeDocument", [&] {
        {
            px::test::DesignerFixture fixture(suite);
            suite.Expect(static_cast<bool>(fixture.session.Commands().BeginPropertyGesture(
                             fixture.parent, "offsets", "Move",
                             px::editor::DesignerDirtyFlags::Layout)) &&
                             static_cast<bool>(fixture.session.Commands().UpdatePropertyGesture(
                                 px::Rect{10, 20, 160, 100})),
                         "single gesture starts before Close");
            fixture.session.Close();
            suite.Expect(!fixture.session.HasDocument() &&
                             !fixture.session.Commands().GestureActive() &&
                             fixture.session.Selection().Empty() && CleanState(fixture.session),
                         "Close leaves no document, transaction, gesture, or leaked view state");
        }
        {
            px::test::DesignerFixture fixture(suite);
            const std::array<px::Uuid, 2> targets{fixture.parent, fixture.sibling};
            suite.Expect(static_cast<bool>(fixture.session.Commands().BeginPropertyGesture(
                             targets, "visibility", "Batch",
                             px::editor::DesignerDirtyFlags::Layout)) &&
                             static_cast<bool>(fixture.session.Commands().UpdatePropertyGesture(
                                 std::string("Hidden"))),
                         "multi gesture starts before Close");
            fixture.session.Close();
            suite.Expect(!fixture.session.HasDocument() &&
                             !fixture.session.Commands().GestureActive() &&
                             CleanState(fixture.session),
                         "Close safely clears a multi-property transaction");
        }
    });

    return suite.Finish();
}
