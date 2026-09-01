#include "Engine/Preview/PreviewProtocolV2.h"
#include "Engine/SDK/ContractVersions.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    std::ifstream stream(PRISMATIX_SDK_MANIFEST_PATH, std::ios::binary);
    Check(stream.good(), "configured SDK manifest must exist");
    const auto manifest = nlohmann::json::parse(stream, nullptr, false);
    Check(!manifest.is_discarded(), "configured SDK manifest must be valid JSON");
    Check(manifest.at("sdkVersion") == "0.2.0" &&
              manifest.at("nativeNamespace") == "px::sdk::v0_2",
          "SDK manifest must publish the 0.2 versioned Native namespace");
    Check(manifest.at("contracts").at("uiTypeRegistry") ==
              std::to_string(px::sdk::kUiTypeRegistryContractRevision),
          "SDK manifest and UI TypeRegistry contract revisions must match");
    Check(manifest.at("contracts").at("authoringSchemas") ==
              std::to_string(px::sdk::kAuthoringContractRevision),
          "SDK manifest and Authoring schema contract revisions must match");
    Check(manifest.at("contracts").at("ui") ==
              std::to_string(px::sdk::kUiContractRevision),
          "SDK manifest and UI document contract revisions must match");
    Check(manifest.at("contracts").at("previewSession") ==
              std::to_string(px::sdk::kPreviewSessionContractRevision),
          "SDK manifest and PreviewSession contract revisions must match");
    Check(!manifest.at("contracts").contains("studioUi") &&
              !manifest.contains("deprecatedContracts"),
          "removed StudioUi compatibility must not remain in the 0.2 SDK manifest");
    Check(manifest.at("previewProtocolRange").at("min") ==
              px::sdk::kPreviewProtocolVersion &&
              manifest.at("previewProtocolRange").at("max") ==
                  px::sdk::kPreviewProtocolVersion,
          "SDK manifest and Preview protocol versions must match");
    Check(px::preview::kSchemaRevision == px::sdk::kPreviewSchemaRevision &&
              px::preview::kProtocolVersion ==
                  px::sdk::kPreviewProtocolVersion,
          "Preview protocol constants must use the authoritative revisions");
    return 0;
}
