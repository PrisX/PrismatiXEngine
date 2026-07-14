#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/TypedDocument.h"

#include <string_view>
#include <vector>

namespace px { struct TypeInfo; }
namespace px::editor {

class DesignerNodeFactory {
public:
    [[nodiscard]] static std::vector<const TypeInfo*> Palette(std::string_view search = {},
                                                              std::string_view category = {});
    [[nodiscard]] static Result<VariantObject> Create(std::string_view type, Vec2 position = {},
                                                      std::string_view asset = {});
    [[nodiscard]] static bool AcceptsAsset(std::string_view type, std::string_view assetType);
};

}  // namespace px::editor
