#include "Editor/Tools/UIDesigner/Canvas/CanvasTransform.h"
#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"
#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
#include "Editor/Tools/UIDesigner/Components/ComponentService.h"
#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
#include "Editor/Tools/UIDesigner/DesignerUiState.h"
#include "Engine/UI/UITypeRegistry.h"

#include <cmath>
#include <array>
#include <filesystem>
#include <iostream>
#include <span>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool Near(float left, float right) { return std::abs(left - right) < 0.001f; }

struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("prismatix-designer-core-" + px::Uuid::Random().ToString() +
                                  ".pxscene");
    px::editor::UIDesignerSession session;
    px::Uuid root;
    px::Uuid parent = px::Uuid::Random();
    px::Uuid child = px::Uuid::Random();
    px::Uuid sibling = px::Uuid::Random();

    Fixture() {
        Check(static_cast<bool>(session.New(path, 800, 600)),
              "headless Designer session should create a current UI document");
        root = session.DocumentView().Root();
        auto& nodes = session.Document()->Data().nodes;
        nodes.push_back({parent, root, "Parent", "Panel", {{"visibility", std::string("Visible")}}});
        nodes.push_back({child, parent, "Child", "Button", {{"visibility", std::string("Visible")}}});
        nodes.push_back({sibling, root, "Sibling", "Panel", {{"visibility", std::string("Visible")}}});
        Check(static_cast<bool>(session.DocumentView().Rebuild(*session.Document())),
              "DesignerDocumentView should index the fixture tree");
        session.DocumentView().SetLayoutRect(root, {0, 0, 800, 600});
        session.DocumentView().SetLayoutRect(parent, {20, 20, 160, 100});
        session.DocumentView().SetLayoutRect(child, {40, 40, 80, 40});
        session.DocumentView().SetLayoutRect(sibling, {30, 30, 160, 100});
    }
};

void TestSelectionAndDocumentView() {
    Fixture fixture;
    auto& selection = fixture.session.Selection();
    selection.Replace({fixture.parent, fixture.child, fixture.sibling}, fixture.child);
    selection.Canonicalize(fixture.session.DocumentView());
    Check(selection.Size() == 2 && selection.Contains(fixture.parent) &&
              selection.Contains(fixture.sibling) && selection.Primary() == fixture.parent,
          "selection canonicalization should retain ordered transform roots and promote primary");
    Check(selection.SetScope(fixture.parent, fixture.session.DocumentView()) &&
              selection.Size() == 1 && selection.Primary() == fixture.parent,
          "selection scope should prune nodes outside the active subtree");
    Check(fixture.session.DocumentView().Parent(fixture.child) == fixture.parent &&
              fixture.session.DocumentView().IsAncestor(fixture.root, fixture.child) &&
              fixture.session.DocumentView().ChildIndex(fixture.sibling) == 1,
          "document view should be the authoritative parent/child/order index");
}

void TestDesignerUiPresentationStates() {
    px::editor::UIDesignerSession empty;
    Check(px::editor::CaptureDesignerUiState(empty).selection ==
              px::editor::DesignerSelectionPresentation::NoDocument,
          "UI presentation state should distinguish no document");

    Fixture fixture;
    fixture.session.Selection().Clear();
    Check(px::editor::CaptureDesignerUiState(fixture.session).selection ==
              px::editor::DesignerSelectionPresentation::None,
          "UI presentation state should distinguish no selection");
    fixture.session.Selection().Replace(fixture.parent);
    auto single = px::editor::CaptureDesignerUiState(fixture.session);
    Check(single.selection == px::editor::DesignerSelectionPresentation::Single &&
              !single.positionManaged,
          "UI presentation state should identify a free-layout single selection");
    fixture.session.Selection().Replace({fixture.parent, fixture.sibling}, fixture.sibling);
    Check(px::editor::CaptureDesignerUiState(fixture.session).selection ==
              px::editor::DesignerSelectionPresentation::Multiple,
          "UI presentation state should identify mixed/multi-selection mode");
    fixture.session.DocumentView().SetChildPolicy(
        fixture.root, px::ui::ChildLayoutPolicy::LinearY);
    auto managed = px::editor::CaptureDesignerUiState(fixture.session);
    Check(managed.positionManaged &&
              managed.parentPolicy == px::ui::ChildLayoutPolicy::LinearY,
          "UI presentation state should expose the controlling managed-layout policy");
}

