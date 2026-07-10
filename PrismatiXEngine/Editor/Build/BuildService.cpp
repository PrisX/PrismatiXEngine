#include "Editor/Build/BuildService.h"

#include "Engine/IO/Archive.h"
#include "Engine/IO/AtomicFile.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Resources/TypedDocument.h"

#include <fstream>
#include <optional>

namespace px::editor {
namespace {
using Bytes=io::Bytes;
std::string RuntimePath(const std::filesystem::path& root,const std::filesystem::path& path){std::error_code ec;return std::filesystem::relative(path,root,ec).generic_string();}
std::optional<Bytes> ReadFile(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return std::nullopt;const auto size=in.tellg();in.seekg(0);Bytes bytes(static_cast<std::size_t>(size));if(size>0&&!in.read(reinterpret_cast<char*>(bytes.data()),size))return std::nullopt;return bytes;}
bool CopyFile(const std::filesystem::path& from,const std::filesystem::path& to,std::error_code& ec){std::filesystem::create_directories(to.parent_path(),ec);return !ec&&std::filesystem::copy_file(from,to,std::filesystem::copy_options::overwrite_existing,ec);}
bool RuntimeLibrary(const std::filesystem::path& path){
#if defined(_WIN32)
return path.extension()==".dll";
#elif defined(__APPLE__)
return path.extension()==".dylib";
#else
return path.extension()==".so";
#endif
}
}

bool BuildService::Build(const BuildOptions& options) const {
    const auto fail=[this](std::string code,const std::filesystem::path& path,std::string message,std::string details={}){diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),.category="Editor.Build",.message=std::move(message),.details=std::move(details)};diagnostic.source.path=path.generic_string();Log(diagnostic.message+(diagnostic.details.empty()?"":"："+diagnostic.details));diag::Emit(std::move(diagnostic));return false;};
    if(options.projectRoot.empty()||!std::filesystem::exists(options.projectRoot))return fail("PXBUILD9601",options.projectRoot,"Build 失敗：專案根目錄不存在");
    if(!std::filesystem::exists(options.projectRoot/"Content"))return fail("PXBUILD9602",options.projectRoot/"Content","Build 失敗：專案沒有 Content 資料夾");
    if(options.playerExe.empty()||!std::filesystem::exists(options.playerExe))return fail("PXBUILD9603",options.playerExe,"Build 失敗：找不到 Player executable");
    resource::AssetRegistry registry;const Status scanned=registry.Scan(options.projectRoot);
    if(!scanned||!registry.Valid())return fail("PXBUILD9604",options.projectRoot/"Content","Build 已阻擋：請先在 Asset Identity Resolver 解決缺少或重複 GUID");

    std::error_code ec;std::filesystem::remove_all(options.outputDir,ec);ec.clear();std::filesystem::create_directories(options.outputDir,ec);
    if(ec)return fail("PXBUILD9605",options.outputDir,"Build 失敗：無法建立輸出資料夾",ec.message());
    io::ArchiveWriter archive;archive.SetCompression(true);if(options.encrypt)archive.SetKey(crypto::DeriveKey(options.key));
    std::size_t count=0;
    for(const auto& entry:registry.Entries()){
        if(!entry.includeInBuild)continue;
        for(const auto& file:{entry.sourcePath,entry.metaPath}){auto bytes=ReadFile(file);if(!bytes)return fail("PXBUILD9606",file,"Build 失敗：無法讀取素材");
            archive.Add(RuntimePath(options.projectRoot,file),*bytes);++count;}
    }
    const auto archivePath=options.outputDir/"Content.pdx";if(!archive.Write(archivePath.string()))return fail("PXBUILD9607",archivePath,"Build 失敗：無法寫入封裝檔");
    const auto player=options.outputDir/options.playerExe.filename();if(!CopyFile(options.playerExe,player,ec))return fail("PXBUILD9608",player,"Build 失敗：無法複製 Player",ec.message());
    std::filesystem::permissions(player,std::filesystem::perms::owner_exec|std::filesystem::perms::group_exec|std::filesystem::perms::others_exec,std::filesystem::perm_options::add,ec);
    for(auto it=std::filesystem::directory_iterator(options.playerExe.parent_path(),ec);!ec&&it!=std::filesystem::directory_iterator();it.increment(ec))if(it->is_regular_file(ec)&&RuntimeLibrary(it->path())){std::error_code copy;CopyFile(it->path(),options.outputDir/it->path().filename(),copy);}

    resource::TypedDocument package;package.kind=resource::DocumentKind::Resource;package.id=Uuid::Random();package.type="GamePackage";
    package.properties={{"title",options.title},{"archive",std::string("Content.pdx")},{"encrypt",options.encrypt},{"key",options.encrypt?options.key:std::string{}},
        {"gameWidth",static_cast<std::int64_t>(options.gameWidth)},{"gameHeight",static_cast<std::int64_t>(options.gameHeight)},
        {"startUI",options.startUI},{"startScript",options.startScript}};
    const Status manifest=io::AtomicFile::WriteText(options.outputDir/"game.pxpackage",resource::WriteTypedDocument(package));
    if(!manifest)return fail("PXBUILD9609",options.outputDir/"game.pxpackage","Build 失敗：無法寫入 typed package manifest");
    Log("Build complete: "+archivePath.string()+" ("+std::to_string(count)+" registered files, encrypted="+(options.encrypt?"true":"false")+")");return true;
}

}  // namespace px::editor
