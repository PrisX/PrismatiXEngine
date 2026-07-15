#include "Editor/Build/BuildService.h"
#include "Editor/Project/ProjectService.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Startup/SplashTypes.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

}  // namespace

int main(int argc, char** argv) {
    px::test::Suite suite("BuildPipelineE2E");
    if (argc == 0 || !argv || !argv[0]) {
        suite.Expect(false, "test executable path is available",
                     "argv[0] is required to locate Player and fixture assets");
        return suite.Finish();
    }

    const auto executableDirectory =
        std::filesystem::absolute(argv[0]).parent_path();
    px::test::TempDirectory projectRoot("prismatix-build-e2e");
    px::editor::ProjectService project;

    bool created = false;
    suite.Run("CreateProject_ScaffoldsVersionedRuntimeResources", [&] {
        created = static_cast<bool>(project.Create(
            projectRoot.path, "Build E2E",
            executableDirectory / "EditorAssets" / "UIFont.ttf"));
        suite.Expect(created, "ProjectService creates a runnable project",
                     "root=" + projectRoot.path.generic_string());
        if (!created) return;
        for (const char* asset : {"dialog-stage.png", "notice.png"}) {
            const auto path = projectRoot.path / "Content/Images/UI/HUD" / asset;
            suite.Expect(std::filesystem::is_regular_file(path),
                         std::string("official project template contains ") + asset,
                         "expected=" + path.generic_string());
        }
        for (const char* asset : {
                 "Content/UI/Splash/PrismatiXEngine.pxscene",
                 "Content/Images/UI/Splash/PrismatiXEngine_Logo.png",
                 "Content/Audio/SFX/Splash/logo.wav"}) {
            const auto path = projectRoot.path / asset;
            suite.Expect(std::filesystem::is_regular_file(path),
                         std::string("default splash scaffold contains ") + asset,
                         "expected=" + path.generic_string());
        }
        suite.Expect(project.Context().manifest.splashes.size() == 1 &&
                         !project.Context().manifest.splashes.front().scene.id.Empty() &&
                         project.Context().manifest.splashes.front().audio &&
                         !project.Context().manifest.splashes.front().audio->id.Empty(),
                     "new project immediately owns one identity-safe default splash");

        const auto splashPath =
            projectRoot.path / "Content/UI/Splash/PrismatiXEngine.pxscene";
        const auto splashDocument = px::resource::ParseTypedDocument(
            ReadAll(splashPath), splashPath.generic_string());
        suite.Require(splashDocument && splashDocument.Value().type == "UIScene",
                      "default splash is an ordinary typed UIScene");
        suite.Require(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                      "runtime UI metadata is available for scaffold validation");
        px::ui::FormatterRegistry formatters;
        const auto instantiated = px::ui::InstantiateUIScene(
            splashDocument.Value(), nullptr, formatters);
        suite.Expect(static_cast<bool>(instantiated),
                     "default splash instantiates through the production UIScene loader");
        const auto dependencies = splashDocument.Value().properties.find("dependencies");
        const auto* dependencyArray = dependencies == splashDocument.Value().properties.end()
                                          ? nullptr
                                          : dependencies->second.AsArray();
        const auto* logoReference = dependencyArray && !dependencyArray->empty()
                                        ? dependencyArray->front()
                                              .TryGet<px::ResourceRefValue>()
                                        : nullptr;
        px::resource::AssetRegistry identities;
        suite.Require(static_cast<bool>(identities.Scan(projectRoot.path)),
                      "scaffold asset identities scan");
        const auto* logoAsset = identities.FindPath(
            projectRoot.path /
            "Content/Images/UI/Splash/PrismatiXEngine_Logo.png");
        suite.Expect(logoReference && logoAsset &&
                         logoReference->id == logoAsset->id &&
                         logoReference->lastKnownPath ==
                             "Content/Images/UI/Splash/PrismatiXEngine_Logo.png",
                     "default splash declares its Logo as an identity-safe dependency");
        const auto logo = std::find_if(
            splashDocument.Value().nodes.begin(), splashDocument.Value().nodes.end(),
            [](const auto& node) { return node.name == "PrismatiXEngineLogo"; });
        suite.Expect(logo != splashDocument.Value().nodes.end() &&
                         logo->properties.at("path").TryGet<std::string>() &&
                         *logo->properties.at("path").TryGet<std::string>() ==
                             "Content/Images/UI/Splash/PrismatiXEngine_Logo.png" &&
                         logo->properties.at("scaleMode").TryGet<std::string>() &&
                         *logo->properties.at("scaleMode").TryGet<std::string>() == "Fit",
                     "default scene uses a centered-fit Logo control");
        const auto animations = px::ui::ParseUIAnimationLibrary(
            splashDocument.Value().properties.at("animations"),
            splashPath.generic_string());
        suite.Expect(animations && animations.Value().machine.FindState("enter") &&
                         animations.Value().machine.FindState("exit"),
                     "default scene owns editable enter and exit animation states");
    });
    if (!created) return suite.Finish();

    bool opened = false;
    suite.Run("OpenProject_ReloadsTheCreatedManifest", [&] {
        opened = static_cast<bool>(project.Open(projectRoot.path));
        suite.Expect(opened, "newly created project reopens",
                     "root=" + projectRoot.path.generic_string());
    });
    if (!opened) return suite.Finish();

    px::editor::BuildOptions options;
    options.projectRoot = projectRoot.path;
    options.outputDir = projectRoot.path / "Export";
#ifdef _WIN32
    options.playerExe = executableDirectory / "PrismatiXPlayer.exe";
#else
    options.playerExe = executableDirectory / "PrismatiXPlayer";
#endif
    options.title = project.Context().manifest.name;
    options.startRoute = project.Context().manifest.startRoute;
    options.routes = project.Context().manifest.routes;
    options.startScript = project.Context().manifest.startScript;
    options.splashes = project.Context().manifest.splashes;
    options.encrypt = true;
    options.key = "e2e-key";

    bool built = false;
    suite.Run("BuildProject_ProducesStrictEncryptedDistribution", [&] {
        built = static_cast<bool>(px::editor::BuildService{}.Build(options));
        suite.Expect(built, "BuildService completes the user-facing export flow",
                     "output=" + options.outputDir.generic_string());
        if (!built) return;

        const auto packagePath = options.outputDir / "game.pxpackage";
        const auto package = px::resource::ParseTypedDocument(
            ReadAll(packagePath), packagePath.generic_string());
        suite.Expect(package && package.Value().formatVersion ==
                                    px::resource::TypedDocument::CurrentVersion,
                     "export metadata uses the current strict typed format",
                     "package=" + packagePath.generic_string());
        if (package) {
            const auto found = package.Value().properties.find("splashes");
            const auto splashes = found == package.Value().properties.end()
                                      ? px::Result<std::vector<px::ui::startup::SplashScreenEntry>>{}
                                      : px::ui::startup::ParseSplashSequence(
                                            found->second, packagePath.generic_string());
            suite.Expect(splashes && splashes.Value() == options.splashes,
                         "GamePackage preserves authored splash order and identities");
        }
    });
    if (!built) return suite.Finish();

    suite.Run("PackagedArchive_ContainsRunnableEntryPointsAndOfficialHUD", [&] {
        px::io::Archive archive;
        const auto key = px::crypto::DeriveKey(options.key);
        const auto archivePath = options.outputDir / "Content.pdx";
        const bool openedArchive = archive.Open(archivePath.string(), &key);
        suite.Expect(openedArchive, "encrypted Content.pdx opens with the export key",
                     "archive=" + archivePath.generic_string());
        if (!openedArchive) return;

        const std::vector<std::string> requiredPaths{
            options.startScript,
            project.Context().StartScenePath(),
            "Content/UI/Splash/PrismatiXEngine.pxscene",
            "Content/Images/UI/Splash/PrismatiXEngine_Logo.png",
            "Content/Audio/SFX/Splash/logo.wav",
            "Content/Images/UI/HUD/dialog-stage.png",
            "Content/Images/UI/HUD/notice.png"};
        for (const std::string& path : requiredPaths) {
            suite.Expect(archive.Contains(path), "packaged archive contains " + path,
                         "archive=" + archivePath.generic_string());
        }
    });

    suite.Run("BrokenSplashScene_BlocksReleaseBuild", [&] {
        const auto scenePath =
            projectRoot.path / "Content/UI/Splash/PrismatiXEngine.pxscene";
        auto scene = px::resource::ParseTypedDocument(ReadAll(scenePath),
                                                       scenePath.generic_string());
        suite.Require(static_cast<bool>(scene),
                      "valid splash fixture parses before failure injection");
        scene.Value().type = "NotUIScene";
        {
            std::ofstream output(scenePath, std::ios::binary | std::ios::trunc);
            output << px::resource::WriteTypedDocument(scene.Value());
        }
        auto brokenOptions = options;
        brokenOptions.outputDir = projectRoot.path / "BrokenExport";
        suite.Expect(!px::editor::BuildService{}.Build(brokenOptions),
                     "release build rejects a configured splash that is not UIScene");
    });

    return suite.Finish();
}
