#include "Engine/Preview/LocalizationPreview.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace px::preview {
namespace {

using Json = nlohmann::json;

diag::Diagnostic Error(std::string code, std::string message,
                       const std::string& documentId = {},
                       const std::string& documentUri = {},
                       const std::string& sourceId = {},
                       std::string property = {}) {
    diag::Diagnostic diagnostic;
    diagnostic.severity = diag::Severity::Error;
    diagnostic.code = std::move(code);
    diagnostic.category = "Preview.Localization";
    diagnostic.message = std::move(message);
    diagnostic.source.resourceId = documentId;
    diagnostic.source.path = documentUri;
    diagnostic.source.nodeId = sourceId;
    diagnostic.source.property = std::move(property);
    return diagnostic;
}

diag::Diagnostic Warning(std::string code, std::string message,
                         const std::string& documentId,
                         const std::string& documentUri,
                         const std::string& sourceId,
                         std::string property) {
    auto diagnostic = Error(std::move(code), std::move(message), documentId,
                            documentUri, sourceId, std::move(property));
    diagnostic.severity = diag::Severity::Warning;
    return diagnostic;
}

struct Utf8Metrics {
    int characters = 0;
    int visualUnits = 0;
};

Utf8Metrics MeasureUtf8(const std::string_view text) {
    Utf8Metrics metrics;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t codepoint = first;
        std::size_t width = 1;
        if ((first & 0xE0U) == 0xC0U && index + 1 < text.size()) {
            width = 2;
            codepoint = ((first & 0x1FU) << 6U) |
                        (static_cast<unsigned char>(text[index + 1]) & 0x3FU);
        } else if ((first & 0xF0U) == 0xE0U && index + 2 < text.size()) {
            width = 3;
            codepoint = ((first & 0x0FU) << 12U) |
                        ((static_cast<unsigned char>(text[index + 1]) & 0x3FU)
                         << 6U) |
                        (static_cast<unsigned char>(text[index + 2]) & 0x3FU);
        } else if ((first & 0xF8U) == 0xF0U && index + 3 < text.size()) {
            width = 4;
            codepoint = ((first & 0x07U) << 18U) |
                        ((static_cast<unsigned char>(text[index + 1]) & 0x3FU)
                         << 12U) |
                        ((static_cast<unsigned char>(text[index + 2]) & 0x3FU)
                         << 6U) |
                        (static_cast<unsigned char>(text[index + 3]) & 0x3FU);
        }
        ++metrics.characters;
        metrics.visualUnits += codepoint > 0xFFU ? 2 : 1;
        index += width;
    }
    return metrics;
}

std::optional<std::string> OptionalString(const Json& value,
                                          const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || found->is_null()) return std::string{};
    if (!found->is_string()) return std::nullopt;
    return found->get<std::string>();
}

}  // namespace

std::string PseudoLocalize(const std::string_view text) {
    std::string output = "［";
    for (const char character : text) {
        switch (character) {
            case 'a': output += "á"; break;
            case 'e': output += "ë"; break;
            case 'i': output += "ï"; break;
            case 'o': output += "ô"; break;
            case 'u': output += "ü"; break;
            case 'A': output += "Â"; break;
            case 'E': output += "Ë"; break;
            case 'I': output += "Ï"; break;
            case 'O': output += "Ö"; break;
            case 'U': output += "Û"; break;
            default: output.push_back(character); break;
        }
    }
    output += "··］";
    return output;
}

std::string LocalizationPreviewTable::Translate(
    const std::string_view sourceId, const std::string_view fallback) const {
    const auto found = entries.find(std::string(sourceId));
    return found == entries.end() ? std::string(fallback) : found->second.text;
}

