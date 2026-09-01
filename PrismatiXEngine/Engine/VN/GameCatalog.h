#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/ResourceRef.h"
#include "Engine/SDK/CharacterResources.h"
#include "Engine/SDK/GameCatalogResources.h"
#include "Engine/VN/Expression/Expression.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::vn {

struct CatalogVariable {
    enum class Scope : std::uint8_t { Session, Profile };
    std::string name;
    int defaultValue = 0;
    Value typedDefault;
    Scope scope = Scope::Session;
};
struct CatalogCharacterExpression {
    std::string id;
    std::string name;
    std::vector<std::string> aliases;
    ResourceRefValue image;
};
struct CatalogCharacter {
    std::string id;
    std::string name;
    std::vector<std::string> aliases;
    std::string voiceDirectory;
    std::string defaultExpression;
    std::vector<CatalogCharacterExpression> expressions;
};
struct CatalogGalleryItem {
    std::string id;
    std::string title;
    // Resolved paths remain the presentation-facing compatibility surface;
    // identity is carried separately and always comes from the manifest UUID.
    std::string image;
    std::string thumbnail;
    ResourceRefValue imageReference;
    std::optional<ResourceRefValue> thumbnailReference;
};
struct CatalogInputBinding { std::string key; std::string command; std::string argument; };
struct CatalogUnlockable {
    std::string id;
    std::string kind;
    std::string condition;
    Value payload;
};

class GameCatalog {
public:
    Status Load(std::string_view typedResource, const std::string& sourcePath = {});
    Status LoadCanonical(
        std::string_view json,
        std::string_view projectManifest,
        const sdk::GameCatalogResourceExists& exists,
        const std::string& sourcePath = "Content/game.pxgame");
    // Loads only the post-character-migration runtime entries. Character state
    // is preserved so characterResources and the residual Game.pxres can be
    // composed without creating two character authorities.
    Status LoadRuntimeResources(
        std::string_view typedResource,
        const std::string& sourcePath = "Content/Game.pxres",
        sdk::LegacyGameCatalogPolicy legacyPolicy =
            sdk::LegacyGameCatalogPolicy::RejectCharacterNodes,
        sdk::LegacyGalleryReferencePolicy galleryPolicy =
            sdk::LegacyGalleryReferencePolicy::RejectPathStrings,
        std::string_view projectManifest = {},
        const sdk::GameCatalogResourceExists& exists = {});
    // Replaces only Character semantics from the public characterResources
    // contract. Variables, gallery items, and input bindings already loaded
    // from an optional legacy Game.pxres remain intact during migration.
    Status LoadCharacterResources(
        std::string_view projectManifest,
        const sdk::CharacterResourceReadText& readText,
        const sdk::CharacterResourceExists& exists,
        bool& declared);
    [[nodiscard]] const std::vector<CatalogVariable>& Variables() const { return m_variables; }
    [[nodiscard]] const std::vector<CatalogCharacter>& Characters() const { return m_characters; }
    [[nodiscard]] const std::vector<CatalogGalleryItem>& Gallery() const { return m_gallery; }
    [[nodiscard]] const std::vector<CatalogInputBinding>& InputBindings() const { return m_input; }
    [[nodiscard]] const std::vector<CatalogUnlockable>& Unlockables() const { return m_unlockables; }
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
    std::vector<CatalogUnlockable> m_unlockables;
};

}  // namespace px::vn