void TestDesignerWorkspaceGeometrySnapshots() {
    using px::editor::CalculateDesignerWorkspaceGeometry;
    using px::editor::DesignerWorkspaceGeometryInput;

    const auto defaultWorkspace = CalculateDesignerWorkspaceGeometry(
        {.width = 1600.0f, .height = 800.0f});
    Check(!defaultWorkspace.compact && defaultWorkspace.showLeft &&
              defaultWorkspace.showRight && !defaultWorkspace.showBottom &&
              defaultWorkspace.left.width == 260.0f &&
              defaultWorkspace.canvas == px::editor::DesignerUiRect{260, 0, 1000, 800} &&
              defaultWorkspace.inspector == px::editor::DesignerUiRect{1260, 0, 340, 800},
          "default Designer geometry should preserve left/canvas/Inspector bounds");

    const auto drawerWorkspace = CalculateDesignerWorkspaceGeometry(
        {.width = 1600.0f, .height = 800.0f, .bottomPanelVisible = true});
    Check(drawerWorkspace.showBottom &&
              drawerWorkspace.main.height == 554.0f &&
              drawerWorkspace.bottomSplitter ==
                  px::editor::DesignerUiRect{0, 554, 1600, 6} &&
              drawerWorkspace.bottomDrawer ==
                  px::editor::DesignerUiRect{0, 560, 1600, 240},
          "open drawer geometry should reserve deterministic splitter and content bounds");

    const auto compactWorkspace = CalculateDesignerWorkspaceGeometry(
        {.width = 900.0f, .height = 600.0f});
    Check(compactWorkspace.compact && !compactWorkspace.showLeft &&
              compactWorkspace.showRight && compactWorkspace.canvas.width == 560.0f,
          "compact Designer geometry should collapse navigation but retain a usable canvas");

    const auto narrowWorkspace = CalculateDesignerWorkspaceGeometry(
        {.width = 740.0f, .height = 600.0f});
    Check(!narrowWorkspace.showLeft && !narrowWorkspace.showRight &&
              narrowWorkspace.canvas == px::editor::DesignerUiRect{0, 0, 740, 600},
          "narrow Designer geometry should collapse both side panels before crushing canvas");

    const auto shortTimeline = CalculateDesignerWorkspaceGeometry(
        {.width = 1200.0f, .height = 420.0f, .bottomPanelHeight = 240.0f,
         .clipTimeline = true});
    Check(shortTimeline.showBottom && shortTimeline.main.height == 240.0f &&
              shortTimeline.bottomDrawer.height == 174.0f,
          "short timeline geometry should retain the minimum main workspace height");
}

void TestPreviewDpiAndCjkSmokeState() {
    px::editor::PreviewFixture fixture;
    Check(fixture.Context().locale == "zh-TW" &&
              static_cast<bool>(fixture.SelectDevice("high-dpi")) &&
              fixture.Context().dpiScale == 2.0f &&
              static_cast<bool>(fixture.SetUIScale(1.5f)) &&
              fixture.Context().uiScale == 1.5f,
          "preview state should support Traditional Chinese at 150% UI and 200% DPI");
    const auto text = fixture.Read("game.dialogue.text");
    const auto* value = text ? text.Value().TryGet<std::string>() : nullptr;
    Check(value && value->find("預覽文字") != std::string::npos,
          "CJK preview fixture text should survive the typed ViewModel path");
}

