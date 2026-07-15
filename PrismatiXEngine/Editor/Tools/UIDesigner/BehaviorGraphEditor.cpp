#include "Editor/Tools/UIDesigner/BehaviorGraphEditor.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/Actions/ActionCatalog.h"

#include <imgui.h>
#include <imgui_node_editor.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace ed = ax::NodeEditor;

namespace px::editor {
namespace {

enum class PinType { Flow, Any, Bool, Integer, Number, String, Color, Object };
struct PinSpec { std::string name; std::string label; PinType type=PinType::Any; bool input=false; };
struct PinAddress { Uuid node; std::string name; PinType type=PinType::Any; bool input=false; };

std::uintptr_t StableId(const Uuid& id,const std::string_view suffix={}){
    const Uuid stable=suffix.empty()?id:Uuid::FromName(id.ToString()+"/"+std::string(suffix));
    const auto value=static_cast<std::uintptr_t>(UuidHash{}(stable));return value?value:1;
}
ed::NodeId NodeId(const Uuid& id){return ed::NodeId(StableId(id,"node"));}
ed::NodeId GroupId(const Uuid& id){return ed::NodeId(StableId(id,"group"));}
ed::PinId PinId(const Uuid& node,const bool input,const std::string_view name){return ed::PinId(StableId(node,std::string(input?"in/":"out/")+std::string(name)));}
ed::LinkId LinkId(const Uuid& id){return ed::LinkId(StableId(id,"link"));}

PinType TypeFor(const VariantType type){switch(type){case VariantType::Bool:return PinType::Bool;case VariantType::Integer:return PinType::Integer;case VariantType::Number:return PinType::Number;case VariantType::String:return PinType::String;case VariantType::Color:return PinType::Color;case VariantType::Object:return PinType::Object;default:return PinType::Any;}}
ImColor PinColor(const PinType type){switch(type){case PinType::Flow:return {255,255,255};case PinType::Bool:return {239,102,122};case PinType::Integer:return {100,205,255};case PinType::Number:return {84,225,180};case PinType::String:return {202,137,255};case PinType::Color:return {255,190,90};case PinType::Object:return {130,155,190};case PinType::Any:return {190,190,200};}return {255,255,255};}
bool Compatible(const PinAddress& from,const PinAddress& to){if(from.input||!to.input)return false;if(from.type==PinType::Flow||to.type==PinType::Flow)return from.type==to.type;if(from.type==PinType::Any||to.type==PinType::Any)return true;return from.type==to.type||(from.type==PinType::Integer&&to.type==PinType::Number);}

std::vector<PinSpec> Pins(const ui::BehaviorNode& node,const UISceneDocument& document){
    using Kind=ui::BehaviorNodeKind;std::vector<PinSpec> result;
    const auto in=[&](std::string name,std::string label,PinType type){result.push_back({std::move(name),std::move(label),type,true});};
    const auto out=[&](std::string name,std::string label,PinType type){result.push_back({std::move(name),std::move(label),type,false});};
    if(node.kind==Kind::SignalEntry){out("out","Flow",PinType::Flow);const auto control=node.properties.find("control"),signal=node.properties.find("signal");const auto* id=control==node.properties.end()?nullptr:control->second.TryGet<Uuid>();const auto* name=signal==node.properties.end()?nullptr:signal->second.TryGet<std::string>();const auto* record=id?document.Find(*id):nullptr;const auto* metadata=record&&name?TypeRegistry::Global().FindSignal(record->type,*name):nullptr;if(metadata)for(const auto& argument:metadata->arguments)out(argument.name,argument.name,TypeFor(argument.type));}
    else if(node.kind==Kind::Action){in("in","In",PinType::Flow);out("out","Out",PinType::Flow);const auto action=node.properties.find("action");const auto* id=action==node.properties.end()?nullptr:action->second.TryGet<std::string>();if(const auto* descriptor=id?ui::ActionCatalog::Global().Find(*id):nullptr)for(const auto& argument:descriptor->arguments)in("arg:"+argument.name,argument.displayName.empty()?argument.name:argument.displayName,TypeFor(argument.type));}
    else if(node.kind==Kind::Sequence){in("in","In",PinType::Flow);for(int i=0;i<4;++i)out(std::to_string(i),"Then "+std::to_string(i+1),PinType::Flow);}
    else if(node.kind==Kind::Branch){in("in","In",PinType::Flow);in("condition","Condition",PinType::Bool);out("true","True",PinType::Flow);out("false","False",PinType::Flow);}
    else if(node.kind==Kind::Delay){in("in","In",PinType::Flow);in("seconds","Seconds",PinType::Number);out("out","Out",PinType::Flow);}
    else if(node.kind==Kind::Constant){const auto value=node.properties.find("value");out("value","Value",value==node.properties.end()?PinType::Any:TypeFor(value->second.Type()));}
    else if(node.kind==Kind::Compare){in("left","Left",PinType::Any);in("right","Right",PinType::Any);out("value","Result",PinType::Bool);}
    else if(node.kind==Kind::Boolean){in("left","Left",PinType::Bool);in("right","Right",PinType::Bool);out("value","Result",PinType::Bool);}
    else if(node.kind==Kind::GetVariable||node.kind==Kind::GetProperty)out("value","Value",PinType::Any);
    else if(node.kind==Kind::SetVariable||node.kind==Kind::SetProperty){in("in","In",PinType::Flow);in("value","Value",PinType::Any);out("out","Out",PinType::Flow);}
    else if(node.kind==Kind::PlayAnimation){in("in","In",PinType::Flow);out("out","Out",PinType::Flow);}
    else if(node.kind==Kind::SetAnimationParameter){in("in","In",PinType::Flow);in("value","Value",PinType::Any);out("out","Out",PinType::Flow);}
    else if(node.kind==Kind::TravelAnimationState){in("in","In",PinType::Flow);out("out","Out",PinType::Flow);}
    return result;
}

std::string NodeTitle(const ui::BehaviorNode& node,const UISceneDocument& document){
    if(node.kind==ui::BehaviorNodeKind::SignalEntry){
        const auto control=node.properties.find("control"),signal=node.properties.find("signal");
        const auto* id=control==node.properties.end()?nullptr:control->second.TryGet<Uuid>();
        const auto* name=signal==node.properties.end()?nullptr:signal->second.TryGet<std::string>();
        const auto* record=id?document.Find(*id):nullptr;
        const auto* metadata=record&&name?TypeRegistry::Global().FindSignal(record->type,*name):nullptr;
        return "WHEN  "+std::string(record?record->name:"Missing Control")+" · "+
               std::string(metadata&&!metadata->displayName.empty()?metadata->displayName:(name?*name:"Missing signal"));
    }
    const auto action=node.properties.find("action");
    if(node.kind==ui::BehaviorNodeKind::Action&&action!=node.properties.end())
        if(const auto* id=action->second.TryGet<std::string>())
            if(const auto* descriptor=ui::ActionCatalog::Global().Find(*id))return "DO  "+descriptor->displayName;
    return ui::BehaviorNodeKindName(node.kind);
}

Variant GraphBefore(const UISceneDocument& document){const auto found=document.Data().properties.find("interactionGraph");return found==document.Data().properties.end()?Variant{}:found->second.Clone();}
Result<ui::BehaviorGraph> ReadGraph(const UISceneDocument& document){const Variant value=GraphBefore(document);if(value.Type()==VariantType::Null)return Result<ui::BehaviorGraph>::Success(ui::BehaviorGraph{});return ui::ParseBehaviorGraph(value,document.Path().generic_string());}
bool Commit(UISceneDocument& document,DesignerCommandService& commands,const Variant& before,const ui::BehaviorGraph& graph,const char* label){const Status valid=graph.Validate(document.Path().generic_string());if(!valid){for(const auto& diagnostic:valid.Diagnostics())diag::Emit(diagnostic);return false;}auto command=std::make_unique<PropertyChangeCommand>(label,document.DocumentId(),"interactionGraph",before,ui::WriteBehaviorGraph(graph),std::chrono::steady_clock::now(),false);const Status status=commands.Execute(std::move(command),DocumentChangeSet::Property(document.DocumentId(),"interactionGraph",DesignerDirtyFlags::Binding));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return static_cast<bool>(status);}

VariantObject Defaults(const ui::BehaviorNodeKind kind,const UISceneDocument& document,const Uuid& selected){using Kind=ui::BehaviorNodeKind;switch(kind){case Kind::SignalEntry:{VariantObject result{{"control",selected}};if(const auto* record=document.Find(selected)){const auto signals=TypeRegistry::Global().SignalsForType(record->type);if(!signals.empty())result["signal"]=signals.front()->name;}return result;}case Kind::Action:{const auto& actions=ui::ActionCatalog::Global().Descriptors();return {{"action",actions.empty()?std::string{}:actions.front().id},{"arguments",VariantObject{}},{"wait",true}};}case Kind::Delay:return {{"seconds",.5}};case Kind::Constant:return {{"value",false}};case Kind::Branch:return {{"condition",false}};case Kind::Compare:return {{"operator",std::string("Equal")},{"left",Variant{}},{"right",Variant{}}};case Kind::Boolean:return {{"operator",std::string("And")}};case Kind::GetVariable:case Kind::SetVariable:return {{"name",std::string("variable")},{"value",Variant{}}};case Kind::GetProperty:case Kind::SetProperty:return {{"target",selected},{"property",std::string("visibility")},{"value",std::string("Visible")}};case Kind::PlayAnimation:return {{"name",std::string("Default")},{"wait",true}};case Kind::SetAnimationParameter:return {{"name",std::string("parameter")},{"value",false}};case Kind::TravelAnimationState:return {{"state",std::string("Default")},{"duration",0.15}};default:return {};}}

bool ReferencedEntry(const UISceneDocument& document,const Uuid& id){for(const auto& record:document.Data().nodes)if(const auto triggers=record.properties.find("triggers");triggers!=record.properties.end())if(const auto* definitions=triggers->second.AsObject())for(const auto& [_,value]:*definitions)if(const auto* binding=value.AsObject())if(const auto entry=binding->find("entry");entry!=binding->end()&&entry->second.TryGet<Uuid>()&&*entry->second.TryGet<Uuid>()==id)return true;return false;}

}  // namespace

BehaviorGraphEditor::BehaviorGraphEditor()=default;
BehaviorGraphEditor::~BehaviorGraphEditor(){if(m_context)ed::DestroyEditor(m_context);}
void BehaviorGraphEditor::EnsureContext(){if(m_context)return;ed::Config config;config.SettingsFile=nullptr;m_context=ed::CreateEditor(&config);}
void BehaviorGraphEditor::FrameSelection(){m_frameSelection=true;}
void BehaviorGraphEditor::FocusNode(DesignerBehaviorGraphState& state,const Uuid& node){state.focusNode=node;m_frameSelection=true;}
void BehaviorGraphEditor::ClearSelection(DesignerBehaviorGraphState& state){
    state.selectedNode={};state.selectedGroup={};state.focusNode={};
    if(m_context){ed::SetCurrentEditor(m_context);ed::ClearSelection();ed::SetCurrentEditor(nullptr);}
}

bool BehaviorGraphEditor::Render(UISceneDocument& document,DesignerCommandService& commands,DesignerBehaviorGraphState& state,const Uuid& selectedControl){
    EnsureContext();auto parsed=ReadGraph(document);if(!parsed){for(const auto& diagnostic:parsed.Diagnostics())diag::Emit(diagnostic);ImGui::TextColored({1,.4f,.35f,1},"Behavior Graph 無法解析，請查看 Problems。");return false;}ui::BehaviorGraph graph=parsed.TakeValue();const Variant before=GraphBefore(document);bool dirty=false;std::unordered_map<std::uintptr_t,PinAddress> pinMap;
    if(ImGui::Button("＋ 節點")){m_pendingCreatePin=0;const ImVec2 mouse=ImGui::GetMousePos();m_pendingCreateX=mouse.x;m_pendingCreateY=mouse.y;ImGui::OpenPopup("新增 Behavior 節點");}ImGui::SameLine();if(ImGui::Button("＋ 群組／註解")){const Uuid id=Uuid::Random();const float offset=40.0f*static_cast<float>(graph.groups.size()%8);graph.groups.push_back({id,"註解",{60+offset,60+offset,360,220}});state.selectedGroup=id;state.selectedNode={};dirty=true;}ImGui::SameLine();if(ImGui::Button("框選內容")){m_frameSelection=true;}ImGui::SameLine();ImGui::TextDisabled("右鍵或從 pin 拖到空白處開啟 Palette；Ctrl+C / Ctrl+V 複製貼上");if(!m_debugState.fibers.empty()){ImGui::SameLine();ImGui::TextColored({1.0f,.7f,.25f,1.0f},"● %zu fibers",m_debugState.fibers.size());}
    ed::SetCurrentEditor(m_context);ed::Begin("##behavior-graph",ImVec2(0,0));
    for(auto& group:graph.groups){if(m_initializedGroups.insert(group.id).second)ed::SetNodePosition(GroupId(group.id),{group.bounds.x,group.bounds.y});ed::BeginNode(GroupId(group.id));ImGui::TextDisabled("%s",group.title.c_str());ImGui::Dummy({std::max(120.0f,group.bounds.w),std::max(80.0f,group.bounds.h)});ed::Group({std::max(120.0f,group.bounds.w),std::max(80.0f,group.bounds.h)});ed::EndNode();}
    for(const auto& node:graph.nodes){if(m_initializedNodes.insert(node.id).second)ed::SetNodePosition(NodeId(node.id),{node.position.x,node.position.y});const bool active=std::any_of(m_debugState.fibers.begin(),m_debugState.fibers.end(),[&](const ui::BehaviorFiberState& fiber){return fiber.current==node.id;});ed::PushStyleColor(ed::StyleColor_NodeBg,active?ImColor(118,72,22,252):ImColor(24,30,43,245));ed::BeginNode(NodeId(node.id));ImGui::PushID(node.id.ToString().c_str());const bool entry=node.kind==ui::BehaviorNodeKind::SignalEntry;if(entry)ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.98f,.73f,.32f,1));else ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.8f,.9f,1,1));ImGui::TextUnformatted(NodeTitle(node,document).c_str());ImGui::PopStyleColor();ImGui::Separator();const auto pins=Pins(node,document);for(const auto& pin:pins){const auto id=PinId(node.id,pin.input,pin.name);pinMap.emplace(id.Get(),PinAddress{node.id,pin.name,pin.type,pin.input});ed::BeginPin(id,pin.input?ed::PinKind::Input:ed::PinKind::Output);if(!pin.input)ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),170.0f-ImGui::CalcTextSize(pin.label.c_str()).x));ImGui::TextColored(PinColor(pin.type),"%s%s",pin.input?"● ":"",pin.label.c_str());if(!pin.input){ImGui::SameLine();ImGui::TextColored(PinColor(pin.type),"●");}ed::EndPin();}ImGui::PopID();ed::EndNode();ed::PopStyleColor();}
    for(const auto& link:graph.links)ed::Link(LinkId(link.id),PinId(link.fromNode,false,link.fromPin),PinId(link.toNode,true,link.toPin),ImColor(150,205,255),2.0f);
    if(ed::BeginCreate()){
        ed::PinId start=0,end=0;
        if(ed::QueryNewLink(&start,&end)){
            auto a=pinMap.find(start.Get()),b=pinMap.find(end.Get());
            if(a!=pinMap.end()&&b!=pinMap.end()){
                PinAddress from=a->second,to=b->second;if(from.input){std::swap(from,to);std::swap(start,end);}
                if(Compatible(from,to)){if(ed::AcceptNewItem()){std::erase_if(graph.links,[&](const ui::BehaviorLink& link){return link.toNode==to.node&&link.toPin==to.name;});graph.links.push_back({Uuid::Random(),from.node,from.name,to.node,to.name});dirty=true;}}
                else ed::RejectNewItem(ImColor(255,80,80),2.0f);
            }
        }
        ed::PinId createPin=0;
        if(ed::QueryNewNode(&createPin)&&ed::AcceptNewItem()){
            m_pendingCreatePin=createPin.Get();const ImVec2 mouse=ImGui::GetMousePos();m_pendingCreateX=mouse.x;m_pendingCreateY=mouse.y;m_filter[0]='\0';ImGui::OpenPopup("新增 Behavior 節點");
        }
    }
    ed::EndCreate();
    if(ed::BeginDelete()){ed::NodeId nodeId=0;while(ed::QueryDeletedNode(&nodeId)){const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const ui::BehaviorNode& node){return NodeId(node.id)==nodeId;});const auto group=std::find_if(graph.groups.begin(),graph.groups.end(),[&](const ui::BehaviorGroup& item){return GroupId(item.id)==nodeId;});if(found!=graph.nodes.end()&&!ReferencedEntry(document,found->id)&&ed::AcceptDeletedItem()){const Uuid id=found->id;std::erase_if(graph.nodes,[&](const ui::BehaviorNode& node){return node.id==id;});std::erase_if(graph.links,[&](const ui::BehaviorLink& link){return link.fromNode==id||link.toNode==id;});m_initializedNodes.erase(id);dirty=true;}else if(group!=graph.groups.end()&&ed::AcceptDeletedItem()){m_initializedGroups.erase(group->id);graph.groups.erase(group);state.selectedGroup={};dirty=true;}else ed::RejectDeletedItem();}ed::LinkId linkId=0;while(ed::QueryDeletedLink(&linkId))if(ed::AcceptDeletedItem()){std::erase_if(graph.links,[&](const ui::BehaviorLink& link){return LinkId(link.id)==linkId;});dirty=true;}}ed::EndDelete();
    if(!state.focusNode.Empty()){ed::ClearSelection();ed::SelectNode(NodeId(state.focusNode));state.selectedNode=state.focusNode;state.selectedGroup={};state.focusNode={};}
    if(m_frameSelection){ed::NavigateToSelection(true,0.25f);m_frameSelection=false;}
    ed::Suspend();if(ed::ShowBackgroundContextMenu()){m_pendingCreatePin=0;const ImVec2 mouse=ImGui::GetMousePos();m_pendingCreateX=mouse.x;m_pendingCreateY=mouse.y;ImGui::OpenPopup("新增 Behavior 節點");}
    if(ImGui::BeginPopup("新增 Behavior 節點")){
        ImGui::InputTextWithHint("##behavior-search","搜尋節點或 Action…",m_filter,sizeof(m_filter));const std::string filter=m_filter;
        const auto pending=pinMap.find(m_pendingCreatePin);const PinAddress* source=pending==pinMap.end()?nullptr:&pending->second;
        const auto canConnect=[&](ui::BehaviorNodeKind kind,VariantObject properties){
            if(!source)return true;if(properties.empty())properties=Defaults(kind,document,selectedControl);const ui::BehaviorNode candidate{Uuid::Random(),kind,{},std::move(properties)};
            for(const auto& pin:Pins(candidate,document)){const PinAddress address{candidate.id,pin.name,pin.type,pin.input};if(source->input?Compatible(address,*source):Compatible(*source,address))return true;}return false;
        };
        const auto add=[&](ui::BehaviorNodeKind kind,VariantObject properties={}){
            if(properties.empty())properties=Defaults(kind,document,selectedControl);const Uuid id=Uuid::Random();const ImVec2 position=ed::ScreenToCanvas({m_pendingCreateX,m_pendingCreateY});const ui::BehaviorNode created{id,kind,{position.x,position.y},std::move(properties)};graph.nodes.push_back(created);
            if(source)for(const auto& pin:Pins(created,document)){const PinAddress address{id,pin.name,pin.type,pin.input};if(source->input&&Compatible(address,*source)){std::erase_if(graph.links,[&](const ui::BehaviorLink& link){return link.toNode==source->node&&link.toPin==source->name;});graph.links.push_back({Uuid::Random(),id,pin.name,source->node,source->name});break;}if(!source->input&&Compatible(*source,address)){graph.links.push_back({Uuid::Random(),source->node,source->name,id,pin.name});break;}}
            m_pendingCreatePin=0;dirty=true;ImGui::CloseCurrentPopup();
        };
        for(const auto kind:{ui::BehaviorNodeKind::SignalEntry,ui::BehaviorNodeKind::Sequence,ui::BehaviorNodeKind::Branch,ui::BehaviorNodeKind::Delay,ui::BehaviorNodeKind::Constant,ui::BehaviorNodeKind::Compare,ui::BehaviorNodeKind::Boolean,ui::BehaviorNodeKind::GetVariable,ui::BehaviorNodeKind::SetVariable,ui::BehaviorNodeKind::GetProperty,ui::BehaviorNodeKind::SetProperty,ui::BehaviorNodeKind::PlayAnimation,ui::BehaviorNodeKind::SetAnimationParameter,ui::BehaviorNodeKind::TravelAnimationState}){const std::string label=ui::BehaviorNodeKindName(kind);const VariantObject properties=Defaults(kind,document,selectedControl);if(canConnect(kind,properties)&&(filter.empty()||label.find(filter)!=std::string::npos)&&ImGui::MenuItem(label.c_str()))add(kind,properties);}
        ImGui::SeparatorText("Actions");for(const auto& action:ui::ActionCatalog::Global().Descriptors())if(action.available&&(filter.empty()||action.id.find(filter)!=std::string::npos||action.displayName.find(filter)!=std::string::npos)){VariantObject arguments;for(const auto& argument:action.arguments)if(argument.defaultValue)arguments[argument.name]=argument.defaultValue->Clone();VariantObject properties{{"action",action.id},{"arguments",std::move(arguments)},{"wait",true}};if(canConnect(ui::BehaviorNodeKind::Action,properties)&&ImGui::MenuItem((action.category+" / "+action.displayName+"##graph-"+action.id).c_str()))add(ui::BehaviorNodeKind::Action,std::move(properties));}ImGui::EndPopup();
    }
    ed::Resume();ed::End();
    if(!graph.nodes.empty()){ImVec2 minimum{graph.nodes.front().position.x,graph.nodes.front().position.y},maximum=minimum;for(const auto& node:graph.nodes){minimum.x=std::min(minimum.x,node.position.x);minimum.y=std::min(minimum.y,node.position.y);maximum.x=std::max(maximum.x,node.position.x+220);maximum.y=std::max(maximum.y,node.position.y+120);}const ImVec2 size{170,105},end=ImGui::GetItemRectMax(),origin{end.x-size.x-12,end.y-size.y-12};ImDrawList* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(8,12,20,220),6);draw->AddRect(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(95,120,160,210),6);const float scale=std::min((size.x-12)/std::max(1.0f,maximum.x-minimum.x),(size.y-12)/std::max(1.0f,maximum.y-minimum.y));for(const auto& node:graph.nodes){const ImVec2 p{origin.x+6+(node.position.x-minimum.x)*scale,origin.y+6+(node.position.y-minimum.y)*scale};draw->AddRectFilled(p,{p.x+std::max(4.0f,210*scale),p.y+std::max(3.0f,90*scale)},IM_COL32(90,160,225,220),2);}}
    const int selectedCount=ed::GetSelectedObjectCount();if(selectedCount>0){std::vector<ed::NodeId> selected(static_cast<std::size_t>(selectedCount));const int count=ed::GetSelectedNodes(selected.data(),selectedCount);if(count>0){const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const ui::BehaviorNode& node){return NodeId(node.id)==selected.front();});const auto group=std::find_if(graph.groups.begin(),graph.groups.end(),[&](const ui::BehaviorGroup& item){return GroupId(item.id)==selected.front();});if(found!=graph.nodes.end()){state.selectedNode=found->id;state.selectedGroup={};}else if(group!=graph.groups.end()){state.selectedGroup=group->id;state.selectedNode={};}}}
    const ImGuiIO& io=ImGui::GetIO();if(io.KeyCtrl&&!io.WantTextInput&&ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)){
        if(ImGui::IsKeyPressed(ImGuiKey_C,false)){m_clipboard={};std::vector<ed::NodeId> selected(static_cast<std::size_t>(std::max(0,selectedCount)));const int count=ed::GetSelectedNodes(selected.data(),selectedCount);std::unordered_set<Uuid,UuidHash> ids;for(int i=0;i<count;++i)for(const auto& node:graph.nodes)if(NodeId(node.id)==selected[static_cast<std::size_t>(i)]){m_clipboard.nodes.push_back(node);ids.insert(node.id);}for(const auto& link:graph.links)if(ids.contains(link.fromNode)&&ids.contains(link.toNode))m_clipboard.links.push_back(link);}
        if(ImGui::IsKeyPressed(ImGuiKey_V,false)&&!m_clipboard.nodes.empty()){std::unordered_map<Uuid,Uuid,UuidHash> ids;for(const auto& source:m_clipboard.nodes){auto copy=source;ids[source.id]=copy.id=Uuid::Random();copy.position.x+=32;copy.position.y+=32;graph.nodes.push_back(std::move(copy));}for(const auto& source:m_clipboard.links){auto copy=source;copy.id=Uuid::Random();copy.fromNode=ids.at(source.fromNode);copy.toNode=ids.at(source.toNode);graph.links.push_back(std::move(copy));}dirty=true;}
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){for(auto& node:graph.nodes){const ImVec2 position=ed::GetNodePosition(NodeId(node.id));if(std::abs(position.x-node.position.x)>.5f||std::abs(position.y-node.position.y)>.5f){node.position={position.x,position.y};dirty=true;}}for(auto& group:graph.groups){const ImVec2 position=ed::GetNodePosition(GroupId(group.id));const ImVec2 size=ed::GetNodeSize(GroupId(group.id));if(std::abs(position.x-group.bounds.x)>.5f||std::abs(position.y-group.bounds.y)>.5f){group.bounds.x=position.x;group.bounds.y=position.y;dirty=true;}group.bounds.w=std::max(120.0f,size.x);group.bounds.h=std::max(80.0f,size.y);}}
    ed::SetCurrentEditor(nullptr);return dirty&&Commit(document,commands,before,graph,"Edit Behavior Graph");
}

