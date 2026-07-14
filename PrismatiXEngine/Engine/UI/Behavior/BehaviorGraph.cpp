#include "Engine/UI/Behavior/BehaviorGraph.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/Actions/ActionCatalog.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace px::ui {
namespace {

diag::Diagnostic Error(std::string code, std::string message, const std::string& source = {},
                       const Uuid& node = {}, std::string property = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,
                                .code=std::move(code),
                                .category="UI.Behavior",
                                .message=std::move(message)};
    diagnostic.source.path=source;
    if(!node.Empty())diagnostic.source.nodeId=node.ToString();
    diagnostic.source.property=std::move(property);
    return diagnostic;
}

bool FlowNode(const BehaviorNodeKind kind) {
    return kind == BehaviorNodeKind::SignalEntry || kind == BehaviorNodeKind::Action ||
           kind == BehaviorNodeKind::Sequence || kind == BehaviorNodeKind::Branch ||
           kind == BehaviorNodeKind::Delay || kind == BehaviorNodeKind::SetVariable ||
           kind == BehaviorNodeKind::SetProperty || kind == BehaviorNodeKind::PlayAnimation ||
           kind == BehaviorNodeKind::SetAnimationParameter ||
           kind == BehaviorNodeKind::TravelAnimationState;
}

const Variant* Property(const BehaviorNode& node, std::string_view name);

enum class GraphPinType { Flow, Any, Bool, Integer, Number, String, Color, Object };
GraphPinType PinTypeFor(const VariantType type){switch(type){case VariantType::Bool:return GraphPinType::Bool;case VariantType::Integer:return GraphPinType::Integer;case VariantType::Number:return GraphPinType::Number;case VariantType::String:return GraphPinType::String;case VariantType::Color:return GraphPinType::Color;case VariantType::Object:return GraphPinType::Object;default:return GraphPinType::Any;}}
std::optional<GraphPinType> PinType(const BehaviorNode& node,const std::string_view pin,const bool input){using Kind=BehaviorNodeKind;
    if(node.kind==Kind::SignalEntry&&!input)return pin=="out"?GraphPinType::Flow:GraphPinType::Any;
    if(node.kind==Kind::Action){if(input&&pin=="in")return GraphPinType::Flow;if(!input&&pin=="out")return GraphPinType::Flow;if(input&&pin.starts_with("arg:")){const auto action=Property(node,"action");const auto* id=action?action->TryGet<std::string>():nullptr;if(const auto* descriptor=id?ActionCatalog::Global().Find(*id):nullptr)for(const auto& argument:descriptor->arguments)if(pin.substr(4)==argument.name)return PinTypeFor(argument.type);}return std::nullopt;}
    if(node.kind==Kind::Sequence)return input?(pin=="in"?std::optional(GraphPinType::Flow):std::nullopt):(!pin.empty()?std::optional(GraphPinType::Flow):std::nullopt);
    if(node.kind==Kind::Branch){if(input&&pin=="in")return GraphPinType::Flow;if(input&&pin=="condition")return GraphPinType::Bool;if(!input&&(pin=="true"||pin=="false"))return GraphPinType::Flow;return std::nullopt;}
    if(node.kind==Kind::Delay){if(input&&pin=="in")return GraphPinType::Flow;if(input&&pin=="seconds")return GraphPinType::Number;if(!input&&pin=="out")return GraphPinType::Flow;return std::nullopt;}
    if(node.kind==Kind::Constant&&!input&&pin=="value"){const auto value=Property(node,"value");return value?PinTypeFor(value->Type()):GraphPinType::Any;}
    if(node.kind==Kind::Compare){if(input&&(pin=="left"||pin=="right"))return GraphPinType::Any;if(!input&&pin=="value")return GraphPinType::Bool;return std::nullopt;}
    if(node.kind==Kind::Boolean){if(input&&(pin=="left"||pin=="right"))return GraphPinType::Bool;if(!input&&pin=="value")return GraphPinType::Bool;return std::nullopt;}
    if((node.kind==Kind::GetVariable||node.kind==Kind::GetProperty)&&!input&&pin=="value")return GraphPinType::Any;
    if(node.kind==Kind::SetVariable||node.kind==Kind::SetProperty){if(input&&pin=="in")return GraphPinType::Flow;if(input&&pin=="value")return GraphPinType::Any;if(!input&&pin=="out")return GraphPinType::Flow;return std::nullopt;}
    if(node.kind==Kind::PlayAnimation){if(input&&pin=="in")return GraphPinType::Flow;if(!input&&pin=="out")return GraphPinType::Flow;}
    if(node.kind==Kind::SetAnimationParameter){if(input&&pin=="in")return GraphPinType::Flow;if(input&&pin=="value")return GraphPinType::Any;if(!input&&pin=="out")return GraphPinType::Flow;}
    if(node.kind==Kind::TravelAnimationState){if(input&&pin=="in")return GraphPinType::Flow;if(!input&&pin=="out")return GraphPinType::Flow;}
    return std::nullopt;
}
bool Compatible(const GraphPinType from,const GraphPinType to){if(from==GraphPinType::Flow||to==GraphPinType::Flow)return from==to;if(from==GraphPinType::Any||to==GraphPinType::Any)return true;return from==to||(from==GraphPinType::Integer&&to==GraphPinType::Number);}