void TestCommandsAndTransactions() {
    Fixture fixture;
    auto& commands = fixture.session.Commands();
    int changeCount = 0;
    bool previewSawRollback = false;
    std::vector<px::editor::DocumentChangeSet> changes;
    fixture.session.SetChangeListener([&](const px::editor::DocumentChangeSet& change) {
        ++changeCount;
        changes.push_back(change);
        if (change.properties == std::vector<px::editor::DocumentPropertyChange>{
                                     {fixture.parent, "offsets"}}) {
            const auto value = fixture.session.Document()->ReadProperty(
                fixture.parent, "offsets");
            if (value && value.Value().Type() == px::VariantType::Null)
                previewSawRollback = true;
        }
    });

    Check(static_cast<bool>(commands.Rename(fixture.parent, "Renamed")) &&
              fixture.session.Document()->Find(fixture.parent)->name == "Renamed",
          "rename should execute through DesignerCommandService");
    Check(changes.back().properties == std::vector<px::editor::DocumentPropertyChange>{
              {fixture.parent, "$name"}} &&
              changes.back().nodes == std::vector<px::Uuid>{fixture.parent} &&
              changes.back().dirty == px::editor::DesignerDirtyFlags::Paint,
          "property commands should publish exact node, property, and dirty metadata");
    Check(static_cast<bool>(commands.Undo()) &&
              fixture.session.Document()->Find(fixture.parent)->name == "Parent",
          "command undo should restore the exact prior value");
    Check(changes.back().historyNavigation &&
              changes.back().properties == std::vector<px::editor::DocumentPropertyChange>{
                  {fixture.parent, "$name"}},
          "undo should replay the original exact ChangeSet");
    Check(static_cast<bool>(commands.Redo()) &&
              fixture.session.Document()->Find(fixture.parent)->name == "Renamed",
          "command redo should replay the edit");

    const px::Rect moved{10, 20, 110, 70};
    const std::size_t beforeCancel = commands.HistoryCursor();
    const px::Rect originalLayout =
        *fixture.session.DocumentView().LayoutRect(fixture.parent);
    Check(static_cast<bool>(commands.BeginPropertyGesture(
              fixture.parent, "offsets", "Move", px::editor::DesignerDirtyFlags::Layout)) &&
              static_cast<bool>(commands.UpdatePropertyGesture(moved)),
          "a property gesture should update through the command boundary");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent, moved);
    Check(static_cast<bool>(commands.CancelPropertyGesture()),
          "a property gesture should cancel through the command boundary");
    const auto cancelled = fixture.session.Document()->ReadProperty(fixture.parent, "offsets");
    Check(cancelled && cancelled.Value().Type() == px::VariantType::Null,
          "gesture cancellation should restore an absent property exactly");
    Check(commands.HistoryCursor() == beforeCancel,
          "gesture cancellation should not create a history entry");
    Check(fixture.session.DocumentView().LayoutRect(fixture.parent) ==
              std::optional<px::Rect>(originalLayout) && previewSawRollback,
          "gesture cancellation should restore layout before publishing preview rollback");

    const std::size_t beforeCommit = commands.HistoryCursor();
    Check(static_cast<bool>(commands.BeginPropertyGesture(
              fixture.parent, "offsets", "Move", px::editor::DesignerDirtyFlags::Layout)) &&
              static_cast<bool>(commands.UpdatePropertyGesture(moved)) &&
              static_cast<bool>(commands.CommitPropertyGesture()),
          "a committed gesture should produce a history entry");
    Check(commands.HistoryCursor() == beforeCommit + 1,
          "one gesture should create exactly one history entry");
    Check(static_cast<bool>(commands.Undo()) &&
              fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value().Type() ==
                  px::VariantType::Null &&
              changeCount >= 5,
          "one undo should revert the whole gesture and publish dirty notifications");

    const std::array<px::Uuid, 2> multiTargets{fixture.parent, fixture.sibling};
    const std::size_t beforeMulti = commands.HistoryCursor();
    Check(static_cast<bool>(commands.BeginPropertyGesture(
              multiTargets, "visibility", "Batch visibility",
              px::editor::DesignerDirtyFlags::Layout | px::editor::DesignerDirtyFlags::Paint)) &&
              static_cast<bool>(commands.UpdatePropertyGesture(std::string("Hidden"))) &&
              static_cast<bool>(commands.CommitPropertyGesture()) &&
              commands.HistoryCursor() == beforeMulti + 1 &&
              changes.back().properties.size() == 2,
          "multi-property gestures should commit once with one exact property per target");

    auto failing = std::make_unique<px::editor::CompositeEditCommand>("Atomic failure");
    failing->Add(std::make_unique<px::editor::PropertyChangeCommand>(
        "Temporary rename", fixture.parent, "$name", std::string("Renamed"),
        std::string("MustRollBack")));
    failing->Add(std::make_unique<px::editor::PropertyChangeCommand>(
        "Invalid write", px::Uuid::Random(), "$name", px::Variant{}, std::string("Fail")));
    const auto failure = commands.Execute(
        std::move(failing), px::editor::DocumentChangeSet::Property(
                                fixture.parent, "$name",
                                px::editor::DesignerDirtyFlags::Paint));
    Check(!failure && fixture.session.Document()->Find(fixture.parent)->name == "Renamed",
          "failed composite commands should roll back every already-applied child");

    fixture.session.Selection().Replace(fixture.parent);
    Check(static_cast<bool>(commands.DeleteSelection()) &&
              !fixture.session.DocumentView().Contains(fixture.parent) &&
              fixture.session.Selection().Primary() == fixture.root &&
              static_cast<bool>(commands.Undo()) &&
              fixture.session.DocumentView().Contains(fixture.parent),
          "structural command completion and undo should keep indexes and selection valid");
}

void TestSessionDocumentLifecycle() {
    using px::editor::DesignerCanvasGesture;
    using px::editor::DesignerDirtyFlags;

    const auto pathA = std::filesystem::temp_directory_path() / "prismatix-session-a.pxscene";
    const auto pathB = std::filesystem::temp_directory_path() / "prismatix-session-b.pxscene";

    {
        Fixture fixture;
        auto& commands = fixture.session.Commands();
        Check(static_cast<bool>(commands.BeginPropertyGesture(
                  fixture.parent, "offsets", "Move", DesignerDirtyFlags::Layout)) &&
                  static_cast<bool>(commands.UpdatePropertyGesture(px::Rect{8, 9, 100, 50})),
              "single-property lifecycle test should begin an active transaction");
        fixture.session.Close();
        Check(!fixture.session.HasDocument() && !commands.GestureActive() &&
                  fixture.session.Selection().Empty() &&
                  fixture.session.canvas.gesture == DesignerCanvasGesture::None,
              "Close must destroy transactions before the document and leave an idle Session");
    }

    {
        Fixture fixture;
        auto& commands = fixture.session.Commands();
        const std::array<px::Uuid, 2> targets{fixture.parent, fixture.sibling};
        Check(static_cast<bool>(commands.BeginPropertyGesture(
                  targets, "visibility", "Batch visibility",
                  DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint)) &&
                  static_cast<bool>(commands.UpdatePropertyGesture(std::string("Hidden"))),
              "multi-property lifecycle test should begin an active transaction");
        fixture.session.Close();
        Check(!fixture.session.HasDocument() && !commands.GestureActive(),
              "Close must destroy a multi-property transaction before its document");
    }

    {
        px::editor::UIDesignerSession session;
        Check(static_cast<bool>(session.New(pathA, 640, 360)),
              "document A should be created for Adopt isolation");
        const px::Uuid documentA = session.Document()->DocumentId();
        const px::Uuid rootA = session.DocumentView().Root();
        session.Selection().SetScope(rootA, session.DocumentView());
        session.hoveredNode = rootA;
        session.viewport.zoom = 2.5f;
        session.viewport.gridVisible = true;
        session.timeline.currentTime = 12.0f;
        session.canvas.gesture = DesignerCanvasGesture::Move;
        Check(static_cast<bool>(session.Commands().BeginPropertyGesture(
                  rootA, "offsets", "Move A", DesignerDirtyFlags::Layout)) &&
                  static_cast<bool>(session.Commands().UpdatePropertyGesture(
                      px::Rect{30, 40, 200, 100})),
              "document A should own an active gesture before replacement");

        Check(static_cast<bool>(session.New(pathB, 800, 600)),
              "creating document B should safely Adopt over document A");
        Check(session.Document()->DocumentId() != documentA &&
                  session.Selection().Primary() == session.DocumentView().Root() &&
                  session.Selection().Scope().Empty() && session.hoveredNode.Empty() &&
                  Near(session.viewport.zoom, 1.0f) && !session.viewport.gridVisible &&
                  Near(session.timeline.currentTime, 0.0f) &&
                  session.canvas.gesture == DesignerCanvasGesture::None &&
                  !session.Commands().GestureActive() &&
                  session.Commands().HistoryCursor() == 0,
              "Adopt must isolate selection, scope, hover, viewport, timeline, canvas and commands");
    }

    std::error_code error;
    std::filesystem::remove(pathA, error);
    std::filesystem::remove(pathB, error);
}

