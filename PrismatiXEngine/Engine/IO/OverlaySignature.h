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

// Signs exact overlay metadata with a domain-separated SHA-256
// digest. Refuses a private key that does not derive the pinned public key.
[[nodiscard]] std::optional<std::string> SignOverlayMetadata(
    std::string_view metadata, std::string_view privateKeyHex,
    std::string_view pinnedPublicKeyHex);

[[nodiscard]] bool VerifyOverlayMetadataSignature(
    std::string_view metadata, std::string_view signatureHex,
    std::string_view pinnedPublicKeyHex);

// Plain ECDSA P-256 over SHA-256(message), raw r||s in hex: the form Web
// Crypto and Node's `dsaEncoding: "ieee-p1363"` use. Native extension
// libraries are signed this way (their message carries its own domain line).
[[nodiscard]] bool VerifyEcdsaP256Signature(std::string_view message,
                                            std::string_view signatureHex,
                                            std::string_view publicKeyHex);
// Test and tooling helper: signs `message` with a 32-byte hex private key.
[[nodiscard]] std::optional<std::string> SignEcdsaP256(
    std::string_view message, std::string_view privateKeyHex);

}  // namespace px::io
