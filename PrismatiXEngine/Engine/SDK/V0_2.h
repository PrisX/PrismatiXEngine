#pragma once

#include "Engine/SDK/CharacterResources.h"
#include "Engine/SDK/ContractVersions.h"
#include "Engine/SDK/GameCatalogResources.h"
#include "Engine/SDK/Packager.h"
#include "Engine/SDK/PreviewSession.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/SDK/Ui.h"
#include "Engine/SDK/UiTypeRegistry.h"

#include <string_view>

// Public Native SDK entrypoint. New SDK revisions add another versioned
// namespace instead of changing the meaning/layout of names already published
// here. Engine implementation code may continue to use px::sdk internally.
namespace px::sdk::v0_2 {

inline constexpr std::string_view kEngineVersion = "0.2.0";
using namespace ::px::sdk;

}  // namespace px::sdk::v0_2
