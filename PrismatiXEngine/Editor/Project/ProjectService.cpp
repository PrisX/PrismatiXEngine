#include "Editor/Project/ProjectService.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Startup/SplashTypes.h"
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

Variant DirectAction(std::string action) {
    return VariantObject{{"action", std::move(action)}, {"arguments", VariantObject{}},
                         {"kind", std::string("action")},
                         {"reentry", std::string("Allow")}};
}

Variant DialogueStyle() {
    return VariantObject{
        {"appliedStyles", VariantArray{}}, {"componentOverrides", VariantObject{}},
        {"localOverrides", VariantObject{
            {"background.color", Color{14,17,25,224}},
            {"border.color", Color{101,114,146,190}},
            {"padding", Vec2{28,22}}, {"radius.all", 12.0}}},
        {"variants", VariantObject{}}};
}

resource::TypedDocument ScreenDocument(std::string_view screen, std::string_view projectName) {
    resource::TypedDocument document; document.kind=resource::DocumentKind::Scene;
    document.id=Uuid::Random(); document.type="UIScene";
    document.properties["canvasSize"]=Vec2{1280,720};
    document.properties["uiSchemaVersion"]=std::int64_t{5};
    auto root=Node("Root","StackContainer",{},{{"anchors",Rect{0,0,1,1}}}); const Uuid rootId=root.id; document.nodes.push_back(std::move(root));
    auto button=[&](std::string name,Uuid parent,std::string text,std::string command){document.nodes.push_back(Node(std::move(name),"Button",parent,{{"text",std::move(text)},{"triggers",VariantObject{{"activated",DirectAction(std::move(command))}}}}));};
    if(screen=="HUD"){
        auto nvl=Node("NVLPanel","Panel",rootId,{{"anchors",Rect{0.055f,0.06f,0.945f,0.88f}},{"styleBinding",DialogueStyle()}});const Uuid nvlId=nvl.id;document.nodes.push_back(std::move(nvl));
        document.nodes.push_back(Node("NVLText","Label",nvlId,{{"anchors",Rect{0.04f,0.04f,0.96f,0.96f}},{"wrap",true}}));
        auto adv=Node("ADVPanel","Panel",rootId,{{"anchors",Rect{0.045f,0.64f,0.955f,0.955f}},{"styleBinding",DialogueStyle()}});const Uuid advId=adv.id;document.nodes.push_back(std::move(adv));
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
        const auto iconButton=[&](std::string name,std::string icon,std::string tooltip,std::string action){document.nodes.push_back(Node(std::move(name),"IconButton",quickId,{{"text",std::move(icon)},{"minimumSize",Vec2{44,44}},{"tooltip",std::move(tooltip)},{"triggers",VariantObject{{"activated",DirectAction(std::move(action))}}}}));};
        iconButton("Auto","▶","自動播放","mode.auto");iconButton("Skip","»","快進","mode.skip");iconButton("Backlog","≡","歷史紀錄","backlog.open");iconButton("Save","↓","儲存","save.open");iconButton("Load","↑","讀取","load.open");iconButton("Settings","⚙","設定","settings.open");iconButton("PinToolbar","◆","固定／取消固定工具列","hud.toolbar.pin");
        document.nodes.push_back(Node("ModeState","Label",rootId,{{"anchors",Rect{0.02f,0.02f,0.45f,0.09f}}}));
        return document;
    }
    if(screen=="VideoOverlay"){
        document.nodes.push_back(Node("SkipHint","Label",rootId,{{"anchors",Rect{0.78f,0.03f,0.97f,0.10f}},{"text",std::string("點擊跳過 ▶")}}));
        return document;
    }
    auto panel=Node("Surface","Panel",rootId,{{"anchors",Rect{0.03f,0.03f,0.97f,0.97f}},{"styleBinding",DialogueStyle()}});const Uuid panelId=panel.id;document.nodes.push_back(std::move(panel));
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

resource::TypedDocument SplashDocument(const ResourceRefValue& logoReference) {
    resource::TypedDocument document;
    document.kind = resource::DocumentKind::Scene;
    document.id = Uuid::Random();
    document.type = "UIScene";
    document.properties["canvasSize"] = Vec2{1280, 720};
    document.properties["uiSchemaVersion"] = std::int64_t{5};
    document.properties["dependencies"] = VariantArray{logoReference};
    auto root = Node("Root", "Panel", {},
                     {{"anchors", Rect{0, 0, 1, 1}},
                      {"styleBinding",
                       VariantObject{
                           {"appliedStyles", VariantArray{}},
                           {"componentOverrides", VariantObject{}},
                           {"localOverrides",
                            VariantObject{{"background.color", Color{0, 0, 0, 255}}}},
                           {"variants", VariantObject{}}}}});
    const Uuid rootId = root.id;
    document.nodes.push_back(std::move(root));
    auto logo = Node("PrismatiXEngineLogo", "TextureRect", rootId,
                     {{"anchors", Rect{0.25f, 0.30f, 0.75f, 0.70f}},
                      {"path", std::string(
                                   "Content/Images/UI/Splash/PrismatiXEngine_Logo.png")},
                      {"scaleMode", std::string("Fit")},
                      {"lockAspectRatio", true},
                      {"horizontalAlignment", std::string("Center")},
                      {"verticalAlignment", std::string("Center")},
                      {"opacity", 0.0}});
    const Uuid logoId = logo.id;
    document.nodes.push_back(std::move(logo));

    ui::AnimationClip enter;
    enter.id = Uuid::Random();
    enter.name = "enter";
    enter.duration = 0.5f;
    enter.tracks.push_back({logoId,
                            "opacity",
                            {{0.0f, 0.0, ui::Ease::EaseOut},
                             {0.5f, 1.0, ui::Ease::EaseOut}}});
    ui::AnimationClip exit;
    exit.id = Uuid::Random();
    exit.name = "exit";
    exit.duration = 0.5f;
    exit.tracks.push_back({logoId,
                           "opacity",
                           {{0.0f, 1.0, ui::Ease::EaseIn},
                            {0.5f, 0.0, ui::Ease::EaseIn}}});
    ui::UIAnimationLibrary animations;
    const Uuid enterClip = enter.id;
    const Uuid exitClip = exit.id;
    animations.clips.push_back(std::move(enter));
    animations.clips.push_back(std::move(exit));
    const Uuid enterState = Uuid::Random();
    const Uuid exitState = Uuid::Random();
    animations.machine.entry = enterState;
    animations.machine.states = {{enterState, "enter", enterClip, {0, 0}},
                                 {exitState, "exit", exitClip, {220, 0}}};
    document.properties["animations"] = ui::WriteUIAnimationLibrary(animations);
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
        if(const auto splashes=parsed.Value().properties.find("splashes");splashes!=parsed.Value().properties.end()){auto decoded=ui::startup::ParseSplashSequence(splashes->second,m_context.ManifestPath().generic_string());if(!decoded){for(const auto& diagnostic:decoded.Diagnostics())diag::Emit(diagnostic);return false;}m.splashes=decoded.TakeValue();}
        ReadBool(parsed.Value(),"encrypt",m.encrypt);ReadString(parsed.Value(),"encryptKey",m.encryptKey);ReadBool(parsed.Value(),"singleFile",m.singleFile);
    }else{EnsureEssentials({});(void)ConfigureDefaultSplash();if(!SaveManifest())return false;Log("Created project.pxproject with typed defaults.");}
    Log("Opened project: "+m_context.manifest.name+" ("+root.string()+")");return true;
}

