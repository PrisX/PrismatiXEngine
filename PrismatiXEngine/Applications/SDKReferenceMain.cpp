#include "Engine/SDK/UiTypeRegistry.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaximumInputBytes = 16 * 1024 * 1024;

std::optional<std::string> ReadBounded(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumInputBytes) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    if (!stream) return std::nullopt;
    return contents.str();
}

std::string MarkdownCell(std::string value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char byte : value) {
        if (byte == '|') escaped += "\\|";
        else if (byte == '\n' || byte == '\r') escaped += ' ';
        else escaped += byte;
    }
    return escaped;
}

std::optional<std::string> Generate(const std::filesystem::path& contractsPath,
                                    const std::filesystem::path& registryPath) {
    const auto contractsSource = ReadBounded(contractsPath);
    const auto registrySource = ReadBounded(registryPath);
    if (!contractsSource || !registrySource) {
        std::cerr << "SDK reference input is missing or exceeds 16 MiB\n";
        return std::nullopt;
    }

    const auto contracts = nlohmann::json::parse(*contractsSource, nullptr, false);
    if (contracts.is_discarded() || !contracts.is_object() ||
        contracts.value("format", "") != "PrismatiXContractManifest" ||
        contracts.value("schemaRevision", 0u) != 2u ||
        !contracts.contains("contracts") || !contracts["contracts"].is_array()) {
        std::cerr << "SDK contract manifest is unsupported\n";
        return std::nullopt;
    }
    const auto registry = px::sdk::ParseUiTypeRegistry(*registrySource);
    if (!registry.Valid()) {
        for (const auto& diagnostic : registry.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return std::nullopt;
    }

    std::ostringstream output;
    output << "# PrismatiX SDK Reference 0.2.0\n\n"
              "> Generated from the canonical contract manifest and the Runtime UI "
              "TypeRegistry. Do not edit this artifact by hand.\n\n"
              "## Public entry points\n\n"
              "- Native C++: `#include <Engine/SDK/V0_2.h>` and namespace "
              "`px::sdk::v0_2`\n"
              "- Authoring TypeScript: `@prismatix/authoring-sdk`\n"
              "- Extension TypeScript: `@prismatix/runtime`\n"
              "- CLI: `prismatix validate|build|run|pack|migrate|inspect-save`\n\n"
              "## Canonical documents\n\n"
              "| Contract | Extension | Schema | Revision | Migration policy |\n"
              "| --- | --- | --- | ---: | --- |\n";
    for (const auto& contract : contracts["contracts"]) {
        if (!contract.is_object() || !contract.contains("id") ||
            !contract["id"].is_string() || !contract.contains("extension") ||
            !contract["extension"].is_string() || !contract.contains("schema") ||
            !contract["schema"].is_string() ||
            !contract.contains("schemaRevision") ||
            !contract["schemaRevision"].is_number_unsigned() ||
            !contract.contains("migration") ||
            !contract["migration"].is_string()) {
            std::cerr << "SDK contract descriptor is malformed\n";
            return std::nullopt;
        }
        output << "| `" << MarkdownCell(contract["id"].get<std::string>())
               << "` | `" << MarkdownCell(contract["extension"].get<std::string>())
               << "` | `" << MarkdownCell(contract["schema"].get<std::string>())
               << "` | " << contract["schemaRevision"].get<unsigned int>()
               << " | `" << MarkdownCell(contract["migration"].get<std::string>())
               << "` |\n";
    }

    output << "\n## UI TypeRegistry\n\n"
           << "Registry schema revision " << registry.manifest.schemaRevision
           << ", contract revision " << registry.manifest.contractRevision
           << ", hash `" << registry.manifest.contractHash << "`.\n\n";
    for (const auto& control : registry.manifest.controls) {
        output << "### `" << MarkdownCell(control.id) << "` (`"
               << MarkdownCell(control.runtimeType) << "`)\n\n"
               << MarkdownCell(control.description) << "\n\n"
               << "Category: `" << MarkdownCell(control.category)
               << "`; node kind: `" << MarkdownCell(control.nodeKind)
               << "`; children: " << (control.canHaveChildren ? "yes" : "no")
               << ".\n\n";
        if (!control.properties.empty()) {
            output << "| Property | Type | Default | Writable | Bindable | Animatable |\n"
                      "| --- | --- | --- | :---: | :---: | :---: |\n";
            for (const auto& property : control.properties)
                output << "| `" << MarkdownCell(property.id) << "` | `"
                       << MarkdownCell(property.valueType) << "` | `"
                       << MarkdownCell(property.defaultValueJson) << "` | "
                       << (property.writable ? "yes" : "no") << " | "
                       << (property.bindable ? "yes" : "no") << " | "
                       << (property.animatable ? "yes" : "no") << " |\n";
            output << '\n';
        }
        if (!control.signals.empty()) {
            output << "Signals: ";
            for (std::size_t index = 0; index < control.signals.size(); ++index) {
                if (index != 0) output << ", ";
                output << '`' << MarkdownCell(control.signals[index].id) << '`';
            }
            output << ".\n\n";
        }
    }
    return output.str();
}

bool WriteIfDifferent(const std::filesystem::path& path,
                      const std::string_view contents) {
    if (const auto existing = ReadBounded(path); existing && *existing == contents)
        return true;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(stream);
}

}  // namespace

int main(int argc, char** argv) {
    const bool check = argc == 5 && std::string_view(argv[1]) == "--check";
    if ((!check && argc != 4) || (check && argc != 5)) {
        std::cerr << "usage: PrismatiXSDKReference [--check] "
                     "<contract-manifest.json> <ui-registry.json> <output.md>\n";
        return 2;
    }
    const int offset = check ? 1 : 0;
    const auto generated = Generate(argv[1 + offset], argv[2 + offset]);
    if (!generated) return 1;
    const std::filesystem::path output(argv[3 + offset]);
    if (check) {
        const auto existing = ReadBounded(output);
        if (!existing || *existing != *generated) {
            std::cerr << "generated SDK reference is stale\n";
            return 1;
        }
        return 0;
    }
    if (!WriteIfDifferent(output, *generated)) {
        std::cerr << "could not write SDK reference\n";
        return 1;
    }
    return 0;
}
