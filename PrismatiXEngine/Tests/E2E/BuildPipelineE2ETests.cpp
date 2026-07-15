#include "Editor/Build/BuildService.h"
#include "Editor/Project/ProjectService.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Resources/TypedDocument.h"
#include "Tests/TestSupport/TestHarness.h"

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
            "Content/Images/UI/HUD/dialog-stage.png",
            "Content/Images/UI/HUD/notice.png"};
        for (const std::string& path : requiredPaths) {
            suite.Expect(archive.Contains(path), "packaged archive contains " + path,
                         "archive=" + archivePath.generic_string());
        }
    });

    return suite.Finish();
}
