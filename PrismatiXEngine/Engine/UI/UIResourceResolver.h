#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/UI/Theme.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace px::ui {

using UIDocumentLoader =
    std::function<Result<resource::TypedDocument>(const ResourceRefValue& reference)>;

struct ExpandedNodeOrigin {
    Uuid runtimeId;
    Uuid instanceId;
    Uuid sourceId;
};

struct ExpandedUIDocument {
    resource::TypedDocument document;
    std::vector<ExpandedNodeOrigin> origins;
};

[[nodiscard]] Result<ExpandedUIDocument> ExpandUIComponents(
    const resource::TypedDocument& source, const UIDocumentLoader& loader);

[[nodiscard]] Result<Variant> ResolveThemeValue(
    const Variant& value, const VariantObject& tokens);

[[nodiscard]] Result<Theme> LoadUITheme(
    const resource::TypedDocument& document);

}  // namespace px::ui
