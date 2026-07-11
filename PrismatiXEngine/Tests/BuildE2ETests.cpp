#include "Editor/Build/BuildService.h"
#include "Editor/Project/ProjectService.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Resources/TypedDocument.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _MSC_VER
#pragma warning(disable : 4100)
#endif
int main(int argc,char** argv){const auto root=std::filesystem::temp_directory_path()/("prismatix-build-e2e-"+px::Uuid::Random().ToString());std::filesystem::create_directories(root);struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{root};px::editor::ProjectService project;if(!project.Create(root,"Commercial Acceptance",{})){std::cerr<<"project creation failed\n";return 1;}px::editor::BuildOptions options;options.projectRoot=root;options.outputDir=root/"Export";options.playerExe=std::filesystem::absolute(argv[0]).parent_path()/
#ifdef _WIN32
"PrismatiXPlayer.exe";
#else
"PrismatiXPlayer";
#endif
options.title=project.Context().manifest.name;options.startRoute=project.Context().manifest.startRoute;options.routes=project.Context().manifest.routes;options.startScript=project.Context().manifest.startScript;options.encrypt=true;options.key="e2e-key";if(!px::editor::BuildService{}.Build(options)){std::cerr<<"build failed\n";return 2;}std::ifstream packageStream(options.outputDir/"game.pxpackage",std::ios::binary);std::ostringstream packageText;packageText<<packageStream.rdbuf();const auto package=px::resource::ParseTypedDocument(packageText.str(),"game.pxpackage");if(!package||package.Value().formatVersion!=3){std::cerr<<"strict package invalid\n";return 3;}px::io::Archive archive;const auto key=px::crypto::DeriveKey("e2e-key");if(!archive.Open((options.outputDir/"Content.pdx").string(),&key)||!archive.Contains(options.startScript)||!archive.Contains(project.Context().StartScenePath())){std::cerr<<"packaged content invalid\n";return 4;}return 0;}
