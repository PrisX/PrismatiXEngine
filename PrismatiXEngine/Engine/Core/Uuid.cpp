#include "Engine/Core/Uuid.h"

#include <charconv>
#include <random>

namespace px {

namespace {
int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

Uuid Uuid::Random() {
    thread_local std::mt19937_64 random(std::random_device{}());
    Bytes bytes;
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        const std::uint64_t word = random();
        for (std::size_t b = 0; b < 8; ++b) {
            bytes[i + b] = static_cast<std::uint8_t>((word >> (b * 8)) & 0xff);
        }
    }
    // RFC 4122 version 4, variant 1.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return Uuid(bytes);
}

Uuid Uuid::FromName(std::string_view name) {
    std::uint64_t a = 1469598103934665603ull;
    std::uint64_t b = 1099511628211ull;
    for (unsigned char c : name) {
        a = (a ^ c) * 1099511628211ull;
        b ^= static_cast<std::uint64_t>(c) + 0x9e3779b97f4a7c15ull + (b << 6) + (b >> 2);
    }
    Bytes bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>((a >> (i * 8)) & 0xff);
        bytes[i + 8] = static_cast<std::uint8_t>((b >> (i * 8)) & 0xff);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return Uuid(bytes);
}

std::optional<Uuid> Uuid::Parse(std::string_view text) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
        text[23] != '-') {
        return std::nullopt;
    }
    Bytes bytes{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        if (i + 1 >= text.size() || out >= bytes.size()) return std::nullopt;
        const int hi = Hex(text[i]);
        const int lo = Hex(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        bytes[out++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return out == bytes.size() ? std::optional<Uuid>(Uuid(bytes)) : std::nullopt;
}

std::string Uuid::ToString() const {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < m_bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHex[m_bytes[i] >> 4]);
        out.push_back(kHex[m_bytes[i] & 0x0f]);
    }
    return out;
}

bool Uuid::Empty() const {
    for (std::uint8_t b : m_bytes)
        if (b != 0) return false;
    return true;
}

std::size_t UuidHash::operator()(const Uuid& value) const noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint8_t b : value.Data()) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    if constexpr (sizeof(std::size_t) < sizeof(hash)) {
        hash ^= hash >> 32;
    }
    return static_cast<std::size_t>(hash);
}

}  // namespace px
