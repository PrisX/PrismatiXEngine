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
ResourceRefValue Resource(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<ResourceRefValue>())return *value;return {};}
}

Status GameCatalog::Load(std::string_view text,const std::string& sourcePath){
    const Status runtimeResources = LoadRuntimeResources(
        text, sourcePath,
        sdk::LegacyGameCatalogPolicy::AllowCharacterNodes,
        sdk::LegacyGalleryReferencePolicy::AllowPathStrings);
    if (!runtimeResources) return runtimeResources;
    auto parsed=resource::ParseTypedDocument(text,sourcePath);if(!parsed){for(const auto& d:parsed.Diagnostics())diag::Emit(d);return Status::Fail(parsed.Diagnostics());}
    if(parsed.Value().kind!=resource::DocumentKind::Resource||parsed.Value().type!="GameCatalog")return Status::Fail(CatalogError("PXCAT5001","Resource is not a GameCatalog",sourcePath));
    m_characters.clear();
    std::unordered_map<Uuid,std::size_t,UuidHash> charactersByNode;
    std::unordered_set<std::string> characterIds;
    for(const auto& node:parsed.Value().nodes){
        if(node.type=="Character"){
            CatalogCharacter value;value.id=Text(node,"id");value.name=Text(node,"name");
            value.voiceDirectory=Text(node,"voiceDirectory");value.defaultExpression=Text(node,"defaultExpression");
            if(value.id.empty()&&value.name.empty())return Status::Fail(CatalogError("PXCAT5003","Character requires an id or name",sourcePath,&node));
            const std::string key=value.id.empty()?value.name:value.id;
            if(!characterIds.insert(key).second)return Status::Fail(CatalogError("PXCAT5007","Duplicate Character id: "+key,sourcePath,&node));
            charactersByNode[node.id]=m_characters.size();m_characters.push_back(std::move(value));
        }
        else if(node.type=="CharacterExpression")continue;
        else if(node.type=="Variable"||node.type=="GalleryItem"||node.type=="InputBinding")continue;
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

Status GameCatalog::LoadRuntimeResources(
    const std::string_view text, const std::string& sourcePath,
    const sdk::LegacyGameCatalogPolicy legacyPolicy,
    const sdk::LegacyGalleryReferencePolicy galleryPolicy,
    const std::string_view projectManifest,
    const sdk::GameCatalogResourceExists& exists) {
    auto loaded = sdk::LoadGameCatalogResources(text, sourcePath, legacyPolicy,
                                                galleryPolicy);
    if (loaded.Valid() && !loaded.document.gallery.empty() &&
        galleryPolicy == sdk::LegacyGalleryReferencePolicy::RejectPathStrings) {
        loaded = sdk::ResolveGameCatalogGalleryResources(
            std::move(loaded.document), projectManifest, exists, sourcePath);
    }
    if (!loaded.Valid()) {
        std::vector<diag::Diagnostic> diagnostics;
        diagnostics.reserve(loaded.diagnostics.size());
        for (const auto& source : loaded.diagnostics) {
            diag::Diagnostic diagnostic{
                .severity = diag::Severity::Error,
                .code = source.code,
                .category = "VN.GameCatalogResources",
                .message = source.message};
            diagnostic.source.path = source.path;
            diagnostic.source.nodeId = source.nodeId;
            diagnostic.source.property = source.property;
            diagnostic.source.line = source.line;
            diag::Emit(diagnostic);
            diagnostics.push_back(std::move(diagnostic));
        }
        return Status::Fail(std::move(diagnostics));
    }

    std::vector<CatalogVariable> variables;
    variables.reserve(loaded.document.variables.size());
    for (const auto& source : loaded.document.variables) {
        variables.push_back(
            {source.name, source.defaultValue, source.persistent});
    }
    std::vector<CatalogGalleryItem> gallery;
    gallery.reserve(loaded.document.gallery.size());
    for (const auto& source : loaded.document.gallery) {
        const auto imageId = Uuid::Parse(source.image.assetId);
        if (!source.image.assetId.empty() && !imageId) {
            return Status::Fail(CatalogError(
                "PXCAT1099",
                "validated GalleryItem image UUID could not be converted",
                sourcePath));
        }
        CatalogGalleryItem item{
            .id = source.id,
            .title = source.title,
            .image = source.image.assetPath,
            .imageReference = ResourceRefValue{
                imageId.value_or(Uuid{}), source.image.assetPath}};
        if (source.thumbnail) {
            const auto thumbnailId = Uuid::Parse(source.thumbnail->assetId);
            if (!source.thumbnail->assetId.empty() && !thumbnailId) {
                return Status::Fail(CatalogError(
                    "PXCAT1099",
                    "validated GalleryItem thumbnail UUID could not be converted",
                    sourcePath));
            }
            item.thumbnail = source.thumbnail->assetPath;
            item.thumbnailReference = ResourceRefValue{
                thumbnailId.value_or(Uuid{}),
                source.thumbnail->assetPath};
        }
        gallery.push_back(std::move(item));
    }
    std::vector<CatalogInputBinding> input;
    input.reserve(loaded.document.inputBindings.size());
    for (const auto& source : loaded.document.inputBindings) {
        input.push_back({source.key, source.command, source.argument});
    }
    m_variables = std::move(variables);
    m_gallery = std::move(gallery);
    m_input = std::move(input);
    return Status::Ok();
}

Status GameCatalog::LoadCharacterResources(
    const std::string_view projectManifest,
    const sdk::CharacterResourceReadText& readText,
    const sdk::CharacterResourceExists& exists,
    bool& declared) {
    const auto loaded =
        sdk::LoadCharacterResources(projectManifest, readText, exists);
    declared = loaded.declared;
    if (!loaded.diagnostics.empty()) {
        std::vector<diag::Diagnostic> diagnostics;
        diagnostics.reserve(loaded.diagnostics.size());
        for (const auto& source : loaded.diagnostics) {
            diag::Diagnostic diagnostic{
                .severity = diag::Severity::Error,
                .code = source.code,
                .category = "VN.CharacterResources",
                .message = source.message};
            diagnostic.source.path = source.path;
            diagnostic.source.nodeId = source.characterId;
            diagnostic.source.property = source.expressionId;
            diag::Emit(diagnostic);
            diagnostics.push_back(std::move(diagnostic));
        }
        return Status::Fail(std::move(diagnostics));
    }
    if (!declared) return Status::Ok();

    std::vector<CatalogCharacter> characters;
    characters.reserve(loaded.document.characters.size());
    for (const auto& source : loaded.document.characters) {
        CatalogCharacter character;
        character.id = source.id;
        character.name = source.displayName;
        character.aliases = source.aliases;
        character.voiceDirectory = source.voiceDirectory;
        character.defaultExpression =
            source.defaultExpressionId.value_or(std::string{});
        character.expressions.reserve(source.expressions.size());
        for (const auto& expression : source.expressions) {
            const auto assetId = Uuid::Parse(expression.assetId);
            if (!assetId) {
                return Status::Fail(CatalogError(
                    "PXCHAR1099",
                    "validated characterResources asset UUID could not be converted",
                    expression.assetPath));
            }
            character.expressions.push_back(
                {.id = expression.id,
                 .name = expression.name,
                 .aliases = expression.aliases,
                 .image = ResourceRefValue{*assetId, expression.assetPath}});
        }
        characters.push_back(std::move(character));
    }
    m_characters = std::move(characters);
    return Status::Ok();
}

const CatalogCharacter* GameCatalog::FindCharacter(const std::string_view idOrName) const{
    for(const auto& character:m_characters) {
        if(character.id==idOrName||character.name==idOrName)return &character;
        for(const auto& alias:character.aliases)if(alias==idOrName)return &character;
    }
    return nullptr;
}

const CatalogCharacterExpression* GameCatalog::FindExpression(
    const CatalogCharacter& character,const std::string_view idOrName) const{
    for(const auto& expression:character.expressions) {
        if(expression.id==idOrName||expression.name==idOrName)return &expression;
        for(const auto& alias:expression.aliases)if(alias==idOrName)return &expression;
    }
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
