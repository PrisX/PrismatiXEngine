#include "Engine/IO/OverlaySignature.h"

#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace px::io {
namespace {

constexpr std::string_view kSignatureDomain =
    {"PrismatiXPackageOverlaySignature",
     sizeof("PrismatiXPackageOverlaySignature")};

int Nibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::optional<std::vector<std::uint8_t>> DecodeHex(
    const std::string_view hex, const std::size_t bytes) {
    if (hex.size() != bytes * 2) return std::nullopt;
    std::vector<std::uint8_t> decoded(bytes);
    for (std::size_t index = 0; index < bytes; ++index) {
        const auto high = Nibble(hex[index * 2]);
        const auto low = Nibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return decoded;
}

std::string Hex(const std::uint8_t* data, const std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(digits[data[index] >> 4]);
        result.push_back(digits[data[index] & 15]);
    }
    return result;
}

std::optional<std::array<std::uint8_t, 32>> MetadataDigest(
    const std::string_view metadata,
    const std::string_view domain = kSignatureDomain) {
    if (psa_crypto_init() != PSA_SUCCESS) return std::nullopt;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS)
        return std::nullopt;
    if (psa_hash_update(&operation,
                        reinterpret_cast<const std::uint8_t*>(
                            domain.data()),
                        domain.size()) != PSA_SUCCESS ||
        psa_hash_update(&operation,
                        reinterpret_cast<const std::uint8_t*>(metadata.data()),
                        metadata.size()) != PSA_SUCCESS) {
        psa_hash_abort(&operation);
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> digest{};
    std::size_t written = 0;
    if (psa_hash_finish(&operation, digest.data(), digest.size(), &written) !=
            PSA_SUCCESS || written != digest.size()) {
        psa_hash_abort(&operation);
        return std::nullopt;
    }
    return digest;
}

psa_key_attributes_t KeyAttributes(const bool privateKey) {
    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes,
                     privateKey
                         ? PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
                         : PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes,
                            privateKey
                                ? PSA_KEY_USAGE_SIGN_HASH |
                                      PSA_KEY_USAGE_EXPORT
                                : PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    return attributes;
}

}  // namespace

std::optional<OverlaySigningKeyPair> GenerateOverlaySigningKeyPair() {
    if (psa_crypto_init() != PSA_SUCCESS) return std::nullopt;
    auto attributes = KeyAttributes(true);
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    const auto generated = psa_generate_key(&attributes, &keyId);
    psa_reset_key_attributes(&attributes);
    if (generated != PSA_SUCCESS) return std::nullopt;
    std::array<std::uint8_t, 32> privateKey{};
    std::array<std::uint8_t, 65> publicKey{};
    std::size_t privateLength = 0;
    std::size_t publicLength = 0;
    const auto privateStatus = psa_export_key(
        keyId, privateKey.data(), privateKey.size(), &privateLength);
    const auto publicStatus = psa_export_public_key(
        keyId, publicKey.data(), publicKey.size(), &publicLength);
    psa_destroy_key(keyId);
    if (privateStatus != PSA_SUCCESS || publicStatus != PSA_SUCCESS ||
        privateLength != privateKey.size() ||
        publicLength != publicKey.size() || publicKey.front() != 0x04)
        return std::nullopt;
    OverlaySigningKeyPair result{
        Hex(privateKey.data(), privateKey.size()),
        Hex(publicKey.data(), publicKey.size())};
    privateKey.fill(0);
    return result;
}

std::optional<std::string> SignOverlayMetadata(
    const std::string_view metadata, const std::string_view privateKeyHex,
    const std::string_view pinnedPublicKeyHex) {
    auto privateKey = DecodeHex(privateKeyHex, 32);
    const auto digest = MetadataDigest(metadata);
    if (!privateKey || !digest) return std::nullopt;
    auto attributes = KeyAttributes(true);
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    const auto imported = psa_import_key(&attributes, privateKey->data(),
                                         privateKey->size(), &keyId);
    std::fill(privateKey->begin(), privateKey->end(), 0);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return std::nullopt;
    std::array<std::uint8_t, 65> publicKey{};
    std::size_t publicLength = 0;
    const auto exported = psa_export_public_key(
        keyId, publicKey.data(), publicKey.size(), &publicLength);
    if (exported != PSA_SUCCESS || publicLength != publicKey.size() ||
        Hex(publicKey.data(), publicKey.size()) != pinnedPublicKeyHex) {
        psa_destroy_key(keyId);
        return std::nullopt;
    }
    std::array<std::uint8_t, 64> signature{};
    std::size_t length = 0;
    const auto signedStatus = psa_sign_hash(
        keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest->data(),
        digest->size(), signature.data(), signature.size(), &length);
    psa_destroy_key(keyId);
    if (signedStatus != PSA_SUCCESS || length != signature.size())
        return std::nullopt;
    return Hex(signature.data(), signature.size());
}

namespace {

bool VerifyDigestSignature(
    const std::optional<std::array<std::uint8_t, 32>>& digest,
    const std::string_view signatureHex,
    const std::string_view pinnedPublicKeyHex) {
    const auto signature = DecodeHex(signatureHex, 64);
    const auto publicKey = DecodeHex(pinnedPublicKeyHex, 65);
    if (!signature || !publicKey || !digest || publicKey->front() != 0x04)
        return false;
    auto attributes = KeyAttributes(false);
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    const auto imported = psa_import_key(&attributes, publicKey->data(),
                                         publicKey->size(), &keyId);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return false;
    const auto verified = psa_verify_hash(
        keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest->data(), digest->size(),
        signature->data(), signature->size());
    psa_destroy_key(keyId);
    return verified == PSA_SUCCESS;
}

}  // namespace

bool VerifyOverlayMetadataSignature(
    const std::string_view metadata, const std::string_view signatureHex,
    const std::string_view pinnedPublicKeyHex) {
    return VerifyDigestSignature(MetadataDigest(metadata), signatureHex,
                                 pinnedPublicKeyHex);
}

bool VerifyEcdsaP256Signature(const std::string_view message,
                              const std::string_view signatureHex,
                              const std::string_view publicKeyHex) {
    return VerifyDigestSignature(MetadataDigest(message, {}), signatureHex,
                                 publicKeyHex);
}

std::optional<std::string> SignEcdsaP256(const std::string_view message,
                                         const std::string_view privateKeyHex) {
    auto privateKey = DecodeHex(privateKeyHex, 32);
    const auto digest = MetadataDigest(message, {});
    if (!privateKey || !digest) return std::nullopt;
    auto attributes = KeyAttributes(true);
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    const auto imported = psa_import_key(&attributes, privateKey->data(),
                                         privateKey->size(), &keyId);
    std::fill(privateKey->begin(), privateKey->end(), 0);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return std::nullopt;
    std::array<std::uint8_t, 64> signature{};
    std::size_t length = 0;
    const auto signedStatus = psa_sign_hash(
        keyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest->data(), digest->size(),
        signature.data(), signature.size(), &length);
    psa_destroy_key(keyId);
    if (signedStatus != PSA_SUCCESS || length != signature.size())
        return std::nullopt;
    return Hex(signature.data(), signature.size());
}

}  // namespace px::io
