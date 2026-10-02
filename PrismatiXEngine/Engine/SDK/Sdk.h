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


// Public Native SDK entrypoint. Engine implementation code may continue to use
// px::sdk internally; the contract revisions in ContractVersions.h say when a
// published name changes meaning.
namespace px::sdk::api {

inline constexpr std::string_view kEngineVersion = ::px::sdk::kEngineVersion;
using namespace ::px::sdk;

}  // namespace px::sdk::api
