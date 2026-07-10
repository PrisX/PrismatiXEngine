#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace px::resource {

enum class DocumentKind { Project, Resource, Scene, Meta };

struct NodeRecord {
    Uuid id;
    Uuid parent;
    std::string name;
    std::string type;
    std::map<std::string, Variant> properties;
};

struct TypedDocument {
    DocumentKind kind = DocumentKind::Resource;
    int formatVersion = 1;
    Uuid id;
    std::string type;
    std::map<std::string, Variant> properties;
    std::vector<NodeRecord> nodes;
};

[[nodiscard]] Result<TypedDocument> ParseTypedDocument(std::string_view text,
                                                       const std::string& sourcePath = {});
[[nodiscard]] std::string WriteTypedDocument(const TypedDocument& document);

}  // namespace px::resource
