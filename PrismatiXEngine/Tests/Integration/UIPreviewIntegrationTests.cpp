#include "Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/DesignerFixture.h"

#include <memory>
#include <optional>

namespace {

class FakePreviewHost {
public:
    bool Load(const px::resource::TypedDocument& document) {
        px::ui::FormatterRegistry formatters;
        auto loaded = px::ui::InstantiateUIScene(document, nullptr, formatters);
        if (!loaded) return false;
        m_root = std::move(loaded.Value().root);
        (void)m_root->Measure({800, 600});
        m_root->Arrange({0, 0, 800, 600});
        return true;
    }

    [[nodiscard]] px::ui::Control* Find(const px::Uuid& id) const {
        return m_root ? dynamic_cast<px::ui::Control*>(m_root->Find(id)) : nullptr;
    }

    [[nodiscard]] std::optional<px::Rect> Layout(const px::Uuid& id) const {
        const auto* control = Find(id);
        return control ? std::optional<px::Rect>(control->LayoutRect()) : std::nullopt;
    }

private:
    std::unique_ptr<px::ui::Control> m_root;
};

void ConfigureFreeLayout(px::test::DesignerFixture& fixture) {
    auto* root = fixture.session.Document()->Find(fixture.root);
    root->type = "Control";
    auto set = [&](const px::Uuid& id, px::Rect rect) {
        auto* node = fixture.session.Document()->Find(id);
        node->properties["anchors"] = px::Rect{};
        node->properties["offsets"] = rect;
        fixture.session.DocumentView().SetLayoutRect(id, rect);
    };
    set(fixture.parent, {20, 20, 160, 100});
    set(fixture.child, {40, 40, 80, 40});
    set(fixture.sibling, {30, 30, 160, 100});
    (void)fixture.session.DocumentView().Rebuild(*fixture.session.Document());
    fixture.ResetLayout();
}

void ApplyLayoutChange(px::test::DesignerFixture& fixture, const px::Uuid& node) {
    const auto value = fixture.session.Document()->ReadProperty(node, "offsets");
    if (value)
        if (const auto* rect = value.Value().TryGet<px::Rect>())
            fixture.session.DocumentView().SetLayoutRect(node, *rect);
}

bool Parity(px::test::DesignerFixture& fixture, FakePreviewHost& preview,
            const px::Uuid& node, px::Rect expected) {
    const auto authored = fixture.session.Document()->ReadProperty(node, "offsets");
    const auto* authoredRect = authored ? authored.Value().TryGet<px::Rect>() : nullptr;
    return authoredRect && *authoredRect == expected &&
           fixture.session.DocumentView().LayoutRect(node) == expected &&
           preview.Load(fixture.session.Document()->Data()) &&
           preview.Layout(node) == expected;
}

}  // namespace

