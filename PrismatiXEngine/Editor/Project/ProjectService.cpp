#include "Editor/Project/ProjectService.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Editor/Build/ExportProfile.h"

#include <fstream>
#include <sstream>
#include <tuple>

namespace px::editor {
namespace {
constexpr const char* kDefaultFont = "Content/Fonts/NotoSansTC-Bold.ttf";

std::string StartScript(std::string_view name) {
    vn::scenario::ScenarioDocument document;
    document.id=Uuid::Random();document.name="Start";
    vn::scenario::ScenarioNode chapter{Uuid::Random(),"chapter",{{"title",std::string(name)}}};
    vn::scenario::ScenarioNode dialogue{Uuid::Random(),"say",{{"textId",Uuid::Random().ToString()},{"speaker",std::string{}},{"value","Welcome to "+std::string(name)+"!\nEdit this scene in the Story workspace."}}};
    document.entry=chapter.id;document.nodes={chapter,dialogue};
    document.edges.push_back({Uuid::Random(),chapter.id,"flow",dialogue.id,"in"});
    return vn::scenario::WriteScenario(document);
}

resource::NodeRecord Node(std::string name, std::string type, Uuid parent = {},
                          std::map<std::string, Variant> properties = {}) {
    return {Uuid::Random(), parent, std::move(name), std::move(type), std::move(properties)};
}

resource::TypedDocument ScreenDocument(std::string_view screen, std::string_view projectName) {
    resource::TypedDocument document; document.kind=resource::DocumentKind::Scene;
    document.id=Uuid::Random(); document.type="UIScene"; document.properties["canvasSize"]=Vec2{1280,720};
    auto root=Node("Root","StackContainer",{},{{"anchors",Rect{0,0,1,1}}}); const Uuid rootId=root.id; document.nodes.push_back(std::move(root));
    auto button=[&](std::string name,Uuid parent,std::string text,std::string command){document.nodes.push_back(Node(std::move(name),"Button",parent,{{"text",std::move(text)},{"events",VariantObject{{"activated",VariantObject{{"action",std::move(command)}}}}}}));};
    if(screen=="HUD"){
        auto nvl=Node("NVLPanel","Panel",rootId,{{"anchors",Rect{0.055f,0.06f,0.945f,0.88f}},{"themeVariant",std::string("Dialogue")}});const Uuid nvlId=nvl.id;document.nodes.push_back(std::move(nvl));
        document.nodes.push_back(Node("NVLText","Label",nvlId,{{"anchors",Rect{0.04f,0.04f,0.96f,0.96f}},{"wrap",true}}));
        auto adv=Node("ADVPanel","Panel",rootId,{{"anchors",Rect{0.045f,0.64f,0.955f,0.955f}},{"themeVariant",std::string("Dialogue")}});const Uuid advId=adv.id;document.nodes.push_back(std::move(adv));
        document.nodes.push_back(Node("DialogueFrame","TextureRect",advId,{{"anchors",Rect{0,0,1,1}},{"path",std::string("Content/Images/UI/HUD/dialog-stage.png")},{"scaleMode",std::string("Stretch")},{"lockAspectRatio",false}}));
        document.nodes.push_back(Node("Speaker","Label",advId,{{"anchors",Rect{0.03f,0.08f,0.97f,0.25f}},{"bindings",VariantObject{{"text",VariantObject{{"path",std::string("dialogue.speaker")}}}}}}));
        document.nodes.push_back(Node("Dialogue","RichTextLabel",advId,{{"anchors",Rect{0.03f,0.28f,0.97f,0.91f}},{"bindings",VariantObject{{"markup",VariantObject{{"path",std::string("dialogue.text")}}}}}}));
        document.nodes.push_back(Node("Choices","VBoxContainer",rootId,{{"anchors",Rect{0.20f,0.18f,0.80f,0.60f}},{"separation",std::int64_t{12}}}));
        auto notice=Node("NoticePanel","EdgeRevealContainer",rootId,{{"anchors",Rect{0.0f,0.025f,0.31f,0.13f}},{"edge",std::string("Left")},{"revealTrigger",std::string("Manual")},{"revealSpeed",12.0},{"triggerSize",1.0},{"pinned",false}});const Uuid noticeId=notice.id;document.nodes.push_back(std::move(notice));
        document.nodes.push_back(Node("NoticeFrame","TextureRect",noticeId,{{"anchors",Rect{0,0,1,1}},{"path",std::string("Content/Images/UI/HUD/notice.png")},{"scaleMode",std::string("Stretch")},{"lockAspectRatio",false}}));
        document.nodes.push_back(Node("ChapterNotice","Label",noticeId,{{"anchors",Rect{0.08f,0.12f,0.90f,0.88f}},{"text",std::string("CHAPTER 01")},{"verticalAlignment",std::string("Center")},{"bindings",VariantObject{{"text",VariantObject{{"path",std::string("chapter.title")}}}}}}));
        document.nodes.push_back(Node("MusicNotice","Label",noticeId,{{"anchors",Rect{0.08f,0.12f,0.90f,0.88f}},{"text",std::string("♪ Now Playing")},{"verticalAlignment",std::string("Center")},{"bindings",VariantObject{{"text",VariantObject{{"path",std::string("music.title")}}}}}}));
        auto edgeToolbar=Node("EdgeToolbar","EdgeRevealContainer",rootId,{{"anchors",Rect{0.34f,0.0f,0.98f,0.085f}},{"edge",std::string("Top")},{"revealSpeed",14.0},{"triggerSize",10.0},{"pinned",false}});const Uuid edgeId=edgeToolbar.id;document.nodes.push_back(std::move(edgeToolbar));
        auto quick=Node("QuickMenu","HBoxContainer",edgeId,{{"anchors",Rect{0.08f,0.08f,0.92f,0.92f}},{"separation",std::int64_t{12}}});const Uuid quickId=quick.id;document.nodes.push_back(std::move(quick));
        const auto iconButton=[&](std::string name,std::string icon,std::string tooltip,std::string action){document.nodes.push_back(Node(std::move(name),"IconButton",quickId,{{"text",std::move(icon)},{"minimumSize",Vec2{44,44}},{"tooltip",std::move(tooltip)},{"events",VariantObject{{"activated",VariantObject{{"action",std::move(action)}}}}}}));};
        iconButton("Auto","▶","自動播放","mode.auto");iconButton("Skip","»","快進","mode.skip");iconButton("Backlog","≡","歷史紀錄","backlog.open");iconButton("Save","↓","儲存","save.open");iconButton("Load","↑","讀取","load.open");iconButton("Settings","⚙","設定","settings.open");iconButton("PinToolbar","◆","固定／取消固定工具列","hud.toolbar.pin");
        document.nodes.push_back(Node("ModeState","Label",rootId,{{"anchors",Rect{0.02f,0.02f,0.45f,0.09f}}}));
        return document;
    }
    if(screen=="VideoOverlay"){
        document.nodes.push_back(Node("SkipHint","Label",rootId,{{"anchors",Rect{0.78f,0.03f,0.97f,0.10f}},{"text",std::string("點擊跳過 ▶")}}));
        return document;
    }
    auto panel=Node("Surface","Panel",rootId,{{"anchors",Rect{0.03f,0.03f,0.97f,0.97f}},{"themeVariant",std::string("Dialogue")}});const Uuid panelId=panel.id;document.nodes.push_back(std::move(panel));
    auto column=Node("Content","VBoxContainer",panelId,{{"anchors",Rect{0.08f,0.08f,0.92f,0.92f}},{"separation",std::int64_t{10}}});const Uuid columnId=column.id;document.nodes.push_back(std::move(column));
    const std::string title=screen=="Title"?std::string(projectName):std::string(screen);
    document.nodes.push_back(Node("Heading","Label",columnId,{{"text",title}}));
    if(screen=="Title"){
        button("Start",columnId,"開始遊戲","game.start");button("Load",columnId,"讀取遊戲","load.open");button("Gallery",columnId,"CG 鑑賞","gallery.open");button("Settings",columnId,"設定","settings.open");button("Quit",columnId,"離開","app.quit");
    }else if(screen=="Backlog"){
        document.nodes.push_back(Node("Entries","ListView",columnId));button("Close",columnId,"返回","overlay.close");
    }else if(screen=="SaveLoad"){
        document.nodes.push_back(Node("Slots","GridView",columnId,{{"columns",std::int64_t{3}}}));button("Close",columnId,"返回","overlay.close");
    }else if(screen=="Gallery"){
        document.nodes.push_back(Node("Items","GridView",columnId,{{"columns",std::int64_t{4}}}));button("Close",columnId,"返回","overlay.close");
    }else if(screen=="Settings"){
        for(const auto& [name,label,max]:std::vector<std::tuple<std::string,std::string,double>>{{"BGM","BGM",128},{"SE","SE",128},{"Voice","Voice",128},{"TextSpeed","文字速度",120},{"TextScale","文字縮放",200}}){document.nodes.push_back(Node(name+"Label","Label",columnId,{{"text",label}}));document.nodes.push_back(Node(name,"Slider",columnId,{{"maximum",max}}));}
        document.nodes.push_back(Node("SkipRead","CheckBox",columnId,{{"text",std::string("只快進已讀")}}));document.nodes.push_back(Node("Fullscreen","CheckBox",columnId,{{"text",std::string("全螢幕")}}));document.nodes.push_back(Node("HighContrast","CheckBox",columnId,{{"text",std::string("高對比")}}));document.nodes.push_back(Node("ReducedMotion","CheckBox",columnId,{{"text",std::string("減少動態效果")}}));document.nodes.push_back(Node("SelfVoicing","CheckBox",columnId,{{"text",std::string("朗讀介面")}}));button("Close",columnId,"完成","overlay.close");
    }else button("Close",columnId,"返回","overlay.close");
    return document;
}

bool ReadString(const resource::TypedDocument& document,const char* key,std::string& out){const auto it=document.properties.find(key);if(it==document.properties.end())return false;if(const auto* value=it->second.TryGet<std::string>()){out=*value;return true;}return false;}
bool ReadInt(const resource::TypedDocument& document,const char* key,int& out){const auto it=document.properties.find(key);if(it==document.properties.end())return false;if(const auto* value=it->second.TryGet<std::int64_t>()){out=static_cast<int>(*value);return true;}return false;}
bool ReadBool(const resource::TypedDocument& document,const char* key,bool& out){const auto it=document.properties.find(key);if(it==document.properties.end())return false;if(const auto* value=it->second.TryGet<bool>()){out=*value;return true;}return false;}
}

bool ProjectService::Open(const std::filesystem::path& root) {
    if(!std::filesystem::exists(root)){Log("Project root does not exist: "+root.string());return false;}
    m_context.root=root;m_context.manifest=ProjectManifest{};
    std::ifstream stream(m_context.ManifestPath(),std::ios::binary);
    if(stream){std::ostringstream text;text<<stream.rdbuf();auto parsed=resource::ParseTypedDocument(text.str(),m_context.ManifestPath().string());
        if(!parsed||parsed.Value().kind!=resource::DocumentKind::Project){Log("Invalid typed project manifest: "+m_context.ManifestPath().string());return false;}
        auto& m=m_context.manifest;ReadString(parsed.Value(),"name",m.name);ReadInt(parsed.Value(),"gameWidth",m.gameWidth);ReadInt(parsed.Value(),"gameHeight",m.gameHeight);
        ReadString(parsed.Value(),"startRoute",m.startRoute);ReadString(parsed.Value(),"startScript",m.startScript);ReadString(parsed.Value(),"theme",m.theme);if(const auto it=parsed.Value().properties.find("uiTheme");it!=parsed.Value().properties.end())if(const auto* reference=it->second.TryGet<ResourceRefValue>())m.uiThemePath=reference->lastKnownPath;
        if(const auto routes=parsed.Value().properties.find("routes");routes!=parsed.Value().properties.end()){const auto* array=routes->second.AsArray();if(!array)return false;m.routes.clear();for(const auto& value:*array){const auto* object=value.AsObject();if(!object)return false;const auto get=[&](const char* key)->std::string{const auto found=object->find(key);return found!=object->end()&&found->second.TryGet<std::string>()?*found->second.TryGet<std::string>():std::string{};};const auto scene=object->find("scene"),modal=object->find("modal");const auto* reference=scene!=object->end()?scene->second.TryGet<ResourceRefValue>():nullptr;if(!reference||reference->id.Empty()||reference->lastKnownPath.empty())return false;m.routes.push_back({get("id"),reference->lastKnownPath,modal!=object->end()&&modal->second.TryGet<bool>()?*modal->second.TryGet<bool>():false,get("cache")});}if(m.routes.empty())return false;}
        ReadBool(parsed.Value(),"encrypt",m.encrypt);ReadString(parsed.Value(),"encryptKey",m.encryptKey);ReadBool(parsed.Value(),"singleFile",m.singleFile);
    }else{EnsureEssentials({});if(!SaveManifest())return false;Log("Created project.pxproject with typed defaults.");}
    Log("Opened project: "+m_context.manifest.name+" ("+root.string()+")");return true;
}

bool ProjectService::Create(const std::filesystem::path& root,const std::string& name,const std::filesystem::path& fontSource){
    m_context.root=root;m_context.manifest=ProjectManifest{};m_context.manifest.name=name;std::error_code ec;std::filesystem::create_directories(root,ec);
    if(ec){Log("Could not create project: "+ec.message());return false;}EnsureEssentials(fontSource);if(!SaveManifest())return false;Log("Created project '"+name+"' at "+root.string());return true;
}

std::vector<std::string> ProjectService::EnsureEssentials(const std::filesystem::path& fontSource){
    std::vector<std::string> created;if(!m_context.IsOpen())return created;const auto& root=m_context.root;std::error_code ec;
    for(const char* folder:{"Content/UI","Content/Scenario","Content/Images/Background","Content/Images/Character","Content/Images/CG","Content/Images/Rules","Content/Audio/Music","Content/Audio/SFX","Content/Audio/Voice","Content/Audio/Ambience","Content/Video","Content/Fonts","Content/Localization","Content/Extensions","Content/Animations"})std::filesystem::create_directories(root/folder,ec);
    if(!std::filesystem::exists(root/kDefaultFont)&&!fontSource.empty()&&std::filesystem::exists(fontSource))if(std::filesystem::copy_file(fontSource,root/kDefaultFont,std::filesystem::copy_options::skip_existing,ec))created.push_back(kDefaultFont);
    const auto hudSource=fontSource.parent_path()/"HUD",hudTarget=root/"Content/Images/UI/HUD";std::filesystem::create_directories(hudTarget,ec);
    for(const char* name:{"dialog-stage.png","notice.png"}){const auto source=hudSource/name,target=hudTarget/name;if(!std::filesystem::exists(target)&&std::filesystem::exists(source)&&std::filesystem::copy_file(source,target,std::filesystem::copy_options::skip_existing,ec))created.push_back(std::filesystem::relative(target,root,ec).generic_string());ec.clear();}
    for(const char* screen:{"Title","HUD","Backlog","SaveLoad","Gallery","MusicRoom","History","Settings","Accessibility","ConfirmDialog","VideoOverlay"}){const auto relative=std::filesystem::path("Content/UI")/(std::string(screen)+".pxscene");const auto path=root/relative;
        if(!std::filesystem::exists(path)){const auto text=resource::WriteTypedDocument(ScreenDocument(screen,m_context.manifest.name));const Status status=io::AtomicFile::WriteText(path,text);if(status)created.push_back(relative.generic_string());}}
    const auto script=root/m_context.manifest.startScript;if(!std::filesystem::exists(script)){std::filesystem::create_directories(script.parent_path(),ec);if(io::AtomicFile::WriteText(script,StartScript(m_context.manifest.name)))created.push_back(m_context.manifest.startScript);}
    const auto catalogPath=root/"Content/Game.pxres";
    if(!std::filesystem::exists(catalogPath)){
        resource::TypedDocument catalog;catalog.kind=resource::DocumentKind::Resource;catalog.id=Uuid::Random();catalog.type="GameCatalog";
        auto input=Node("OpenSettings","InputBinding",{},{{"key",std::string("Escape")},{"command",std::string("screen.open")},{"argument",std::string("settings")}});catalog.nodes.push_back(std::move(input));
        if(io::AtomicFile::WriteText(catalogPath,resource::WriteTypedDocument(catalog)))created.push_back("Content/Game.pxres");
    }
    const auto themePath = root / m_context.manifest.uiThemePath;
    if (!std::filesystem::exists(themePath)) {
        resource::TypedDocument theme;
        theme.kind = resource::DocumentKind::Resource;
        theme.id = Uuid::Random();
        theme.type = "UITheme";
        VariantObject tokens{
            {"color.surface", Color{28, 31, 40, 255}},
            {"color.border", Color{65, 72, 91, 255}},
            {"color.text", Color{236, 239, 244, 255}},
            {"type.body", std::int64_t{24}},
        };
        VariantObject defaultStyle{{"background", TokenRefValue{"color.surface"}},
                                   {"border", TokenRefValue{"color.border"}},
                                   {"text", TokenRefValue{"color.text"}},
                                   {"fontSize", TokenRefValue{"type.body"}}};
        if(std::filesystem::exists(root/kDefaultFont)){
            tokens["font.body"]=ResourceRefValue{{},"Content/Fonts/NotoSansTC-Bold.ttf"};
            defaultStyle["font"]=TokenRefValue{"font.body"};
        }
        theme.properties["tokens"] = std::move(tokens);
        theme.properties["styles"] = VariantObject{{"Default",std::move(defaultStyle)}};
        if (io::AtomicFile::WriteText(themePath, resource::WriteTypedDocument(theme)))
            created.push_back(m_context.manifest.uiThemePath);
    }
    const auto extensionScript=root/"Content/Extensions/default.lua";
    if(!std::filesystem::exists(extensionScript)){
        const std::string source="Engine.RegisterCommand(\"demo.toast\", function(args)\n    Engine.log(\"toast: \" .. tostring(args.message))\nend)\n";
        if(io::AtomicFile::WriteText(extensionScript,source))created.push_back("Content/Extensions/default.lua");
    }
    const auto extensionManifest=root/"Content/Extensions/default.pxextension";
    if(!std::filesystem::exists(extensionManifest)){
        nlohmann::json manifest{{"format","PrismatiXExtension"},{"version",4},{"id","prismatix.default"},{"order",0},{"entry","default.lua"},{"capabilities",nlohmann::json::array({"runtime"})}};
        manifest["commands"]=nlohmann::json::array({{{"id","demo.toast"},{"displayName","Show Toast"},{"category","Extension"},{"description","Example typed Lua command"},{"await",false},{"rollback","reversible"},{"parameters",nlohmann::json::array({{{"name","message"},{"label","Message"},{"type","string"},{"required",true},{"default","Hello from Lua"}}})}}});
        if(io::AtomicFile::WriteText(extensionManifest,manifest.dump(2)+"\n"))created.push_back("Content/Extensions/default.pxextension");
    }
    const auto extensionIndex=root/"Content/Extensions/extensions.pxindex";
    if(!std::filesystem::exists(extensionIndex))
        if(io::AtomicFile::WriteText(extensionIndex,"[\n  \"Content/Extensions/default.pxextension\"\n]\n"))created.push_back("Content/Extensions/extensions.pxindex");
    const auto exportProfile=root/".prismatix/ExportProfiles/windows-release.pxexport";
    if(!std::filesystem::exists(exportProfile)){std::filesystem::create_directories(exportProfile.parent_path(),ec);ExportProfile profile;profile.encryption=m_context.manifest.encrypt;if(io::AtomicFile::WriteText(exportProfile,WriteExportProfile(profile)))created.push_back(".prismatix/ExportProfiles/windows-release.pxexport");}
    resource::AssetRegistry identities;
    for(auto it=std::filesystem::recursive_directory_iterator(root/"Content",ec);!ec&&it!=std::filesystem::recursive_directory_iterator();it.increment(ec)){
        if(!it->is_regular_file(ec)||it->path().extension()==".pxmeta")continue;
        const auto meta=resource::AssetRegistry::MetaPath(it->path());
        if(!std::filesystem::exists(meta)){auto registered=identities.RegisterAsset(root,it->path());if(registered)Log("Registered asset identity: "+std::filesystem::relative(it->path(),root,ec).generic_string());}
    }
    for(const auto& file:created)Log("Scaffolded missing file: "+file);return created;
}

bool ProjectService::SaveManifest() const {
    if(!m_context.IsOpen())return false;const auto& m=m_context.manifest;resource::TypedDocument document;document.kind=resource::DocumentKind::Project;
    document.id=Uuid::FromName(std::filesystem::absolute(m_context.root).generic_string());document.type="PrismatiXProject";
    resource::AssetRegistry registry;(void)registry.Scan(m_context.root);VariantArray routes;for(const auto& route:m.routes){const auto* asset=registry.FindPath(m_context.root/route.scene);routes.emplace_back(VariantObject{{"id",route.id},{"scene",ResourceRefValue{asset?asset->id:Uuid{},route.scene}},{"modal",route.modal},{"cache",route.cache}});}
    const auto* themeAsset=registry.FindPath(m_context.root/m.uiThemePath);
    document.properties={{"name",m.name},{"version",static_cast<std::int64_t>(m.version)},{"gameWidth",static_cast<std::int64_t>(m.gameWidth)},
        {"gameHeight",static_cast<std::int64_t>(m.gameHeight)},{"startRoute",m.startRoute},{"routes",Variant(std::move(routes))},{"startScript",m.startScript},{"theme",m.theme},
        {"uiTheme",ResourceRefValue{themeAsset?themeAsset->id:Uuid{},m.uiThemePath}},{"encrypt",m.encrypt},{"encryptKey",m.encryptKey},{"singleFile",m.singleFile}};
    const Status status=io::AtomicFile::WriteText(m_context.ManifestPath(),resource::WriteTypedDocument(document));
    if(!status){for(const auto& d:status.Diagnostics())diag::Emit(d);return false;}return true;
}

}  // namespace px::editor