const Variant* Property(const BehaviorNode& node, const std::string_view name) {
    const auto found=node.properties.find(std::string(name));
    return found==node.properties.end()?nullptr:&found->second;
}

bool Number(const Variant& value, double& output) {
    if(const auto* real=value.TryGet<double>()){output=*real;return true;}
    if(const auto* integer=value.TryGet<std::int64_t>()){output=static_cast<double>(*integer);return true;}
    return false;
}

}  // namespace

const BehaviorNode* BehaviorGraph::Find(const Uuid& id) const {
    const auto found=std::find_if(nodes.begin(),nodes.end(),[&](const BehaviorNode& node){return node.id==id;});
    return found==nodes.end()?nullptr:&*found;
}

const char* BehaviorNodeKindName(const BehaviorNodeKind kind) {
    switch(kind){
        case BehaviorNodeKind::SignalEntry:return "SignalEntry";
        case BehaviorNodeKind::Action:return "Action";
        case BehaviorNodeKind::Sequence:return "Sequence";
        case BehaviorNodeKind::Branch:return "Branch";
        case BehaviorNodeKind::Delay:return "Delay";
        case BehaviorNodeKind::Constant:return "Constant";
        case BehaviorNodeKind::Compare:return "Compare";
        case BehaviorNodeKind::Boolean:return "Boolean";
        case BehaviorNodeKind::GetVariable:return "GetVariable";
        case BehaviorNodeKind::SetVariable:return "SetVariable";
        case BehaviorNodeKind::GetProperty:return "GetProperty";
        case BehaviorNodeKind::SetProperty:return "SetProperty";
        case BehaviorNodeKind::PlayAnimation:return "PlayAnimation";
        case BehaviorNodeKind::SetAnimationParameter:return "SetAnimationParameter";
        case BehaviorNodeKind::TravelAnimationState:return "TravelAnimationState";
    }
    return "Constant";
}

std::optional<BehaviorNodeKind> ParseBehaviorNodeKind(const std::string_view value) {
    for(const auto kind:{BehaviorNodeKind::SignalEntry,BehaviorNodeKind::Action,
        BehaviorNodeKind::Sequence,BehaviorNodeKind::Branch,BehaviorNodeKind::Delay,
        BehaviorNodeKind::Constant,BehaviorNodeKind::Compare,BehaviorNodeKind::Boolean,
        BehaviorNodeKind::GetVariable,BehaviorNodeKind::SetVariable,
        BehaviorNodeKind::GetProperty,BehaviorNodeKind::SetProperty,
        BehaviorNodeKind::PlayAnimation,BehaviorNodeKind::SetAnimationParameter,
        BehaviorNodeKind::TravelAnimationState})
        if(value==BehaviorNodeKindName(kind))return kind;
    return std::nullopt;
}

