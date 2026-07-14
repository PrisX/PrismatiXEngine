#include "Engine/VN/GameCatalog.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Resources/TypedDocument.h"

#include <unordered_map>
#include <unordered_set>

namespace px::vn {
namespace {
diag::Diagnostic CatalogError(std::string code,std::string message,const std::string& path={},const resource::NodeRecord* node=nullptr){
    diag::Diagnostic d{.severity=diag::Severity::Error,.code=std::move(code),.category="VN.GameCatalog",.message=std::move(message)};d.source.path=path;if(node)d.source.nodeId=node->id.ToString();diag::Emit(d);return d;
}
std::string Text(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<std::string>())return *value;return {};}
int Integer(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<std::int64_t>())return static_cast<int>(*value);return 0;}
bool Boolean(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<bool>())return *value;return false;}
ResourceRefValue Resource(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<ResourceRefValue>())return *value;return {};}
}

Status GameCatalog::Load(std::string_view text,const std::string& sourcePath){
    auto parsed=resource::ParseTypedDocument(text,sourcePath);if(!parsed){for(const auto& d:parsed.Diagnostics())diag::Emit(d);return Status::Fail(parsed.Diagnostics());}
    if(parsed.Value().kind!=resource::DocumentKind::Resource||parsed.Value().type!="GameCatalog")return Status::Fail(CatalogError("PXCAT5001","Resource is not a GameCatalog",sourcePath));
    m_variables.clear();m_characters.clear();m_gallery.clear();m_input.clear();
    std::unordered_map<Uuid,std::size_t,UuidHash> charactersByNode;
    std::unordered_set<std::string> characterIds;
    for(const auto& node:parsed.Value().nodes){
        if(node.type=="Variable"){CatalogVariable value{Text(node,"name"),Integer(node,"default"),Boolean(node,"persistent")};if(value.name.empty())return Status::Fail(CatalogError("PXCAT5002","Variable requires a name",sourcePath,&node));m_variables.push_back(std::move(value));}
        else if(node.type=="Character"){
            CatalogCharacter value;value.id=Text(node,"id");value.name=Text(node,"name");
            value.voiceDirectory=Text(node,"voiceDirectory");value.defaultExpression=Text(node,"defaultExpression");
            if(value.id.empty()&&value.name.empty())return Status::Fail(CatalogError("PXCAT5003","Character requires an id or name",sourcePath,&node));
            const std::string key=value.id.empty()?value.name:value.id;
            if(!characterIds.insert(key).second)return Status::Fail(CatalogError("PXCAT5007","Duplicate Character id: "+key,sourcePath,&node));
            charactersByNode[node.id]=m_characters.size();m_characters.push_back(std::move(value));
        }
        else if(node.type=="CharacterExpression")continue;
        else if(node.type=="GalleryItem"){CatalogGalleryItem value{Text(node,"id"),Text(node,"title"),Text(node,"image"),Text(node,"thumbnail")};if(value.id.empty()||value.image.empty())return Status::Fail(CatalogError("PXCAT5004","Gallery item requires id and image",sourcePath,&node));m_gallery.push_back(std::move(value));}
        else if(node.type=="InputBinding"){CatalogInputBinding value{Text(node,"key"),Text(node,"command"),Text(node,"argument")};if(value.key.empty()||value.command.empty())return Status::Fail(CatalogError("PXCAT5005","Input binding requires key and command",sourcePath,&node));m_input.push_back(std::move(value));}
        else return Status::Fail(CatalogError("PXCAT5006","Unknown GameCatalog entry type: "+node.type,sourcePath,&node));
    }
    for(const auto& node:parsed.Value().nodes){
        if(node.type!="CharacterExpression")continue;
        const auto owner=charactersByNode.find(node.parent);
        if(owner==charactersByNode.end())return Status::Fail(CatalogError("PXCAT5008","CharacterExpression requires a Character parent",sourcePath,&node));
        CatalogCharacterExpression expression;expression.id=Text(node,"id");expression.name=Text(node,"name");expression.image=Resource(node,"image");
        if(expression.id.empty())expression.id=expression.name;
        if(expression.name.empty())expression.name=expression.id;
        if(expression.id.empty()||expression.image.id.Empty()||expression.image.lastKnownPath.empty())return Status::Fail(CatalogError("PXCAT5009","CharacterExpression requires id and image ResourceRef",sourcePath,&node));
        auto& expressions=m_characters[owner->second].expressions;
        for(const auto& existing:expressions)if(existing.id==expression.id)return Status::Fail(CatalogError("PXCAT5010","Duplicate CharacterExpression id: "+expression.id,sourcePath,&node));
        expressions.push_back(std::move(expression));
    }
    for(const auto& character:m_characters){
        if(character.expressions.empty())continue;
        if(character.defaultExpression.empty())return Status::Fail(CatalogError("PXCAT5011","Character with expressions requires defaultExpression: "+character.id,sourcePath));
        bool found=false;for(const auto& expression:character.expressions)if(expression.id==character.defaultExpression){found=true;break;}
        if(!found)return Status::Fail(CatalogError("PXCAT5012","Character defaultExpression does not exist: "+character.defaultExpression,sourcePath));
    }
    return Status::Ok();
}

const CatalogCharacter* GameCatalog::FindCharacter(const std::string_view idOrName) const{
    for(const auto& character:m_characters)if(character.id==idOrName||character.name==idOrName)return &character;
    return nullptr;
}

const CatalogCharacterExpression* GameCatalog::FindExpression(
    const CatalogCharacter& character,const std::string_view idOrName) const{
    for(const auto& expression:character.expressions)if(expression.id==idOrName||expression.name==idOrName)return &expression;
    return nullptr;
}

std::optional<ResourceRefValue> GameCatalog::ResolveCharacterImage(
    const std::string_view characterId,const std::string_view expressionId) const{
    const auto* character=FindCharacter(characterId);if(!character)return std::nullopt;
    const std::string_view selected=expressionId.empty()?std::string_view(character->defaultExpression):expressionId;
    const auto* expression=FindExpression(*character,selected);if(!expression)return std::nullopt;
    return expression->image;
}

std::string GameCatalog::CharacterDisplayName(const std::string_view characterId) const{
    const auto* character=FindCharacter(characterId);if(!character)return std::string(characterId);
    return character->name.empty()?character->id:character->name;
}

}  // namespace px::vn
