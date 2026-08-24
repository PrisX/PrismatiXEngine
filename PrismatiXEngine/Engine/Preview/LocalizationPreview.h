#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::preview {

struct LocalizationPreviewEntry {
    std::string stringId;
    std::string sourceId;
    std::string documentId;
    std::string documentUri;
    std::string text;
    std::string voiceAssetId;
    std::string fontAssetId;
    std::optional<int> maxCharacters;
};

// Immutable localization projection installed into the shared VM text filter.
// Studio sends the canonical document; RuntimeCore owns validation, pseudo
// transformation and the text actually measured and rendered by GalgameUI.
struct LocalizationPreviewTable {
    std::string locale;
    std::string focusSourceId;
    std::uint64_t documentRevision = 0;
    bool pseudo = false;
    std::unordered_map<std::string, LocalizationPreviewEntry> entries;
    std::vector<diag::Diagnostic> diagnostics;

    [[nodiscard]] std::string Translate(std::string_view sourceId,
                                        std::string_view fallback) const;
};

[[nodiscard]] std::string PseudoLocalize(std::string_view text);

[[nodiscard]] Result<LocalizationPreviewTable> BuildLocalizationPreviewTable(
    std::string_view localizationJson);

}  // namespace px::preview
