#pragma once

#include "Engine/Core/Result.h"

#include <string>
#include <string_view>
#include <vector>

namespace px::editor {

enum class ExportConfiguration : std::uint8_t { Debug, Release };
struct ExportContentGroup { std::string id; std::vector<std::string> roots; bool optional=false; };
struct ExportProfile {
    static constexpr int CurrentVersion=4;
    std::string id="windows-release";
    std::string platform="windows-x64";
    ExportConfiguration configuration=ExportConfiguration::Release;
    std::string productVersion="0.1.0";
    std::string icon;
    bool compression=true;
    bool encryption=true;
    bool reproducible=true;
    // Must be explicitly enabled after LGPL/dynamic-linking, codec patent,
    // and target-store policy review when shipping MP4/WebM content.
    bool ffmpegLicenseReviewAccepted=false;
    std::vector<std::string> excludePatterns;
    std::vector<ExportContentGroup> contentGroups{{"base",{"Content"},false}};
};

[[nodiscard]] Result<ExportProfile> ParseExportProfile(std::string_view text,const std::string& path={});
[[nodiscard]] std::string WriteExportProfile(const ExportProfile& profile);

}  // namespace px::editor
