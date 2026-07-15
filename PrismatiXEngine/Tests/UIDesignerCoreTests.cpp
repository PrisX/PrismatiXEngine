#include "Editor/Tools/UIDesigner/Canvas/CanvasTransform.h"
#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Editor/Tools/UIDesigner/Canvas/DesignerTools.h"
#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"
#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
#include "Editor/Tools/UIDesigner/Components/ComponentService.h"
#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
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

void TestCommandsAndTransactions() {
    Fixture fixture;
    auto& commands = fixture.session.Commands();
    int changeCount = 0;
    std::vector<px::editor::DocumentChangeSet> changes;
    commands.SetChanged([&](const px::editor::DocumentChangeSet& change) {
        ++changeCount;
        changes.push_back(change);
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
    Check(static_cast<bool>(commands.BeginPropertyGesture(
              fixture.parent, "offsets", "Move", px::editor::DesignerDirtyFlags::Layout)) &&
              static_cast<bool>(commands.UpdatePropertyGesture(moved)) &&
              static_cast<bool>(commands.CancelPropertyGesture()),
          "a property gesture should update and cancel through the command boundary");
    const auto cancelled = fixture.session.Document()->ReadProperty(fixture.parent, "offsets");
    Check(cancelled && cancelled.Value().Type() == px::VariantType::Null,
          "gesture cancellation should restore an absent property exactly");
    Check(commands.HistoryCursor() == beforeCancel,
          "gesture cancellation should not create a history entry");

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

    px::editor::CanvasInteractionController interaction;
    px::editor::SelectTool selectTool;
    px::editor::AnchorTool anchorTool;
    const px::editor::DesignerPointerEvent pointer{.screenPosition={150,110},
                                                    .canvasPosition={25,30},
                                                    .button=px::editor::DesignerMouseButton::Left};
    interaction.SetActiveTool(&selectTool);
    Check(interaction.PointerDown(pointer) && interaction.HasCapture() &&
              interaction.PointerMove(pointer),
          "select tool should own pointer capture after an accepted press");
    interaction.SetActiveTool(&anchorTool);
    Check(!interaction.HasCapture() && interaction.PointerDown(pointer) &&
              interaction.PointerUp(pointer) && !interaction.HasCapture(),
          "tool switching should cancel old capture and pointer-up should release the new tool");
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
    TestCommandsAndTransactions();
    TestCanvasServices();
    TestPreviewChangePlanning();
    TestComponentServiceBoundary();
    if (failures == 0) std::cout << "All headless UI Designer core tests passed.\n";
    return failures == 0 ? 0 : 1;
}