int main() {
    px::test::Suite suite("UIPreviewIntegration");
    suite.Run("ChangePlanner_MapsEveryInvalidationDomain", [&] {
        const px::Uuid document = px::Uuid::FromName("PrismatiX.Preview.Document");
        const px::Uuid node = px::Uuid::FromName("PrismatiX.Preview.Node");
        using px::editor::DesignerDirtyFlags;
        using px::editor::DocumentChangeSet;
        using px::editor::PreviewUpdate;
        using px::editor::HasPreviewUpdate;

        const auto paint = px::editor::PlanPreviewUpdate(
            DocumentChangeSet::Property(node, "text", DesignerDirtyFlags::Paint), document);
        const auto layout = px::editor::PlanPreviewUpdate(
            DocumentChangeSet::Property(node, "offsets",
                                        DesignerDirtyFlags::Layout |
                                            DesignerDirtyFlags::Paint),
            document);
        const auto structure = px::editor::PlanPreviewUpdate(
            DocumentChangeSet::Structure(node), document);
        const auto theme = px::editor::PlanPreviewUpdate(
            DocumentChangeSet::Property(node, "styleBinding", DesignerDirtyFlags::Theme),
            document);
        const auto binding = px::editor::PlanPreviewUpdate(
            DocumentChangeSet::Property(node, "bindings", DesignerDirtyFlags::Binding),
            document);
        const auto animation = px::editor::PlanPreviewUpdate(
            DocumentChangeSet::Property(document, "animations",
                                        DesignerDirtyFlags::Animation),
            document);
        suite.Expect(HasPreviewUpdate(paint, PreviewUpdate::PatchProperties) &&
                         !HasPreviewUpdate(paint, PreviewUpdate::Relayout) &&
                         HasPreviewUpdate(layout, PreviewUpdate::PatchProperties) &&
                         HasPreviewUpdate(layout, PreviewUpdate::Relayout) &&
                         HasPreviewUpdate(structure, PreviewUpdate::RebuildScene) &&
                         HasPreviewUpdate(theme, PreviewUpdate::InvalidateStyle) &&
                         HasPreviewUpdate(binding, PreviewUpdate::ReconnectBindings) &&
                         HasPreviewUpdate(animation, PreviewUpdate::UpdateAnimations),
                     "paint/layout/structure/theme/binding/animation have distinct plans");
    });

    suite.Run("MoveResizeUndoRedoCancel_MaintainDocumentLayoutPreviewParity", [&] {
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "preview integration registers runtime UI metadata");
        px::test::DesignerFixture fixture(suite);
        ConfigureFreeLayout(fixture);
        FakePreviewHost preview;
        px::editor::DocumentChangeSet lastChange;
        fixture.session.SetChangeListener(
            [&](const px::editor::DocumentChangeSet& change) { lastChange = change; });

        const px::Rect moved{100, 90, 160, 100};
        suite.Expect(static_cast<bool>(fixture.session.Commands().SetProperty(
                         fixture.parent, "offsets", moved, "Move",
                         px::editor::DesignerDirtyFlags::Layout)),
                     "move command succeeds");
        ApplyLayoutChange(fixture, fixture.parent);
        suite.Expect(px::editor::HasDesignerUpdate(
                         px::editor::PlanDesignerUpdate(lastChange),
                         px::editor::DesignerUpdate::Relayout) &&
                         Parity(fixture, preview, fixture.parent, moved),
                     "move yields authored == Designer cache == runtime preview rect");

        suite.Expect(static_cast<bool>(fixture.session.Commands().Undo()),
                     "move undo succeeds");
        ApplyLayoutChange(fixture, fixture.parent);
        suite.Expect(Parity(fixture, preview, fixture.parent, {20, 20, 160, 100}),
                     "undo restores preview parity");
        suite.Expect(static_cast<bool>(fixture.session.Commands().Redo()),
                     "move redo succeeds");
        ApplyLayoutChange(fixture, fixture.parent);
        suite.Expect(Parity(fixture, preview, fixture.parent, moved),
                     "redo restores preview parity");

        const px::Rect resized{100, 90, 240, 140};
        suite.Expect(static_cast<bool>(fixture.session.Commands().SetProperty(
                         fixture.parent, "offsets", resized, "Resize",
                         px::editor::DesignerDirtyFlags::Layout)),
                     "resize command succeeds");
        ApplyLayoutChange(fixture, fixture.parent);
        suite.Expect(Parity(fixture, preview, fixture.parent, resized),
                     "resize preserves document/layout/runtime parity");

        const auto cursor = fixture.session.Commands().HistoryCursor();
        suite.Expect(static_cast<bool>(fixture.session.Commands().BeginPropertyGesture(
                         fixture.parent, "offsets", "Cancelled move",
                         px::editor::DesignerDirtyFlags::Layout)) &&
                         static_cast<bool>(fixture.session.Commands().UpdatePropertyGesture(
                             px::Rect{500, 500, 240, 140})),
                     "cancel scenario begins and updates");
        fixture.session.DocumentView().SetLayoutRect(fixture.parent,
                                                     {500, 500, 240, 140});
        suite.Expect(static_cast<bool>(fixture.session.Commands().CancelPropertyGesture()) &&
                         fixture.session.Commands().HistoryCursor() == cursor &&
                         Parity(fixture, preview, fixture.parent, resized),
                     "cancel restores parity and creates no history entry");
    });

    suite.Run("StyleAndStructureEdits_AppearInRuntimeObservableState", [&] {
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "style/structure preview registers runtime UI metadata");
        px::test::DesignerFixture fixture(suite);
        ConfigureFreeLayout(fixture);
        FakePreviewHost preview;

        px::ui::ControlStyleBinding binding;
        const px::Color color{12, 34, 56, 255};
        binding.localOverrides["background.color"] =
            px::ui::StyleValue::Literal(color);
        suite.Expect(static_cast<bool>(fixture.session.Commands().SetProperty(
                         fixture.parent, "styleBinding",
                         px::ui::WriteStyleBinding(binding), "Style edit",
                         px::editor::DesignerDirtyFlags::Theme)) &&
                         preview.Load(fixture.session.Document()->Data()),
                     "style edit loads into runtime preview");
        const auto* runtimeParent = preview.Find(fixture.parent);
        bool styleMatches = false;
        if (runtimeParent) {
            const auto& overrides = runtimeParent->StyleBinding().localOverrides;
            const auto style = overrides.find("background.color");
            styleMatches = style != overrides.end() && style->second.IsLiteral() &&
                           style->second.LiteralValue() == px::Variant(color);
        }
        suite.Expect(styleMatches,
                     "runtime observable style binding matches authored local override");

        fixture.session.Selection().Replace(fixture.parent);
        suite.Expect(static_cast<bool>(fixture.session.Commands().DuplicateSelection()),
                     "structural duplicate succeeds");
        const px::Uuid duplicate = fixture.session.Selection().Primary();
        suite.Expect(preview.Load(fixture.session.Document()->Data()) &&
                         preview.Find(duplicate) != nullptr &&
                         fixture.session.DocumentView().Contains(duplicate),
                     "structure edit appears in both Designer index and runtime preview tree");
    });

    return suite.Finish();
}
