#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace px::io {

struct PackageContentVerification {
    bool valid = false;
    std::string error;
    std::uint64_t resources = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t archives = 0;
};

using PackageVerificationProgress =
    std::function<void(std::uint64_t current, std::uint64_t total,
                       std::uint64_t rawBytes)>;

// Checks the packaged resource inventory against every mounted PDX entry.
// This is an integrity check, not signature/authenticity verification.
[[nodiscard]] PackageContentVerification VerifyPackageContents(
    const std::filesystem::path& distribution,
    const PackageVerificationProgress& onProgress = {});

}  // namespace px::io
