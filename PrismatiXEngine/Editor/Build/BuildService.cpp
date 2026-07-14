#include "Editor/Build/BuildService.h"

#include "Engine/IO/Archive.h"
#include "Engine/IO/AtomicFile.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/VN/GameCatalog.h"
#include "Engine/UI/UIResourceResolver.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace px::editor {
namespace {
using Bytes=io::Bytes;
std::string RuntimePath(const std::filesystem::path& root,const std::filesystem::path& path){std::error_code ec;return std::filesystem::relative(path,root,ec).generic_string();}
std::optional<Bytes> ReadFile(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)return std::nullopt;const auto size=in.tellg();if(size<0||static_cast<std::uintmax_t>(size)>static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)())||static_cast<std::uintmax_t>(size)>static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)()))return std::nullopt;in.seekg(0);Bytes bytes(static_cast<std::size_t>(size));if(size>0&&!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))return std::nullopt;return bytes;}
bool CopyFile(const std::filesystem::path& from,const std::filesystem::path& to,std::error_code& ec){std::filesystem::create_directories(to.parent_path(),ec);return !ec&&std::filesystem::copy_file(from,to,std::filesystem::copy_options::overwrite_existing,ec);}
std::filesystem::path ComparablePath(const std::filesystem::path& path){std::error_code ec;auto value=std::filesystem::weakly_canonical(std::filesystem::absolute(path,ec),ec);return ec?path.lexically_normal():value.lexically_normal();}
std::string ComparableKey(const std::filesystem::path& path){std::string key=ComparablePath(path).generic_string();
#if defined(_WIN32)
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
#endif
    return key;
}
bool SamePath(const std::filesystem::path& a,const std::filesystem::path& b){return ComparableKey(a)==ComparableKey(b);}
bool IsWithin(const std::filesystem::path& path,const std::filesystem::path& root){const auto child=ComparablePath(path);const auto parent=ComparablePath(root);const auto relative=child.lexically_relative(parent);if(relative.empty())return true;const auto first=relative.begin();return first!=relative.end()&&*first!=".."&&!relative.is_absolute();}
struct DirectoryCleanup{std::filesystem::path path;bool keep=false;~DirectoryCleanup(){if(keep||path.empty())return;std::error_code ec;std::filesystem::remove_all(path,ec);}};
bool RuntimeLibrary(const std::filesystem::path& path){
#if defined(_WIN32)
return path.extension()==".dll";
#elif defined(__APPLE__)
return path.extension()==".dylib";
#else
return path.extension()==".so";
#endif
}
bool WildcardMatch(std::string_view pattern,std::string_view text){std::size_t p=0,t=0,star=std::string_view::npos,mark=0;while(t<text.size()){if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==text[t])){++p;++t;}else if(p<pattern.size()&&pattern[p]=='*'){star=p++;mark=t;}else if(star!=std::string_view::npos){p=star+1;t=++mark;}else return false;}while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();}
void CollectResourceRefs(const Variant& value,std::vector<ResourceRefValue>& output){
    if(const auto* reference=value.TryGet<ResourceRefValue>()){output.push_back(*reference);return;}
    if(const auto* values=value.AsArray())for(const auto& item:*values)CollectResourceRefs(item,output);
    if(const auto* values=value.AsObject())for(const auto& [name,item]:*values){(void)name;CollectResourceRefs(item,output);}
}
std::vector<ResourceRefValue> DocumentResourceRefs(const resource::TypedDocument& document){
    std::vector<ResourceRefValue> references;for(const auto& [name,value]:document.properties){(void)name;CollectResourceRefs(value,references);}for(const auto& node:document.nodes)for(const auto& [name,value]:node.properties){(void)name;CollectResourceRefs(value,references);}return references;
}
std::optional<std::filesystem::path> EnvironmentPath(const char* name){
#if defined(_WIN32)
    char* value=nullptr;std::size_t size=0;if(_dupenv_s(&value,&size,name)!=0||!value)return std::nullopt;std::filesystem::path result(value);std::free(value);return result;
#else
    if(const char* value=std::getenv(name))return std::filesystem::path(value);return std::nullopt;
#endif
}
}

