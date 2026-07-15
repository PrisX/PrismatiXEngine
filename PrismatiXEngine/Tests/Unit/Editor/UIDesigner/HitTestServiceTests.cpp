#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"
#include "Tests/TestSupport/DesignerFixture.h"

#include <limits>

int main() {
    px::test::Suite suite("HitTestService");

    suite.Run("OverlappingSiblings_UseReverseAuthoredDrawOrder", [&] {
        px::test::DesignerFixture fixture(suite);
        px::editor::HitTestService hitTest;
        const auto hits = hitTest.HitStack(*fixture.session.Document(),
                                           fixture.session.DocumentView(), {50, 50});
        suite.Expect(hits.size() == 3 && hits[0] == fixture.sibling &&
                         hits[1] == fixture.child && hits[2] == fixture.parent &&
                         hitTest.Topmost(*fixture.session.Document(),
                                         fixture.session.DocumentView(), {50, 50}) ==
                             fixture.sibling,
                     "topmost and overlap stack follow reverse sibling draw order");
    });

    suite.Run("LockedHiddenCollapsedAndInvalidGeometry_AreNeverSelectable", [&] {
        px::test::DesignerFixture fixture(suite);
        px::editor::HitTestService hitTest;
        fixture.session.Document()->Find(fixture.sibling)->properties["editorLocked"] = true;
        fixture.session.Document()->Find(fixture.child)->properties["visibility"] =
            std::string("Hidden");
        fixture.session.Document()->Find(fixture.parent)->properties["visibility"] =
            std::string("Collapsed");
        suite.Expect(hitTest.HitStack(*fixture.session.Document(),
                                      fixture.session.DocumentView(), {50, 50}).empty(),
                     "locked, hidden, and collapsed nodes are excluded");

        fixture.session.Document()->Find(fixture.parent)->properties["visibility"] =
            std::string("Visible");
        fixture.session.DocumentView().SetLayoutRect(fixture.parent, {50, 50, 0, 0});
        suite.Expect(hitTest.Topmost(*fixture.session.Document(),
                                     fixture.session.DocumentView(), {50, 50}).Empty(),
                     "zero-size geometry is not a one-pixel hit target");
        fixture.session.DocumentView().SetLayoutRect(
            fixture.parent,
            {0, 0, std::numeric_limits<float>::quiet_NaN(), 100});
        suite.Expect(hitTest.Topmost(*fixture.session.Document(),
                                     fixture.session.DocumentView(), {0, 0}).Empty(),
                     "non-finite geometry is rejected deterministically");
    });

    suite.Run("ScopedNestedHitTest_StaysInsideActiveSubtree", [&] {
        px::test::DesignerFixture fixture(suite);
        px::editor::HitTestService hitTest;
        const auto hits = hitTest.HitStack(*fixture.session.Document(),
                                           fixture.session.DocumentView(), {50, 50},
                                           fixture.parent);
        suite.Expect(hits.size() == 2 && hits[0] == fixture.child &&
                         hits[1] == fixture.parent &&
                         hitTest.Topmost(*fixture.session.Document(),
                                         fixture.session.DocumentView(), {50, 50},
                                         fixture.parent) == fixture.child,
                     "scope includes its nested children and excludes overlapping outsiders");
    });

    return suite.Finish();
}
