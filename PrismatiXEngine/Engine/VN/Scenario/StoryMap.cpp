#include "Engine/VN/Scenario/StoryMap.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <algorithm>

namespace px::vn::scenario {
namespace {

Status StoryError(std::string code, std::string message, const Uuid& node = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),
                                .category="VN.StoryMap",.message=std::move(message)};
    if(!node.Empty())diagnostic.source.nodeId=node.ToString();return Status::Fail(std::move(diagnostic));
}

std::string Key(std::string_view port,const char* suffix){return std::string(port)+suffix;}

}  // namespace

std::optional<StoryTarget> GetStoryTarget(const ScenarioNode& node,const std::string_view port){
    const auto scenario=node.parameters.find(Key(port,"Scenario"));
    const auto entry=node.parameters.find(Key(port,"Entry"));
    if(scenario==node.parameters.end()||entry==node.parameters.end())return std::nullopt;
    resource::ResourceId scenarioId;std::string path;
    if(const auto* reference=scenario->second.TryGet<ResourceRefValue>()){scenarioId=reference->id;path=reference->lastKnownPath;}
    else if(const auto* id=scenario->second.TryGet<Uuid>())scenarioId=*id;
    const auto* entryId=entry->second.TryGet<Uuid>();
    if(scenarioId.Empty()||!entryId||entryId->Empty())return std::nullopt;
    return StoryTarget{scenarioId,*entryId,std::move(path)};
}

std::vector<StoryLink> DeriveStoryLinks(const ScenarioDocument& document){
    std::vector<StoryLink> links;
    for(const auto& node:document.nodes){
        if(node.command!="choice"&&node.command!="jump"&&node.command!="call")continue;
        std::vector<std::string> ports{"target"};
        if(const auto* dynamic=node.parameters.contains("ports")?node.parameters.at("ports").AsArray():nullptr)
            for(const auto& value:*dynamic)if(const auto* name=value.TryGet<std::string>())ports.push_back(*name);
        for(const auto& port:ports)if(const auto target=GetStoryTarget(node,port))
            links.push_back({document.id,node.id,port,*target});
    }
    return links;
}

Status ConnectStoryTarget(ScenarioDocument& source,const StatementId& statement,std::string port,
                          const StoryTarget& target){
    if(port.empty()||target.scenario.Empty()||target.entry.Empty())return StoryError("PXSTORY7701","Story target is incomplete",statement);
    const auto node=std::find_if(source.nodes.begin(),source.nodes.end(),[&](const auto& value){return value.id==statement;});
    if(node==source.nodes.end())return StoryError("PXSTORY7702","Source statement does not exist",statement);
    if(node->command!="choice"&&node->command!="jump"&&node->command!="call")return StoryError("PXSTORY7703","Only Choice, Jump, and Call nodes expose Story Map targets",statement);
    node->parameters[Key(port,"Scenario")]=ResourceRefValue{target.scenario,target.lastKnownPath};
    node->parameters[Key(port,"Entry")]=target.entry;
    return Status::Ok();
}

Status DisconnectStoryTarget(ScenarioDocument& source,const StatementId& statement,
                             const std::string_view port){
    const auto node=std::find_if(source.nodes.begin(),source.nodes.end(),[&](const auto& value){return value.id==statement;});
    if(node==source.nodes.end())return StoryError("PXSTORY7702","Source statement does not exist",statement);
    const auto removed=node->parameters.erase(Key(port,"Scenario"))+node->parameters.erase(Key(port,"Entry"));
    if(removed==0)return StoryError("PXSTORY7704","Story target was already disconnected",statement);
    return Status::Ok();
}

}  // namespace px::vn::scenario