void TestDomainCommandRules() {
    {
        Fixture fixture;
        auto& commands = fixture.session.Commands();
        fixture.session.Selection().Replace(
            {fixture.parent, fixture.child, fixture.sibling}, fixture.sibling);
        const std::size_t before = commands.HistoryCursor();
        Check(static_cast<bool>(commands.DuplicateSelection()) &&
                  commands.HistoryCursor() == before + 1,
              "multi-selection duplication should be one domain command");
        const auto children = fixture.session.DocumentView().Children(fixture.root);
        Check(children.size() == 4 && children[0] == fixture.parent &&
                  children[2] == fixture.sibling,
              "duplicate insertion order should be deterministic and adjacent to sources");
        if (children.size() == 4) {
            const auto* parentCopy = fixture.session.DocumentView().Find(
                *fixture.session.Document(), children[1]);
            const auto* siblingCopy = fixture.session.DocumentView().Find(
                *fixture.session.Document(), children[3]);
            Check(parentCopy && parentCopy->name == "ParentCopy" && siblingCopy &&
                      siblingCopy->name == "SiblingCopy" &&
                      fixture.session.Selection().Contains(children[1]) &&
                      fixture.session.Selection().Contains(children[3]),
                  "duplicate naming and post-command selection should be deterministic");
            const auto copiedChildren = fixture.session.DocumentView().Children(children[1]);
            Check(copiedChildren.size() == 1 && copiedChildren.front() != fixture.child,
                  "duplicated subtrees must regenerate every descendant UUID");
        }
        Check(static_cast<bool>(commands.Undo()) &&
                  fixture.session.DocumentView().Children(fixture.root).size() == 2 &&
                  static_cast<bool>(commands.Redo()) &&
                  fixture.session.DocumentView().Children(fixture.root).size() == 4,
              "duplicate undo and redo should preserve canonical indexes and order");
    }

    {
        Fixture fixture;
        auto& commands = fixture.session.Commands();
        fixture.session.Selection().Replace({fixture.parent, fixture.child}, fixture.child);
        const std::size_t before = commands.HistoryCursor();
        const px::Status cycle = commands.Reparent(fixture.parent, fixture.child, 0);
        Check(!cycle && commands.HistoryCursor() == before &&
                  fixture.session.DocumentView().Parent(fixture.parent) == fixture.root &&
                  fixture.session.DocumentView().Parent(fixture.child) == fixture.parent,
              "cycle-producing reparent must fail atomically without index drift");

        fixture.session.Selection().Replace({fixture.parent, fixture.child}, fixture.child);
        Check(static_cast<bool>(commands.DeleteSelection()) &&
                  !fixture.session.DocumentView().Contains(fixture.parent) &&
                  !fixture.session.DocumentView().Contains(fixture.child) &&
                  fixture.session.Selection().Primary() == fixture.root &&
                  commands.HistoryCursor() == before + 1,
              "delete must canonicalize ancestor/descendant selection and choose root fallback");
    }
}

