#include "Engine/Animation/Timeline.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,std::size_t size){
    if(size>16u*1024u*1024u)return 0;const std::string_view input(reinterpret_cast<const char*>(data),size);
    (void)px::vn::scenario::ParseScenario(input,"fuzz.pxscenario");
    (void)px::vn::scenario::ParseScenarioLayout(input,"fuzz.pxlayout");
    (void)px::animation::ParseAnimationClip(input,"fuzz.pxanim");
    (void)px::resource::ParseTypedDocument(input,"fuzz.pxscene");
    return 0;
}
