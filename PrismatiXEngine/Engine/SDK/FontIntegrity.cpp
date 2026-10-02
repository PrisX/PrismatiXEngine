#include "Engine/SDK/FontIntegrity.h"

#include <set>

namespace px::sdk {
namespace {

std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes, const std::uint64_t offset) {
    return (std::uint32_t{bytes[offset]} << 24) | (std::uint32_t{bytes[offset + 1]} << 16) |
           (std::uint32_t{bytes[offset + 2]} << 8) | std::uint32_t{bytes[offset + 3]};
}

std::uint16_t ReadU16(const std::span<const std::uint8_t> bytes, const std::uint64_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

std::string TagName(const std::uint32_t tag) {
    std::string name;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto value = static_cast<char>((tag >> shift) & 0xFFu);
        name.push_back(value >= 0x20 && value < 0x7F ? value : '?');
    }
    return name;
}

constexpr std::uint32_t Tag(const char (&name)[5]) {
    return (std::uint32_t(static_cast<std::uint8_t>(name[0])) << 24) |
           (std::uint32_t(static_cast<std::uint8_t>(name[1])) << 16) |
           (std::uint32_t(static_cast<std::uint8_t>(name[2])) << 8) |
           std::uint32_t(static_cast<std::uint8_t>(name[3]));
}

std::string CheckFace(const std::span<const std::uint8_t> bytes, const std::uint64_t face,
                      std::set<std::uint64_t>& checkedTables) {
    if (face + 12 > bytes.size()) return "the table directory is cut off";
    const std::uint16_t tableCount = ReadU16(bytes, face + 4);
    if (face + 12 + std::uint64_t{tableCount} * 16 > bytes.size())
        return "the table directory is cut off";
    for (std::uint16_t index = 0; index < tableCount; ++index) {
        const std::uint64_t record = face + 12 + std::uint64_t{index} * 16;
        const std::uint32_t tag = ReadU32(bytes, record);
        const std::uint32_t expected = ReadU32(bytes, record + 4);
        const std::uint64_t offset = ReadU32(bytes, record + 8);
        const std::uint64_t length = ReadU32(bytes, record + 12);
        if (offset + length > bytes.size())
            return "table '" + TagName(tag) + "' extends past the end of the file";
        if (!checkedTables.insert(offset).second) continue;
        // The sum runs over whole big-endian words; the zero padding after a
        // table that ends mid-word counts as zeros, including at end of file.
        std::uint32_t sum = 0;
        for (std::uint64_t at = 0; at < length; at += 4) {
            std::uint32_t word = 0;
            for (std::uint64_t byte = 0; byte < 4; ++byte)
                word = (word << 8) | (at + byte < length ? bytes[offset + at + byte] : 0u);
            // head.checkSumAdjustment is excluded from the head table's own sum.
            if (tag == Tag("head") && at == 8) word = 0;
            sum += word;
        }
        if (sum != expected)
            return "table '" + TagName(tag) + "' does not match its checksum";
    }
    return {};
}

}  // namespace

std::string FindFontFileDamage(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4) return {};
    const std::uint32_t signature = ReadU32(bytes, 0);
    std::set<std::uint64_t> checkedTables;
    if (signature == 0x00010000u || signature == Tag("OTTO") || signature == Tag("true"))
        return CheckFace(bytes, 0, checkedTables);
    if (signature != Tag("ttcf")) return {};
    if (bytes.size() < 12) return "the collection header is cut off";
    const std::uint64_t faceCount = ReadU32(bytes, 8);
    if (12 + faceCount * 4 > bytes.size()) return "the collection header is cut off";
    for (std::uint64_t index = 0; index < faceCount; ++index) {
        auto damage = CheckFace(bytes, ReadU32(bytes, 12 + index * 4), checkedTables);
        if (!damage.empty()) return damage;
    }
    return {};
}

}  // namespace px::sdk
