#include "Engine/VN/Scenario/ScenarioDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace px::vn::scenario {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxNodes = 1'000'000;
constexpr std::size_t kMaxEdges = 4'000'000;

diag::Diagnostic ScenarioDiagnostic(diag::Severity severity,std::string code,std::string message,
                                    const std::string& path={},const Uuid& node={},std::string property={}){
    diag::Diagnostic value{.severity=severity,.code=std::move(code),.category="VN.Scenario",.message=std::move(message)};
    value.source.path=path;if(!node.Empty())value.source.nodeId=node.ToString();value.source.property=std::move(property);return value;
}

Json VariantToJson(const Variant& value) {
    switch(value.Type()){
        case VariantType::Null:return nullptr;
        case VariantType::Bool:return *value.TryGet<bool>();
        case VariantType::Integer:return *value.TryGet<std::int64_t>();
        case VariantType::Number:return *value.TryGet<double>();
        case VariantType::String:return *value.TryGet<std::string>();
        case VariantType::Vec2:{const auto& v=*value.TryGet<Vec2>();return Json{{"$type","vec2"},{"x",v.x},{"y",v.y}};}
        case VariantType::Rect:{const auto& v=*value.TryGet<Rect>();return Json{{"$type","rect"},{"x",v.x},{"y",v.y},{"w",v.w},{"h",v.h}};}
        case VariantType::Color:{const auto& v=*value.TryGet<Color>();return Json{{"$type","color"},{"r",v.r},{"g",v.g},{"b",v.b},{"a",v.a}};}
        case VariantType::Uuid:return Json{{"$type","uuid"},{"value",value.TryGet<Uuid>()->ToString()}};
        case VariantType::ResourceRef:{const auto& v=*value.TryGet<ResourceRefValue>();return Json{{"$type","resource"},{"id",v.id.ToString()},{"path",v.lastKnownPath}};}
        case VariantType::Array:{Json result=Json::array();for(const auto& item:*value.AsArray())result.push_back(VariantToJson(item));return result;}
        case VariantType::Object:{Json result=Json::object();for(const auto& [name,item]:*value.AsObject())result[name]=VariantToJson(item);return result;}
    }
    return nullptr;
}

std::optional<Variant> VariantFromJson(const Json& json,int depth=0){
    if(depth>64)return std::nullopt;
    try{
        if(json.is_null())return Variant{};
        if(json.is_boolean())return Variant(json.get<bool>());
        if(json.is_number_integer())return Variant(json.get<std::int64_t>());
        if(json.is_number_unsigned()){const auto value=json.get<std::uint64_t>();if(value>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))return std::nullopt;return Variant(static_cast<std::int64_t>(value));}
        if(json.is_number_float())return Variant(json.get<double>());
        if(json.is_string())return Variant(json.get<std::string>());
        if(json.is_array()){VariantArray values;values.reserve(json.size());for(const auto& item:json){auto converted=VariantFromJson(item,depth+1);if(!converted)return std::nullopt;values.push_back(std::move(*converted));}return Variant(std::move(values));}
        if(!json.is_object())return std::nullopt;
        if(const auto type=json.find("$type");type!=json.end()&&type->is_string()){
            const std::string kind=type->get<std::string>();
            if(kind=="vec2")return Variant(Vec2{json.at("x").get<float>(),json.at("y").get<float>()});
            if(kind=="rect")return Variant(Rect{json.at("x").get<float>(),json.at("y").get<float>(),json.at("w").get<float>(),json.at("h").get<float>()});
            if(kind=="color")return Variant(Color{json.at("r").get<std::uint8_t>(),json.at("g").get<std::uint8_t>(),json.at("b").get<std::uint8_t>(),json.at("a").get<std::uint8_t>()});
            if(kind=="uuid"){auto id=Uuid::Parse(json.at("value").get<std::string>());if(!id)return std::nullopt;return Variant(*id);}
            if(kind=="resource"){auto id=Uuid::Parse(json.at("id").get<std::string>());if(!id)return std::nullopt;return Variant(ResourceRefValue{*id,json.value("path",std::string{})});}
            return std::nullopt;
        }
        VariantObject values;for(auto it=json.begin();it!=json.end();++it){auto converted=VariantFromJson(it.value(),depth+1);if(!converted)return std::nullopt;values.emplace(it.key(),std::move(*converted));}return Variant(std::move(values));
    }catch(const Json::exception&){return std::nullopt;}
}

Json NodeToJson(const ScenarioNode& node){Json parameters=Json::object();for(const auto& [name,value]:node.parameters)parameters[name]=VariantToJson(value);return Json{{"id",node.id.ToString()},{"command",node.command},{"parameters",std::move(parameters)}};}
Json EdgeToJson(const ScenarioEdge& edge){return Json{{"id",edge.id.ToString()},{"from",{{"node",edge.fromNode.ToString()},{"port",edge.fromPort}}},{"to",{{"node",edge.toNode.ToString()},{"port",edge.toPort}}}};}