bool BuildService::Build(const BuildOptions& options) const {
    const auto fail=[this](std::string code,const std::filesystem::path& path,std::string message,std::string details={}){diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),.category="Editor.Build",.message=std::move(message),.details=std::move(details)};diagnostic.source.path=path.generic_string();Log(diagnostic.message+(diagnostic.details.empty()?"":"："+diagnostic.details));diag::Emit(std::move(diagnostic));return false;};
    if(options.projectRoot.empty()||!std::filesystem::exists(options.projectRoot))return fail("PXBUILD9601",options.projectRoot,"Build 失敗：專案根目錄不存在");
    if(!std::filesystem::exists(options.projectRoot/"Content"))return fail("PXBUILD9602",options.projectRoot/"Content","Build 失敗：專案沒有 Content 資料夾");
    if(options.playerExe.empty()||!std::filesystem::exists(options.playerExe))return fail("PXBUILD9603",options.playerExe,"Build 失敗：找不到 Player executable");
    if(options.outputDir.empty())return fail("PXBUILD9610",options.outputDir,"Build 已阻擋：輸出資料夾不可為空");
    const auto projectRoot=ComparablePath(options.projectRoot),contentRoot=ComparablePath(options.projectRoot/"Content"),outputDir=ComparablePath(options.outputDir);
    if(SamePath(outputDir,projectRoot)||IsWithin(outputDir,contentRoot)||SamePath(outputDir,outputDir.root_path()))
        return fail("PXBUILD9611",options.outputDir,"Build 已阻擋：輸出位置會覆蓋專案或系統目錄");
    if(const auto home=EnvironmentPath("USERPROFILE");home&&SamePath(outputDir,*home))return fail("PXBUILD9612",options.outputDir,"Build 已阻擋：不可使用使用者主目錄作為輸出");
    if(const auto home=EnvironmentPath("HOME");home&&SamePath(outputDir,*home))return fail("PXBUILD9612",options.outputDir,"Build 已阻擋：不可使用使用者主目錄作為輸出");
    resource::AssetRegistry registry;const Status scanned=registry.Scan(options.projectRoot);
    if(!scanned||!registry.Valid())return fail("PXBUILD9604",options.projectRoot/"Content","Build 已阻擋：請先在 Asset Identity Resolver 解決缺少或重複 GUID");
    std::vector<std::pair<std::filesystem::path,resource::TypedDocument>> typedAssets;
    for (const auto& entry : registry.Entries()) {
        std::string extension = entry.sourcePath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if ((extension == ".mp4" || extension == ".webm" || extension == ".mkv" ||
             extension == ".mov") && !options.profile.ffmpegLicenseReviewAccepted) {
            return fail("PXBUILD9619", entry.sourcePath,
                "Build 已阻擋：現代影片需要完成 FFmpeg 發佈審查",
                "請確認 LGPL 動態連結、編解碼專利與商店政策後，在 Export Profile 明確設定 ffmpegLicenseReviewAccepted=true");
        }
        if (extension == ".pxscenario" || extension == ".pxlayout" ||
            extension == ".pxanim" || extension == ".pxscene" || extension == ".pxcomponent" ||
            extension == ".pxres" || extension == ".pxtheme" ||
            extension == ".pxextension" || extension == ".pxindex") {
            const auto bytes = ReadFile(entry.sourcePath);
            if (!bytes) return fail("PXBUILD9620", entry.sourcePath, "Build 已阻擋：無法讀取結構化資產");
            const std::string text(bytes->begin(), bytes->end());
            if (extension == ".pxscenario") {
                const auto parsed = vn::scenario::ParseScenario(text, entry.sourcePath.generic_string());
                if (!parsed) return fail("PXBUILD9621", entry.sourcePath, "Build 已阻擋：Scenario 格式無效", diag::Describe(parsed.Diagnostics().front()));
                const auto validation = vn::scenario::ValidateScenario(parsed.Value(), vn::CommandRegistry::Global(), entry.sourcePath.generic_string());
                if (!validation.Valid()) {
                    const auto diagnostic = std::find_if(validation.diagnostics.begin(), validation.diagnostics.end(), [](const auto& item) { return item.BlocksBuild(); });
                    return fail("PXBUILD9622", entry.sourcePath, "Build 已阻擋：Scenario 契約驗證失敗", diagnostic == validation.diagnostics.end() ? std::string{} : diag::Describe(*diagnostic));
                }
            } else if (extension == ".pxlayout") {
                const auto parsed = vn::scenario::ParseScenarioLayout(text, entry.sourcePath.generic_string());
                if (!parsed) return fail("PXBUILD9623", entry.sourcePath, "Build 已阻擋：Scenario layout 無效", diag::Describe(parsed.Diagnostics().front()));
            } else if (extension == ".pxanim") {
                const auto parsed = animation::ParseAnimationClip(text, entry.sourcePath.generic_string());
                if (!parsed) return fail("PXBUILD9624", entry.sourcePath, "Build 已阻擋：AnimationClip 無效", diag::Describe(parsed.Diagnostics().front()));
            } else if (extension == ".pxscene" || extension == ".pxcomponent" || extension == ".pxres" || extension == ".pxtheme") {
                const auto parsed = resource::ParseTypedDocument(text, entry.sourcePath.generic_string());
                if (!parsed) return fail("PXBUILD9625", entry.sourcePath, "Build 已阻擋：typed asset 無效", diag::Describe(parsed.Diagnostics().front()));
                if(parsed.Value().type=="GameCatalog"){
                    vn::GameCatalog catalog;const Status validCatalog=catalog.Load(text,entry.sourcePath.generic_string());
                    if(!validCatalog)return fail("PXBUILD9636",entry.sourcePath,"Build 已阻擋：GameCatalog 無效",diag::Describe(validCatalog.Diagnostics().front()));
                }
                if(extension==".pxtheme"){const auto theme=ui::LoadUITheme(parsed.Value());if(!theme)return fail("PXBUILD9628",entry.sourcePath,"Build 已阻擋：UITheme token 或 style 無效",diag::Describe(theme.Diagnostics().front()));}
                typedAssets.emplace_back(entry.sourcePath,parsed.Value());
            } else if (extension == ".pxextension") {
                const auto manifest = nlohmann::json::parse(text, nullptr, false);
                if (manifest.is_discarded() || !manifest.is_object() ||
                    manifest.value("format", std::string{}) != "PrismatiXExtension" ||
                    manifest.value("version", 0) != 4 ||
                    !manifest.contains("id") || !manifest.contains("entry") ||
                    !manifest["id"].is_string() || !manifest["entry"].is_string())
                    return fail("PXBUILD9626", entry.sourcePath, "Build 已阻擋：Lua extension manifest 無效");
            } else {
                const auto index = nlohmann::json::parse(text, nullptr, false);
                if (index.is_discarded() || !index.is_array() || index.size() > 4096 ||
                    !std::all_of(index.begin(), index.end(), [](const auto& item) {
                        if (!item.is_string()) return false;
                        const std::string path = item.template get<std::string>();
                        return path.starts_with("Content/Extensions/") &&
                               path.ends_with(".pxextension") && path.find("..") == std::string::npos;
                    })) return fail("PXBUILD9627", entry.sourcePath, "Build 已阻擋：extension index 無效");
            }
        }
    }

    std::unordered_set<std::string> typedPaths;for(const auto& [path,document]:typedAssets){(void)document;typedPaths.insert(ComparableKey(path));}
    std::unordered_map<std::string,std::vector<std::string>> dependencyGraph;
    std::unordered_map<std::string,std::filesystem::path> dependencySources;
    for(const auto& [source,document]:typedAssets){
        const std::string sourceKey=ComparableKey(source);dependencySources[sourceKey]=source;
        for(const auto& reference:DocumentResourceRefs(document)){
            if(reference.lastKnownPath.empty())return fail("PXBUILD9629",source,"Build 已阻擋：UI resource reference 沒有 path");
            const auto target=ComparablePath(options.projectRoot/reference.lastKnownPath);
            if(!IsWithin(target,options.projectRoot)||!std::filesystem::exists(target))return fail("PXBUILD9630",source,"Build 已阻擋：UI resource dependency 遺失",reference.lastKnownPath);
            const auto* asset=registry.FindPath(target);
            if(!asset)return fail("PXBUILD9631",source,"Build 已阻擋：UI resource dependency 沒有 AssetMeta",reference.lastKnownPath);
            if(!reference.id.Empty()&&asset->id!=reference.id)return fail("PXBUILD9632",source,"Build 已阻擋：UI resource GUID 與 path 不一致",reference.lastKnownPath);
            const std::string targetKey=ComparableKey(target);if(typedPaths.contains(targetKey))dependencyGraph[sourceKey].push_back(targetKey);
        }
    }
    std::unordered_set<std::string> visiting,visited;std::function<bool(const std::string&)> visit=[&](const std::string& node){if(visited.contains(node))return true;if(!visiting.insert(node).second)return false;for(const auto& target:dependencyGraph[node])if(!visit(target))return false;visiting.erase(node);visited.insert(node);return true;};
    for(const auto& [source,targets]:dependencyGraph){(void)targets;if(!visit(source))return fail("PXBUILD9633",dependencySources[source],"Build 已阻擋：UI resource dependency cycle");}

    const Status uiTypes=ui::RegisterBuiltinUITypes();if(!uiTypes)return fail("PXBUILD9634",options.projectRoot,"Build 已阻擋：UI metadata 無法註冊",diag::Describe(uiTypes.Diagnostics().front()));
    ui::FormatterRegistry uiFormatters;ui::ObservableViewModel buildViewModel;std::unordered_map<std::string,VariantType> bindingTypes;const auto defaultValue=[](const VariantType type)->Variant{switch(type){case VariantType::Bool:return false;case VariantType::Integer:return std::int64_t{0};case VariantType::Number:return 0.0;case VariantType::String:return std::string{};case VariantType::Vec2:return Vec2{};case VariantType::Rect:return Rect{};case VariantType::Color:return Color{};case VariantType::Uuid:return Uuid{};case VariantType::ResourceRef:return ResourceRefValue{};case VariantType::TokenRef:return TokenRefValue{};case VariantType::Array:return VariantArray{};case VariantType::Object:return VariantObject{};default:return Variant{};}};
    for(const auto& [source,document]:typedAssets)for(const auto& node:document.nodes)if(const auto found=node.properties.find("bindings");found!=node.properties.end())if(const auto* bindings=found->second.AsObject())for(const auto& [target,value]:*bindings){const auto* definition=value.AsObject();const auto path=definition?definition->find("path"):VariantObject::const_iterator{};const auto* pathText=definition&&path!=definition->end()?path->second.TryGet<std::string>():nullptr;const auto* targetProperty=TypeRegistry::Global().FindProperty(node.type,target);if(!pathText||!targetProperty)continue;VariantType sourceType=targetProperty->type;if(const auto formatter=definition->find("formatter");formatter!=definition->end())if(const auto* name=formatter->second.TryGet<std::string>();name&&!name->empty())if(const auto* descriptor=uiFormatters.Find(*name))sourceType=descriptor->input;if(const auto existing=bindingTypes.find(*pathText);existing!=bindingTypes.end()&&existing->second!=sourceType)return fail("PXBUILD9639",source,"Build 已阻擋：ViewModel binding path 被宣告為不相容型別",*pathText);bindingTypes[*pathText]=sourceType;}
    for(const auto& [path,type]:bindingTypes){const Status defined=buildViewModel.Define(path,defaultValue(type),true);if(!defined)return fail("PXBUILD9639",options.projectRoot,"Build 已阻擋：ViewModel binding path 無效",diag::Describe(defined.Diagnostics().front()));}
    const ui::UIDocumentLoader uiLoader=[&](const ResourceRefValue& reference)->Result<resource::TypedDocument>{const auto target=ComparableKey(options.projectRoot/reference.lastKnownPath);const auto found=std::find_if(typedAssets.begin(),typedAssets.end(),[&](const auto& item){return ComparableKey(item.first)==target;});if(found==typedAssets.end())return Result<resource::TypedDocument>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXBUILD9635",.category="Build.UI",.message="Referenced typed UI asset was not indexed",.details=reference.lastKnownPath});return Result<resource::TypedDocument>::Success(found->second);};
    for(const auto& [source,document]:typedAssets){if(document.type=="UIComponent"){const Status interfaceStatus=ui::ValidateUIComponentInterface(document);if(!interfaceStatus)return fail("PXBUILD9636",source,"Build 已阻擋：UIComponent 公開介面無效",diag::Describe(interfaceStatus.Diagnostics().front()));const auto expanded=ui::ExpandUIComponents(document,uiLoader);if(!expanded)return fail("PXBUILD9637",source,"Build 已阻擋：UIComponent 巢狀相依或 slot 無效",diag::Describe(expanded.Diagnostics().front()));}else if(document.type=="UIScene"){const auto instantiated=ui::InstantiateUIScene(document,&buildViewModel,uiFormatters,uiLoader);if(!instantiated)return fail("PXBUILD9638",source,"Build 已阻擋：UIScene schema、signals、Behavior 或 animation 無效",diag::Describe(instantiated.Diagnostics().front()));}}

    const auto staging=outputDir.parent_path()/(outputDir.filename().string()+".staging-"+Uuid::Random().ToString());
    const auto backup=outputDir.parent_path()/(outputDir.filename().string()+".backup-"+Uuid::Random().ToString());
    DirectoryCleanup stagingCleanup{staging};DirectoryCleanup backupCleanup{backup};
    std::error_code ec;std::filesystem::create_directories(staging,ec);
    if(ec)return fail("PXBUILD9605",staging,"Build 失敗：無法建立 staging 資料夾",ec.message());
    struct GroupArchive { ExportContentGroup group; std::unique_ptr<io::ArchiveWriter> writer; std::filesystem::path filename; std::size_t count=0; };
    std::vector<GroupArchive> groups;
    const bool encrypted=options.encrypt&&options.profile.encryption;
    for(const auto& group:options.profile.contentGroups){if(group.id.empty()||group.roots.empty()||!std::all_of(group.id.begin(),group.id.end(),[](const unsigned char c){return std::isalnum(c)||c=='-'||c=='_';}))return fail("PXBUILD9617",options.projectRoot,"Build 失敗：content group id/roots 無效",group.id);auto writer=std::make_unique<io::ArchiveWriter>();writer->SetCompression(options.profile.compression);if(encrypted)writer->SetKey(crypto::DeriveKey(options.key));groups.push_back({group,std::move(writer),group.id=="base"?std::filesystem::path("Content.pdx"):std::filesystem::path("Content."+group.id+".pdx")});}
    if(groups.empty())return fail("PXBUILD9617",options.projectRoot,"Build 失敗：Export Profile 沒有 content group");
    std::size_t count=0;
    for(const auto& entry:registry.Entries()){
        if(!entry.includeInBuild)continue;
        const std::string sourceRuntime=RuntimePath(options.projectRoot,entry.sourcePath);if(std::any_of(options.profile.excludePatterns.begin(),options.profile.excludePatterns.end(),[&](const auto& pattern){return WildcardMatch(pattern,sourceRuntime);}))continue;
        GroupArchive* selected=nullptr;std::size_t selectedLength=0;for(auto& group:groups)for(const auto& rawRoot:group.group.roots){std::string root=std::filesystem::path(rawRoot).lexically_normal().generic_string();while(root.ends_with('/'))root.pop_back();if(sourceRuntime==root||sourceRuntime.starts_with(root+"/")){if(!selected||root.size()>selectedLength){selected=&group;selectedLength=root.size();}}}if(!selected)return fail("PXBUILD9618",entry.sourcePath,"Build 失敗：素材沒有對應 content group",sourceRuntime);
        for(const auto& file:{entry.sourcePath,entry.metaPath}){auto bytes=ReadFile(file);if(!bytes)return fail("PXBUILD9606",file,"Build 失敗：無法讀取素材");
            selected->writer->Add(RuntimePath(options.projectRoot,file),*bytes);++selected->count;++count;}
    }
    VariantArray archiveValues;for(auto& group:groups){if(group.count==0&&group.group.optional)continue;const auto stagedArchive=staging/group.filename;if(!group.writer->Write(stagedArchive.string()))return fail("PXBUILD9607",stagedArchive,"Build 失敗：無法寫入封裝檔");archiveValues.emplace_back(VariantObject{{"group",group.group.id},{"file",group.filename.generic_string()},{"optional",group.group.optional}});}
    const auto player=staging/options.playerExe.filename();if(!CopyFile(options.playerExe,player,ec))return fail("PXBUILD9608",player,"Build 失敗：無法複製 Player",ec.message());
    std::filesystem::permissions(player,std::filesystem::perms::owner_exec|std::filesystem::perms::group_exec|std::filesystem::perms::others_exec,std::filesystem::perm_options::add,ec);
    for(auto it=std::filesystem::directory_iterator(options.playerExe.parent_path(),ec);!ec&&it!=std::filesystem::directory_iterator();it.increment(ec))if(it->is_regular_file(ec)&&RuntimeLibrary(it->path())){std::error_code copy;if(!CopyFile(it->path(),staging/it->path().filename(),copy))return fail("PXBUILD9613",it->path(),"Build 失敗：無法複製 runtime library",copy.message());}

    resource::TypedDocument package;package.kind=resource::DocumentKind::Resource;package.id=Uuid::FromName(options.title+"/"+options.profile.id+"/"+options.profile.productVersion);package.type="GamePackage";
    VariantArray routeValues;for(const auto& route:options.routes){const auto* sceneAsset=registry.FindPath(options.projectRoot/route.scene);if(!sceneAsset)return fail("PXBUILD9616",options.projectRoot/route.scene,"Build 失敗：Route scene 沒有 ResourceId");routeValues.emplace_back(VariantObject{{"id",route.id},{"scene",ResourceRefValue{sceneAsset->id,route.scene}},{"modal",route.modal},{"cache",route.cache}});}
    package.properties={{"title",options.title},{"archives",Variant(std::move(archiveValues))},{"encrypt",encrypted},{"key",encrypted?options.key:std::string{}},
        {"gameWidth",static_cast<std::int64_t>(options.gameWidth)},{"gameHeight",static_cast<std::int64_t>(options.gameHeight)},
        {"startRoute",options.startRoute},{"routes",Variant(std::move(routeValues))},{"startScript",options.startScript},{"profile",options.profile.id},{"productVersion",options.profile.productVersion},{"platform",options.profile.platform},{"reproducible",options.profile.reproducible}};
    const Status manifest=io::AtomicFile::WriteText(staging/"game.pxpackage",resource::WriteTypedDocument(package));
    if(!manifest)return fail("PXBUILD9609",staging/"game.pxpackage","Build 失敗：無法寫入 typed package manifest");

    ec.clear();
    if(std::filesystem::exists(outputDir,ec)){
        ec.clear();std::filesystem::rename(outputDir,backup,ec);
        if(ec)return fail("PXBUILD9614",outputDir,"Build 失敗：無法保留現有輸出",ec.message());
    }
    ec.clear();std::filesystem::rename(staging,outputDir,ec);
    if(ec){
        std::error_code restore;if(std::filesystem::exists(backup,restore)){restore.clear();std::filesystem::rename(backup,outputDir,restore);}
        return fail("PXBUILD9615",outputDir,"Build 失敗：無法啟用 staging 輸出",ec.message());
    }
    stagingCleanup.keep=true;
    if(std::filesystem::exists(backup,ec)){ec.clear();std::filesystem::remove_all(backup,ec);if(!ec)backupCleanup.keep=true;}
    Log("Build complete: "+outputDir.string()+" ("+std::to_string(count)+" registered files, "+std::to_string(groups.size())+" content groups, encrypted="+(encrypted?"true":"false")+", profile="+options.profile.id+")");return true;
}

}  // namespace px::editor
