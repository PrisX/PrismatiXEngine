#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace px::io {

struct OverlaySigningKeyPair {
    std::string privateKeyHex;
    std::string publicKeyHex;
};

// Keys use lowercase hex. The private key is a 32-byte P-256 scalar; the
// public key is a 65-byte uncompressed SEC1 point. Keep the private key out of
// package manifests, command-line arguments, and diagnostic output.
[[nodiscard]] std::optional<OverlaySigningKeyPair>
GenerateOverlaySigningKeyPair();

// Signs exact revision-2 or revision-3 overlay metadata with a domain-separated SHA-256
// digest. Refuses a private key that does not derive the pinned public key.
[[nodiscard]] std::optional<std::string> SignOverlayMetadata(
    std::string_view metadata, std::string_view privateKeyHex,
    std::string_view pinnedPublicKeyHex, int schemaRevision = 2);

[[nodiscard]] bool VerifyOverlayMetadataSignature(
    std::string_view metadata, std::string_view signatureHex,
    std::string_view pinnedPublicKeyHex, int schemaRevision = 2);

}  // namespace px::io
