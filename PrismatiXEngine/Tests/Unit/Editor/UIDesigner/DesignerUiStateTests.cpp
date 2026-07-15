#include "Editor/Tools/UIDesigner/DesignerUiState.h"
#include "Tests/TestSupport/DesignerFixture.h"

int main() {
    px::test::Suite suite("DesignerUiState");

    suite.Run("PresentationState_DistinguishesDocumentSelectionAndManagedLayout", [&] {
        px::editor::UIDesignerSession empty;
        suite.Expect(px::editor::CaptureDesignerUiState(empty).selection ==
                         px::editor::DesignerSelectionPresentation::NoDocument,
                     "no document is distinct from no selection");
        px::test::DesignerFixture fixture(suite);
        fixture.session.Selection().Clear();
        suite.Expect(px::editor::CaptureDesignerUiState(fixture.session).selection ==
                         px::editor::DesignerSelectionPresentation::None,
                     "empty document selection has its own presentation state");
        fixture.session.Selection().Replace(fixture.parent);
        suite.Expect(px::editor::CaptureDesignerUiState(fixture.session).selection ==
                         px::editor::DesignerSelectionPresentation::Single,
                     "single selection is observable");
        fixture.session.Selection().Replace({fixture.parent, fixture.sibling},
                                            fixture.sibling);
        fixture.session.DocumentView().SetChildPolicy(
            fixture.root, px::ui::ChildLayoutPolicy::LinearY);
        const auto managed = px::editor::CaptureDesignerUiState(fixture.session);
        suite.Expect(managed.selection ==
                         px::editor::DesignerSelectionPresentation::Multiple &&
                         managed.positionManaged &&
                         managed.parentPolicy == px::ui::ChildLayoutPolicy::LinearY,
                     "multi-selection and its controlling managed policy are observable");
    });

    suite.Run("WorkspaceGeometry_CollapsesPanelsBeforeCanvas", [&] {
        const auto normal = px::editor::CalculateDesignerWorkspaceGeometry(
            {.width = 1600, .height = 800});
        const auto compact = px::editor::CalculateDesignerWorkspaceGeometry(
            {.width = 900, .height = 600});
        const auto narrow = px::editor::CalculateDesignerWorkspaceGeometry(
            {.width = 740, .height = 600});
        const auto drawer = px::editor::CalculateDesignerWorkspaceGeometry(
            {.width = 1600, .height = 800, .bottomPanelVisible = true});
        suite.Expect(normal.showLeft && normal.showRight &&
                         normal.canvas == px::editor::DesignerUiRect{260, 0, 1000, 800} &&
                         compact.compact && !compact.showLeft && compact.showRight &&
                         !narrow.showLeft && !narrow.showRight &&
                         drawer.showBottom && drawer.bottomDrawer.height == 240,
                     "responsive geometry preserves usable canvas and deterministic drawer");
    });

    suite.Run("PreviewFixture_SupportsDpiScaleLocaleAndCjk", [&] {
        px::editor::PreviewFixture preview;
        const auto text = preview.Read("game.dialogue.text");
        const auto* value = text ? text.Value().TryGet<std::string>() : nullptr;
        suite.Expect(static_cast<bool>(preview.SelectDevice("high-dpi")) &&
                         preview.Context().dpiScale == 2.0f &&
                         static_cast<bool>(preview.SetUIScale(1.5f)) &&
                         preview.Context().uiScale == 1.5f && value &&
                         value->find("預覽文字") != std::string::npos,
                     "preview state supports Traditional Chinese at scaled DPI/UI");
    });

    return suite.Finish();
}