Result<LocalizationPreviewTable> BuildLocalizationPreviewTable(
    const std::string_view localizationJson) {
    const Json request = Json::parse(localizationJson, nullptr, false);
    if (request.is_discarded() || !request.is_object() ||
        !request.contains("document") || !request["document"].is_object() ||
        !request.contains("locale") || !request["locale"].is_string() ||
        !request.contains("focusSourceId") ||
        !request["focusSourceId"].is_string()) {
        return Result<LocalizationPreviewTable>::Failure(Error(
            "PXWASM-LOCALIZATION-001",
            "Localization Preview requires a canonical document, locale, and focused Story source."));
    }
    const Json& document = request["document"];
    const std::string locale = request["locale"].get<std::string>();
    const std::string focusSourceId =
        request["focusSourceId"].get<std::string>();
    if (document.value("format", std::string{}) != "PrismatiXLocalization" ||
        document.value("schemaRevision", 0) != 1 ||
        !document.contains("revision") ||
        !document["revision"].is_number_unsigned() ||
        !document.contains("locales") || !document["locales"].is_array() ||
        !document.contains("entries") || !document["entries"].is_array()) {
        return Result<LocalizationPreviewTable>::Failure(Error(
            "PXWASM-LOCALIZATION-002",
            "Localization Preview rejected an incompatible document schema."));
    }
    const bool localeDeclared = std::ranges::any_of(
        document["locales"], [&locale](const Json& value) {
            return value.is_string() && value.get<std::string>() == locale;
        });
    if (!localeDeclared) {
        return Result<LocalizationPreviewTable>::Failure(Error(
            "PXWASM-LOCALIZATION-003",
            "The requested Preview locale is not declared by the Localization document."));
    }

    LocalizationPreviewTable table;
    table.locale = locale;
    table.focusSourceId = focusSourceId;
    table.documentRevision = document["revision"].get<std::uint64_t>();
    table.pseudo = request.value("pseudo", false);
    std::unordered_set<std::string> sourceIds;
    bool focusedEntryFound = false;
    for (const auto& entry : document["entries"]) {
        if (!entry.is_object() || !entry.contains("sourceId") ||
            !entry["sourceId"].is_string() ||
            !entry.contains("stringId") || !entry["stringId"].is_string() ||
            !entry.contains("documentId") ||
            !entry["documentId"].is_string() ||
            !entry.contains("documentUri") ||
            !entry["documentUri"].is_string() ||
            !entry.contains("sourceText") ||
            !entry["sourceText"].is_string() ||
            !entry.contains("translations") ||
            !entry["translations"].is_array()) {
            return Result<LocalizationPreviewTable>::Failure(Error(
                "PXWASM-LOCALIZATION-004",
                "A Localization entry is missing stable source identity or translations."));
        }
        if (entry.value("obsolete", false)) continue;
        LocalizationPreviewEntry projected;
        projected.sourceId = entry["sourceId"].get<std::string>();
        projected.stringId = entry["stringId"].get<std::string>();
        projected.documentId = entry["documentId"].get<std::string>();
        projected.documentUri = entry["documentUri"].get<std::string>();
        if (projected.sourceId.empty() ||
            !sourceIds.insert(projected.sourceId).second) {
            return Result<LocalizationPreviewTable>::Failure(Error(
                "PXWASM-LOCALIZATION-005",
                "Localization source identities must be non-empty and unique.",
                projected.documentId, projected.documentUri,
                projected.sourceId, "sourceId"));
        }
        if (entry.contains("maxCharacters") &&
            !entry["maxCharacters"].is_null()) {
            if (!entry["maxCharacters"].is_number_integer() ||
                entry["maxCharacters"].get<int>() <= 0) {
                return Result<LocalizationPreviewTable>::Failure(Error(
                    "PXWASM-LOCALIZATION-006",
                    "Localization maxCharacters must be null or a positive integer.",
                    projected.documentId, projected.documentUri,
                    projected.sourceId, "maxCharacters"));
            }
            projected.maxCharacters = entry["maxCharacters"].get<int>();
        }
        const Json* selected = nullptr;
        for (const auto& translation : entry["translations"]) {
            if (translation.is_object() &&
                translation.value("locale", std::string{}) == locale) {
                selected = &translation;
                break;
            }
        }
        std::string localized;
        if (selected && selected->contains("text") &&
            (*selected)["text"].is_string()) {
            localized = (*selected)["text"].get<std::string>();
            const auto voice = OptionalString(*selected, "voiceAssetId");
            const auto font = OptionalString(*selected, "fontAssetId");
            if (!voice || !font) {
                return Result<LocalizationPreviewTable>::Failure(Error(
                    "PXWASM-LOCALIZATION-007",
                    "Localization voice and font references must be UUID strings or null.",
                    projected.documentId, projected.documentUri,
                    projected.sourceId, "translations"));
            }
            projected.voiceAssetId = *voice;
            projected.fontAssetId = *font;
            if (selected->contains("ruby")) {
                if (!(*selected)["ruby"].is_array()) {
                    return Result<LocalizationPreviewTable>::Failure(Error(
                        "PXWASM-LOCALIZATION-RUBY-001",
                        "Localization ruby annotations must be an array.",
                        projected.documentId, projected.documentUri,
                        projected.sourceId, "translations.ruby"));
                }
                const int length = MeasureUtf8(localized).characters;
                for (const auto& ruby : (*selected)["ruby"]) {
                    if (!ruby.is_object() || !ruby.contains("start") ||
                        !ruby["start"].is_number_integer() ||
                        !ruby.contains("end") ||
                        !ruby["end"].is_number_integer() ||
                        !ruby.contains("text") || !ruby["text"].is_string() ||
                        ruby["start"].get<int>() < 0 ||
                        ruby["end"].get<int>() <= ruby["start"].get<int>() ||
                        ruby["end"].get<int>() > length) {
                        return Result<LocalizationPreviewTable>::Failure(Error(
                            "PXWASM-LOCALIZATION-RUBY-002",
                            "Localization ruby range is outside the translated text.",
                            projected.documentId, projected.documentUri,
                            projected.sourceId, "translations.ruby"));
                    }
                }
            }
        }
        const std::string sourceText = entry["sourceText"].get<std::string>();
        projected.text = table.pseudo
                             ? PseudoLocalize(localized.empty()
                                                   ? sourceText
                                                   : localized)
                             : localized.empty() ? sourceText : localized;
        if (projected.sourceId == focusSourceId) {
            focusedEntryFound = true;
            if (!table.pseudo && localized.empty()) {
                return Result<LocalizationPreviewTable>::Failure(Error(
                    "PXWASM-LOCALIZATION-MISSING-001",
                    locale + " translation is missing for the selected Story source.",
                    projected.documentId, projected.documentUri,
                    projected.sourceId, "translations.text"));
            }
            if (projected.maxCharacters) {
                const auto metrics = MeasureUtf8(projected.text);
                if (metrics.characters > *projected.maxCharacters) {
                    table.diagnostics.push_back(Warning(
                        "PXWASM-LOCALIZATION-LENGTH-001",
                        std::to_string(metrics.characters) + "/" +
                            std::to_string(*projected.maxCharacters) +
                            " characters in the Runtime-rendered text.",
                        projected.documentId, projected.documentUri,
                        projected.sourceId, "maxCharacters"));
                }
                if (static_cast<double>(metrics.visualUnits) >
                    static_cast<double>(*projected.maxCharacters) * 1.6) {
                    table.diagnostics.push_back(Warning(
                        "PXWASM-LOCALIZATION-OVERFLOW-001",
                        "Runtime text may overflow the authored character budget; inspect the WASM canvas.",
                        projected.documentId, projected.documentUri,
                        projected.sourceId, "translations.text"));
                }
            }
        }
        table.entries.emplace(projected.sourceId, std::move(projected));
    }
    if (!focusedEntryFound) {
        return Result<LocalizationPreviewTable>::Failure(Error(
            "PXWASM-LOCALIZATION-FOCUS-001",
            "The selected Localization source is obsolete or absent from the canonical document.",
            {}, {}, focusSourceId, "focusSourceId"));
    }
    return Result<LocalizationPreviewTable>::Success(std::move(table));
}

}  // namespace px::preview