void TestCanvasServices() {
    Fixture fixture;
    const px::editor::CanvasTransform transform({100, 50}, 2.0f);
    const px::Vec2 screen = transform.CanvasToScreen({25, 30});
    const px::Vec2 roundTrip = transform.ScreenToCanvas(screen);
    Check(Near(screen.x, 150) && Near(screen.y, 110) && Near(roundTrip.x, 25) &&
              Near(roundTrip.y, 30),
          "CanvasTransform should round-trip coordinates at non-default zoom and origin");

    px::editor::HitTestService hitTest;
    auto hits = hitTest.HitStack(*fixture.session.Document(), fixture.session.DocumentView(),
                                 {50, 50}, fixture.root);
    Check(hits.size() >= 3 && hits[0] == fixture.sibling && hits[1] == fixture.child,
          "hit stacks should follow reverse authored order for overlapping siblings");
    fixture.session.Document()->Find(fixture.sibling)->properties["editorLocked"] = true;
    hits = hitTest.HitStack(*fixture.session.Document(), fixture.session.DocumentView(),
                            {50, 50}, fixture.root);
    Check(!hits.empty() && hits.front() == fixture.child,
          "locked nodes should be skipped by canvas hit testing");

    const std::vector<px::Uuid> ignored{fixture.parent};
    const px::editor::SnapRequest request{.movingRect = {194, 200, 40, 30},
                                          .mode = px::editor::SnapMode::Move,
                                          .parent = fixture.root,
                                          .ignoredNodes = std::span<const px::Uuid>(ignored),
                                          .zoom = 1.0f,
                                          .canvasRect = {0, 0, 800, 600}};
    const auto snapped = px::editor::SnapEngine{}.Snap(request, fixture.session.DocumentView());
    Check(Near(snapped.rect.x, 190) && !snapped.guides.empty(),
          "SnapEngine should use sibling edges and expose the winning guide");

    const px::editor::SnapRequest gridOnly{.movingRect={3,5,31,27},
                                           .mode=px::editor::SnapMode::Resize,
                                           .parent=fixture.root,
                                           .ignoredNodes=std::span<const px::Uuid>(ignored),
                                           .zoom=1.0f,
                                           .canvasRect={0,0,800,600},
                                           .alignmentEnabled=false,
                                           .gridEnabled=true,
                                           .gridSize=16.0f,
                                           .snapLeft=false,
                                           .snapRight=true,
                                           .snapTop=false,
                                           .snapBottom=false};
    const auto resized = px::editor::SnapEngine{}.Snap(gridOnly,
                                                       fixture.session.DocumentView());
    Check(Near(resized.rect.x,3) && Near(resized.rect.y,5) &&
              Near(resized.rect.w,29) && Near(resized.rect.h,27) &&
              resized.guides.empty(),
          "grid-only resize should snap only the active edge without alignment guides");
    Check(Near(px::editor::SnapEngine{}.SnapNormalized(0.52f),0.5f) &&
              Near(px::editor::SnapEngine{}.SnapNormalized(1.2f),1.0f),
          "anchor and pivot normalization should share the authoritative SnapEngine");

}