std::optional<Uuid> JsonUuid(const Json& json,const char* key){if(!json.contains(key)||!json[key].is_string())return std::nullopt;return Uuid::Parse(json[key].get<std::string>());}

std::string ParameterText(const Variant& value){
    if(const auto* text=value.TryGet<std::string>())return *text;
    if(const auto* integer=value.TryGet<std::int64_t>())return std::to_string(*integer);
    if(const auto* number=value.TryGet<double>())return std::to_string(*number);
    if(const auto* boolean=value.TryGet<bool>())return *boolean?"true":"false";
    if(const auto* resource=value.TryGet<ResourceRefValue>())return resource->lastKnownPath.empty()?resource->id.ToString():resource->lastKnownPath;
    if(const auto* id=value.TryGet<Uuid>())return id->ToString();return {};
}

}  // namespace

bool ValidationReport::Valid()const{return std::none_of(diagnostics.begin(),diagnostics.end(),[](const diag::Diagnostic& diagnostic){return diagnostic.BlocksBuild();});}

Result<ScenarioDocument> ParseScenario(std::string_view text,const std::string& sourcePath){
    Json json=Json::parse(text,nullptr,false);if(json.is_discarded()||!json.is_object())return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7101","Scenario JSON is corrupt",sourcePath));
    try{
        if(json.value("format",std::string{})!="PrismatiXScenario")return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7102","File is not a PrismatiX Scenario",sourcePath));
        ScenarioDocument document;document.version=json.at("version").get<int>();
        auto id=JsonUuid(json,"id"),entry=JsonUuid(json,"entry");if(!id||!entry)return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7103","Scenario id or entry is invalid",sourcePath));
        document.id=*id;document.entry=*entry;document.name=json.value("name",std::string{});
        const auto& nodes=json.at("nodes");const auto& edges=json.at("edges");if(!nodes.is_array()||!edges.is_array()||nodes.size()>kMaxNodes||edges.size()>kMaxEdges)return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7104","Scenario node/edge collection is invalid or excessive",sourcePath));
        document.nodes.reserve(nodes.size());for(const auto& item:nodes){auto nodeId=JsonUuid(item,"id");if(!nodeId||!item.contains("command")||!item["command"].is_string()||!item.contains("parameters")||!item["parameters"].is_object())return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7105","Scenario node is malformed",sourcePath));ScenarioNode node;node.id=*nodeId;node.command=item["command"].get<std::string>();for(auto parameter=item["parameters"].begin();parameter!=item["parameters"].end();++parameter){auto value=VariantFromJson(parameter.value());if(!value)return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7106","Scenario parameter is malformed",sourcePath,node.id,parameter.key()));node.parameters.emplace(parameter.key(),std::move(*value));}document.nodes.push_back(std::move(node));}
        document.edges.reserve(edges.size());for(const auto& item:edges){auto edgeId=JsonUuid(item,"id");if(!edgeId||!item.contains("from")||!item.contains("to"))return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7107","Scenario edge is malformed",sourcePath));auto from=JsonUuid(item["from"],"node"),to=JsonUuid(item["to"],"node");if(!from||!to)return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7107","Scenario edge endpoint is malformed",sourcePath));document.edges.push_back({*edgeId,*from,item["from"].value("port",std::string("flow")),*to,item["to"].value("port",std::string("in"))});}
        return Result<ScenarioDocument>::Success(std::move(document));
    }catch(const Json::exception& error){return Result<ScenarioDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7108","Scenario fields have invalid types",sourcePath,{},error.what()));}
}

std::string WriteScenario(const ScenarioDocument& document){Json json{{"format","PrismatiXScenario"},{"version",document.version},{"id",document.id.ToString()},{"name",document.name},{"entry",document.entry.ToString()}};json["nodes"]=Json::array();for(const auto& node:document.nodes)json["nodes"].push_back(NodeToJson(node));json["edges"]=Json::array();for(const auto& edge:document.edges)json["edges"].push_back(EdgeToJson(edge));return json.dump(2)+"\n";}

