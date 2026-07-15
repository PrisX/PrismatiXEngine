#include "Engine/IO/Crypto.h"

#include <algorithm>
#include <psa/crypto.h>

namespace px::crypto {

namespace {
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagSize = 16;

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
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);

    const psa_status_t status = psa_import_key(&attributes, key.data(), key.size(), &keyId);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS;
}

Bytes EncryptAuthenticated(const Bytes& input,const Key& key,const Iv& iv){
    Bytes seed(iv.begin(),iv.end());seed.insert(seed.end(),input.begin(),input.end());std::array<std::uint8_t,32> digest{};if(!ComputeSha256(seed.data(),seed.size(),digest.data(),digest.size()))return {};
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    if(!ImportAesKey(key,PSA_KEY_USAGE_ENCRYPT,keyId))return {};
    Bytes output(kNonceSize+input.size()+kTagSize);std::copy_n(digest.begin(),kNonceSize,output.begin());std::size_t written=0;const psa_status_t status=psa_aead_encrypt(keyId,PSA_ALG_GCM,output.data(),kNonceSize,nullptr,0,input.data(),input.size(),output.data()+kNonceSize,output.size()-kNonceSize,&written);psa_destroy_key(keyId);if(status!=PSA_SUCCESS)return {};output.resize(kNonceSize+written);return output;
}
Bytes DecryptAuthenticated(const Bytes& input,const Key& key,const Iv& iv){
    (void)iv;if(input.size()<kNonceSize+kTagSize)return {};mbedtls_svc_key_id_t keyId=MBEDTLS_SVC_KEY_ID_INIT;if(!ImportAesKey(key,PSA_KEY_USAGE_DECRYPT,keyId))return {};Bytes output(input.size()-kNonceSize-kTagSize);std::size_t written=0;const psa_status_t status=psa_aead_decrypt(keyId,PSA_ALG_GCM,input.data(),kNonceSize,nullptr,0,input.data()+kNonceSize,input.size()-kNonceSize,output.data(),output.size(),&written);psa_destroy_key(keyId);if(status!=PSA_SUCCESS)return {};output.resize(written);return output;
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
    for (std::size_t i = 0; i < iv.size(); ++i) {
        iv[i] = static_cast<std::uint8_t>(digest[i] ^ digest[i + iv.size()]);
    }
    return iv;
}

Bytes Encrypt(const Bytes& plain, const Key& key, const Iv& iv) {
    return EncryptAuthenticated(plain,key,iv);
}

Bytes Decrypt(const Bytes& cipher, const Key& key, const Iv& iv) {
    return DecryptAuthenticated(cipher,key,iv);
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
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

}
