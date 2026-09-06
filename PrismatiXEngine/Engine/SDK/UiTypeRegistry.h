#pragma once

#include "Engine/SDK/ContractVersions.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

struct UiTypeRegistryRange {
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.1;
};

struct UiTypeRegistryProperty {
    std::string id;
    std::string displayName;
    std::string description;
    std::string category;
    std::string valueType;
    std::string defaultValueJson;
    bool writable = false;
    bool bindable = false;
    bool animatable = false;
    bool advanced = false;
    std::vector<std::string> enumChoices;
    bool hasRange = false;
    UiTypeRegistryRange range;
    std::string resourceFilter;
    std::string editorHint;
    bool multiline = false;
    bool tokenBindable = false;
};

struct UiTypeRegistrySignalArgument {
    std::string id;
    std::string valueType;
};

struct UiTypeRegistrySignal {
    std::string id;
    std::string displayName;
    std::string description;
    std::vector<UiTypeRegistrySignalArgument> arguments{};
};

struct UiTypeRegistryControl {
    std::string id;
    std::string runtimeType;
    std::string nodeKind;
    std::string displayName;
    std::string description;
    std::string category;
    std::string iconId;
    bool canHaveChildren = false;
    std::vector<std::string> acceptedResourceKinds;
    std::vector<UiTypeRegistryProperty> properties;
    std::vector<UiTypeRegistrySignal> signals;
};

struct UiTypeRegistryManifest {
    std::uint32_t schemaRevision = kUiTypeRegistrySchemaRevision;
    std::uint32_t contractRevision = kUiTypeRegistryContractRevision;
    std::string contractHash;
    std::vector<UiTypeRegistryControl> controls;
};

struct UiTypeRegistryDiagnostic {
    std::string code;
    std::string message;
};

struct UiTypeRegistryParseResult {
    UiTypeRegistryManifest manifest;
    std::vector<UiTypeRegistryDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

// Produces canonical JSON and computes contractHash over the same document with
// an all-zero hash. Callers must sort control/property/signal identities first.
[[nodiscard]] std::string SerializeUiTypeRegistry(
    const UiTypeRegistryManifest& manifest);

// Strict, bounded parser used by SDK consumers before exposing authoring
// metadata. Unknown fields are rejected at this contract revision.
[[nodiscard]] UiTypeRegistryParseResult ParseUiTypeRegistry(
    std::string_view json);

}  // namespace px::sdk
