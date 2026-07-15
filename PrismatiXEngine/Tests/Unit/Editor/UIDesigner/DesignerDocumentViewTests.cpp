#include "Tests/TestSupport/DesignerFixture.h"

#include "Editor/Workspace/EditHistory.h"

int main() {
    px::test::Suite suite("DesignerDocumentView");

    suite.Run("LookupHierarchyAndLayout_ReflectAuthoredDocument", [&] {
        px::test::DesignerFixture fixture(suite);
        const auto& view = fixture.session.DocumentView();
        suite.Expect(view.Root() == fixture.root && view.Contains(fixture.child) &&
                         view.Find(*fixture.session.Document(), fixture.child) != nullptr,
                     "Root, Contains, and Find use the canonical index");
        const auto children = view.Children(fixture.root);
        suite.Expect(children.size() == 2 && children[0] == fixture.parent &&
                         children[1] == fixture.sibling &&
                         view.Parent(fixture.child) == fixture.parent &&
                         view.ChildIndex(fixture.sibling) == 1,
                     "Parent, Children, and ChildIndex reflect authored sibling order");
        const auto snapshot = view.CaptureLayout();
        suite.Expect(snapshot.rects.size() == 4 &&
                         snapshot.rects.at(fixture.child) == px::Rect{40, 40, 80, 40},
                     "layout cache is disposable derived state keyed by node ID");
    });

    suite.Run("EveryStructuralEdit_RebuildsIndexAndSurvivesUndoRedo", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& commands = fixture.session.Commands();
        const px::Uuid added = px::Uuid::FromName("PrismatiX.Test.DocumentView.Added");
        px::VariantObject subtree{{"id", added},
                                  {"name", std::string("Added")},
                                  {"type", std::string("Label")},
                                  {"properties", px::VariantObject{}},
                                  {"children", px::VariantArray{}}};
        suite.Expect(static_cast<bool>(commands.Execute(
                         std::make_unique<px::editor::SubtreeEditCommand>(
                             "Add Label", px::editor::SubtreeOperation::Insert, added,
                             fixture.root, 1, std::move(subtree)),
                         px::editor::DocumentChangeSet::Structure(fixture.root))) &&
                         fixture.IndexMatchesDocument() &&
                         fixture.session.DocumentView().ChildIndex(added) == 1,
                     "add rebuilds an index matching authored document order");

        fixture.session.Selection().Replace(fixture.parent);
        suite.Expect(static_cast<bool>(commands.DuplicateSelection()) &&
                         fixture.IndexMatchesDocument(),
                     "duplicate rebuilds a matching index");
        const px::Uuid duplicate = fixture.session.Selection().Primary();

        suite.Expect(static_cast<bool>(commands.Reparent(duplicate, fixture.sibling, 0)) &&
                         fixture.IndexMatchesDocument() &&
                         fixture.session.DocumentView().Parent(duplicate) == fixture.sibling,
                     "reparent rebuilds a matching parent/child index");
        suite.Expect(static_cast<bool>(commands.Undo()) && fixture.IndexMatchesDocument() &&
                         fixture.session.DocumentView().Parent(duplicate) == fixture.root &&
                         static_cast<bool>(commands.Redo()) && fixture.IndexMatchesDocument() &&
                         fixture.session.DocumentView().Parent(duplicate) == fixture.sibling,
                     "reparent undo/redo never lets the derived index drift");

        suite.Expect(static_cast<bool>(commands.Reparent(duplicate, fixture.root, 0)) &&
                         static_cast<bool>(commands.Reorder(duplicate, 3)) &&
                         fixture.IndexMatchesDocument() &&
                         fixture.session.DocumentView().ChildIndex(duplicate) == 3,
                     "reorder rebuilds the exact sibling-order index");
        suite.Expect(static_cast<bool>(commands.Undo()) && fixture.IndexMatchesDocument() &&
                         static_cast<bool>(commands.Redo()) && fixture.IndexMatchesDocument(),
                     "reorder undo/redo preserves index parity");

        fixture.session.Selection().Replace(duplicate);
        suite.Expect(static_cast<bool>(commands.DeleteSelection()) &&
                         !fixture.session.DocumentView().Contains(duplicate) &&
                         fixture.IndexMatchesDocument() &&
                         static_cast<bool>(commands.Undo()) &&
                         fixture.session.DocumentView().Contains(duplicate) &&
                         fixture.IndexMatchesDocument() &&
                         static_cast<bool>(commands.Redo()) &&
                         !fixture.session.DocumentView().Contains(duplicate) &&
                         fixture.IndexMatchesDocument(),
                     "delete and its history navigation preserve document/index parity");
    });

    return suite.Finish();
}