void TestCanvasInteractionAuthority() {
    Fixture fixture;
    auto& interaction = fixture.session.Interaction();
    const auto pointer = [](float x, float y,
                            px::editor::DesignerModifierKeys modifiers = {}) {
        return px::editor::DesignerPointerEvent{
            .screenPosition = {x, y},
            .canvasPosition = {x, y},
            .zoom = 1.0f,
            .button = px::editor::DesignerMouseButton::Left,
            .modifiers = modifiers};
    };
    const auto click = [&](const px::editor::DesignerPointerEvent& event) {
        interaction.UpdateHover(event);
        return interaction.PointerDown(event) && interaction.HasCapture() &&
               interaction.PointerUp(event) && !interaction.HasCapture();
    };

    Check(click(pointer(35, 35)) &&
              fixture.session.Selection().Primary() == fixture.sibling,
          "Canvas click should replace selection with the topmost hit");
    Check(click(pointer(20, 60, {.controlOrCommand = true})) &&
              fixture.session.Selection().Contains(fixture.parent) &&
              fixture.session.Selection().Contains(fixture.sibling),
          "modifier click should add a second ordered selection");
    Check(click(pointer(28, 60, {.controlOrCommand = true})) &&
              !fixture.session.Selection().Contains(fixture.parent) &&
              fixture.session.Selection().Contains(fixture.sibling),
          "modifier click on a selected Control should toggle it off");

    fixture.session.Selection().Replace(fixture.sibling);
    Check(click(pointer(50, 50, {.alt = true})) &&
              fixture.session.Selection().Primary() != fixture.sibling,
          "Alt-click should deterministically cycle the overlapping hit stack");

    const auto marqueeStart = pointer(300, 300);
    const auto marqueeEnd = pointer(0, 0);
    interaction.UpdateHover(marqueeStart);
    Check(interaction.PointerDown(marqueeStart) &&
              interaction.PointerMove(marqueeEnd) &&
              interaction.PointerUp(marqueeEnd) &&
              fixture.session.Selection().Size() == 2 &&
              fixture.session.Selection().Contains(fixture.parent) &&
              fixture.session.Selection().Contains(fixture.sibling),
          "empty-space marquee should replace and canonicalize selection");

    fixture.session.Selection().Replace(fixture.parent);
    const px::Rect originalLayout =
        *fixture.session.DocumentView().LayoutRect(fixture.parent);
    const std::size_t beforeCancel = fixture.session.Commands().HistoryCursor();
    const auto moveStart = pointer(28, 60);
    const auto moveEnd = pointer(48, 80);
    interaction.UpdateHover(moveStart);
    Check(interaction.PointerDown(moveStart) && interaction.PointerMove(moveEnd),
          "move gesture should be owned by CanvasInteractionController");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent,
                                                 {40, 40, 160, 100});
    interaction.Cancel();
    const auto cancelledMove =
        fixture.session.Document()->ReadProperty(fixture.parent, "offsets");
    Check(cancelledMove && cancelledMove.Value().Type() == px::VariantType::Null &&
              fixture.session.DocumentView().LayoutRect(fixture.parent) ==
                  std::optional<px::Rect>(originalLayout) &&
              fixture.session.Commands().HistoryCursor() == beforeCancel &&
              !interaction.HasCapture(),
          "move cancel should restore authored data, layout, history and capture");

    const std::size_t beforeMoveCommit = fixture.session.Commands().HistoryCursor();
    interaction.UpdateHover(moveStart);
    Check(interaction.PointerDown(moveStart) &&
              interaction.PointerMove(pointer(38, 70)) &&
              interaction.PointerMove(moveEnd) &&
              interaction.PointerUp(moveEnd) &&
              fixture.session.Commands().HistoryCursor() == beforeMoveCommit + 1,
          "many move events should commit exactly one undo entry");
    const px::Variant movedOffsets =
        fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value();
    fixture.session.DocumentView().SetLayoutRect(fixture.parent,
                                                 {40, 40, 160, 100});

    const auto resizeStart = pointer(40, 40);
    interaction.UpdateHover(resizeStart);
    Check(fixture.session.canvas.hoveredResizeHandle == 1 &&
              interaction.PointerDown(resizeStart) &&
              interaction.PointerMove(pointer(30, 30)),
          "resize handle should begin the authoritative resize gesture");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent,
                                                 {30, 30, 170, 110});
    interaction.Cancel();
    Check(fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value() ==
              movedOffsets &&
              fixture.session.DocumentView().LayoutRect(fixture.parent) ==
                  std::optional<px::Rect>({40, 40, 160, 100}),
          "resize cancel should restore authored offsets and the exact layout snapshot");

    interaction.SetAnchorTool(true);
    const auto anchorStart = pointer(0, 0);
    interaction.UpdateHover(anchorStart);
    const px::Variant anchorOffsetsBefore =
        fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value();
    Check(fixture.session.canvas.hoveredAnchorHandle != 0 &&
              interaction.PointerDown(anchorStart) &&
              interaction.PointerMove(pointer(100, 100)),
          "anchor tool should own anchor hit testing and updates");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent,
                                                 {100, 100, 160, 100});
    interaction.Cancel();
    Check(fixture.session.Document()->ReadProperty(fixture.parent, "anchors").Value().Type() ==
              px::VariantType::Null &&
              fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value() ==
                  anchorOffsetsBefore &&
              fixture.session.DocumentView().LayoutRect(fixture.parent) ==
                  std::optional<px::Rect>({40, 40, 160, 100}),
          "anchor cancel should restore exact absent anchors, offsets and layout");

    interaction.SetAnchorTool(false);
    const auto pivotStart = pointer(120, 90);
    interaction.UpdateHover(pivotStart);
    Check(fixture.session.canvas.hoveredPivotHandle &&
              interaction.PointerDown(pivotStart) &&
              interaction.PointerMove(pointer(150, 110)),
          "select mode should own pivot hit testing and updates");
    interaction.Cancel();
    Check(fixture.session.Document()->ReadProperty(fixture.parent, "pivot").Value().Type() ==
              px::VariantType::Null,
          "pivot cancel should restore the exact authored value");

    fixture.session.Document()->WriteProperty(
        fixture.sibling, "offsets", px::Rect{30, 30, 160, 100});
    fixture.session.Selection().Replace({fixture.parent, fixture.sibling},
                                        fixture.sibling);
    const px::Variant parentBefore =
        fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value();
    const px::Variant siblingBefore =
        fixture.session.Document()->ReadProperty(fixture.sibling, "offsets").Value();
    const auto groupStart = pointer(100, 100);
    interaction.UpdateHover(groupStart);
    Check(interaction.PointerDown(groupStart) &&
              fixture.session.canvas.groupMove &&
              interaction.PointerMove(pointer(120, 120)),
          "multi-selection move should use canonical selected roots");
    fixture.session.DocumentView().SetLayoutRect(fixture.parent,
                                                 {60, 60, 160, 100});
    fixture.session.DocumentView().SetLayoutRect(fixture.sibling,
                                                 {50, 50, 160, 100});
    interaction.Cancel();
    Check(fixture.session.Document()->ReadProperty(fixture.parent, "offsets").Value() ==
              parentBefore &&
              fixture.session.Document()->ReadProperty(fixture.sibling, "offsets").Value() ==
                  siblingBefore &&
              fixture.session.DocumentView().LayoutRect(fixture.parent) ==
                  std::optional<px::Rect>({40, 40, 160, 100}) &&
              fixture.session.DocumentView().LayoutRect(fixture.sibling) ==
                  std::optional<px::Rect>({30, 30, 160, 100}),
          "multi-move cancel should restore every property and cached rectangle");

    fixture.session.DocumentView().SetChildPolicy(
        fixture.root, px::ui::ChildLayoutPolicy::LinearY);
    fixture.session.Selection().Replace(fixture.sibling);
    const std::size_t beforeManaged = fixture.session.Commands().HistoryCursor();
    const auto managedStart = pointer(100, 80);
    interaction.UpdateHover(managedStart);
    Check(interaction.PointerDown(managedStart) &&
              fixture.session.canvas.gesture ==
                  px::editor::DesignerCanvasGesture::Reorder &&
              interaction.PointerMove(pointer(100, 10)),
          "managed-layout drag should enter reorder preview without authored mutation");
    interaction.Cancel();
    Check(fixture.session.Commands().HistoryCursor() == beforeManaged &&
              fixture.session.canvas.gesture ==
                  px::editor::DesignerCanvasGesture::None,
          "managed reorder cancel should leave history and authored order unchanged");
}

