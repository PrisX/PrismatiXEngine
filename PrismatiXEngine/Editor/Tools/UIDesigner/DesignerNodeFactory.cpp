#include "Editor/Tools/UIDesigner/DesignerNodeFactory.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UITypeRegistry.h"

#include <algorithm>
#include <cctype>

namespace px::editor {
namespace {
std::string Lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}
}

std::vector<const TypeInfo*> DesignerNodeFactory::Palette(std::string_view search,
                                                          std::string_view category) {
    (void)ui::RegisterBuiltinUITypes();
    auto types = TypeRegistry::Global().TypesDerivedFrom("Control");
    const std::string needle = Lower(search);
    std::erase_if(types, [&](const TypeInfo* type) {
        if (!type || !type->designer || !type->designer->paletteVisible) return true;
        if (!category.empty() && type->designer->category != category) return true;
        return !needle.empty() && Lower(type->name + " " + type->designer->displayName + " " +
                                        type->designer->description).find(needle) == std::string::npos;
    });
    std::ranges::sort(types, {}, [](const TypeInfo* type) {
        return std::pair{type->designer->category, type->designer->displayName};
    });
    return types;
}

bool DesignerNodeFactory::AcceptsAsset(std::string_view type, std::string_view assetType) {
    (void)ui::RegisterBuiltinUITypes();
    const auto* info = TypeRegistry::Global().Find(std::string(type));
    if (!info || !info->designer) return false;
    return std::ranges::find(info->designer->acceptedAssetTypes, assetType) !=
           info->designer->acceptedAssetTypes.end();
}

Result<VariantObject> DesignerNodeFactory::Create(std::string_view type, Vec2 position,
                                                  std::string_view asset) {
    (void)ui::RegisterBuiltinUITypes();
    const auto* info = TypeRegistry::Global().Find(std::string(type));
    if (!info || !info->designer || !info->designer->paletteVisible) {
        return Result<VariantObject>::Failure(diag::Diagnostic{
            .severity=diag::Severity::Error, .code="PXEDUI3101", .category="Editor.UIDesigner",
            .message="Control type is not available in the Designer palette", .details=std::string(type)});
    }
    VariantObject properties;
    for (const auto& [name, value] : info->designer->defaultProperties)
        properties.emplace(name, value.Clone());
    properties["offsets"] = Rect{position.x, position.y, info->designer->defaultSize.x,
                                  info->designer->defaultSize.y};
    if (!asset.empty()) {
        if (std::ranges::find(info->designer->acceptedAssetTypes, "image") !=
            info->designer->acceptedAssetTypes.end()) properties[type=="TextureRect"?"path":"image"] = std::string(asset);
        else if (std::ranges::find(info->designer->acceptedAssetTypes, "font") !=
                 info->designer->acceptedAssetTypes.end()) properties["font"] = std::string(asset);
    }
    VariantObject node;
    node["id"] = Uuid::Random();
    node["type"] = std::string(type);
    node["name"] = info->designer->displayName;
    node["properties"] = std::move(properties);
    node["children"] = VariantArray{};
    return Result<VariantObject>::Success(std::move(node));
}
}  // namespace px::editor