Status BehaviorGraph::Validate(const std::string& sourcePath) const {
    if(version!=CurrentVersion)return Status::Fail(Error("PXUIBEH4001","Behavior Graph version must be 1",sourcePath));
    std::unordered_set<Uuid,UuidHash> nodeIds,linkIds;
    for(const auto& node:nodes){
        if(node.id.Empty()||!nodeIds.insert(node.id).second)
            return Status::Fail(Error("PXUIBEH4002","Behavior Graph has an empty or duplicate node UUID",sourcePath,node.id));
    }
    std::unordered_map<Uuid,std::vector<Uuid>,UuidHash> flow;std::unordered_set<std::string> occupiedInputs;
    for(const auto& link:links){
        if(link.id.Empty()||!linkIds.insert(link.id).second)
            return Status::Fail(Error("PXUIBEH4003","Behavior Graph has an empty or duplicate link UUID",sourcePath));
        const auto* from=Find(link.fromNode);const auto* to=Find(link.toNode);
        if(!from||!to||link.fromPin.empty()||link.toPin.empty())
            return Status::Fail(Error("PXUIBEH4004","Behavior Graph link endpoint is invalid",sourcePath));
        if(link.fromNode==link.toNode)
            return Status::Fail(Error("PXUIBEH4005","Behavior Graph self-links are forbidden",sourcePath,link.fromNode));
        const auto fromType=PinType(*from,link.fromPin,false),toType=PinType(*to,link.toPin,true);
        if(!fromType||!toType)return Status::Fail(Error("PXUIBEH4007","Behavior link references a missing pin",sourcePath,fromType?link.toNode:link.fromNode,fromType?link.toPin:link.fromPin));
        if(!Compatible(*fromType,*toType))return Status::Fail(Error("PXUIBEH4008","Behavior link pin types are incompatible",sourcePath,link.toNode,link.toPin));
        const std::string inputKey=link.toNode.ToString()+"/"+link.toPin;if(!occupiedInputs.insert(inputKey).second)return Status::Fail(Error("PXUIBEH4009","Behavior input pin has more than one link",sourcePath,link.toNode,link.toPin));
        if(*fromType==GraphPinType::Flow)flow[link.fromNode].push_back(link.toNode);
    }
    std::unordered_set<Uuid,UuidHash> visiting,visited;
    std::function<bool(const Uuid&)> cycle=[&](const Uuid& id){
        if(visiting.contains(id))return true;if(visited.contains(id))return false;
        visiting.insert(id);for(const auto& next:flow[id])if(cycle(next))return true;
        visiting.erase(id);visited.insert(id);return false;};
    for(const auto& node:nodes)if(FlowNode(node.kind)&&cycle(node.id))
        return Status::Fail(Error("PXUIBEH4006","Behavior Graph flow cycles are forbidden",sourcePath,node.id));
    return Status::Ok();
}

