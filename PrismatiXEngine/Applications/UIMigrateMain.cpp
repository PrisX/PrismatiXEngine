#include "Engine/UI/UISchemaMigration.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

int main(int argc,char** argv){
    std::filesystem::path project;std::optional<bool> write;for(int index=1;index<argc;++index){const std::string argument=argv[index];if(argument=="--project"&&index+1<argc)project=argv[++index];else if(argument=="--write"){if(write){std::cerr<<"Choose exactly one of --check or --write\n";return 2;}write=true;}else if(argument=="--check"){if(write){std::cerr<<"Choose exactly one of --check or --write\n";return 2;}write=false;}else{std::cerr<<"Unknown argument: "<<argument<<"\n";return 2;}}
    if(project.empty()||!write){std::cerr<<"Usage: PrismatiXUIMigrate --project <root> (--check|--write)\n";return 2;}
    auto report=px::ui::MigrateUIProjectV5(project,*write);if(!report){for(const auto& diagnostic:report.Diagnostics())std::cerr<<diagnostic.code<<": "<<diagnostic.message<<" "<<diagnostic.details<<"\n";return 1;}
    std::cout<<"Scanned "<<report.Value().scanned<<", current "<<report.Value().alreadyCurrent<<", "<<(*write?"migrated ":"would migrate ")<<report.Value().changed.size()<<"\n";for(const auto& item:report.Value().changed)std::cout<<item.path.generic_string()<<"\n";return 0;
}
