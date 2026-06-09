#include "Engine/IO/Crypto.h"

#include <psa/crypto.h>

namespace px::crypto {

namespace {
constexpr std::size_t kBlock = 16;

bool EnsureCryptoInitialized() {
    static const bool initialized = psa_crypto_init() == PSA_SUCCESS;
    return initialized;
}

bool ComputeSha256(const std::uint8_t* data, std::size_t size, std::uint8_t* out,
                   std::size_t outSize) {
    if (!EnsureCryptoInitialized()) {
        return false;
    }

    std::size_t hashSize = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data, size, out, outSize, &hashSize) ==
               PSA_SUCCESS &&
           hashSize == 32;
}

bool ImportAesKey(const Key& key, psa_key_usage_t usage, mbedtls_svc_key_id_t& keyId) {
    if (!EnsureCryptoInitialized()) {
        return false;
    }

    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key.size() * 8);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);

    const psa_status_t status = psa_import_key(&attributes, key.data(), key.size(), &keyId);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS;
}

Bytes CryptAesCbcNoPadding(const Bytes& input, const Key& key, const Iv& iv,
                           psa_key_usage_t usage) {
    if (input.empty() || (input.size() % kBlock) != 0) {
        return {};
    }

    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    if (!ImportAesKey(key, usage, keyId)) {
        return {};
    }

    psa_cipher_operation_t operation = psa_cipher_operation_init();
    psa_status_t status = usage == PSA_KEY_USAGE_ENCRYPT
                              ? psa_cipher_encrypt_setup(&operation, keyId,
                                                         PSA_ALG_CBC_NO_PADDING)
                              : psa_cipher_decrypt_setup(&operation, keyId,
                                                         PSA_ALG_CBC_NO_PADDING);

    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, iv.data(), iv.size());
    }

    Bytes out(input.size() + kBlock, 0);
    std::size_t updateSize = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, input.data(), input.size(), out.data(),
                                   out.size(), &updateSize);
    }

    std::size_t finishSize = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, out.data() + updateSize,
                                   out.size() - updateSize, &finishSize);
    } else {
        psa_cipher_abort(&operation);
    }

    psa_destroy_key(keyId);

    if (status != PSA_SUCCESS) {
        return {};
    }

    out.resize(updateSize + finishSize);
    return out;
}
}

Key DeriveKey(std::string_view passphrase) {
    Key key{};
    ComputeSha256(reinterpret_cast<const std::uint8_t*>(passphrase.data()), passphrase.size(),
                  key.data(), key.size());
    return key;
}

Iv DeriveIv(std::string_view salt) {
    std::array<std::uint8_t, 32> digest{};
    ComputeSha256(reinterpret_cast<const std::uint8_t*>(salt.data()), salt.size(), digest.data(),
                  digest.size());
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

    return CryptAesCbcNoPadding(padded, key, iv, PSA_KEY_USAGE_ENCRYPT);
}

Bytes Decrypt(const Bytes& cipher, const Key& key, const Iv& iv) {
    if (cipher.empty() || (cipher.size() % kBlock) != 0) {
        return {};
    }
    Bytes out = CryptAesCbcNoPadding(cipher, key, iv, PSA_KEY_USAGE_DECRYPT);
    if (out.empty()) {
        return {};
    }

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
