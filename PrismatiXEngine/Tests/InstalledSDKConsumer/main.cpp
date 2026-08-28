#include <Engine/SDK/ContractVersions.h>
#include <Engine/SDK/PreviewSession.h>
#include <Engine/SDK/RuntimeIr.h>
#include <Engine/SDK/Ui.h>

#include <filesystem>
#include <string>
#include <type_traits>

#ifndef PRISMATIX_INSTALLED_PREVIEW_HOST
#error "Installed SDK consumer must receive the PreviewHost path"
#endif

int main() {
    static_assert(px::sdk::kPreviewSessionContractRevision == 2);
    static_assert(std::is_member_function_pointer_v<
                  decltype(&px::sdk::PreviewSession::ApplyTimeline)>);
    static_assert(std::is_member_function_pointer_v<
                  decltype(&px::sdk::PreviewSession::SeekTimeline)>);

    if (!std::filesystem::is_regular_file(PRISMATIX_INSTALLED_PREVIEW_HOST))
        return 3;

    const std::string runtimeIr =
        R"({"format":"PrismatiXRuntimeIR","schemaRevision":1,"documentId":"consumer","committedRevision":1,"operations":[]})";
    const auto parsed = px::sdk::ParseRuntimeIr(runtimeIr);
    if (!parsed.Valid() || parsed.document.documentId != "consumer") return 1;

    const auto invalidUi = px::sdk::ParseUi("{}");
    return !invalidUi.Valid() && !invalidUi.diagnostics.empty() ? 0 : 2;
}