void TestPreviewChangePlanning() {
    const px::Uuid document = px::Uuid::Random();
    const px::Uuid node = px::Uuid::Random();
    using px::editor::DesignerDirtyFlags;
    using px::editor::DocumentChangeSet;
    using px::editor::PreviewUpdate;
    using px::editor::HasPreviewUpdate;

    auto text = DocumentChangeSet::Property(node, "text", DesignerDirtyFlags::Paint);
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(text, document),
                           PreviewUpdate::PatchProperties),
          "content edits should plan an in-place preview property patch");
    auto layout = DocumentChangeSet::Property(
        node, "offsets", DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint);
    const auto layoutPlan = px::editor::PlanPreviewUpdate(layout, document);
    Check(HasPreviewUpdate(layoutPlan, PreviewUpdate::PatchProperties) &&
              HasPreviewUpdate(layoutPlan, PreviewUpdate::Relayout),
          "layout edits should patch and request relayout");
    auto reset = DocumentChangeSet::Property(node, "text", DesignerDirtyFlags::Paint);
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(reset, document),
                           PreviewUpdate::PatchProperties),
          "property removals should use the same default-restoring patch path");
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(
                               DocumentChangeSet::Structure(node), document),
                           PreviewUpdate::RebuildScene),
          "structural edits should rebuild the preview scene");
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(
                               DocumentChangeSet::Property(
                                   node, "styleBinding", DesignerDirtyFlags::Theme),
                               document),
                           PreviewUpdate::InvalidateStyle),
          "style binding edits should invalidate resolved style");
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(
                               DocumentChangeSet::Property(
                                   node, "bindings", DesignerDirtyFlags::Binding),
                               document),
                           PreviewUpdate::ReconnectBindings),
          "binding edits should reconnect preview bindings");
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(
                               DocumentChangeSet::Property(
                                   document, "animations", DesignerDirtyFlags::Animation),
                               document),
                           PreviewUpdate::UpdateAnimations),
          "animation library edits should update the preview controller");
    Check(HasPreviewUpdate(px::editor::PlanPreviewUpdate(
                               DocumentChangeSet::Property(
                                   node, "overrides", DesignerDirtyFlags::Structure),
                               document),
                           PreviewUpdate::RebuildScene),
          "component expansion edits should rebuild their preview scene");
}

void TestDesignerInvalidationPlanning() {
    using px::editor::DesignerDirtyFlags;
    using px::editor::DesignerUpdate;
    using px::editor::DocumentChangeSet;
    using px::editor::HasDesignerUpdate;
    const px::Uuid node = px::Uuid::Random();

    const auto paint = px::editor::PlanDesignerUpdate(DocumentChangeSet::Property(
        node, "background.color", DesignerDirtyFlags::Paint));
    Check(HasDesignerUpdate(paint, DesignerUpdate::Repaint) &&
              !HasDesignerUpdate(paint, DesignerUpdate::Relayout) &&
              !HasDesignerUpdate(paint, DesignerUpdate::RebuildLayoutScene),
          "paint-only edits must repaint without layout work or scene reconstruction");

    const auto layout = px::editor::PlanDesignerUpdate(DocumentChangeSet::Property(
        node, "offsets", DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint));
    Check(HasDesignerUpdate(layout, DesignerUpdate::PatchLayoutProperties) &&
              HasDesignerUpdate(layout, DesignerUpdate::Relayout) &&
              !HasDesignerUpdate(layout, DesignerUpdate::RebuildLayoutScene),
          "layout property edits must patch and relayout the retained scene");

    const auto structure =
        px::editor::PlanDesignerUpdate(DocumentChangeSet::Structure(node));
    Check(HasDesignerUpdate(structure, DesignerUpdate::RebuildIndex) &&
              HasDesignerUpdate(structure, DesignerUpdate::RebuildLayoutScene),
          "structural edits must rebuild both index and retained layout scene");

    const auto themePaint = px::editor::PlanDesignerUpdate(DocumentChangeSet::Property(
        node, "styleBinding", DesignerDirtyFlags::Theme | DesignerDirtyFlags::Paint));
    Check(HasDesignerUpdate(themePaint, DesignerUpdate::InvalidateStyle) &&
              HasDesignerUpdate(themePaint, DesignerUpdate::Repaint) &&
              !HasDesignerUpdate(themePaint, DesignerUpdate::Relayout),
          "paint-only style overrides must not trigger layout work");
    Check(!px::editor::IsLayoutAffectingStyleProperty("background.color") &&
              px::editor::IsLayoutAffectingStyleProperty("padding") &&
              px::editor::IsLayoutAffectingStyleProperty("spacing") &&
              px::editor::IsLayoutAffectingStyleProperty("typography.font") &&
              px::editor::IsLayoutAffectingStyleProperty("typography.size"),
          "style invalidation must distinguish paint-only and geometry properties");

    for (int move = 0; move < 100; ++move) {
        const auto drag = px::editor::PlanDesignerUpdate(DocumentChangeSet::Property(
            node, "offsets", DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint));
        Check(!HasDesignerUpdate(drag, DesignerUpdate::RebuildLayoutScene),
              "pointer-move layout updates must never reconstruct the runtime scene");
    }
}