Result<ScenarioLayoutDocument> ParseScenarioLayout(std::string_view text,const std::string& sourcePath){Json json=Json::parse(text,nullptr,false);if(json.is_discarded()||!json.is_object())return Result<ScenarioLayoutDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7110","Scenario layout JSON is corrupt",sourcePath));try{ScenarioLayoutDocument layout;layout.version=json.at("version").get<int>();if(json.value("format",std::string{})!="PrismatiXScenarioLayout"||layout.version!=ScenarioLayoutDocument::CurrentVersion)return Result<ScenarioLayoutDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7115","Scenario layout must be strict version 4",sourcePath));auto scenario=JsonUuid(json,"scenario");if(!scenario)return Result<ScenarioLayoutDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7111","Scenario layout identity is invalid",sourcePath));layout.scenario=*scenario;const auto& nodes=json.at("nodes");if(!nodes.is_array()||nodes.size()>kMaxNodes)return Result<ScenarioLayoutDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7112","Scenario layout nodes are invalid",sourcePath));for(const auto& item:nodes){auto id=JsonUuid(item,"node");if(!id)return Result<ScenarioLayoutDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7113","Scenario layout node id is invalid",sourcePath));layout.nodes.push_back({*id,{item.value("x",0.0f),item.value("y",0.0f)},{item.value("w",0.0f),item.value("h",0.0f)},item.value("group",std::string{})});}return Result<ScenarioLayoutDocument>::Success(std::move(layout));}catch(const Json::exception& error){return Result<ScenarioLayoutDocument>::Failure(ScenarioDiagnostic(diag::Severity::Error,"PXSCENARIO7114","Scenario layout fields have invalid types",sourcePath,{},error.what()));}}

std::string WriteScenarioLayout(const ScenarioLayoutDocument& document){Json json{{"format","PrismatiXScenarioLayout"},{"version",document.version},{"scenario",document.scenario.ToString()}};json["nodes"]=Json::array();for(const auto& node:document.nodes)json["nodes"].push_back({{"node",node.node.ToString()},{"x",node.position.x},{"y",node.position.y},{"w",node.size.x},{"h",node.size.y},{"group",node.group}});return json.dump(2)+"\n";}

ValidationReport ValidateScenario(const ScenarioDocument& document, const CommandRegistry& registry,
                                  const std::string& sourcePath) {
    ValidationReport report;
    if (document.version != ScenarioDocument::CurrentVersion)
        report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7120", "Unsupported Scenario version", sourcePath));
    if (document.id.Empty())
        report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7121", "Scenario identity is empty", sourcePath));
    std::unordered_map<Uuid, const ScenarioNode*, UuidHash> nodes;
    for (const auto& node : document.nodes) {
        if (node.id.Empty() || !nodes.emplace(node.id, &node).second) {
            report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7122", "Duplicate or empty statement id", sourcePath, node.id));
            continue;
        }
        const Status command = registry.ValidateParameters(node.command, node.parameters, sourcePath, node.id);
        report.diagnostics.insert(report.diagnostics.end(), command.Diagnostics().begin(), command.Diagnostics().end());
    }
    if (!nodes.contains(document.entry))
        report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7123", "Scenario entry does not reference a node", sourcePath));

    std::unordered_map<Uuid, std::vector<Uuid>, UuidHash> adjacency;
    std::unordered_map<Uuid, std::unordered_set<std::string>, UuidHash> ports;
    std::unordered_set<Uuid, UuidHash> edgeIds;
    for (const auto& edge : document.edges) {
        if (edge.id.Empty() || !edgeIds.insert(edge.id).second)
            report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7124", "Duplicate or empty edge id", sourcePath));
        if (!nodes.contains(edge.fromNode) || !nodes.contains(edge.toNode))
            report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7125", "Scenario edge references a missing node", sourcePath, edge.fromNode));
        else {
            adjacency[edge.fromNode].push_back(edge.toNode);
            if (!ports[edge.fromNode].insert(edge.fromPort).second)
                report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7127", "A scenario output port can only have one target", sourcePath, edge.fromNode, edge.fromPort));
        }
    }
    for (const auto& node : document.nodes) {
        if (node.command == "choice" && !ports[node.id].contains("choice"))
            report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7128", "Choice output is not connected", sourcePath, node.id, "choice"));
        if (node.command == "branch") {
            for (const char* required : {"true", "false"})
                if (!ports[node.id].contains(required))
                    report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Error, "PXSCENARIO7129", std::string("If ") + required + " output is not connected", sourcePath, node.id, required));
        }
    }
    if (nodes.contains(document.entry)) {
        std::unordered_set<Uuid, UuidHash> reached;
        std::queue<Uuid> queue;
        queue.push(document.entry); reached.insert(document.entry);
        while (!queue.empty()) {
            const Uuid current = queue.front(); queue.pop();
            for (const auto& next : adjacency[current]) if (reached.insert(next).second) queue.push(next);
        }
        for (const auto& [id, _] : nodes)
            if (!reached.contains(id)) report.diagnostics.push_back(ScenarioDiagnostic(diag::Severity::Warning, "PXSCENARIO7126", "Unreachable scenario node", sourcePath, id));
    }
    return report;
}