bool BehaviorGraphEditor::RenderInspector(UISceneDocument& document,DesignerCommandService& commands,DesignerBehaviorGraphState& state,const Uuid& selectedControl){auto parsed=ReadGraph(document);if(!parsed){ImGui::TextDisabled("Behavior Graph 無法解析。");return false;}auto graph=parsed.TakeValue();const Variant before=GraphBefore(document);if(state.selectedNode.Empty()){const auto group=std::find_if(graph.groups.begin(),graph.groups.end(),[&](const ui::BehaviorGroup& item){return item.id==state.selectedGroup;});if(group==graph.groups.end()){ImGui::TextDisabled("選取 Behavior 節點或群組以編輯屬性。");return false;}std::string title=group->title;if(ImGui::InputText("群組／註解標題",&title)){group->title=std::move(title);return Commit(document,commands,before,graph,"Edit Behavior Group");}ImGui::Text("UUID  %s",group->id.ToString().c_str());return false;}auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const ui::BehaviorNode& node){return node.id==state.selectedNode;});if(found==graph.nodes.end())return false;auto& node=*found;bool changed=false;ImGui::TextDisabled("%s",ui::BehaviorNodeKindName(node.kind));ImGui::Text("UUID  %s",node.id.ToString().c_str());
    const auto text=[&](const char* label,const char* key,std::string fallback={}){auto value=node.properties.find(key);std::string edited=value!=node.properties.end()&&value->second.TryGet<std::string>()?*value->second.TryGet<std::string>():std::move(fallback);if(ImGui::InputText(label,&edited)){node.properties[key]=std::move(edited);changed=true;}};
    const auto boolean=[&](const char* label,const char* key,bool fallback){auto value=node.properties.find(key);bool edited=value!=node.properties.end()&&value->second.TryGet<bool>()?*value->second.TryGet<bool>():fallback;if(ImGui::Checkbox(label,&edited)){node.properties[key]=edited;changed=true;}};
    if(node.kind==ui::BehaviorNodeKind::SignalEntry){auto* record=document.Find(selectedControl);Uuid control=selectedControl;if(const auto value=node.properties.find("control");value!=node.properties.end()&&value->second.TryGet<Uuid>())control=*value->second.TryGet<Uuid>();if(ImGui::Button("使用目前 Control")){node.properties["control"]=selectedControl;control=selectedControl;record=document.Find(control);changed=true;}std::string signal;if(const auto value=node.properties.find("signal");value!=node.properties.end()&&value->second.TryGet<std::string>())signal=*value->second.TryGet<std::string>();if(record&&ImGui::BeginCombo("Signal",signal.empty()?"選擇 Signal":signal.c_str())){for(const auto* item:TypeRegistry::Global().SignalsForType(record->type))if(ImGui::Selectable(item->displayName.c_str(),signal==item->name)){node.properties["signal"]=item->name;signal=item->name;changed=true;}ImGui::EndCombo();}}
    else if(node.kind==ui::BehaviorNodeKind::Action){std::string action;if(const auto value=node.properties.find("action");value!=node.properties.end()&&value->second.TryGet<std::string>())action=*value->second.TryGet<std::string>();if(ImGui::BeginCombo("Action",action.empty()?"選擇 Action":action.c_str())){for(const auto& item:ui::ActionCatalog::Global().Descriptors())if(item.available&&ImGui::Selectable((item.category+" / "+item.displayName+"##inspect-"+item.id).c_str(),item.id==action)){node.properties["action"]=item.id;node.properties["arguments"]=VariantObject{};action=item.id;changed=true;}ImGui::EndCombo();}boolean("等待完成","wait",true);if(const auto* descriptor=ui::ActionCatalog::Global().Find(action)){auto* arguments=node.properties["arguments"].AsObject();if(!arguments){node.properties["arguments"]=VariantObject{};arguments=node.properties["arguments"].AsObject();}for(const auto& argument:descriptor->arguments){auto value=arguments->find(argument.name);if(value==arguments->end())value=arguments->emplace(argument.name,argument.defaultValue?argument.defaultValue->Clone():Variant{}).first;if(auto* string=value->second.TryGet<std::string>()){if(ImGui::InputText(argument.displayName.c_str(),string))changed=true;}else if(auto* integer=value->second.TryGet<std::int64_t>()){int edited=static_cast<int>(*integer);if(ImGui::DragInt(argument.displayName.c_str(),&edited)){*integer=edited;changed=true;}}else if(auto* flag=value->second.TryGet<bool>())if(ImGui::Checkbox(argument.displayName.c_str(),flag))changed=true;}}}
    else if(node.kind==ui::BehaviorNodeKind::Delay){double seconds=.5;if(const auto value=node.properties.find("seconds");value!=node.properties.end()){if(const auto* number=value->second.TryGet<double>())seconds=*number;else if(const auto* integer=value->second.TryGet<std::int64_t>())seconds=static_cast<double>(*integer);}float edited=static_cast<float>(seconds);if(ImGui::DragFloat("秒數",&edited,.05f,0,3600)){node.properties["seconds"]=static_cast<double>(edited);changed=true;}}
    else if(node.kind==ui::BehaviorNodeKind::Constant){auto& value=node.properties["value"];if(value.Type()==VariantType::Null)value=false;if(auto* flag=value.TryGet<bool>()){if(ImGui::Checkbox("Value",flag))changed=true;}else if(auto* string=value.TryGet<std::string>()){if(ImGui::InputText("Value",string))changed=true;}else if(auto* number=value.TryGet<double>()){float edited=static_cast<float>(*number);if(ImGui::DragFloat("Value",&edited)){*number=edited;changed=true;}}}
    else if(node.kind==ui::BehaviorNodeKind::Compare){text("Operator","operator","Equal");}
    else if(node.kind==ui::BehaviorNodeKind::Boolean){text("Operator","operator","And");}
    else if(node.kind==ui::BehaviorNodeKind::GetVariable||node.kind==ui::BehaviorNodeKind::SetVariable)text("Variable","name","variable");
    else if(node.kind==ui::BehaviorNodeKind::GetProperty||node.kind==ui::BehaviorNodeKind::SetProperty){if(ImGui::Button("Target = selected Control")){node.properties["target"]=selectedControl;changed=true;}text("Property","property","visibility");}
    else if(node.kind==ui::BehaviorNodeKind::PlayAnimation){text("Animation / .pxanim","name","default");if(ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){const std::string path(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);if(std::filesystem::path(path).extension()==".pxanim"){node.properties["name"]=path;changed=true;}}ImGui::EndDragDropTarget();}if(ImGui::IsItemHovered())ImGui::SetTooltip("default / embedded 播放場景內嵌 clip；也可拖入外部 .pxanim，Preview 與 Player 共用 Timeline runtime。");boolean("等待完成","wait",true);}
    else if(node.kind==ui::BehaviorNodeKind::SetAnimationParameter){text("Parameter","name","parameter");auto& value=node.properties["value"];if(value.Type()==VariantType::Null)value=false;if(auto* flag=value.TryGet<bool>()){if(ImGui::Checkbox("Value",flag))changed=true;}else if(auto* number=value.TryGet<double>()){float edited=static_cast<float>(*number);if(ImGui::DragFloat("Value",&edited,.05f)){*number=edited;changed=true;}}else if(ImGui::Button("Fire Trigger")){value=true;changed=true;}}
    else if(node.kind==ui::BehaviorNodeKind::TravelAnimationState){text("State","state","Default");double duration=.15;if(const auto value=node.properties.find("duration");value!=node.properties.end()&&value->second.TryGet<double>())duration=*value->second.TryGet<double>();float edited=static_cast<float>(duration);if(ImGui::DragFloat("Transition Duration",&edited,.01f,0,10)){node.properties["duration"]=static_cast<double>(edited);changed=true;}}
    bool debugHeader=false;for(const auto& fiber:m_debugState.fibers)if(fiber.current==node.id){if(!debugHeader){ImGui::SeparatorText("Interactive Preview");debugHeader=true;}ImGui::PushID(static_cast<int>(fiber.id));ImGui::TextColored({1,.72f,.28f,1},"Fiber %llu · running",static_cast<unsigned long long>(fiber.id));if(fiber.delayRemaining>0)ImGui::Text("Delay  %.3fs",fiber.delayRemaining);if(fiber.animationHandle)ImGui::Text("Animation handle  %llu",static_cast<unsigned long long>(fiber.animationHandle));if(fiber.actionExecution){const auto action=std::find_if(m_debugState.actions.begin(),m_debugState.actions.end(),[&](const ui::ActionExecutionCheckpoint& checkpoint){return checkpoint.execution==fiber.actionExecution;});if(action!=m_debugState.actions.end())ImGui::Text("Action  %s · %s",action->invocation.action.c_str(),action->providerId.c_str());else ImGui::Text("Action execution  %llu",static_cast<unsigned long long>(fiber.actionExecution));}if(!fiber.signalArguments.empty()&&ImGui::TreeNode("Signal arguments")){for(const auto& [name,value]:fiber.signalArguments)ImGui::BulletText("%s  (%s)",name.c_str(),ToString(value.Type()));ImGui::TreePop();}ImGui::PopID();}
    return changed&&Commit(document,commands,before,graph,"Edit Behavior Node");}

}  // namespace px::editor