Result<BehaviorGraph> ParseBehaviorGraph(const Variant& value,const std::string& sourcePath){
    const auto* object=value.AsObject();if(!object)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4010","interactionGraph must be an Object",sourcePath));
    BehaviorGraph graph;
    const auto versionIt=object->find("version"),nodesIt=object->find("nodes"),linksIt=object->find("links");
    const auto* version=versionIt==object->end()?nullptr:versionIt->second.TryGet<std::int64_t>();
    const auto* nodes=nodesIt==object->end()?nullptr:nodesIt->second.AsArray();
    const auto* links=linksIt==object->end()?nullptr:linksIt->second.AsArray();
    if(!version||!nodes||!links)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4011","interactionGraph requires version, nodes, and links",sourcePath));
    graph.version=*version;
    for(const auto& item:*nodes){const auto* node=item.AsObject();if(!node)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4012","Behavior node must be an Object",sourcePath));
        const auto idIt=node->find("id"),kindIt=node->find("kind");const auto* id=idIt==node->end()?nullptr:idIt->second.TryGet<Uuid>();const auto* kindName=kindIt==node->end()?nullptr:kindIt->second.TryGet<std::string>();const auto kind=kindName?ParseBehaviorNodeKind(*kindName):std::nullopt;if(!id||!kind)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4013","Behavior node id or kind is invalid",sourcePath));
        BehaviorNode parsed{.id=*id,.kind=*kind};if(const auto p=node->find("position");p!=node->end())if(const auto* position=p->second.TryGet<Vec2>())parsed.position=*position;if(const auto p=node->find("properties");p!=node->end()){const auto* properties=p->second.AsObject();if(!properties)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4014","Behavior node properties must be an Object",sourcePath,*id));parsed.properties=*properties;}graph.nodes.push_back(std::move(parsed));}
    for(const auto& item:*links){const auto* link=item.AsObject();if(!link)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4015","Behavior link must be an Object",sourcePath));BehaviorLink parsed;const auto id=link->find("id"),from=link->find("fromNode"),fromPin=link->find("fromPin"),to=link->find("toNode"),toPin=link->find("toPin");const auto* idValue=id==link->end()?nullptr:id->second.TryGet<Uuid>();const auto* fromValue=from==link->end()?nullptr:from->second.TryGet<Uuid>();const auto* fromPinValue=fromPin==link->end()?nullptr:fromPin->second.TryGet<std::string>();const auto* toValue=to==link->end()?nullptr:to->second.TryGet<Uuid>();const auto* toPinValue=toPin==link->end()?nullptr:toPin->second.TryGet<std::string>();if(!idValue||!fromValue||!fromPinValue||!toValue||!toPinValue)return Result<BehaviorGraph>::Failure(Error("PXUIBEH4016","Behavior link fields are invalid",sourcePath));parsed={*idValue,*fromValue,*fromPinValue,*toValue,*toPinValue};graph.links.push_back(std::move(parsed));}
    if(const auto groupsIt=object->find("groups");groupsIt!=object->end())if(const auto* groups=groupsIt->second.AsArray())for(const auto& item:*groups)if(const auto* group=item.AsObject()){const auto id=group->find("id"),title=group->find("title"),bounds=group->find("bounds");const auto* idValue=id==group->end()?nullptr:id->second.TryGet<Uuid>();const auto* titleValue=title==group->end()?nullptr:title->second.TryGet<std::string>();const auto* boundsValue=bounds==group->end()?nullptr:bounds->second.TryGet<Rect>();if(idValue&&titleValue&&boundsValue)graph.groups.push_back({*idValue,*titleValue,*boundsValue});}
    const Status valid=graph.Validate(sourcePath);return valid?Result<BehaviorGraph>::Success(std::move(graph)):Result<BehaviorGraph>::Failure(valid.Diagnostics());
}

Variant WriteBehaviorGraph(const BehaviorGraph& graph){VariantArray nodes,links,groups;for(const auto& node:graph.nodes)nodes.emplace_back(VariantObject{{"id",node.id},{"kind",std::string(BehaviorNodeKindName(node.kind))},{"position",node.position},{"properties",node.properties}});for(const auto& link:graph.links)links.emplace_back(VariantObject{{"id",link.id},{"fromNode",link.fromNode},{"fromPin",link.fromPin},{"toNode",link.toNode},{"toPin",link.toPin}});for(const auto& group:graph.groups)groups.emplace_back(VariantObject{{"id",group.id},{"title",group.title},{"bounds",group.bounds}});return VariantObject{{"version",graph.version},{"nodes",std::move(nodes)},{"links",std::move(links)},{"groups",std::move(groups)}};}

Status BehaviorGraphRunner::SetGraph(BehaviorGraph graph,std::string sourceScene){const Status valid=graph.Validate(sourceScene);if(!valid)return valid;m_graph=std::move(graph);m_sourceScene=std::move(sourceScene);CancelAll();return Status::Ok();}
const BehaviorLink* BehaviorGraphRunner::Incoming(const Uuid& node,const std::string_view pin)const{const auto found=std::find_if(m_graph.links.begin(),m_graph.links.end(),[&](const BehaviorLink& link){return link.toNode==node&&link.toPin==pin;});return found==m_graph.links.end()?nullptr:&*found;}
std::vector<const BehaviorLink*> BehaviorGraphRunner::Outgoing(const Uuid& node)const{std::vector<const BehaviorLink*> result;for(const auto& link:m_graph.links)if(link.fromNode==node&&link.toPin=="in")result.push_back(&link);std::sort(result.begin(),result.end(),[](const auto* left,const auto* right){return std::tie(left->fromPin,left->id)<std::tie(right->fromPin,right->id);});return result;}

Result<Variant> BehaviorGraphRunner::Input(const BehaviorFiberState& fiber,const BehaviorNode& node,const std::string_view pin,const Variant* fallback,std::unordered_set<Uuid,UuidHash>& visiting,std::size_t& budget)const{if(const auto* link=Incoming(node.id,pin))return Evaluate(fiber,link->fromNode,link->fromPin,visiting,budget);return fallback?Result<Variant>::Success(fallback->Clone()):Result<Variant>::Success(Variant{});}

Result<Variant> BehaviorGraphRunner::Evaluate(const BehaviorFiberState& fiber,const Uuid& id,const std::string_view output,std::unordered_set<Uuid,UuidHash>& visiting,std::size_t& budget)const{if(budget--==0)return Result<Variant>::Failure(Error("PXUIBEH4020","Behavior data evaluation budget exceeded",m_sourceScene,id));if(!visiting.insert(id).second)return Result<Variant>::Failure(Error("PXUIBEH4021","Behavior data dependency cycle detected",m_sourceScene,id));const auto* node=m_graph.Find(id);if(!node){visiting.erase(id);return Result<Variant>::Failure(Error("PXUIBEH4022","Behavior data node is missing",m_sourceScene,id));}Result<Variant> result=Result<Variant>::Success(Variant{});
    if(node->kind==BehaviorNodeKind::Constant){const auto* value=Property(*node,"value");result=Result<Variant>::Success(value?value->Clone():Variant{});}
    else if(node->kind==BehaviorNodeKind::SignalEntry){std::string argument(output);if(argument.starts_with("arg:"))argument.erase(0,4);const auto found=fiber.signalArguments.find(argument);result=Result<Variant>::Success(found==fiber.signalArguments.end()?Variant{}:found->second.Clone());}
    else if(node->kind==BehaviorNodeKind::GetVariable){const auto* name=Property(*node,"name");const auto* text=name?name->TryGet<std::string>():nullptr;const auto value=text&&m_services.readVariable?m_services.readVariable(*text):std::nullopt;result=Result<Variant>::Success(value?value->Clone():Variant{});}
    else if(node->kind==BehaviorNodeKind::GetProperty){const auto* target=Property(*node,"target");const auto* property=Property(*node,"property");const auto* targetId=target?target->TryGet<Uuid>():nullptr;const auto* name=property?property->TryGet<std::string>():nullptr;auto* object=m_services.root&&targetId?m_services.root->Find(*targetId):nullptr;const auto* descriptor=object&&name?TypeRegistry::Global().FindProperty(std::string(object->TypeName()),*name):nullptr;if(!object||!descriptor||!descriptor->get)result=Result<Variant>::Failure(Error("PXUIBEH4023","Behavior property source is invalid",m_sourceScene,id));else result=Result<Variant>::Success(descriptor->get(*object));}
    else if(node->kind==BehaviorNodeKind::Compare){auto left=Input(fiber,*node,"left",Property(*node,"left"),visiting,budget);auto right=Input(fiber,*node,"right",Property(*node,"right"),visiting,budget);if(!left||!right)result=Result<Variant>::Failure(!left?left.Diagnostics():right.Diagnostics());else{const auto* opValue=Property(*node,"operator");const std::string op=opValue&&opValue->TryGet<std::string>()?*opValue->TryGet<std::string>():"Equal";bool compared=false;double a=0,b=0;if(op=="Equal")compared=left.Value()==right.Value();else if(op=="NotEqual")compared=!(left.Value()==right.Value());else if(Number(left.Value(),a)&&Number(right.Value(),b)){if(op=="Less")compared=a<b;else if(op=="LessEqual")compared=a<=b;else if(op=="Greater")compared=a>b;else if(op=="GreaterEqual")compared=a>=b;}result=Result<Variant>::Success(Variant(compared));}}
    else if(node->kind==BehaviorNodeKind::Boolean){auto left=Input(fiber,*node,"left",Property(*node,"left"),visiting,budget);auto right=Input(fiber,*node,"right",Property(*node,"right"),visiting,budget);const auto* a=left&&left.Value().TryGet<bool>()?left.Value().TryGet<bool>():nullptr;const auto* b=right&&right.Value().TryGet<bool>()?right.Value().TryGet<bool>():nullptr;const auto* opValue=Property(*node,"operator");const std::string op=opValue&&opValue->TryGet<std::string>()?*opValue->TryGet<std::string>():"Not";result=Result<Variant>::Success(Variant(op=="Not"?!(a&&*a):op=="And"?(a&&*a&&b&&*b):(a&&*a)||(b&&*b)));}
    visiting.erase(id);return result;}

void BehaviorGraphRunner::Advance(BehaviorFiberState& fiber,const std::string_view pin){const auto links=Outgoing(fiber.current);const auto found=std::find_if(links.begin(),links.end(),[&](const BehaviorLink* link){return link->fromPin==pin;});if(found!=links.end()){fiber.current=(*found)->toNode;return;}if(!fiber.continuation.empty()){fiber.current=fiber.continuation.back();fiber.continuation.pop_back();}else fiber.current={};}

Status BehaviorGraphRunner::Run(BehaviorFiberState& fiber,ActionContext context){std::size_t budget=1024;while(!fiber.current.Empty()&&budget--){const auto* node=m_graph.Find(fiber.current);if(!node)return Status::Fail(Error("PXUIBEH4030","Behavior flow node is missing",m_sourceScene,fiber.current));std::unordered_set<Uuid,UuidHash> visiting;std::size_t dataBudget=256;
        if(node->kind==BehaviorNodeKind::SignalEntry){Advance(fiber);continue;}
        if(node->kind==BehaviorNodeKind::Sequence){auto links=Outgoing(node->id);if(links.empty()){Advance(fiber);continue;}for(auto iterator=links.rbegin();iterator!=links.rend();++iterator)fiber.continuation.push_back((*iterator)->toNode);fiber.current=fiber.continuation.back();fiber.continuation.pop_back();continue;}
        if(node->kind==BehaviorNodeKind::Branch){auto condition=Input(fiber,*node,"condition",Property(*node,"condition"),visiting,dataBudget);if(!condition)return Status::Fail(condition.Diagnostics());const auto* value=condition.Value().TryGet<bool>();Advance(fiber,value&&*value?"true":"false");continue;}
        if(node->kind==BehaviorNodeKind::Delay){auto seconds=Input(fiber,*node,"seconds",Property(*node,"seconds"),visiting,dataBudget);if(!seconds)return Status::Fail(seconds.Diagnostics());double value=0;Number(seconds.Value(),value);fiber.delayRemaining=static_cast<float>(std::max(0.0,value));if(fiber.delayRemaining<=0)Advance(fiber);return Status::Ok();}
        if(node->kind==BehaviorNodeKind::Action){const auto* actionValue=Property(*node,"action");const auto* action=actionValue?actionValue->TryGet<std::string>():nullptr;if(!action||!m_services.actions)return Status::Fail(Error("PXUIBEH4031","Behavior Action node is invalid",m_sourceScene,node->id));VariantObject arguments;if(const auto* stored=Property(*node,"arguments"))if(const auto* object=stored->AsObject())arguments=*object;if(const auto* descriptor=m_services.actions->Catalog().Find(*action))for(const auto& argument:descriptor->arguments){const std::string pin="arg:"+argument.name;if(!Incoming(node->id,pin))continue;auto evaluated=Input(fiber,*node,pin,nullptr,visiting,dataBudget);if(!evaluated)return Status::Fail(evaluated.Diagnostics());arguments[argument.name]=evaluated.TakeValue();}context.sourceScene=m_sourceScene;context.sourceNode=node->id;context.signal="behavior";auto started=m_services.actions->Start({*action,std::move(arguments),context});if(!started)return Status::Fail(started.Diagnostics());const auto* wait=Property(*node,"wait");if(wait&&wait->TryGet<bool>()&&*wait->TryGet<bool>()&&m_services.actions->State(started.Value())==ActionExecutionState::Running){fiber.actionExecution=started.Value();return Status::Ok();}m_services.actions->Forget(started.Value());Advance(fiber);continue;}
        if(node->kind==BehaviorNodeKind::SetVariable){const auto* nameValue=Property(*node,"name");const auto* name=nameValue?nameValue->TryGet<std::string>():nullptr;auto value=Input(fiber,*node,"value",Property(*node,"value"),visiting,dataBudget);if(!name||!value||!m_services.writeVariable)return Status::Fail(Error("PXUIBEH4032","Set Variable node is invalid",m_sourceScene,node->id));const Status written=m_services.writeVariable(*name,value.Value());if(!written)return written;Advance(fiber);continue;}
        if(node->kind==BehaviorNodeKind::SetProperty){const auto* targetValue=Property(*node,"target");const auto* propertyValue=Property(*node,"property");const auto* target=targetValue?targetValue->TryGet<Uuid>():nullptr;const auto* property=propertyValue?propertyValue->TryGet<std::string>():nullptr;auto value=Input(fiber,*node,"value",Property(*node,"value"),visiting,dataBudget);auto* object=m_services.root&&target?m_services.root->Find(*target):nullptr;const auto* descriptor=object&&property?TypeRegistry::Global().FindProperty(std::string(object->TypeName()),*property):nullptr;if(!value||!object||!descriptor||!descriptor->set)return Status::Fail(Error("PXUIBEH4033","Set Property node is invalid",m_sourceScene,node->id));const Status set=descriptor->set(*object,value.Value());if(!set)return set;Advance(fiber);continue;}
        if(node->kind==BehaviorNodeKind::PlayAnimation){const auto* nameValue=Property(*node,"name");const auto* name=nameValue?nameValue->TryGet<std::string>():nullptr;if(!name||!m_services.playAnimation)return Status::Fail(Error("PXUIBEH4034","Play Animation node is invalid",m_sourceScene,node->id));auto played=m_services.playAnimation(*name);if(!played)return Status::Fail(played.Diagnostics());const auto* wait=Property(*node,"wait");if(wait&&wait->TryGet<bool>()&&*wait->TryGet<bool>()){fiber.animationHandle=played.Value();return Status::Ok();}Advance(fiber);continue;}
        if(node->kind==BehaviorNodeKind::SetAnimationParameter){const auto* nameValue=Property(*node,"name");const auto* name=nameValue?nameValue->TryGet<std::string>():nullptr;auto value=Input(fiber,*node,"value",Property(*node,"value"),visiting,dataBudget);if(!name||!value||!m_services.setAnimationParameter)return Status::Fail(Error("PXUIBEH4037","Set Animation Parameter node is invalid",m_sourceScene,node->id));const Status set=m_services.setAnimationParameter(*name,value.Value());if(!set)return set;Advance(fiber);continue;}
        if(node->kind==BehaviorNodeKind::TravelAnimationState){const auto* stateValue=Property(*node,"state");const auto* state=stateValue?stateValue->TryGet<std::string>():nullptr;double duration=0.0;if(const auto* stored=Property(*node,"duration"))Number(*stored,duration);if(!state||!m_services.travelAnimationState)return Status::Fail(Error("PXUIBEH4038","Travel Animation State node is invalid",m_sourceScene,node->id));const Status travelled=m_services.travelAnimationState(*state,static_cast<float>(std::max(0.0,duration)));if(!travelled)return travelled;Advance(fiber);continue;}
        return Status::Fail(Error("PXUIBEH4035","Data-only node cannot appear in the flow",m_sourceScene,node->id));}
    return fiber.current.Empty()?Status::Ok():Status::Fail(Error("PXUIBEH4036","Behavior execution budget exceeded",m_sourceScene,fiber.current));}

Result<BehaviorFiberId> BehaviorGraphRunner::Start(const Uuid& entry,VariantObject signalArguments,ActionContext context,const ActionReentryPolicy reentry){const auto* node=m_graph.Find(entry);if(!node||node->kind!=BehaviorNodeKind::SignalEntry)return Result<BehaviorFiberId>::Failure(Error("PXUIBEH4040","Behavior entry is missing or has the wrong kind",m_sourceScene,entry));const auto running=std::find_if(m_fibers.begin(),m_fibers.end(),[&](const BehaviorFiberState& fiber){return fiber.entry==entry;});if(running!=m_fibers.end()&&reentry==ActionReentryPolicy::IgnoreWhileRunning)return Result<BehaviorFiberId>::Success(running->id);if(running!=m_fibers.end()&&reentry==ActionReentryPolicy::Restart){if(m_services.actions)for(const auto& fiber:m_fibers)if(fiber.entry==entry&&fiber.actionExecution)(void)m_services.actions->Cancel(fiber.actionExecution);std::erase_if(m_fibers,[&](const BehaviorFiberState& fiber){return fiber.entry==entry;});}BehaviorFiberState fiber{.id=m_nextFiber++,.entry=entry,.current=entry,.signalArguments=std::move(signalArguments)};context.sourceScene=m_sourceScene;const Status status=Run(fiber,std::move(context));if(!status){Fail(status);return Result<BehaviorFiberId>::Failure(status.Diagnostics());}const auto id=fiber.id;if(!fiber.current.Empty())m_fibers.push_back(std::move(fiber));return Result<BehaviorFiberId>::Success(id);}

void BehaviorGraphRunner::Update(const float deltaSeconds){for(auto iterator=m_fibers.begin();iterator!=m_fibers.end();){auto& fiber=*iterator;if(fiber.delayRemaining>0){fiber.delayRemaining=std::max(0.0f,fiber.delayRemaining-std::max(0.0f,deltaSeconds));if(fiber.delayRemaining>0){++iterator;continue;}Advance(fiber);}if(fiber.actionExecution){const auto state=m_services.actions?m_services.actions->State(fiber.actionExecution):ActionExecutionState::Failed;if(state==ActionExecutionState::Running){++iterator;continue;}if(m_services.actions)m_services.actions->Forget(fiber.actionExecution);fiber.actionExecution=0;if(state==ActionExecutionState::Failed||state==ActionExecutionState::Cancelled){Fail(Status::Fail(Error("PXUIBEH4041","Awaited Action did not complete",m_sourceScene,fiber.current)));iterator=m_fibers.erase(iterator);continue;}Advance(fiber);}if(fiber.animationHandle){if(m_services.animationPlaying&&m_services.animationPlaying(fiber.animationHandle)){++iterator;continue;}fiber.animationHandle=0;Advance(fiber);}const Status status=Run(fiber,{});if(!status){Fail(status);iterator=m_fibers.erase(iterator);continue;}if(fiber.current.Empty())iterator=m_fibers.erase(iterator);else ++iterator;}}
void BehaviorGraphRunner::CancelAll(){if(m_services.actions)for(const auto& fiber:m_fibers)if(fiber.actionExecution)(void)m_services.actions->Cancel(fiber.actionExecution);m_fibers.clear();}
Status BehaviorGraphRunner::RestoreState(std::vector<BehaviorFiberState> state){for(const auto& fiber:state)if(!m_graph.Find(fiber.entry)||(!fiber.current.Empty()&&!m_graph.Find(fiber.current)))return Status::Fail(Error("PXUIBEH4042","Behavior checkpoint references a missing node",m_sourceScene,fiber.current));m_fibers=std::move(state);for(const auto& fiber:m_fibers)m_nextFiber=std::max(m_nextFiber,fiber.id+1);return Status::Ok();}
void BehaviorGraphRunner::Fail(Status status){m_lastFailure=status;for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}

}  // namespace px::ui
