#include "Engine/VN/GameCatalog.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Resources/TypedDocument.h"

namespace px::vn {
namespace {
diag::Diagnostic CatalogError(std::string code,std::string message,const std::string& path={},const resource::NodeRecord* node=nullptr){
    diag::Diagnostic d{.severity=diag::Severity::Error,.code=std::move(code),.category="VN.GameCatalog",.message=std::move(message)};d.source.path=path;if(node)d.source.nodeId=node->id.ToString();diag::Emit(d);return d;
}
std::string Text(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<std::string>())return *value;return {};}
int Integer(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<std::int64_t>())return static_cast<int>(*value);return 0;}
bool Boolean(const resource::NodeRecord& node,const char* key){const auto it=node.properties.find(key);if(it!=node.properties.end())if(const auto* value=it->second.TryGet<bool>())return *value;return false;}
}

Status GameCatalog::Load(std::string_view text,const std::string& sourcePath){
    auto parsed=resource::ParseTypedDocument(text,sourcePath);if(!parsed){for(const auto& d:parsed.Diagnostics())diag::Emit(d);return Status::Fail(parsed.Diagnostics());}
    if(parsed.Value().kind!=resource::DocumentKind::Resource||parsed.Value().type!="GameCatalog")return Status::Fail(CatalogError("PXCAT5001","Resource is not a GameCatalog",sourcePath));
    m_variables.clear();m_characters.clear();m_gallery.clear();m_input.clear();
    for(const auto& node:parsed.Value().nodes){
        if(node.type=="Variable"){CatalogVariable value{Text(node,"name"),Integer(node,"default"),Boolean(node,"persistent")};if(value.name.empty())return Status::Fail(CatalogError("PXCAT5002","Variable requires a name",sourcePath,&node));m_variables.push_back(std::move(value));}
        else if(node.type=="Character"){CatalogCharacter value{Text(node,"id"),Text(node,"name"),Text(node,"voiceDirectory")};if(value.id.empty()&&value.name.empty())return Status::Fail(CatalogError("PXCAT5003","Character requires an id or name",sourcePath,&node));m_characters.push_back(std::move(value));}
        else if(node.type=="GalleryItem"){CatalogGalleryItem value{Text(node,"id"),Text(node,"title"),Text(node,"image"),Text(node,"thumbnail")};if(value.id.empty()||value.image.empty())return Status::Fail(CatalogError("PXCAT5004","Gallery item requires id and image",sourcePath,&node));m_gallery.push_back(std::move(value));}
        else if(node.type=="InputBinding"){CatalogInputBinding value{Text(node,"key"),Text(node,"command"),Text(node,"argument")};if(value.key.empty()||value.command.empty())return Status::Fail(CatalogError("PXCAT5005","Input binding requires key and command",sourcePath,&node));m_input.push_back(std::move(value));}
        else return Status::Fail(CatalogError("PXCAT5006","Unknown GameCatalog entry type: "+node.type,sourcePath,&node));
    }
    return Status::Ok();
}

}  // namespace px::vn
