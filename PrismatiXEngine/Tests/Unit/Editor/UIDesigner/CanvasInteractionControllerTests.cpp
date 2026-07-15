#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Tests/TestSupport/DesignerFixture.h"

namespace {

px::editor::DesignerPointerEvent Pointer(
    float x, float y, px::editor::DesignerModifierKeys modifiers = {}) {
    return {.screenPosition = {x, y},
            .canvasPosition = {x, y},
            .zoom = 1.0f,
            .button = px::editor::DesignerMouseButton::Left,
            .modifiers = modifiers};
}

bool Click(px::editor::CanvasInteractionController& interaction,
           const px::editor::DesignerPointerEvent& event) {
    interaction.UpdateHover(event);
    return interaction.PointerDown(event) && interaction.HasCapture() &&
           interaction.PointerUp(event) && !interaction.HasCapture();
}

}  // namespace

int main() {
    px::test::Suite suite("CanvasInteractionController");

    suite.Run("ClickToggleAltCycleAndScope_UseOneSelectionPath", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& interaction = fixture.session.Interaction();
        suite.Expect(Click(interaction, Pointer(35, 35)) &&
                         fixture.session.Selection().Primary() == fixture.sibling,
                     "plain click replaces selection with the topmost hit");
        suite.Expect(Click(interaction, Pointer(25, 60, {.controlOrCommand = true})) &&
                         fixture.session.Selection().Contains(fixture.parent) &&
                         fixture.session.Selection().Contains(fixture.sibling),
                     "Ctrl/Cmd click adds an ordered second selection");
        suite.Expect(Click(interaction, Pointer(25, 60, {.controlOrCommand = true})) &&
                         !fixture.session.Selection().Contains(fixture.parent) &&
                         fixture.session.Selection().Contains(fixture.sibling),
                     "Ctrl/Cmd click toggles an existing selection off");
        suite.Expect(Click(interaction, Pointer(50, 50, {.alt = true})) &&
                         fixture.session.Selection().Primary() != fixture.sibling,
                     "Alt-click cycles the overlap stack deterministically");

        fixture.session.Selection().SetScope(fixture.parent,
                                             fixture.session.DocumentView());
        suite.Expect(Click(interaction, Pointer(50, 50)) &&
                         fixture.session.Selection().Primary() == fixture.child,
                     "scoped click cannot select an overlapping node outside the scope");
    });

    suite.Run("MarqueeReplaceAndAdditive_CanonicalizeObservableSelection", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& interaction = fixture.session.Interaction();
        auto start = Pointer(300, 300);
        auto end = Pointer(0, 0);
        interaction.UpdateHover(start);
        suite.Expect(interaction.PointerDown(start) && interaction.PointerMove(end) &&
                         interaction.PointerUp(end) &&
                         fixture.session.Selection().Size() == 2 &&
                         fixture.session.Selection().Contains(fixture.parent) &&
                         fixture.session.Selection().Contains(fixture.sibling),
                     "marquee replace selects intersecting transform roots");

        fixture.session.Selection().Replace(fixture.child);
        start = Pointer(250, 150, {.controlOrCommand = true});
        end = Pointer(181, 120, {.controlOrCommand = true});
        interaction.UpdateHover(start);
        suite.Expect(interaction.PointerDown(start) && interaction.PointerMove(end) &&
                         interaction.PointerUp(end) &&
                         fixture.session.Selection().Contains(fixture.child) &&
                         fixture.session.Selection().Contains(fixture.sibling),
                     "modifier marquee adds intersecting nodes without clearing selection");
    });

    suite.Run("MoveSnapCommitAndCancel_KeepHistoryLayoutAndGuidesConsistent", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& interaction = fixture.session.Interaction();
        fixture.session.Selection().Replace(fixture.parent);
        const auto original = fixture.session.DocumentView().LayoutRect(fixture.parent);
        const auto before = fixture.session.Commands().HistoryCursor();
        auto start = Pointer(25, 60);
        interaction.UpdateHover(start);
        suite.Expect(interaction.PointerDown(start) &&
                         interaction.PointerMove(Pointer(191, 60)) &&
                         !fixture.session.canvas.snapGuides.empty(),
                     "move updates expose winning snap guides through Session canvas state");
        interaction.Cancel();
        suite.Expect(!interaction.HasCapture() &&
                         fixture.session.canvas.gesture ==
                             px::editor::DesignerCanvasGesture::None &&
                         fixture.session.canvas.snapGuides.empty() &&
                         fixture.session.canvas.snapDistances.empty() &&
                         fixture.session.Commands().HistoryCursor() == before &&
                         fixture.session.DocumentView().LayoutRect(fixture.parent) == original &&
                         fixture.session.Document()->ReadProperty(
                             fixture.parent, "offsets").Value().Type() ==
                             px::VariantType::Null,
                     "Esc/Cancel restores authored value, layout cache, capture, and history");

        interaction.UpdateHover(start);
        suite.Expect(interaction.PointerDown(start) &&
                         interaction.PointerMove(Pointer(191, 60)) &&
                         !fixture.session.canvas.snapGuides.empty() &&
                         interaction.PointerUp(Pointer(191, 60)) &&
                         fixture.session.canvas.snapGuides.empty() &&
                         fixture.session.canvas.snapDistances.empty() &&
                         fixture.session.canvas.gesture ==
                             px::editor::DesignerCanvasGesture::None &&
                         fixture.session.Commands().HistoryCursor() == before + 1,
                     "move commit clears transient guides and creates exactly one undo entry");
    });

    suite.Run("ResizeAnchorPivotAndMultiMove_CancelExactAuthoredState", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& interaction = fixture.session.Interaction();
        fixture.session.Selection().Replace(fixture.parent);

        auto resize = Pointer(20, 20);
        interaction.UpdateHover(resize);
        suite.Expect(fixture.session.canvas.hoveredResizeHandle == 1 &&
                         interaction.PointerDown(resize) &&
                         interaction.PointerMove(Pointer(10, 10)),
                     "resize handle starts the authoritative resize gesture");
        interaction.Cancel();
        suite.Expect(fixture.session.Document()->ReadProperty(
                         fixture.parent, "offsets").Value().Type() == px::VariantType::Null,
                     "resize cancel restores exact absent offsets");

        interaction.SetAnchorTool(true);
        auto anchor = Pointer(0, 0);
        interaction.UpdateHover(anchor);
        suite.Expect(fixture.session.canvas.hoveredAnchorHandle != 0 &&
                         interaction.PointerDown(anchor) &&
                         interaction.PointerMove(Pointer(100, 100)),
                     "anchor tool owns anchor updates");
        interaction.Cancel();
        suite.Expect(fixture.session.Document()->ReadProperty(
                         fixture.parent, "anchors").Value().Type() == px::VariantType::Null &&
                         fixture.session.Document()->ReadProperty(
                             fixture.parent, "offsets").Value().Type() ==
                             px::VariantType::Null,
                     "anchor cancel restores anchors and companion offsets exactly");

        interaction.SetAnchorTool(false);
        auto pivot = Pointer(100, 70);
        interaction.UpdateHover(pivot);
        suite.Expect(fixture.session.canvas.hoveredPivotHandle &&
                         interaction.PointerDown(pivot) &&
                         interaction.PointerMove(Pointer(120, 80)),
                     "select tool owns pivot updates");
        interaction.Cancel();
        suite.Expect(fixture.session.Document()->ReadProperty(
                         fixture.parent, "pivot").Value().Type() == px::VariantType::Null,
                     "pivot cancel restores exact absent pivot");

        fixture.session.Document()->WriteProperty(
            fixture.parent, "offsets", px::Rect{20, 20, 160, 100});
        fixture.session.Document()->WriteProperty(
            fixture.sibling, "offsets", px::Rect{30, 30, 160, 100});
        fixture.session.Selection().Replace({fixture.parent, fixture.sibling},
                                            fixture.sibling);
        const auto parentBefore = fixture.session.Document()->ReadProperty(
            fixture.parent, "offsets").Value();
        const auto siblingBefore = fixture.session.Document()->ReadProperty(
            fixture.sibling, "offsets").Value();
        auto group = Pointer(100, 100);
        interaction.UpdateHover(group);
        suite.Expect(interaction.PointerDown(group) && fixture.session.canvas.groupMove &&
                         interaction.PointerMove(Pointer(120, 120)),
                     "multi-selection move uses canonical selected roots");
        interaction.Cancel();
        suite.Expect(fixture.session.Document()->ReadProperty(
                         fixture.parent, "offsets").Value() == parentBefore &&
                         fixture.session.Document()->ReadProperty(
                             fixture.sibling, "offsets").Value() == siblingBefore,
                     "multi-move cancel restores every authored value");
    });

    suite.Run("ManagedContainerDrag_CommitsReorderOrCancelsWithoutMutation", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& interaction = fixture.session.Interaction();
        fixture.session.DocumentView().SetChildPolicy(
            fixture.root, px::ui::ChildLayoutPolicy::LinearY);
        fixture.session.Selection().Replace(fixture.sibling);
        const auto before = fixture.session.Commands().HistoryCursor();
        auto start = Pointer(100, 80);
        interaction.UpdateHover(start);
        suite.Expect(interaction.PointerDown(start) &&
                         fixture.session.canvas.gesture ==
                             px::editor::DesignerCanvasGesture::Reorder &&
                         interaction.PointerMove(Pointer(100, 10)),
                     "managed drag enters reorder preview without transient authored writes");
        interaction.Cancel();
        suite.Expect(fixture.session.Commands().HistoryCursor() == before &&
                         fixture.session.DocumentView().ChildIndex(fixture.sibling) == 1,
                     "managed reorder cancel preserves history and sibling order");

        interaction.UpdateHover(start);
        suite.Expect(interaction.PointerDown(start) &&
                         interaction.PointerMove(Pointer(100, 10)) &&
                         interaction.PointerUp(Pointer(100, 10)) &&
                         fixture.session.DocumentView().ChildIndex(fixture.sibling) == 0 &&
                         fixture.session.Commands().HistoryCursor() == before + 1,
                     "managed reorder commit creates one structural undo entry");
    });

    suite.Run("LockedHiddenCollapsedNodes_DoNotStartCanvasGestures", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& interaction = fixture.session.Interaction();
        fixture.session.Document()->Find(fixture.sibling)->properties["editorLocked"] = true;
        fixture.session.Document()->Find(fixture.child)->properties["visibility"] =
            std::string("Hidden");
        fixture.session.Document()->Find(fixture.parent)->properties["visibility"] =
            std::string("Collapsed");
        suite.Expect(Click(interaction, Pointer(50, 50)) &&
                         fixture.session.Selection().Empty() &&
                         fixture.session.canvas.gesture ==
                             px::editor::DesignerCanvasGesture::None,
                     "non-selectable nodes leave click as blank-space selection behavior");
    });

    return suite.Finish();
}
