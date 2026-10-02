#include <Engine/SDK/ContractVersions.h>
#include <Engine/SDK/Motion.h>
#include <Engine/SDK/PreviewSession.h>
#include <Engine/SDK/RuntimeIr.h>
#include <Engine/SDK/Ui.h>
#include <Engine/SDK/Sdk.h>

#include <filesystem>
#include <string>
#include <type_traits>

#ifndef PRISMATIX_INSTALLED_PREVIEW_HOST
#error "Installed SDK consumer must receive the PreviewHost path"
#endif

int main() {
    static_assert(px::sdk::kPreviewSessionContractRevision == 3);
    static_assert(std::is_member_function_pointer_v<
                  decltype(&px::sdk::PreviewSession::ApplyTimeline)>);
    static_assert(std::is_member_function_pointer_v<
                  decltype(&px::sdk::PreviewSession::SeekTimeline)>);
    static_assert(px::sdk::api::kEngineVersion == px::sdk::kEngineVersion);
    static_assert(std::is_same_v<px::sdk::api::RuntimeIrDocument,
                                 px::sdk::RuntimeIrDocument>);

    if (!std::filesystem::is_regular_file(PRISMATIX_INSTALLED_PREVIEW_HOST))
        return 3;

    const std::string runtimeIr =
        R"({"format":"PrismatiXRuntimeIR","schemaRevision":2,"documentId":"consumer","committedRevision":1,"operations":[]})";
    const auto parsed = px::sdk::api::ParseRuntimeIr(runtimeIr);
    if (!parsed.Valid() || parsed.document.documentId != "consumer") return 1;

    const auto invalidUi = px::sdk::ParseUi("{}");
    if (invalidUi.Valid() || invalidUi.diagnostics.empty()) return 2;

    const auto motion = px::sdk::ParseMotion(R"({
      "format":"PrismatiXMotion","schemaRevision":1,"id":"consumer",
      "canvas":{"width":1280,"height":720},"duration":1,"loop":false,
      "layers":[{"id":"logo","image":"Assets/logo.png","width":32,"height":32}],
      "tracks":[]})");
    if (!motion.Valid()) return 4;
    px::sdk::MotionSampler sampler(*motion.clip);
    sampler.Seek(0.5);
    return sampler.Pose().size() == 1 ? 0 : 5;
}
