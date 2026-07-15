#include "Editor/Tools/UIDesigner/DesignerDiagnostics.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/DesignerFixture.h"

#include <algorithm>

namespace {

bool HasCode(const px::editor::DesignerDiagnostics& diagnostics,
             std::string_view code) {
    return std::ranges::any_of(diagnostics.Items(),
                               [&](const auto& item) { return item.code == code; });
}

}  // namespace

int main() {
    px::test::Suite suite("DesignerDiagnostics");

    suite.Run("DefaultOverload_CompilesAndClearsState", [&] {
        px::test::DesignerFixture fixture(suite);
        px::editor::DesignerDiagnostics diagnostics;
        diagnostics.Refresh(*fixture.session.Document());
        suite.Expect(diagnostics.Items().empty(),
                     "default Refresh overload validates a clean document");
        diagnostics.Clear();
        suite.Expect(diagnostics.ErrorCount() == 0 && diagnostics.WarningCount() == 0,
                     "Clear resets all diagnostic indexes and counts");
    });

    suite.Run("TreeGeometryAndTokenProblems_AreFocusedAndQueryable", [&] {
        px::test::DesignerFixture fixture(suite);
        auto& document = *fixture.session.Document();
        document.Find(fixture.parent)->properties["anchors"] = px::Rect{1, 1, 0, 0};
        document.Find(fixture.parent)->properties["minimumSize"] = px::Vec2{100, 100};
        document.Find(fixture.parent)->properties["maximumSize"] = px::Vec2{50, 50};
        document.Find(fixture.child)->parent =
            px::Uuid::FromName("PrismatiX.Test.MissingParent");
        document.Data().properties["theme.tokens"] = px::VariantObject{
            {"a", px::TokenRefValue{"b"}}, {"b", px::TokenRefValue{"a"}}};
        document.Find(fixture.parent)->properties["background.color"] =
            px::TokenRefValue{"missing"};

        px::editor::DesignerDiagnostics diagnostics;
        diagnostics.Refresh(document);
        suite.Expect(HasCode(diagnostics, "PXEDUIP5012") &&
                         HasCode(diagnostics, "PXEDUIP5013") &&
                         HasCode(diagnostics, "PXEDUIP5014") &&
                         HasCode(diagnostics, "PXEDUIP5001") &&
                         HasCode(diagnostics, "PXEDUIP5003"),
                     "missing parent, invalid geometry, token cycles, and missing tokens are "
                     "separate product contracts");
        suite.Expect(diagnostics.HasProblem(fixture.parent) &&
                         !diagnostics.ForProperty(fixture.parent, "anchors").empty() &&
                         diagnostics.ErrorCount() >= 5,
                     "node/property lookup exposes focused errors for Inspector and Problems UI");
    });

    suite.Run("ValidationContext_ReportsResourcesBindingsActionsAndManagedLayout", [&] {
        px::test::DesignerFixture fixture(suite);
        suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                     "built-in metadata is registered for contextual diagnostics");
        auto* parent = fixture.session.Document()->Find(fixture.parent);
        parent->type = "TextureRect";
        parent->properties["path"] = std::string("Content/Images/missing.png");
        auto* child = fixture.session.Document()->Find(fixture.child);
        child->type = "Button";
        child->properties["offsets"] = px::Rect{0, 0, 100, 40};
        child->properties["bindings"] = px::VariantObject{
            {"text", px::VariantObject{{"path", std::string("missing.path")}}}};
        child->properties["triggers"] = px::VariantObject{
            {"activated", px::VariantObject{{"kind", std::string("action")},
                                             {"action", std::string("missing.action")},
                                             {"arguments", px::VariantObject{}},
                                             {"reentry", std::string("Allow")}}}};

        px::editor::DesignerDiagnostics::ValidationContext context;
        context.resourceExists = [](std::string_view, std::string_view) { return false; };
        context.childPolicy = [&](const px::Uuid& parent) {
            return parent == fixture.parent ? px::ui::ChildLayoutPolicy::LinearY
                                            : px::ui::ChildLayoutPolicy::Free;
        };
        context.describeBinding = [](std::string_view)
            -> std::optional<px::ui::PropertyPathInfo> { return std::nullopt; };
        context.validateAction = [](std::string_view, const px::VariantObject&,
                                    const px::diag::Source& source) {
            px::diag::Diagnostic diagnostic{.severity = px::diag::Severity::Error,
                                             .code = "PXTEST-ACTION-MISSING",
                                             .category = "Test.Action",
                                             .message = "Action does not exist"};
            diagnostic.source = source;
            return std::vector<px::diag::Diagnostic>{std::move(diagnostic)};
        };

        px::editor::DesignerDiagnostics diagnostics;
        diagnostics.Refresh(*fixture.session.Document(), context);
        suite.Expect(HasCode(diagnostics, "PXEDUIP5015") &&
                         HasCode(diagnostics, "PXEDUIP5016") &&
                         HasCode(diagnostics, "PXEDUIP5033") &&
                         HasCode(diagnostics, "PXTEST-ACTION-MISSING") &&
                         diagnostics.WarningCount() >= 1,
                     "managed offsets, unresolved bindings, and invalid Actions stay distinct");
    });

    return suite.Finish();
}