Program CompileScenario(const ScenarioDocument& document, const CommandRegistry& registry) {
    std::vector<Command> commands;
    const auto labelName = [](const StatementId& id) { return "@" + id.ToString(); };
    std::unordered_map<StatementId, std::vector<const ScenarioEdge*>, UuidHash> outgoing;
    for (const auto& edge : document.edges) outgoing[edge.fromNode].push_back(&edge);
    std::unordered_map<StatementId, const ScenarioNode*, UuidHash> byId;
    for (const auto& node : document.nodes) byId.emplace(node.id, &node);

    // Execution order is derived from explicit flow edges, never from canvas
    // position or insertion order. Follow the primary flow first so a chain of
    // Choice nodes compiles into one option block; visit branch targets after
    // that chain. This preserves the original PDS graph authoring gesture while
    // making the displayed graph and runtime control flow identical.
    std::vector<const ScenarioNode*> ordered;
    std::unordered_set<StatementId, UuidHash> visited;
    std::function<void(StatementId)> visit = [&](const StatementId id) {
        const auto found = byId.find(id);
        if (found == byId.end() || !visited.insert(id).second) return;
        ordered.push_back(found->second);
        const auto links = outgoing.find(id);
        if (links == outgoing.end()) return;
        for (const ScenarioEdge* edge : links->second)
            if (edge->fromPort == "flow") visit(edge->toNode);
        for (const ScenarioEdge* edge : links->second)
            if (edge->fromPort != "flow") visit(edge->toNode);
    };
    visit(document.entry);
    for (const auto& node : document.nodes) visit(node.id); // retain unreachable diagnostics/debug data

    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& node = *ordered[index];
        Command label;
        label.type = "label";
        label.line = static_cast<int>(index + 1);
        label.args.push_back({"name", labelName(node.id)});
        commands.push_back(std::move(label));

        Command command;
        command.type = node.command;
        command.line = static_cast<int>(index + 1);
        for (const auto& [name, value] : node.parameters) {
            command.typedArgs.emplace(name, value.Clone());
            const std::string text = ParameterText(value);
            if (!text.empty() || value.Type() == VariantType::String) {
                command.args.push_back({name, text});
            }
        }
        if (!command.Has("target")) {
            const auto scenarioTarget = node.parameters.find("targetScenario");
            const auto entryTarget = node.parameters.find("targetEntry");
            if (scenarioTarget != node.parameters.end() && entryTarget != node.parameters.end()) {
                const auto* reference = scenarioTarget->second.TryGet<ResourceRefValue>();
                const auto* entryId = entryTarget->second.TryGet<Uuid>();
                if (reference && entryId && !reference->lastKnownPath.empty()) {
                    command.args.push_back(
                        {"target", reference->lastKnownPath + "#" + labelName(*entryId)});
                }
            }
        }
        const auto edges = outgoing.find(node.id);
        const ScenarioEdge* flow = nullptr;
        const ScenarioEdge* branch = nullptr;
        if (edges != outgoing.end()) {
            for (const ScenarioEdge* edge : edges->second) {
                if (edge->fromPort == "flow") {
                    if (!flow) flow = edge;
                } else if (!branch) {
                    branch = edge;
                }
            }
        }
        if (command.type == "choice" && branch && !command.Has("target")) {
            command.args.push_back({"target", labelName(branch->toNode)});
        }
        if (command.type == "branch" && edges != outgoing.end()) {
            for (const ScenarioEdge* edge : edges->second) {
                if (edge->fromPort == "true")
                    command.args.push_back({"trueTarget", labelName(edge->toNode)});
                else if (edge->fromPort == "false")
                    command.args.push_back({"falseTarget", labelName(edge->toNode)});
            }
        }
        commands.push_back(std::move(command));

        const bool naturalFlow = flow && index + 1 < ordered.size() &&
                                 flow->toNode == ordered[index + 1]->id;
        const bool commandOwnsFlow = node.command == "jump" || node.command == "return" ||
                                    node.command == "choice" || node.command == "branch";
        if (flow && !naturalFlow && !commandOwnsFlow) {
            Command jump;
            jump.type = "jump";
            jump.line = static_cast<int>(index + 1);
            jump.args.push_back({"target", labelName(flow->toNode)});
            commands.push_back(std::move(jump));
        }
    }
    Program program = CompileProgram(std::move(commands));
    const ValidationReport validation = ValidateScenario(document, registry);
    for (const auto& diagnostic : validation.diagnostics) {
        if (diagnostic.BlocksBuild()) program.errors.push_back(diag::Describe(diagnostic));
    }
    return program;
}

}  // namespace px::vn::scenario
