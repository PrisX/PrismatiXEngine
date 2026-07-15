#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Editor/Tools/UIDesigner/Canvas/CanvasTransform.h"
#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"
#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Editor/Tools/UIDesigner/Components/ComponentService.h"
#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
#include "Editor/Tools/UIDesigner/DesignerUiState.h"
#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

int failures = 0;
std::string_view currentTest = "Designer integration setup";

void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL [" << currentTest << "]\n"
              << "  Expected: " << message << '\n'
              << "  Actual: predicate evaluated false\n";
}

void Run(const std::string_view name, void (*test)()) {
    currentTest = name;
    try {
        test();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "UNCAUGHT [" << name << "]: " << error.what() << '\n';
    } catch (...) {
        ++failures;
        std::cerr << "UNCAUGHT [" << name << "]: unknown exception\n";
    }
}

bool Near(float left, float right) { return std::abs(left - right) < 0.001f; }

struct Fixture {
    px::test::TempDirectory temp{"designer-integration-fixture"};
    std::filesystem::path path = temp.path / "Fixture.pxscene";
    px::editor::UIDesignerSession session;
    px::Uuid root;
    px::Uuid parent = px::Uuid::Random();
    px::Uuid child = px::Uuid::Random();
    px::Uuid sibling = px::Uuid::Random();

    Fixture() {
        Check(static_cast<bool>(session.New(path, 800, 600)), "headless Designer session should create a current UI document");
        root = session.DocumentView().Root();
        auto& nodes = session.Document()->Data().nodes;
        nodes.push_back({ parent, root, "Parent", "Panel", { { "visibility", std::string("Visible") } } });
        nodes.push_back({ child, parent, "Child", "Button", { { "visibility", std::string("Visible") } } });
        nodes.push_back({ sibling, root, "Sibling", "Panel", { { "visibility", std::string("Visible") } } });
        Check(static_cast<bool>(session.DocumentView().Rebuild(*session.Document())), "DesignerDocumentView should index the fixture tree");
        session.DocumentView().SetLayoutRect(root, { 0, 0, 800, 600 });
        session.DocumentView().SetLayoutRect(parent, { 20, 20, 160, 100 });
        session.DocumentView().SetLayoutRect(child, { 40, 40, 80, 40 });
        session.DocumentView().SetLayoutRect(sibling, { 30, 30, 160, 100 });
    }
};


void TestSessionDocumentLifecycle() {
    using px::editor::DesignerCanvasGesture;
    using px::editor::DesignerDirtyFlags;

    px::test::TempDirectory temp{"designer-session-switch"};
    const auto pathA = temp.path / "A.pxscene";
    const auto pathB = temp.path / "B.pxscene";

    {
        Fixture fixture;
        auto& commands = fixture.session.Commands();
        Check(
            static_cast<bool>(commands.BeginPropertyGesture(fixture.parent, "offsets", "Move", DesignerDirtyFlags::Layout)) && static_cast<bool>(commands.UpdatePropertyGesture(px::Rect{ 8, 9, 100, 50 })),
            "single-property lifecycle test should begin an active transaction"
        );
        fixture.session.Close();
        Check(
            !fixture.session.HasDocument() && !commands.GestureActive() && fixture.session.Selection().Empty() && fixture.session.canvas.gesture == DesignerCanvasGesture::None, "Close must destroy transactions before the document and leave an idle Session"
        );
    }

    {
        Fixture fixture;
        auto& commands = fixture.session.Commands();
        const std::array<px::Uuid, 2> targets{ fixture.parent, fixture.sibling };
        Check(
            static_cast<bool>(commands.BeginPropertyGesture(targets, "visibility", "Batch visibility", DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint)) && static_cast<bool>(commands.UpdatePropertyGesture(std::string("Hidden"))),
            "multi-property lifecycle test should begin an active transaction"
        );
        fixture.session.Close();
        Check(!fixture.session.HasDocument() && !commands.GestureActive(), "Close must destroy a multi-property transaction before its document");
    }

    {
        px::editor::UIDesignerSession session;
        Check(static_cast<bool>(session.New(pathA, 640, 360)), "document A should be created for Adopt isolation");
        const px::Uuid documentA = session.Document()->DocumentId();
        const px::Uuid rootA = session.DocumentView().Root();
        session.Selection().SetScope(rootA, session.DocumentView());
        session.hoveredNode = rootA;
        session.viewport.zoom = 2.5f;
        session.viewport.gridVisible = true;
        session.timeline.currentTime = 12.0f;
        session.canvas.gesture = DesignerCanvasGesture::Move;
        Check(
            static_cast<bool>(session.Commands().BeginPropertyGesture(rootA, "offsets", "Move A", DesignerDirtyFlags::Layout)) && static_cast<bool>(session.Commands().UpdatePropertyGesture(px::Rect{ 30, 40, 200, 100 })),
            "document A should own an active gesture before replacement"
        );

        Check(static_cast<bool>(session.New(pathB, 800, 600)), "creating document B should safely Adopt over document A");
        Check(
            session.Document()->DocumentId() != documentA && session.Selection().Primary() == session.DocumentView().Root() && session.Selection().Scope().Empty() && session.hoveredNode.Empty() && Near(session.viewport.zoom, 1.0f) &&
                !session.viewport.gridVisible && Near(session.timeline.currentTime, 0.0f) && session.canvas.gesture == DesignerCanvasGesture::None && !session.Commands().GestureActive() && session.Commands().HistoryCursor() == 0,
            "Adopt must isolate selection, scope, hover, viewport, timeline, canvas and commands"
        );
    }

}


