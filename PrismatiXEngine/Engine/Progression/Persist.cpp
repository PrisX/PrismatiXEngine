#include "Engine/Progression/Persist.h"

#include "Engine/Support/Logger.h"

#include <array>
#include <filesystem>
#include <fstream>

namespace px::progress {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::filesystem::path AsPath(const std::string& path) {
    return std::filesystem::path(path);
}
}

std::string Base64Encode(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (i < data.size()) {
        std::uint32_t n = data[i] << 16;
        const bool two = (i + 1 < data.size());
        if (two) n |= data[i + 1] << 8;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += two ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::vector<std::uint8_t> Base64Decode(const std::string& text) {
    std::array<int, 256> rev;
    rev.fill(-1);
    for (int i = 0; i < 64; ++i) {
        rev[static_cast<unsigned char>(kAlphabet[i])] = i;
    }
    std::vector<std::uint8_t> out;
    int val = 0, bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int d = rev[static_cast<unsigned char>(c)];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((val >> bits) & 0xFF));
        }
    }
    return out;
}

bool SaveJson(const std::string& path, const Json& json, const crypto::Key* key) {
    std::error_code ec;
    std::filesystem::create_directories(AsPath(path).parent_path(), ec);

    const std::string text = json.dump();
    crypto::Bytes bytes(text.begin(), text.end());
    if (key) {
        bytes = crypto::Encrypt(bytes, *key, crypto::DeriveIv(AsPath(path).filename().string()));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        PX_LOG_ERROR("Persist: cannot write '{}'", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::optional<Json> LoadJson(const std::string& path, const crypto::Key* key) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    crypto::Bytes bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    if (key) {
        bytes = crypto::Decrypt(bytes, *key, crypto::DeriveIv(AsPath(path).filename().string()));
        if (bytes.empty()) {
            PX_LOG_ERROR("Persist: decrypt failed for '{}'", path);
            return std::nullopt;
        }
    }
    Json json = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        PX_LOG_ERROR("Persist: corrupt JSON in '{}'", path);
        return std::nullopt;
    }
    return json;
}

}
