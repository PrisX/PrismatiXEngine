#include "Engine/UI/Control.h"
#include "Engine/UI/Styles/StylePropertyRegistry.h"
#include "Engine/UI/Theme.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <set>

namespace {

bool RuntimeActuallyApplies(std::string_view property) {
    px::ui::Control control("Style parity");
    px::ui::ControlStyleBinding binding;
    if (property == "background.color")
        binding.localOverrides[std::string(property)] =
            px::ui::StyleValue::Literal(px::Color{1, 2, 3, 4});
    else if (property == "border.color")
        binding.localOverrides[std::string(property)] =
            px::ui::StyleValue::Literal(px::Color{5, 6, 7, 8});
    else if (property == "border.width")
        binding.localOverrides[std::string(property)] = px::ui::StyleValue::Literal(3.25);
    else if (property == "radius.all")
        binding.localOverrides[std::string(property)] = px::ui::StyleValue::Literal(7.5);
    else if (property == "padding")
        binding.localOverrides[std::string(property)] =
            px::ui::StyleValue::Literal(px::Vec2{31, 32});
    else if (property == "spacing")
        binding.localOverrides[std::string(property)] = px::ui::StyleValue::Literal(13.0);
    else if (property == "typography.font")
        binding.localOverrides[std::string(property)] =
            px::ui::StyleValue::Literal(std::string("Content/Fonts/Test.ttf"));
    else if (property == "typography.size")
        binding.localOverrides[std::string(property)] =
            px::ui::StyleValue::Literal(std::int64_t{37});
    else if (property == "typography.color")
        binding.localOverrides[std::string(property)] =
            px::ui::StyleValue::Literal(px::Color{9, 10, 11, 12});
    else
        return false;
    control.SetStyleBinding(std::move(binding));
    const px::ui::ControlStyle applied = px::ui::Theme{}.Resolve(control);
    if (property == "background.color")
        return applied.normal.background == px::Color{1, 2, 3, 4};
    if (property == "border.color")
        return applied.normal.border == px::Color{5, 6, 7, 8};
    if (property == "border.width") return applied.normal.borderWidth == 3.25f;
    if (property == "radius.all") return applied.normal.cornerRadius == 7.5f;
    if (property == "padding") return applied.normal.padding == px::Vec2{31, 32};
    if (property == "spacing") return applied.spacing == 13.0f;
    if (property == "typography.font")
        return applied.font == "Content/Fonts/Test.ttf";
    if (property == "typography.size") return applied.fontSize == 37;
    if (property == "typography.color")
        return applied.text == px::Color{9, 10, 11, 12};
    return false;
}

}  // namespace

int main() {
    px::test::Suite suite("StyleContract");

    suite.Run("RegistryRuntimeInspectorSets_AreExactlyEqual", [&] {
        const px::ui::StylePropertyRegistry registry;
        const auto mapped = px::ui::RuntimeMappedStyleProperties();
        std::set<std::string> registrySupported;
        std::set<std::string> runtimeMapped;
        std::set<std::string> inspectorExposable;
        for (const auto id : mapped) runtimeMapped.emplace(id);
        for (const auto* descriptor : registry.Descriptors()) {
            if (descriptor->runtimeSupported) registrySupported.insert(descriptor->id);
            // Authoring surfaces use this predicate before offering an override.
            if (registry.RuntimeSupports(descriptor->id, "Button"))
                inspectorExposable.insert(descriptor->id);
        }
        suite.Expect(registrySupported == runtimeMapped &&
                         inspectorExposable == runtimeMapped,
                     "registry-supported == runtime mapper == Inspector-exposable sets");
        suite.Expect(!registry.RuntimeSupports("opacity", "Button") &&
                         !inspectorExposable.contains("opacity"),
                     "unsupported style properties are absent from production Inspector choices");
    });

    suite.Run("EveryRuntimeMappedProperty_ChangesTheActualControlStyleField", [&] {
        bool exact = true;
        std::string missing;
        for (const auto property : px::ui::RuntimeMappedStyleProperties()) {
            if (RuntimeActuallyApplies(property)) continue;
            exact = false;
            if (!missing.empty()) missing += ", ";
            missing += property;
        }
        suite.Expect(exact, "every declared runtime mapper changes observable ControlStyle",
                     missing.empty() ? "all mapped" : missing);
    });

    suite.Run("EveryRegistryDescriptor_AgreesWithRuntimeMapperMembership", [&] {
        const px::ui::StylePropertyRegistry registry;
        const auto mapped = px::ui::RuntimeMappedStyleProperties();
        bool exact = true;
        for (const auto* descriptor : registry.Descriptors()) {
            const bool hasMapper =
                std::find(mapped.begin(), mapped.end(), descriptor->id) != mapped.end();
            exact &= descriptor->runtimeSupported == hasMapper;
        }
        suite.Expect(exact,
                     "runtimeSupported true has a mapper and every mapper is marked supported");
    });

    return suite.Finish();
}