bool ProjectService::Create(const std::filesystem::path& root,const std::string& name,const std::filesystem::path& fontSource){
    m_context.root=root;m_context.manifest=ProjectManifest{};m_context.manifest.name=name;std::error_code ec;std::filesystem::create_directories(root,ec);
    if(ec){Log("Could not create project: "+ec.message());return false;}EnsureEssentials(fontSource);if(!ConfigureDefaultSplash())return false;if(!SaveManifest())return false;Log("Created project '"+name+"' at "+root.string());return true;
}

std::vector<std::string> ProjectService::EnsureEssentials(const std::filesystem::path& fontSource){
    std::vector<std::string> created;if(!m_context.IsOpen())return created;const auto& root=m_context.root;std::error_code ec;
    for(const char* folder:{"Content/UI","Content/UI/Splash","Content/Scenario","Content/Images/Background","Content/Images/Character","Content/Images/CG","Content/Images/Rules","Content/Images/UI/Splash","Content/Audio/Music","Content/Audio/SFX","Content/Audio/SFX/Splash","Content/Audio/Voice","Content/Audio/Ambience","Content/Video","Content/Fonts","Content/Localization","Content/Extensions","Content/Animations"})std::filesystem::create_directories(root/folder,ec);
    if(!std::filesystem::exists(root/kDefaultFont)&&!fontSource.empty()&&std::filesystem::exists(fontSource))if(std::filesystem::copy_file(fontSource,root/kDefaultFont,std::filesystem::copy_options::skip_existing,ec))created.push_back(kDefaultFont);
    const auto hudSource=fontSource.parent_path()/"HUD",hudTarget=root/"Content/Images/UI/HUD";std::filesystem::create_directories(hudTarget,ec);
    for(const char* name:{"dialog-stage.png","notice.png"}){const auto source=hudSource/name,target=hudTarget/name;if(!std::filesystem::exists(target)&&std::filesystem::exists(source)&&std::filesystem::copy_file(source,target,std::filesystem::copy_options::skip_existing,ec))created.push_back(std::filesystem::relative(target,root,ec).generic_string());ec.clear();}
    const auto brandingSource=fontSource.parent_path()/"Branding";
    for(const auto& [name,targetPath]:std::vector<std::pair<std::string,std::string>>{
            {"PrismatiXEngine_Logo.png","Content/Images/UI/Splash/PrismatiXEngine_Logo.png"},
            {"logo.wav","Content/Audio/SFX/Splash/logo.wav"}}){
        const auto source=brandingSource/name,target=root/targetPath;
        if(!std::filesystem::exists(target)&&std::filesystem::exists(source)&&std::filesystem::copy_file(source,target,std::filesystem::copy_options::skip_existing,ec))created.push_back(targetPath);ec.clear();
    }
    constexpr const char* logoRuntimePath="Content/Images/UI/Splash/PrismatiXEngine_Logo.png";
    resource::AssetRegistry brandingIdentities;
    ResourceRefValue logoReference{{},logoRuntimePath};
    if(const auto registered=brandingIdentities.RegisterAsset(root,root/logoRuntimePath,"image"))
        logoReference.id=registered.Value().id;
    constexpr const char* audioRuntimePath="Content/Audio/SFX/Splash/logo.wav";
    (void)brandingIdentities.RegisterAsset(root,root/audioRuntimePath,"audio");
    const auto splashPath=root/"Content/UI/Splash/PrismatiXEngine.pxscene";
    if(!std::filesystem::exists(splashPath))if(io::AtomicFile::WriteText(splashPath,resource::WriteTypedDocument(SplashDocument(logoReference))))created.push_back("Content/UI/Splash/PrismatiXEngine.pxscene");
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
        ui::StyleThemeData styles;
        const auto literal=[](Variant value){return ui::StyleValue::Literal(std::move(value));};
        styles.globalDefaults.properties={
            {"background.color",literal(Color{28,31,40,255})},
            {"border.color",literal(Color{65,72,91,255})},
            {"typography.color",literal(Color{236,239,244,255})},
            {"typography.size",literal(std::int64_t{24})},
            {"padding",literal(Vec2{12,8})}, {"radius.all",literal(6.0)}};
        if(std::filesystem::exists(root/kDefaultFont))
            styles.globalDefaults.properties["typography.font"]=literal(std::string(kDefaultFont));
        styles.controlTypeDefaults["Button"].properties["background.color"]=
            literal(Color{47,54,72,245});
        for(auto [name,value]:std::vector<std::pair<std::string,Variant>>{
            {"color.surface",Color{28,31,40,255}},
            {"color.border",Color{65,72,91,255}},
            {"color.text",Color{236,239,244,255}},
            {"type.body",std::int64_t{24}}})
            (void)styles.UpsertToken({.id=Uuid::FromName("PrismatiX.Token."+name),
                .displayName=std::move(name),.type=value.Type(),.value=literal(std::move(value))});
        ui::StyleDefinition dialogue;
        dialogue.id=Uuid::FromName("PrismatiX.Style.Dialogue");dialogue.displayName="Dialogue";
        dialogue.category="Surfaces";dialogue.properties={
            {"background.color",literal(Color{14,17,25,224})},
            {"border.color",literal(Color{101,114,146,190})},
            {"padding",literal(Vec2{28,22})},{"radius.all",literal(12.0)},
            {"typography.size",literal(std::int64_t{30})}};
        (void)styles.UpsertStyle(std::move(dialogue));
        theme.properties["styleSystem"] = ui::WriteStyleTheme(styles);
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

bool ProjectService::ConfigureDefaultSplash() {
    resource::AssetRegistry registry;
    const Status scanned=registry.Scan(m_context.root);
    if(!scanned)return false;
    constexpr const char* scenePath="Content/UI/Splash/PrismatiXEngine.pxscene";
    constexpr const char* audioPath="Content/Audio/SFX/Splash/logo.wav";
    const auto* scene=registry.FindPath(m_context.root/scenePath);
    const auto* audio=registry.FindPath(m_context.root/audioPath);
    if(!scene||!audio){Log("Default splash branding assets are missing from the project scaffold.");return false;}
    ui::startup::SplashScreenEntry entry;
    entry.scene={scene->id,scenePath};entry.audio=ResourceRefValue{audio->id,audioPath};
    m_context.manifest.splashes={std::move(entry)};
    return true;
}

bool ProjectService::SaveManifest() const {
    if(!m_context.IsOpen())return false;const auto& m=m_context.manifest;resource::TypedDocument document;document.kind=resource::DocumentKind::Project;
    document.id=Uuid::FromName(std::filesystem::absolute(m_context.root).generic_string());document.type="PrismatiXProject";
    resource::AssetRegistry registry;(void)registry.Scan(m_context.root);VariantArray routes;for(const auto& route:m.routes){const auto* asset=registry.FindPath(m_context.root/route.scene);routes.emplace_back(VariantObject{{"id",route.id},{"scene",ResourceRefValue{asset?asset->id:Uuid{},route.scene}},{"modal",route.modal},{"cache",route.cache}});}
    const auto* themeAsset=registry.FindPath(m_context.root/m.uiThemePath);
    document.properties={{"name",m.name},{"version",static_cast<std::int64_t>(m.version)},{"gameWidth",static_cast<std::int64_t>(m.gameWidth)},
        {"gameHeight",static_cast<std::int64_t>(m.gameHeight)},{"startRoute",m.startRoute},{"routes",Variant(std::move(routes))},{"startScript",m.startScript},{"theme",m.theme},
        {"uiTheme",ResourceRefValue{themeAsset?themeAsset->id:Uuid{},m.uiThemePath}},{"splashes",ui::startup::WriteSplashSequence(m.splashes)},{"encrypt",m.encrypt},{"encryptKey",m.encryptKey},{"singleFile",m.singleFile}};
    const Status status=io::AtomicFile::WriteText(m_context.ManifestPath(),resource::WriteTypedDocument(document));
    if(!status){for(const auto& d:status.Diagnostics())diag::Emit(d);return false;}return true;
}

}  // namespace px::editor
