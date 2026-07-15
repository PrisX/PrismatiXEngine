#include "Engine/UI/Actions/ActionCatalog.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

px::Variant Example(const px::ui::ActionArgumentDescriptor& argument) {
    if (argument.defaultValue) return argument.defaultValue->Clone();
    switch (argument.type) {
    case px::VariantType::Bool: return false;
    case px::VariantType::Integer: return std::int64_t{1};
    case px::VariantType::Number: return 1.0;
    case px::VariantType::String:
        return argument.enumValues.empty() ? px::Variant(std::string("value"))
                                           : px::Variant(argument.enumValues.front());
    case px::VariantType::Vec2: return px::Vec2{1, 2};
    case px::VariantType::Rect: return px::Rect{1, 2, 3, 4};
    case px::VariantType::Color: return px::Color{1, 2, 3, 255};
    case px::VariantType::ResourceRef:
        return px::ResourceRefValue{px::Uuid::FromName("PrismatiX.Action.Resource"),
                                    "Content/test.asset"};
    case px::VariantType::Uuid: return px::Uuid::FromName("PrismatiX.Action.Node");
    case px::VariantType::Array: return px::VariantArray{};
    case px::VariantType::Object: return px::VariantObject{};
    case px::VariantType::TokenRef: return px::TokenRefValue{"token"};
    case px::VariantType::Null: return {};
    }
    return {};
}

}  // namespace

int main() {
    px::test::Suite suite("ActionContract");

    suite.Run("EveryAvailableDescriptor_AcceptsItsTypedRequiredArguments", [&] {
        const auto& catalog = px::ui::ActionCatalog::Global();
        bool allValid = !catalog.Descriptors().empty();
        std::string failed;
        for (const auto& descriptor : catalog.Descriptors()) {
            if (!descriptor.available) continue;
            px::ui::ActionInvocation invocation{.action = descriptor.id};
            for (const auto& argument : descriptor.arguments)
                if (argument.required) invocation.arguments[argument.name] = Example(argument);
            if (catalog.ValidateAndNormalize(invocation)) continue;
            allValid = false;
            if (!failed.empty()) failed += ", ";
            failed += descriptor.id;
        }
        suite.Expect(allValid, "every available Action descriptor is self-consistent",
                     failed.empty() ? "all descriptors valid" : failed);
    });

    suite.Run("WrongTypeMissingRequiredAndUnknownArguments_AreRejected", [&] {
        auto& catalog = px::ui::ActionCatalog::Global();
        px::ui::ActionInvocation valid{
            .action = "choice.select", .arguments = {{"index", std::int64_t{2}}}};
        suite.Expect(static_cast<bool>(catalog.ValidateAndNormalize(valid)),
                     "known typed arguments validate");
        valid.arguments["index"] = std::string("two");
        suite.Expect(!catalog.ValidateAndNormalize(valid),
                     "mismatched argument type is rejected");
        valid.arguments.clear();
        suite.Expect(!catalog.ValidateAndNormalize(valid),
                     "missing required argument is rejected");
        valid.arguments = {{"index", std::int64_t{2}}, {"unknown", true}};
        suite.Expect(!catalog.ValidateAndNormalize(valid),
                     "unknown argument is rejected unless descriptor explicitly allows it");
    });

    suite.Run("CatalogRejectsDuplicateIdsAndInvalidDescriptors", [&] {
        px::ui::ActionCatalog catalog;
        px::ui::ActionDescriptor descriptor;
        descriptor.id = "test.action";
        descriptor.category = "Test";
        descriptor.displayName = "Test Action";
        suite.Expect(static_cast<bool>(catalog.Register(descriptor)) &&
                         !catalog.Register(descriptor),
                     "Action IDs are unique within the authoritative catalog");
        suite.Expect(!catalog.Register({}),
                     "descriptor without ID/name/category is rejected");
    });

    return suite.Finish();
}
