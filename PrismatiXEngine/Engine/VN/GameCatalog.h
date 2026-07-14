#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/ResourceRef.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::vn {

struct CatalogVariable { std::string name; int defaultValue=0; bool persistent=false; };
struct CatalogCharacterExpression {
    std::string id;
    std::string name;
    ResourceRefValue image;
};
struct CatalogCharacter {
    std::string id;
    std::string name;
    std::string voiceDirectory;
    std::string defaultExpression;
    std::vector<CatalogCharacterExpression> expressions;
};
struct CatalogGalleryItem { std::string id; std::string title; std::string image; std::string thumbnail; };
struct CatalogInputBinding { std::string key; std::string command; std::string argument; };

class GameCatalog {
public:
    Status Load(std::string_view typedResource, const std::string& sourcePath = {});
    [[nodiscard]] const std::vector<CatalogVariable>& Variables() const { return m_variables; }
    [[nodiscard]] const std::vector<CatalogCharacter>& Characters() const { return m_characters; }
    [[nodiscard]] const std::vector<CatalogGalleryItem>& Gallery() const { return m_gallery; }
    [[nodiscard]] const std::vector<CatalogInputBinding>& InputBindings() const { return m_input; }
    [[nodiscard]] const CatalogCharacter* FindCharacter(std::string_view idOrName) const;
    [[nodiscard]] const CatalogCharacterExpression* FindExpression(
        const CatalogCharacter& character, std::string_view idOrName) const;
    [[nodiscard]] std::optional<ResourceRefValue> ResolveCharacterImage(
        std::string_view character, std::string_view expression = {}) const;
    [[nodiscard]] std::string CharacterDisplayName(std::string_view character) const;
private:
    std::vector<CatalogVariable> m_variables;
    std::vector<CatalogCharacter> m_characters;
    std::vector<CatalogGalleryItem> m_gallery;
    std::vector<CatalogInputBinding> m_input;
};

}  // namespace px::vn