void TestComponentServiceBoundary() {
    Check(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
          "component tests require registered UI property metadata");
    Fixture fixture;
    px::editor::ComponentService components;
    const px::Uuid sourceRoot = px::Uuid::Random();
    px::resource::TypedDocument source;
    source.kind = px::resource::DocumentKind::Scene;
    source.id = px::Uuid::Random();
    source.type = "UIComponent";
    source.properties["uiSchemaVersion"] = std::int64_t{5};
    source.properties["component.exposedProperties"] = px::VariantArray{
        px::VariantObject{{"id",std::string("caption")},
                          {"displayName",std::string("Caption")},
                          {"node",sourceRoot},
                          {"property",std::string("text")},
                          {"type",std::string("String")}}};
    source.nodes.push_back({sourceRoot, {}, "Source Label", "Label",
                            {{"text", std::string("Source")}}});
    components.SetLoader([source](const px::ResourceRefValue&) {
        return px::Result<px::resource::TypedDocument>::Success(source);
    });
    const px::ResourceRefValue reference{source.id, "Content/UI/Test.pxcomponent"};
    const auto beforeCount = fixture.session.Document()->Children(fixture.root).size();
    Check(static_cast<bool>(components.Instantiate(
              *fixture.session.Document(), fixture.session.Commands(), reference,
              fixture.root, beforeCount)) &&
              fixture.session.Document()->Children(fixture.root).size() == beforeCount + 1,
          "component instantiation should execute through ComponentService and Commands");
    const auto children = fixture.session.Document()->Children(fixture.root);
    const px::Uuid instanceId = children.back()->id;
    const px::VariantObject interfaceValues{{"caption",std::string("Hello")}};
    Check(children.back()->type == "ComponentInstance" &&
              static_cast<bool>(components.SetInstanceInterface(
                  *fixture.session.Document(), fixture.session.Commands(), instanceId,
                  "componentProperties", interfaceValues, "Set public properties")) &&
              static_cast<bool>(components.AssignSlot(
                  *fixture.session.Document(), fixture.session.Commands(), fixture.child,
                  "content")) &&
              fixture.session.Document()->Find(instanceId)->properties.contains(
                  "componentProperties") &&
              fixture.session.Document()->Find(fixture.child)->properties.contains(
                  "componentSlot") &&
              static_cast<bool>(components.SetPropertyOverride(
                  *fixture.session.Document(), fixture.session.Commands(), instanceId,
                  sourceRoot, "text", std::string("Override"))) &&
              components.OverrideCount(*fixture.session.Document(), instanceId) == 1,
          "validated component overrides should be centralized in ComponentService");
    Check(static_cast<bool>(components.ResetPropertyOverride(
              *fixture.session.Document(), fixture.session.Commands(), instanceId,
              sourceRoot, "text")) &&
              components.OverrideCount(*fixture.session.Document(), instanceId) == 0,
          "component override reset should round-trip through the command boundary");
    Check(static_cast<bool>(components.Detach(*fixture.session.Document(),
                                              fixture.session.Commands(), instanceId)) &&
              fixture.session.Document()->Find(instanceId)->type != "ComponentInstance" &&
              static_cast<bool>(fixture.session.Commands().Undo()) &&
              fixture.session.Document()->Find(instanceId)->type == "ComponentInstance" &&
              static_cast<bool>(fixture.session.Commands().Redo()) &&
              fixture.session.Document()->Find(instanceId)->type != "ComponentInstance",
          "detach and undo/redo should preserve the stable instance root ID");

    Fixture componentFixture;
    componentFixture.session.Document()->Data().type = "UIComponent";
    const px::VariantArray definitions{px::VariantObject{{"id",std::string("caption")}}};
    const auto beforeInterface = componentFixture.session.Commands().HistoryCursor();
    Check(static_cast<bool>(components.SetInterfaceDefinitions(
              *componentFixture.session.Document(), componentFixture.session.Commands(),
              "component.exposedProperties", definitions, "Set component API")) &&
              componentFixture.session.Commands().HistoryCursor() == beforeInterface + 1 &&
              static_cast<bool>(componentFixture.session.Commands().Undo()) &&
              !componentFixture.session.Document()->Data().properties.contains(
                  "component.exposedProperties"),
          "component API definitions should be one command-backed, undoable edit");
}

}  // namespace

int main() {
    TestSelectionAndDocumentView();
    TestDesignerUiPresentationStates();
    TestDesignerWorkspaceGeometrySnapshots();
    TestPreviewDpiAndCjkSmokeState();
    TestCommandsAndTransactions();
    TestSessionDocumentLifecycle();
    TestDomainCommandRules();
    TestCanvasServices();
    TestCanvasInteractionAuthority();
    TestPreviewChangePlanning();
    TestDesignerInvalidationPlanning();
    TestComponentServiceBoundary();
    if (failures == 0) std::cout << "All headless UI Designer core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
