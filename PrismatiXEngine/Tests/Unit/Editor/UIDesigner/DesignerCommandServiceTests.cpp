#include "Tests/TestSupport/DesignerFixture.h"

#include "Editor/Workspace/EditHistory.h"

#include <array>

int main() {
    px::test::Suite suite("DesignerCommandService");

    suite.Run("PropertyCommands_UndoRedoAndAtomicFailure", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& commands = fixture.session.Commands();
        suite.Expect(static_cast<bool>(commands.SetProperty(
                         fixture.parent, "text", std::string("Hello"), "Set text")) &&
                         fixture.session.Document()->ReadProperty(fixture.parent, "text")
                                 .Value() == px::Variant(std::string("Hello")),
                     "SetProperty writes through the command boundary");
        suite.Expect(static_cast<bool>(commands.Undo()) &&
                         fixture.session.Document()->ReadProperty(fixture.parent, "text")
                                 .Value().Type() == px::VariantType::Null &&
                         static_cast<bool>(commands.Redo()) &&
                         fixture.session.Document()->ReadProperty(fixture.parent, "text")
                                 .Value() == px::Variant(std::string("Hello")),
                     "property undo/redo restores exact absent and authored values");

        const std::array<px::Uuid, 2> targets{fixture.parent, fixture.sibling};
        const auto beforeBatch = commands.HistoryCursor();
        suite.Expect(static_cast<bool>(commands.SetProperties(
                         targets, "visibility", std::string("Hidden"),
                         "Hide controls", px::editor::DesignerDirtyFlags::Layout)) &&
                         commands.HistoryCursor() == beforeBatch + 1 &&
                         fixture.session.Document()->ReadProperty(
                             fixture.parent, "visibility").Value() ==
                             px::Variant(std::string("Hidden")) &&
                         fixture.session.Document()->ReadProperty(
                             fixture.sibling, "visibility").Value() ==
                             px::Variant(std::string("Hidden")),
                     "SetProperties is one atomic history entry for all targets");

        auto partial = std::make_unique<px::editor::CompositeEditCommand>("Partial failure");
        partial->Add(std::make_unique<px::editor::PropertyChangeCommand>(
            "Temporary rename", fixture.parent, "$name", std::string("Parent"),
            std::string("MustRollBack")));
        partial->Add(std::make_unique<px::editor::PropertyChangeCommand>(
            "Missing target", px::Uuid::FromName("PrismatiX.Test.Missing"), "$name",
            px::Variant{}, std::string("Fail")));
        const auto cursor = commands.HistoryCursor();
        suite.Expect(!commands.Execute(
                         std::move(partial),
                         px::editor::DocumentChangeSet::Property(
                             fixture.parent, "$name", px::editor::DesignerDirtyFlags::Paint)) &&
                         fixture.session.Document()->Find(fixture.parent)->name == "Parent" &&
                         commands.HistoryCursor() == cursor,
                     "partial command failure rolls back applied children and history");
        suite.Expect(!commands.SetProperty(px::Uuid::FromName("PrismatiX.Test.Invalid"),
                                           "text", std::string("bad")),
                     "invalid target fails without mutation");
    });

    suite.Run("StructuralCommands_DefineSiblingAndSelectionSemantics", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& commands = fixture.session.Commands();
        fixture.session.Selection().Replace(fixture.parent);
        suite.Expect(static_cast<bool>(commands.DuplicateSelection()),
                     "DuplicateSelection succeeds through the domain service");
        const px::Uuid duplicate = fixture.session.Selection().Primary();
        suite.Expect(static_cast<bool>(commands.Reparent(duplicate, fixture.sibling, 0)) &&
                         fixture.session.DocumentView().Parent(duplicate) == fixture.sibling,
                     "Reparent changes the authoritative parent");
        suite.Expect(static_cast<bool>(commands.Reparent(duplicate, fixture.root, 2)) &&
                         static_cast<bool>(commands.Reorder(duplicate, 0)) &&
                         fixture.session.DocumentView().ChildIndex(duplicate) == 0,
                     "Reorder changes canonical sibling order");
        suite.Expect(static_cast<bool>(commands.MoveAfter(duplicate, fixture.sibling)) &&
                         fixture.session.DocumentView().ChildIndex(duplicate) == 2,
                     "MoveAfter places a sibling after its reference");
        const auto cursor = commands.HistoryCursor();
        suite.Expect(static_cast<bool>(commands.MoveBefore(duplicate, fixture.sibling)) &&
                         fixture.session.DocumentView().ChildIndex(duplicate) == 1 &&
                         commands.HistoryCursor() == cursor + 1,
                     "MoveBefore places a sibling before its reference with one history entry");
        const auto noOpCursor = commands.HistoryCursor();
        suite.Expect(static_cast<bool>(commands.MoveBefore(duplicate, fixture.sibling)) &&
                         commands.HistoryCursor() == noOpCursor,
                     "already-satisfied relative moves do not create meaningless history");
        fixture.session.Selection().Replace(duplicate);
        suite.Expect(static_cast<bool>(commands.DeleteSelection()) &&
                         !fixture.session.DocumentView().Contains(duplicate) &&
                         static_cast<bool>(commands.Undo()) &&
                         fixture.session.DocumentView().Contains(duplicate),
                     "DeleteSelection is structural and undoable");
    });

    suite.Run("GestureManyUpdates_CommitsOnceOrCancelsWithoutHistory", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& commands = fixture.session.Commands();
        const auto before = commands.HistoryCursor();
        suite.Expect(static_cast<bool>(commands.BeginPropertyGesture(
                         fixture.parent, "offsets", "Move",
                         px::editor::DesignerDirtyFlags::Layout)),
                     "gesture begins on a valid property");
        bool updated = true;
        for (int index = 0; index < 100; ++index) {
            updated &= static_cast<bool>(commands.UpdatePropertyGesture(
                px::Rect{static_cast<float>(index), 20, 160, 100}));
        }
        suite.Expect(updated && static_cast<bool>(commands.CommitPropertyGesture()) &&
                         commands.HistoryCursor() == before + 1,
                     "pointer down plus 100 updates plus pointer up creates one undo entry");
        suite.Expect(static_cast<bool>(commands.Undo()) &&
                         fixture.session.Document()->ReadProperty(
                             fixture.parent, "offsets").Value().Type() ==
                             px::VariantType::Null &&
                         static_cast<bool>(commands.Redo()),
                     "the whole gesture is one undo/redo unit");

        const auto beforeCancel = commands.HistoryCursor();
        const px::Variant committed = fixture.session.Document()->ReadProperty(
            fixture.parent, "offsets").Value();
        suite.Expect(static_cast<bool>(commands.BeginPropertyGesture(
                         fixture.parent, "offsets", "Move",
                         px::editor::DesignerDirtyFlags::Layout)) &&
                         static_cast<bool>(commands.UpdatePropertyGesture(
                             px::Rect{500, 500, 160, 100})) &&
                         static_cast<bool>(commands.CancelPropertyGesture()) &&
                         fixture.session.Document()->ReadProperty(
                             fixture.parent, "offsets").Value() == committed &&
                         commands.HistoryCursor() == beforeCancel,
                     "cancel restores the original value and creates zero history entries");
    });

    return suite.Finish();
}
