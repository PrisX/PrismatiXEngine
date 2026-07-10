#include "Editor/Project/ProjectService.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Resources/AssetRegistry.h"

#include <fstream>
#include <sstream>
#include <tuple>

namespace px::editor {
namespace {
constexpr const char* kDefaultFont = "Content/Fonts/NotoSansTC-Bold.ttf";

std::string StartScript(std::string_view name) {
    return "// " + std::string(name) + " — PrismatiX visual novel\n"
           "[name speaker=\"\"]\nWelcome to " + std::string(name) +
           "!\nEdit this scene in the Story workspace.\n";
}

resource::NodeRecord Node(std::string name, std::string type, Uuid parent = {},
                          std::map<std::string, Variant> properties = {}) {
    return {Uuid::Random(), parent, std::move(name), std::move(type), std::move(properties)};
}

resource::TypedDocument ScreenDocument(std::string_view screen, std::string_view projectName) {
    resource::TypedDocument document; document.kind=resource::DocumentKind::Scene; document.formatVersion=1;
    document.id=Uuid::Random(); document.type="UIScene"; document.properties["canvasSize"]=Vec2{1280,720};
    auto root=Node("Root","StackContainer",{},{{"anchors",Rect{0,0,1,1}}}); const Uuid rootId=root.id; document.nodes.push_back(std::move(root));
    auto button=[&](std::string name,Uuid parent,std::string text,std::string command){document.nodes.push_back(Node(std::move(name),"Button",parent,{{"text",std::move(text)},{"command",std::move(command)}}));};
    if(screen=="HUD"){
        auto nvl=Node("NVLPanel","Panel",rootId,{{"anchors",Rect{0.055f,0.06f,0.945f,0.88f}},{"themeVariant",std::string("Dialogue")}});const Uuid nvlId=nvl.id;document.nodes.push_back(std::move(nvl));
        document.nodes.push_back(Node("NVLText","Label",nvlId,{{"anchors",Rect{0.04f,0.04f,0.96f,0.96f}},{"wrap",true}}));
        auto adv=Node("ADVPanel","Panel",rootId,{{"anchors",Rect{0.045f,0.64f,0.955f,0.955f}},{"themeVariant",std::string("Dialogue")}});const Uuid advId=adv.id;document.nodes.push_back(std::move(adv));
        document.nodes.push_back(Node("Speaker","Label",advId,{{"anchors",Rect{0.03f,0.08f,0.97f,0.25f}},{"bind.text",std::string("dialogue.speaker")}}));
        document.nodes.push_back(Node("Dialogue","RichTextLabel",advId,{{"anchors",Rect{0.03f,0.28f,0.97f,0.91f}},{"bind.markup",std::string("dialogue.text")}}));
        document.nodes.push_back(Node("Choices","VBoxContainer",rootId,{{"anchors",Rect{0.20f,0.18f,0.80f,0.60f}},{"separation",std::int64_t{12}}}));
        auto quick=Node("QuickMenu","HBoxContainer",rootId,{{"anchors",Rect{0.50f,0.025f,0.97f,0.105f}},{"separation",std::int64_t{8}}});const Uuid quickId=quick.id;document.nodes.push_back(std::move(quick));
        button("Auto",quickId,"AUTO","mode.auto");button("Skip",quickId,"SKIP","mode.skip");button("Backlog",quickId,"LOG","backlog.open");button("Save",quickId,"SAVE","save.open");button("Load",quickId,"LOAD","load.open");button("Settings",quickId,"⚙","settings.open");
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
        for(const auto& [name,label,max]:std::vector<std::tuple<std::string,std::string,double>>{{"BGM","BGM",128},{"SE","SE",128},{"Voice","Voice",128},{"TextSpeed","文字速度",120}}){document.nodes.push_back(Node(name+"Label","Label",columnId,{{"text",label}}));document.nodes.push_back(Node(name,"Slider",columnId,{{"maximum",max}}));}
        document.nodes.push_back(Node("SkipRead","CheckBox",columnId,{{"text",std::string("只快進已讀")}}));document.nodes.push_back(Node("Fullscreen","CheckBox",columnId,{{"text",std::string("全螢幕")}}));button("Close",columnId,"完成","overlay.close");
    }
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
        ReadString(parsed.Value(),"startUI",m.startUI);ReadString(parsed.Value(),"startScript",m.startScript);ReadString(parsed.Value(),"theme",m.theme);
        ReadBool(parsed.Value(),"encrypt",m.encrypt);ReadString(parsed.Value(),"encryptKey",m.encryptKey);ReadBool(parsed.Value(),"singleFile",m.singleFile);
    }else{if(!SaveManifest())return false;Log("Created project.pxproject with typed defaults.");}
    Log("Opened project: "+m_context.manifest.name+" ("+root.string()+")");return true;
}

bool ProjectService::Create(const std::filesystem::path& root,const std::string& name,const std::filesystem::path& fontSource){
    m_context.root=root;m_context.manifest=ProjectManifest{};m_context.manifest.name=name;std::error_code ec;std::filesystem::create_directories(root,ec);
    if(ec){Log("Could not create project: "+ec.message());return false;}if(!SaveManifest())return false;EnsureEssentials(fontSource);Log("Created project '"+name+"' at "+root.string());return true;
}

std::vector<std::string> ProjectService::EnsureEssentials(const std::filesystem::path& fontSource){
    std::vector<std::string> created;if(!m_context.IsOpen())return created;const auto& root=m_context.root;std::error_code ec;
    for(const char* folder:{"Content/UI","Content/Script","Content/Images/Background","Content/Images/Character","Content/Images/CG","Content/Images/Rules","Content/Audio/Music","Content/Audio/SFX","Content/Audio/Voice","Content/Video","Content/Fonts","Content/Localization","Content/Extensions"})std::filesystem::create_directories(root/folder,ec);
    if(!std::filesystem::exists(root/kDefaultFont)&&!fontSource.empty()&&std::filesystem::exists(fontSource))if(std::filesystem::copy_file(fontSource,root/kDefaultFont,std::filesystem::copy_options::skip_existing,ec))created.push_back(kDefaultFont);
    for(const char* screen:{"Title","HUD","Backlog","SaveLoad","Gallery","Settings","VideoOverlay"}){const auto relative=std::filesystem::path("Content/UI")/(std::string(screen)+".pxscene");const auto path=root/relative;
        if(!std::filesystem::exists(path)){const auto text=resource::WriteTypedDocument(ScreenDocument(screen,m_context.manifest.name));const Status status=io::AtomicFile::WriteText(path,text);if(status)created.push_back(relative.generic_string());}}
    const auto script=root/m_context.manifest.startScript;if(!std::filesystem::exists(script)){std::filesystem::create_directories(script.parent_path(),ec);if(io::AtomicFile::WriteText(script,StartScript(m_context.manifest.name)))created.push_back(m_context.manifest.startScript);}
    const auto catalogPath=root/"Content/Game.pxres";
    if(!std::filesystem::exists(catalogPath)){
        resource::TypedDocument catalog;catalog.kind=resource::DocumentKind::Resource;catalog.id=Uuid::Random();catalog.type="GameCatalog";
        auto input=Node("OpenSettings","InputBinding",{},{{"key",std::string("Escape")},{"command",std::string("screen.open")},{"argument",std::string("settings")}});catalog.nodes.push_back(std::move(input));
        if(io::AtomicFile::WriteText(catalogPath,resource::WriteTypedDocument(catalog)))created.push_back("Content/Game.pxres");
    }
    resource::AssetRegistry identities;
    for(auto it=std::filesystem::recursive_directory_iterator(root/"Content",ec);!ec&&it!=std::filesystem::recursive_directory_iterator();it.increment(ec)){
        if(!it->is_regular_file(ec)||it->path().extension()==".pxmeta")continue;
        const auto meta=resource::AssetRegistry::MetaPath(it->path());
        if(!std::filesystem::exists(meta)){auto registered=identities.RegisterAsset(root,it->path());if(registered)Log("Registered asset identity: "+std::filesystem::relative(it->path(),root,ec).generic_string());}
    }
    for(const auto& file:created)Log("Scaffolded missing file: "+file);return created;
}

bool ProjectService::SaveManifest() const {
    if(!m_context.IsOpen())return false;const auto& m=m_context.manifest;resource::TypedDocument document;document.kind=resource::DocumentKind::Project;document.formatVersion=1;
    document.id=Uuid::FromName(std::filesystem::absolute(m_context.root).generic_string());document.type="PrismatiXProject";
    document.properties={{"name",m.name},{"version",static_cast<std::int64_t>(m.version)},{"gameWidth",static_cast<std::int64_t>(m.gameWidth)},
        {"gameHeight",static_cast<std::int64_t>(m.gameHeight)},{"startUI",m.startUI},{"startScript",m.startScript},{"theme",m.theme},
        {"encrypt",m.encrypt},{"encryptKey",m.encryptKey},{"singleFile",m.singleFile}};
    const Status status=io::AtomicFile::WriteText(m_context.ManifestPath(),resource::WriteTypedDocument(document));
    if(!status){for(const auto& d:status.Diagnostics())diag::Emit(d);return false;}return true;
}

}  // namespace px::editor
