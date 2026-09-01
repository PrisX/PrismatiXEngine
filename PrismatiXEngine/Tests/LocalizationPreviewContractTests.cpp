#include "Engine/Preview/LocalizationPreview.h"

#include <cassert>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using Json = nlohmann::json;

Json Document(std::string text = "こんにちは", int maxCharacters = 12) {
    return {{"format", "PrismatiXLocalization"},
            {"schemaRevision", 2},
            {"revision", 8},
            {"primaryLocale", "en-US"},
            {"referenceLocale", "ja-JP"},
            {"locales", Json::array({"en-US", "ja-JP"})},
            {"entries",
             Json::array({{{"id", "entry-01"},
                           {"stringId", "scene.greeting"},
                           {"sourceId", "block-01"},
                           {"documentId", "scene-01"},
                           {"documentUri", "Story/scene-01.pxstory"},
                           {"sourceText", "Open"},
                           {"maxCharacters", maxCharacters},
                           {"obsolete", false},
                           {"translations",
                            Json::array({{{"locale", "ja-JP"},
                                          {"text", std::move(text)},
                                          {"voiceAssetId", nullptr},
                                          {"fontAssetId", nullptr},
                                          {"ruby", Json::array()}}})}}})}};
}

std::string Request(Json document, const bool pseudo = false,
                    std::string focus = "block-01") {
    return Json{{"document", std::move(document)},
                {"locale", "ja-JP"},
                {"pseudo", pseudo},
                {"focusSourceId", std::move(focus)}}
        .dump();
}

bool ContainsCode(const std::vector<px::diag::Diagnostic>& diagnostics,
                  const std::string& code) {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

}  // namespace

int main() {
    auto legacyDocument = Document();
    legacyDocument["schemaRevision"] = 1;
    assert(!px::preview::BuildLocalizationPreviewTable(
        Request(std::move(legacyDocument))));

    const auto localized =
        px::preview::BuildLocalizationPreviewTable(Request(Document()));
    assert(localized);
    assert(localized.Value().locale == "ja-JP");
    assert(localized.Value().documentRevision == 8);
    assert(localized.Value().Translate("block-01", "fallback") ==
           "こんにちは");
    assert(localized.Value().Translate("unknown", "fallback") ==
           "fallback");

    auto pseudoDocument = Document("Open");
    const auto pseudo = px::preview::BuildLocalizationPreviewTable(
        Request(std::move(pseudoDocument), true));
    assert(pseudo);
    assert(pseudo.Value().Translate("block-01", "") == "［Öpën··］");
    assert(px::preview::PseudoLocalize("Open") == "［Öpën··］");

    const auto missing = px::preview::BuildLocalizationPreviewTable(
        Request(Document("")));
    assert(!missing);
    assert(ContainsCode(missing.Diagnostics(),
                        "PXWASM-LOCALIZATION-MISSING-001"));
    assert(missing.Diagnostics().front().source.nodeId == "block-01");
    assert(missing.Diagnostics().front().source.path ==
           "Story/scene-01.pxstory");

    const auto overflow = px::preview::BuildLocalizationPreviewTable(
        Request(Document("日本語の長いテキスト", 3)));
    assert(overflow);
    assert(ContainsCode(overflow.Value().diagnostics,
                        "PXWASM-LOCALIZATION-LENGTH-001"));
    assert(ContainsCode(overflow.Value().diagnostics,
                        "PXWASM-LOCALIZATION-OVERFLOW-001"));

    const auto missingFocus = px::preview::BuildLocalizationPreviewTable(
        Request(Document(), false, "missing-source"));
    assert(!missingFocus);
    assert(ContainsCode(missingFocus.Diagnostics(),
                        "PXWASM-LOCALIZATION-FOCUS-001"));
    return 0;
}
