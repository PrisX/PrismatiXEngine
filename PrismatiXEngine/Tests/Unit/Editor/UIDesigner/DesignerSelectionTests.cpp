#include "Tests/TestSupport/DesignerFixture.h"

#include <vector>

int main() {
    px::test::Suite suite("DesignerSelection");

    suite.Run("ReplaceAddRemoveToggleClear_PreservesOrderedPrimary", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& selection = fixture.session.Selection();
        selection.Replace(fixture.parent);
        suite.Expect(selection.Primary() == fixture.parent && selection.Size() == 1,
                     "Replace establishes the only item as primary");
        suite.Expect(selection.Add(fixture.sibling, false) &&
                         selection.Primary() == fixture.parent,
                     "Add can preserve an explicit primary");
        const auto ordered = selection.OrderedItems();
        suite.Expect(ordered.size() == 2 && ordered[0] == fixture.parent &&
                         ordered[1] == fixture.sibling,
                     "selection order is insertion order, never unordered-set order");
        suite.Expect(selection.SetPrimary(fixture.sibling) &&
                         selection.Primary() == fixture.sibling,
                     "SetPrimary chooses an existing secondary item");
        suite.Expect(!selection.Toggle(fixture.sibling) &&
                         selection.Primary() == fixture.parent,
                     "Toggle removes a selected primary and chooses deterministic fallback");
        suite.Expect(selection.Toggle(fixture.child) && selection.Remove(fixture.parent) &&
                         selection.Primary() == fixture.child,
                     "Toggle adds and Remove preserves the last ordered item as fallback");
        selection.Clear();
        suite.Expect(selection.Empty() && selection.Primary().Empty(),
                     "Clear removes both membership and primary state");
    });

    suite.Run("CanonicalizeParentAndChild_PromotesSelectedAncestor", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& selection = fixture.session.Selection();
        selection.Replace({fixture.parent, fixture.child, fixture.sibling}, fixture.child);
        selection.Canonicalize(fixture.session.DocumentView());
        const auto ordered = selection.OrderedItems();
        suite.Expect(ordered.size() == 2 && ordered[0] == fixture.parent &&
                         ordered[1] == fixture.sibling &&
                         selection.Primary() == fixture.parent,
                     "ancestor/descendant duplicates collapse without losing user-visible order");
    });

    suite.Run("ScopeEnterExitAndPrune_RejectsOutsideOrInvalidNodes", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& selection = fixture.session.Selection();
        selection.Replace({fixture.child, fixture.sibling}, fixture.sibling);
        suite.Expect(selection.SetScope(fixture.parent, fixture.session.DocumentView()) &&
                         selection.Size() == 1 && selection.Primary() == fixture.child,
                     "entering scope prunes selections outside the scoped subtree");
        const px::Uuid invalid = px::Uuid::FromName("PrismatiX.Test.InvalidSelection");
        selection.Add(invalid);
        selection.Prune(fixture.session.DocumentView());
        suite.Expect(!selection.Contains(invalid) && selection.Primary() == fixture.child,
                     "Prune removes stale IDs without disturbing a valid primary");
        suite.Expect(selection.ExitScope(fixture.session.DocumentView()) &&
                         selection.Scope() == fixture.root,
                     "ExitScope moves to the parent scope deterministically");
        suite.Expect(selection.ExitScope(fixture.session.DocumentView()) &&
                         selection.Scope().Empty(),
                     "exiting root scope returns to the whole document");
    });

    suite.Run("StructuralCommands_KeepSelectionValidAcrossDeleteDuplicateReparentUndoRedo", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& selection = fixture.session.Selection();
        auto& commands = fixture.session.Commands();
        selection.Replace(fixture.parent);
        suite.Expect(static_cast<bool>(commands.DuplicateSelection()),
                     "DuplicateSelection succeeds");
        const px::Uuid duplicate = selection.Primary();
        suite.Expect(duplicate != fixture.parent &&
                         fixture.session.DocumentView().Contains(duplicate),
                     "duplicate result becomes the valid primary selection");
        suite.Expect(static_cast<bool>(commands.Reparent(duplicate, fixture.sibling, 0)) &&
                         selection.Primary() == duplicate,
                     "reparent preserves selection identity");
        suite.Expect(static_cast<bool>(commands.Undo()) &&
                         selection.Primary() == duplicate &&
                         fixture.session.DocumentView().Contains(duplicate) &&
                         static_cast<bool>(commands.Redo()) &&
                         selection.Primary() == duplicate,
                     "undo/redo keep a surviving selected ID valid");
        suite.Expect(static_cast<bool>(commands.DeleteSelection()) &&
                         selection.Primary() == fixture.sibling,
                     "deleting selection chooses its parent as deterministic fallback");
    });

    return suite.Finish();
}
