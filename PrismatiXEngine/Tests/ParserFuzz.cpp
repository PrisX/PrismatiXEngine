#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Engine/Animation/Timeline.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/IO/VFS.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 16u * 1024u * 1024u) return 0;
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    (void)px::vn::scenario::ParseScenario(input, "fuzz.pxscenario");
    (void)px::vn::scenario::ParseScenarioLayout(input, "fuzz.pxlayout");
    (void)px::animation::ParseAnimationClip(input, "fuzz.pxanim");
    (void)px::resource::ParseTypedDocument(input, "fuzz.pxscene");
    (void)px::sdk::ParseRuntimeIr(input);
    (void)px::sdk::ParseSourceMap(input);
    (void)px::sdk::ParsePackageRequest(input);
    (void)px::sdk::detail::ParsePackageManifest(input);
    (void)px::progress::ParseSaveEnvelopeV2(input, "fuzz.pxsav");
    (void)px::io::VFS::NormalizeVirtualPath(input);
    // Invalid save inputs intentionally produce structured diagnostics.  A
    // long fuzz campaign must not retain one object per corpus mutation.
    px::diag::Global().Clear();
    return 0;
}
