#include "Engine/IO/Crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

namespace px::crypto {

namespace {
constexpr std::size_t kBlock = 16;
}

Key DeriveKey(std::string_view passphrase) {
    Key key{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(passphrase.data()), passphrase.size(),
                   key.data(), /*is224=*/0);
    return key;
}

Iv DeriveIv(std::string_view salt) {
    std::array<std::uint8_t, 32> digest{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(salt.data()), salt.size(), digest.data(),
                   0);
    Iv iv{};
    for (std::size_t i = 0; i < kBlock; ++i) {
        iv[i] = static_cast<std::uint8_t>(digest[i] ^ digest[i + kBlock]);
    }
    return iv;
}

Bytes Encrypt(const Bytes& plain, const Key& key, const Iv& iv) {
    const std::size_t pad = kBlock - (plain.size() % kBlock);
    Bytes padded = plain;
    padded.insert(padded.end(), pad, static_cast<std::uint8_t>(pad));

    Bytes out(padded.size(), 0);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key.data(), 256);
    Iv ivCopy = iv;
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, padded.size(), ivCopy.data(), padded.data(),
                          out.data());
    mbedtls_aes_free(&ctx);
    return out;
}

Bytes Decrypt(const Bytes& cipher, const Key& key, const Iv& iv) {
    if (cipher.empty() || (cipher.size() % kBlock) != 0) {
        return {};
    }
    Bytes out(cipher.size(), 0);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, key.data(), 256);
    Iv ivCopy = iv;
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipher.size(), ivCopy.data(), cipher.data(),
                          out.data());
    mbedtls_aes_free(&ctx);

    const std::uint8_t pad = out.back();
    if (pad == 0 || pad > kBlock || pad > out.size()) {
        return {};
    }
    for (std::size_t i = out.size() - pad; i < out.size(); ++i) {
        if (out[i] != pad) {
            return {};
        }
    }
    out.resize(out.size() - pad);
    return out;
}

std::uint64_t HashPath(std::string_view path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (char c : path) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

}
