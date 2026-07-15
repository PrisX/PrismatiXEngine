#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Startup/SplashTypes.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

px::ui::startup::SplashScreenEntry Entry(const std::string& name, const float duration) {
    px::ui::startup::SplashScreenEntry entry;
    entry.scene = {px::Uuid::FromName("scene/" + name),
                   "Content/UI/Splash/" + name + ".pxscene"};
    entry.audio = px::ResourceRefValue{px::Uuid::FromName("audio/" + name),
                                       "Content/Audio/SFX/Splash/" + name + ".wav"};
    entry.minimumDuration = duration;
    return entry;
}

}  // namespace

int main() {
    px::test::Suite suite("SplashContract");

    suite.Run("OrderedSequence_RoundTripsResourceIdentity", [&] {
        const std::vector authored{Entry("Engine", 2.0f), Entry("Publisher", 2.5f),
                                   Entry("Studio", 1.5f)};
        px::resource::TypedDocument manifest;
        manifest.kind = px::resource::DocumentKind::Project;
        manifest.id = px::Uuid::Random();
        manifest.type = "PrismatiXProject";
        manifest.properties["splashes"] = px::ui::startup::WriteSplashSequence(authored);

        const auto parsedDocument = px::resource::ParseTypedDocument(
            px::resource::WriteTypedDocument(manifest), "project.pxproject");
        suite.Require(static_cast<bool>(parsedDocument),
                      "typed project manifest containing splashes parses");
        const auto parsed = px::ui::startup::ParseSplashSequence(
            parsedDocument.Value().properties.at("splashes"), "project.pxproject");
        suite.Require(static_cast<bool>(parsed), "typed splash array parses");
        suite.Expect(parsed.Value() == authored,
                     "array order and every ResourceRef identity survive round-trip");
    });

    suite.Run("EmptySequence_IsValid", [&] {
        const auto parsed = px::ui::startup::ParseSplashSequence(
            px::ui::startup::WriteSplashSequence({}), "empty project");
        suite.Expect(parsed && parsed.Value().empty(),
                     "empty splashes is a clean direct-to-title contract");
    });

    suite.Run("InvalidEntry_FailsAtContractBoundary", [&] {
        auto invalidScene = Entry("Invalid", 2.0f);
        invalidScene.scene.id = {};
        auto invalidTiming = Entry("Timing", 1.0f);
        invalidTiming.skipAllowedAfter = 1.5f;
        suite.Expect(!px::ui::startup::ValidateSplashEntry(invalidScene, 0,
                                                           "project.pxproject") &&
                         !px::ui::startup::ValidateSplashEntry(invalidTiming, 1,
                                                               "project.pxproject"),
                     "missing identity and invalid skip timing are rejected");
        suite.Expect(!px::ui::startup::ParseSplashSequence(px::VariantObject{},
                                                           "project.pxproject"),
                     "splashes must have one authoritative ordered array shape");
    });

    return suite.Finish();
}