void TestCanvasInteractionAuthority() {
    Fixture fixture;
    auto& interaction = fixture.session.Interaction();
    const auto pointer = [](float x, float y, px::editor::DesignerModifierKeys modifiers = {}) {
        return px::editor::DesignerPointerEvent{ .screenPosition = { x, y }, .canvasPosition = { x, y }, .zoom = 1.0f, .button = px::editor::DesignerMouseButton::Left, .modifiers = modifiers };
    };
    const auto click = [&](const px::editor::DesignerPointerEvent& event) {
        interaction.UpdateHover(event);
        return interaction.PointerDown(event) && interaction.HasCapture() && interaction.PointerUp(event) && !interaction.HasCapture();
    };

    Check(click(pointer(35, 35)) && fixture.session.Selection().Primary() == fixture.sibling, "Canvas click should replace selection with the topmost hit");
    Check(click(pointer(20, 60, { .controlOrCommand = true })) && fixture.session.Selection().Contains(fixture.parent) && fixture.session.Selection().Contains(fixture.sibling), "modifier click should add a second ordered selection");
    Check(click(pointer(28, 60, { .controlOrCommand = true })) && !fixture.session.Selection().Contains(fixture.parent) && fixture.session.Selection().Contains(fixture.sibling), "modifier click on a selected Control should toggle it off");

    fixture.session.Selection().Replace(fixture.sibling);
    Check(click(pointer(50, 50, { .alt = true })) && fixture.session.Selection().Primary() != fixture.sibling, "Alt-click should deterministically cycle the overlapping hit stack");

    const auto marqueeStart = pointer(300, 300);
    const auto marqueeEnd = pointer(0, 0);
    interaction.UpdateHover(marqueeStart);
    Check(
        interaction.PointerDown(marqueeStart) && interaction.PointerMove(marqueeEnd) && interaction.PointerUp(marqueeEnd) && fixture.session.Selection().Size() == 2 && fixture.session.Selection().Contains(fixture.parent) &&
            fixture.session.Selection().Contains(fixture.sibling),
        "empty-space marquee should replace and canonicalize selection"
    );

    fixture.session.Selection().Replace(fixture.parent);
    const px::Rect originalLayout = *fixture.session.DocumentView().LayoutRect(fixture.parent);
    const std::size_t beforeCancel = fixture.session.Commands().HistoryCursor();
    const auto moveStart = pointer(28, 60);
    const auto moveEnd = pointer(48, 80);
    interaction.UpdateHover(moveStart);
    Check(interaction.PointerDown(moveStart) && interaction.PointerMove(moveEnd), "move gesture should be owned by CanvasInteractionController");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent, { 40, 40, 160, 100 });
    interaction.Cancel();
    const auto cancelledMove = fixture.session.Document()->ReadProperty(fixture.parent, "offsets");
    Check(
        cancelledMove && cancelledMove.Value().Type() == px::VariantType::Null && fixture.session.DocumentView().LayoutRect(fixture.parent) == std::optional<px::Rect>(originalLayout) && fixture.session.Commands().HistoryCursor() == beforeCancel &&
            !interaction.HasCapture(),
        "move cancel should restore authored data, layout, history and capture"
    );

    const std::size_t beforeMoveCommit = fixture.session.Commands().HistoryCursor();
    interaction.UpdateHover(moveStart);
    Check(
        interaction.PointerDown(moveStart) && interaction.PointerMove(pointer(38, 70)) && interaction.PointerMove(moveEnd) && interaction.PointerUp(moveEnd) && fixture.session.Commands().HistoryCursor() == beforeMoveCommit + 1,
        "many move events should commit exactly one undo entry"
    );
    const px::Variant movedOffsets = fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value();
    fixture.session.DocumentView().SetLayoutRect(fixture.parent, { 40, 40, 160, 100 });

    const auto resizeStart = pointer(40, 40);
    interaction.UpdateHover(resizeStart);
    Check(fixture.session.canvas.hoveredResizeHandle == 1 && interaction.PointerDown(resizeStart) && interaction.PointerMove(pointer(30, 30)), "resize handle should begin the authoritative resize gesture");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent, { 30, 30, 170, 110 });
    interaction.Cancel();
    Check(
        fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value() == movedOffsets && fixture.session.DocumentView().LayoutRect(fixture.parent) == std::optional<px::Rect>({ 40, 40, 160, 100 }),
        "resize cancel should restore authored offsets and the exact layout snapshot"
    );

    interaction.SetAnchorTool(true);
    const auto anchorStart = pointer(0, 0);
    interaction.UpdateHover(anchorStart);
    const px::Variant anchorOffsetsBefore = fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value();
    Check(fixture.session.canvas.hoveredAnchorHandle != 0 && interaction.PointerDown(anchorStart) && interaction.PointerMove(pointer(100, 100)), "anchor tool should own anchor hit testing and updates");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent, { 100, 100, 160, 100 });
    interaction.Cancel();
    Check(
        fixture.session.Document()->ReadProperty(fixture.parent, "anchors").Value().Type() == px::VariantType::Null && fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value() == anchorOffsetsBefore &&
            fixture.session.DocumentView().LayoutRect(fixture.parent) == std::optional<px::Rect>({ 40, 40, 160, 100 }),
        "anchor cancel should restore exact absent anchors, offsets and layout"
    );

    interaction.SetAnchorTool(false);
    const auto pivotStart = pointer(120, 90);
    interaction.UpdateHover(pivotStart);
    Check(fixture.session.canvas.hoveredPivotHandle && interaction.PointerDown(pivotStart) && interaction.PointerMove(pointer(150, 110)), "select mode should own pivot hit testing and updates");
    interaction.Cancel();
    Check(fixture.session.Document()->ReadProperty(fixture.parent, "pivot").Value().Type() == px::VariantType::Null, "pivot cancel should restore the exact authored value");

    fixture.session.Document()->WriteProperty(fixture.sibling, "offsets", px::Rect{ 30, 30, 160, 100 });
    fixture.session.Selection().Replace({ fixture.parent, fixture.sibling }, fixture.sibling);
    const px::Variant parentBefore = fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value();
    const px::Variant siblingBefore = fixture.session.Document()->ReadProperty(fixture.sibling, "offsets").Value();
    const auto groupStart = pointer(100, 100);
    interaction.UpdateHover(groupStart);
    Check(interaction.PointerDown(groupStart) && fixture.session.canvas.groupMove && interaction.PointerMove(pointer(120, 120)), "multi-selection move should use canonical selected roots");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent, { 60, 60, 160, 100 });
    fixture.session.DocumentView().SetLayoutRect(fixture.sibling, { 50, 50, 160, 100 });
    interaction.Cancel();
    Check(
        fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value() == parentBefore && fixture.session.Document()->ReadProperty(fixture.sibling, "offsets").Value() == siblingBefore &&
            fixture.session.DocumentView().LayoutRect(fixture.parent) == std::optional<px::Rect>({ 40, 40, 160, 100 }) && fixture.session.DocumentView().LayoutRect(fixture.sibling) == std::optional<px::Rect>({ 30, 30, 160, 100 }),
        "multi-move cancel should restore every property and cached rectangle"
    );

    fixture.session.DocumentView().SetChildPolicy(fixture.root, px::ui::ChildLayoutPolicy::LinearY);
    fixture.session.Selection().Replace(fixture.sibling);
    const std::size_t beforeManaged = fixture.session.Commands().HistoryCursor();
    const auto managedStart = pointer(100, 80);
    interaction.UpdateHover(managedStart);
    Check(interaction.PointerDown(managedStart) && fixture.session.canvas.gesture == px::editor::DesignerCanvasGesture::Reorder && interaction.PointerMove(pointer(100, 10)), "managed-layout drag should enter reorder preview without authored mutation");
    interaction.Cancel();
    Check(fixture.session.Commands().HistoryCursor() == beforeManaged && fixture.session.canvas.gesture == px::editor::DesignerCanvasGesture::None, "managed reorder cancel should leave history and authored order unchanged");
}

}  // namespace

int main() {
    Run("Session_DocumentSwitchCancelsTransientOwnership", TestSessionDocumentLifecycle);
    Run("Canvas_CommandHistoryLayoutAuthority", TestCanvasInteractionAuthority);
    if (failures == 0) std::cout << "Designer cross-subsystem integration scenarios passed.\n";
    return failures == 0 ? 0 : 1;
}
