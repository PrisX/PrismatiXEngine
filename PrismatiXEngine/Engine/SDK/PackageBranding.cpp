#include "Engine/SDK/PackageBranding.h"

#include "Engine/Support/Utf8Path.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace px::sdk::branding {
namespace {

constexpr int kMaxIconSide = 4096;

void PutU16(Bytes& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}
void PutU32(Bytes& out, const std::uint32_t value) {
    PutU16(out, value & 0xffff);
    PutU16(out, value >> 16);
}
void PutU32BE(Bytes& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}
void SetU16(Bytes& out, const std::size_t at, const std::uint32_t value) {
    out[at] = static_cast<std::uint8_t>(value & 0xff);
    out[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}
std::uint16_t U16(const Bytes& in, const std::size_t at) {
    return static_cast<std::uint16_t>(in[at] | (in[at + 1] << 8));
}
std::uint32_t U32(const Bytes& in, const std::size_t at) {
    return static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1]) << 8) |
           (static_cast<std::uint32_t>(in[at + 2]) << 16) |
           (static_cast<std::uint32_t>(in[at + 3]) << 24);
}
std::uint32_t U32BE(const Bytes& in, const std::size_t at) {
    return (static_cast<std::uint32_t>(in[at]) << 24) | (static_cast<std::uint32_t>(in[at + 1]) << 16) |
           (static_cast<std::uint32_t>(in[at + 2]) << 8) | static_cast<std::uint32_t>(in[at + 3]);
}

// UTF-8 to UTF-16; invalid sequences become U+FFFD.
std::u16string Utf16(const std::string_view text) {
    std::u16string result;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t code = 0xfffd;
        std::size_t length = 1;
        if (lead < 0x80) code = lead;
        else if ((lead >> 5) == 0x6) { code = lead & 0x1f; length = 2; }
        else if ((lead >> 4) == 0xe) { code = lead & 0x0f; length = 3; }
        else if ((lead >> 3) == 0x1e) { code = lead & 0x07; length = 4; }
        if (length > 1) {
            if (index + length > text.size()) { code = 0xfffd; length = 1; }
            else {
                for (std::size_t next = 1; next < length; ++next) {
                    const auto byte = static_cast<unsigned char>(text[index + next]);
                    if ((byte >> 6) != 0x2) { code = 0xfffd; length = 1; break; }
                    code = (code << 6) | (byte & 0x3f);
                }
            }
        }
        index += length;
        if (code >= 0x10000 && code <= 0x10ffff) {
            code -= 0x10000;
            result.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
            result.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
        } else {
            result.push_back(static_cast<char16_t>(code > 0xffff ? 0xfffd : code));
        }
    }
    return result;
}

std::string Utf8(const std::u16string_view text) {
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t code = text[index];
        if (code >= 0xd800 && code <= 0xdbff && index + 1 < text.size() &&
            text[index + 1] >= 0xdc00 && text[index + 1] <= 0xdfff) {
            code = 0x10000 + ((code - 0xd800) << 10) + (text[index + 1] - 0xdc00);
            ++index;
        }
        if (code < 0x80) result.push_back(static_cast<char>(code));
        else if (code < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    return result;
}

std::string XmlEscape(const std::string_view text) {
    std::string result;
    for (const char c : text) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:
                // XML 1.0 forbids most C0 controls even as references.
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r')
                    result.push_back(c);
        }
    }
    return result;
}

// One resampling axis: for every destination index, source indices and
// weights summing to 1. Area average when shrinking, bilinear otherwise.
struct Tap { int index; float weight; };
std::vector<std::vector<Tap>> AxisTaps(const int source, const int target) {
    std::vector<std::vector<Tap>> taps(static_cast<std::size_t>(target));
    const double scale = static_cast<double>(source) / target;
    for (int index = 0; index < target; ++index) {
        auto& list = taps[static_cast<std::size_t>(index)];
        if (source > target) {
            const double begin = index * scale, end = (index + 1) * scale;
            for (int sample = static_cast<int>(std::floor(begin));
                 sample < static_cast<int>(std::ceil(end)) && sample < source; ++sample) {
                const double overlap = std::min(end, sample + 1.0) - std::max(begin, static_cast<double>(sample));
                if (overlap > 0) list.push_back({sample, static_cast<float>(overlap / scale)});
            }
        } else {
            const double center = std::clamp((index + 0.5) * scale - 0.5, 0.0, source - 1.0);
            const int low = static_cast<int>(std::floor(center));
            const int high = std::min(low + 1, source - 1);
            const auto fraction = static_cast<float>(center - low);
            list.push_back({low, 1.0f - fraction});
            if (high != low && fraction > 0) list.push_back({high, fraction});
        }
    }
    return taps;
}

RgbaImage SquareCanvas(const RgbaImage& image) {
    if (image.width == image.height) return image;
    const int side = std::max(image.width, image.height);
    RgbaImage square{side, side, std::vector<std::uint8_t>(static_cast<std::size_t>(side) * side * 4, 0)};
    const int left = (side - image.width) / 2, top = (side - image.height) / 2;
    for (int y = 0; y < image.height; ++y)
        std::memcpy(&square.pixels[(static_cast<std::size_t>(top + y) * side + left) * 4],
                    &image.pixels[static_cast<std::size_t>(y) * image.width * 4],
                    static_cast<std::size_t>(image.width) * 4);
    return square;
}

Bytes DibFrame(const RgbaImage& image) {
    const int size = image.width;
    Bytes out;
    PutU32(out, 40);
    PutU32(out, static_cast<std::uint32_t>(size));
    PutU32(out, static_cast<std::uint32_t>(size * 2));
    PutU16(out, 1);
    PutU16(out, 32);
    PutU32(out, 0);
    const std::uint32_t maskStride = static_cast<std::uint32_t>(((size + 31) / 32) * 4);
    PutU32(out, static_cast<std::uint32_t>(size * size * 4) + maskStride * size);
    for (int index = 0; index < 4; ++index) PutU32(out, 0);
    for (int y = size - 1; y >= 0; --y) {
        for (int x = 0; x < size; ++x) {
            const auto* pixel = &image.pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            out.push_back(pixel[2]);
            out.push_back(pixel[1]);
            out.push_back(pixel[0]);
            out.push_back(pixel[3]);
        }
    }
    for (int y = size - 1; y >= 0; --y) {
        Bytes row(maskStride, 0);
        for (int x = 0; x < size; ++x)
            if (image.pixels[(static_cast<std::size_t>(y) * size + x) * 4 + 3] == 0)
                row[static_cast<std::size_t>(x / 8)] |= static_cast<std::uint8_t>(0x80 >> (x % 8));
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

void Align4(Bytes& out) {
    while (out.size() % 4 != 0) out.push_back(0);
}

void PutWide(Bytes& out, const std::u16string_view text) {
    for (const char16_t c : text) PutU16(out, c);
    PutU16(out, 0);
}

// One VS_VERSIONINFO-style block: wLength, wValueLength, wType, szKey,
// padding, value, padding, children.
void VersionBlock(Bytes& out, const std::u16string_view key, const std::uint16_t type,
                  const Bytes& value, const std::uint16_t valueLength,
                  const std::function<void(Bytes&)>& children = {}) {
    Align4(out);
    const std::size_t start = out.size();
    PutU16(out, 0);
    PutU16(out, valueLength);
    PutU16(out, type);
    PutWide(out, key);
    Align4(out);
    out.insert(out.end(), value.begin(), value.end());
    if (children) children(out);
    SetU16(out, start, static_cast<std::uint32_t>(out.size() - start));
}

std::vector<std::string> NumericParts(std::string_view version) {
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) version.remove_prefix(1);
    std::vector<std::string> parts;
    std::string current;
    for (const char c : version) {
        if (c >= '0' && c <= '9') { current.push_back(c); continue; }
        if (c == '.' && !current.empty() && parts.size() < 3) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        break;
    }
    if (!current.empty() && parts.size() < 4) parts.push_back(current);
    return parts;
}

std::uint16_t ClampPart(const std::string& part) {
    if (part.size() > 5) return 65535;
    return static_cast<std::uint16_t>(std::min<unsigned long>(std::stoul(part), 65535ul));
}

bool PortableAscii(const std::string_view text, const std::string_view extra) {
    return std::ranges::all_of(text, [&](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               extra.find(c) != std::string_view::npos;
    });
}

// Desktop Entry string escaping.
std::string DesktopEscape(const std::string_view text) {
    std::string result;
    for (const char c : text) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            case '\r': result += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) result.push_back(c);
        }
    }
    if (!result.empty() && result.front() == ' ') result = "\\s" + result.substr(1);
    return result;
}

}  // namespace

std::optional<RgbaImage> DecodePng(const Bytes& png, std::string& error) {
    if (png.size() < 8 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0) {
        error = "not a PNG file";
        return std::nullopt;
    }
    SDL_IOStream* stream = SDL_IOFromConstMem(png.data(), png.size());
    SDL_Surface* decoded = stream ? SDL_LoadPNG_IO(stream, true) : nullptr;
    if (!decoded) {
        error = std::string("PNG could not be decoded: ") + SDL_GetError();
        return std::nullopt;
    }
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> owner(decoded, SDL_DestroySurface);
    if (decoded->w <= 0 || decoded->h <= 0 || decoded->w > kMaxIconSide || decoded->h > kMaxIconSide) {
        error = "PNG must be between 1 and 4096 pixels on each side";
        return std::nullopt;
    }
    SDL_Surface* converted = SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_RGBA32);
    if (!converted) {
        error = std::string("PNG could not be converted to RGBA: ") + SDL_GetError();
        return std::nullopt;
    }
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> rgba(converted, SDL_DestroySurface);
    RgbaImage image{converted->w, converted->h,
                    std::vector<std::uint8_t>(static_cast<std::size_t>(converted->w) * converted->h * 4)};
    for (int y = 0; y < converted->h; ++y)
        std::memcpy(&image.pixels[static_cast<std::size_t>(y) * converted->w * 4],
                    static_cast<const std::uint8_t*>(converted->pixels) +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(converted->pitch),
                    static_cast<std::size_t>(converted->w) * 4);
    return image;
}

Bytes EncodePng(const RgbaImage& image, std::string& error) {
    std::vector<std::uint8_t> pixels = image.pixels;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_RGBA32,
                                                 pixels.data(), image.width * 4);
    if (!surface) {
        error = std::string("icon surface could not be created: ") + SDL_GetError();
        return {};
    }
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> owner(surface, SDL_DestroySurface);
    SDL_IOStream* stream = SDL_IOFromDynamicMem();
    if (!stream || !SDL_SavePNG_IO(surface, stream, false)) {
        error = std::string("PNG could not be encoded: ") + SDL_GetError();
        if (stream) SDL_CloseIO(stream);
        return {};
    }
    const Sint64 size = SDL_TellIO(stream);
    const auto* data = static_cast<const std::uint8_t*>(SDL_GetPointerProperty(
        SDL_GetIOProperties(stream), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
    Bytes result;
    if (data && size > 0) result.assign(data, data + size);
    SDL_CloseIO(stream);
    if (result.empty()) error = "PNG encoder produced no data";
    return result;
}

std::optional<RgbaImage> DecodeIcoLargest(const Bytes& ico, std::string& error) {
    if (ico.size() < 6 || U16(ico, 0) != 0 || U16(ico, 2) != 1 || U16(ico, 4) == 0 ||
        ico.size() < 6 + 16 * static_cast<std::size_t>(U16(ico, 4))) {
        error = "icon is not a Windows .ico file";
        return std::nullopt;
    }
    std::size_t best = 0;
    int bestSide = -1;
    for (std::size_t index = 0; index < U16(ico, 4); ++index) {
        const std::size_t at = 6 + 16 * index;
        const int side = ico[at] == 0 ? 256 : ico[at];
        if (side > bestSide) { bestSide = side; best = at; }
    }
    const std::size_t size = U32(ico, best + 8), offset = U32(ico, best + 12);
    if (size == 0 || offset > ico.size() || size > ico.size() - offset) {
        error = "icon image entry lies outside the file";
        return std::nullopt;
    }
    const Bytes frame(ico.begin() + static_cast<std::ptrdiff_t>(offset),
                      ico.begin() + static_cast<std::ptrdiff_t>(offset + size));
    if (frame.size() >= 8 && std::memcmp(frame.data(), "\x89PNG", 4) == 0) return DecodePng(frame, error);
    if (frame.size() < 40 || U32(frame, 0) < 40 || U16(frame, 14) != 32 || U32(frame, 16) != 0) {
        error = "the largest .ico frame is neither PNG nor a 32-bit bitmap; use a PNG icon";
        return std::nullopt;
    }
    const int width = static_cast<int>(U32(frame, 4));
    const int height = static_cast<int>(U32(frame, 8)) / 2;
    const std::size_t pixelsAt = U32(frame, 0);
    if (width <= 0 || height <= 0 || width > kMaxIconSide || height > kMaxIconSide ||
        frame.size() < pixelsAt + static_cast<std::size_t>(width) * height * 4) {
        error = "the largest .ico bitmap is truncated";
        return std::nullopt;
    }
    RgbaImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    bool anyAlpha = false;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto* source = &frame[pixelsAt + (static_cast<std::size_t>(height - 1 - y) * width + x) * 4];
            auto* target = &image.pixels[(static_cast<std::size_t>(y) * width + x) * 4];
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
            target[3] = source[3];
            anyAlpha = anyAlpha || source[3] != 0;
        }
    if (!anyAlpha)
        for (std::size_t index = 3; index < image.pixels.size(); index += 4) image.pixels[index] = 255;
    return image;
}

RgbaImage ResizeSquare(const RgbaImage& image, const int size) {
    const RgbaImage square = SquareCanvas(image);
    const int side = square.width;
    if (side == size) return square;
    // Premultiplied alpha, so transparent pixels do not bleed their colour.
    std::vector<float> source(static_cast<std::size_t>(side) * side * 4);
    for (std::size_t index = 0; index < source.size(); index += 4) {
        const float alpha = square.pixels[index + 3] / 255.0f;
        for (std::size_t channel = 0; channel < 3; ++channel)
            source[index + channel] = square.pixels[index + channel] / 255.0f * alpha;
        source[index + 3] = alpha;
    }
    const auto taps = AxisTaps(side, size);
    std::vector<float> horizontal(static_cast<std::size_t>(side) * size * 4, 0.0f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < size; ++x)
            for (const auto& tap : taps[static_cast<std::size_t>(x)])
                for (std::size_t channel = 0; channel < 4; ++channel)
                    horizontal[(static_cast<std::size_t>(y) * size + x) * 4 + channel] +=
                        tap.weight * source[(static_cast<std::size_t>(y) * side + tap.index) * 4 + channel];
    RgbaImage result{size, size, std::vector<std::uint8_t>(static_cast<std::size_t>(size) * size * 4)};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float pixel[4]{};
            for (const auto& tap : taps[static_cast<std::size_t>(y)])
                for (std::size_t channel = 0; channel < 4; ++channel)
                    pixel[channel] += tap.weight *
                        horizontal[(static_cast<std::size_t>(tap.index) * size + x) * 4 + channel];
            const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
            auto* target = &result.pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            for (std::size_t channel = 0; channel < 3; ++channel)
                target[channel] = static_cast<std::uint8_t>(std::lround(
                    std::clamp(alpha > 0 ? pixel[channel] / alpha : 0.0f, 0.0f, 1.0f) * 255.0f));
            target[3] = static_cast<std::uint8_t>(std::lround(alpha * 255.0f));
        }
    return result;
}

Bytes BuildIco(const RgbaImage& source, std::string& error) {
    static constexpr std::array<int, 7> sizes{16, 24, 32, 48, 64, 128, 256};
    std::vector<Bytes> frames;
    for (const int size : sizes) {
        const RgbaImage image = ResizeSquare(source, size);
        Bytes frame = size <= 48 ? DibFrame(image) : EncodePng(image, error);
        if (frame.empty()) return {};
        frames.push_back(std::move(frame));
    }
    Bytes out;
    PutU16(out, 0);
    PutU16(out, 1);
    PutU16(out, static_cast<std::uint32_t>(sizes.size()));
    std::uint32_t offset = static_cast<std::uint32_t>(6 + 16 * sizes.size());
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        out.push_back(static_cast<std::uint8_t>(sizes[index] == 256 ? 0 : sizes[index]));
        out.push_back(static_cast<std::uint8_t>(sizes[index] == 256 ? 0 : sizes[index]));
        out.push_back(0);
        out.push_back(0);
        PutU16(out, 1);
        PutU16(out, 32);
        PutU32(out, static_cast<std::uint32_t>(frames[index].size()));
        PutU32(out, offset);
        offset += static_cast<std::uint32_t>(frames[index].size());
    }
    for (const auto& frame : frames) out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

Bytes BuildIcns(const RgbaImage& source, std::string& error) {
    const int largest = std::max(source.width, source.height);
    std::vector<std::pair<std::string_view, int>> members{
        {"icp4", 16}, {"icp5", 32}, {"ic11", 32}, {"ic12", 64}, {"ic07", 128},
        {"ic13", 256}, {"ic08", 256}};
    if (largest >= 512) {
        members.emplace_back("ic14", 512);
        members.emplace_back("ic09", 512);
    }
    if (largest >= 1024) members.emplace_back("ic10", 1024);
    std::vector<std::pair<int, Bytes>> encoded;
    Bytes body;
    for (const auto& [type, size] : members) {
        auto cached = std::ranges::find(encoded, size, &std::pair<int, Bytes>::first);
        if (cached == encoded.end()) {
            Bytes png = EncodePng(ResizeSquare(source, size), error);
            if (png.empty()) return {};
            encoded.emplace_back(size, std::move(png));
            cached = std::prev(encoded.end());
        }
        body.insert(body.end(), type.begin(), type.end());
        PutU32BE(body, static_cast<std::uint32_t>(cached->second.size() + 8));
        body.insert(body.end(), cached->second.begin(), cached->second.end());
    }
    Bytes out{'i', 'c', 'n', 's'};
    PutU32BE(out, static_cast<std::uint32_t>(body.size() + 8));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::array<std::uint16_t, 4> NumericVersion(const std::string_view version) {
    std::array<std::uint16_t, 4> result{};
    const auto parts = NumericParts(version);
    for (std::size_t index = 0; index < parts.size() && index < 4; ++index)
        result[index] = ClampPart(parts[index]);
    return result;
}

std::string ShortVersion(const std::string_view version) {
    auto parts = NumericParts(version);
    if (parts.size() > 3) parts.resize(3);
    while (parts.size() < 2) parts.emplace_back("0");
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) result.push_back('.');
        result += std::to_string(ClampPart(part));
    }
    return result;
}

Bytes BuildVersionResource(const VersionStrings& strings) {
    const auto numeric = NumericVersion(strings.productVersion);
    const std::uint32_t ms = (static_cast<std::uint32_t>(numeric[0]) << 16) | numeric[1];
    const std::uint32_t ls = (static_cast<std::uint32_t>(numeric[2]) << 16) | numeric[3];
    Bytes fixed;
    PutU32(fixed, 0xFEEF04BD);  // dwSignature
    PutU32(fixed, 0x00010000);  // dwStrucVersion
    PutU32(fixed, ms);          // dwFileVersionMS
    PutU32(fixed, ls);
    PutU32(fixed, ms);          // dwProductVersionMS
    PutU32(fixed, ls);
    PutU32(fixed, 0x3F);        // dwFileFlagsMask
    PutU32(fixed, 0);           // dwFileFlags
    PutU32(fixed, 0x00040004);  // VOS_NT_WINDOWS32
    PutU32(fixed, 1);           // VFT_APP
    PutU32(fixed, 0);
    PutU32(fixed, 0);
    PutU32(fixed, 0);
    const std::string fileVersion = std::to_string(numeric[0]) + '.' + std::to_string(numeric[1]) + '.' +
                                    std::to_string(numeric[2]) + '.' + std::to_string(numeric[3]);
    const std::vector<std::pair<std::u16string, std::string>> entries{
        {u"CompanyName", strings.companyName},
        {u"FileDescription", strings.fileDescription},
        {u"FileVersion", fileVersion},
        {u"InternalName", strings.internalName},
        {u"LegalCopyright", strings.legalCopyright},
        {u"OriginalFilename", strings.originalFilename},
        {u"ProductName", strings.productName},
        {u"ProductVersion", strings.productVersion.empty() ? fileVersion : strings.productVersion},
    };
    Bytes out;
    VersionBlock(out, u"VS_VERSION_INFO", 0, fixed, static_cast<std::uint16_t>(fixed.size()), [&](Bytes& root) {
        VersionBlock(root, u"StringFileInfo", 1, {}, 0, [&](Bytes& info) {
            VersionBlock(info, u"040904B0", 1, {}, 0, [&](Bytes& table) {
                for (const auto& [key, value] : entries) {
                    if (value.empty()) continue;
                    const std::u16string wide = Utf16(value);
                    Bytes text;
                    PutWide(text, wide);
                    VersionBlock(table, key, 1, text, static_cast<std::uint16_t>(wide.size() + 1));
                }
            });
        });
        VersionBlock(root, u"VarFileInfo", 1, {}, 0, [&](Bytes& info) {
            Bytes translation;
            PutU16(translation, 0x0409);
            PutU16(translation, 0x04B0);
            VersionBlock(info, u"Translation", 0, translation, 4);
        });
    });
    return out;
}

std::optional<ParsedVersionResource> ParseVersionResource(const Bytes& resource) {
    ParsedVersionResource parsed;
    // Returns the end of the block starting at `at`, or 0 on malformed input.
    std::function<std::size_t(std::size_t, std::size_t, int)> block =
        [&](std::size_t at, const std::size_t limit, const int depth) -> std::size_t {
        if (at + 6 > limit || depth > 4) return 0;
        const std::size_t length = U16(resource, at);
        const std::size_t valueLength = U16(resource, at + 2);
        const std::uint16_t type = U16(resource, at + 4);
        const std::size_t end = at + length;
        if (length < 6 || end > limit) return 0;
        std::size_t cursor = at + 6;
        std::u16string key;
        while (cursor + 1 < end && U16(resource, cursor) != 0) {
            key.push_back(static_cast<char16_t>(U16(resource, cursor)));
            cursor += 2;
        }
        cursor += 2;
        cursor = (cursor + 3) & ~std::size_t{3};
        const std::size_t valueBytes = type == 1 ? valueLength * 2 : valueLength;
        if (cursor + valueBytes > end && valueLength != 0) return 0;
        if (depth == 0) {
            if (key != u"VS_VERSION_INFO" || valueBytes < 52 || U32(resource, cursor) != 0xFEEF04BD) return 0;
            const std::uint32_t ms = U32(resource, cursor + 8), ls = U32(resource, cursor + 12);
            parsed.fileVersion = {static_cast<std::uint16_t>(ms >> 16), static_cast<std::uint16_t>(ms & 0xffff),
                                  static_cast<std::uint16_t>(ls >> 16), static_cast<std::uint16_t>(ls & 0xffff)};
        } else if (depth == 3 && type == 1) {
            std::u16string value;
            for (std::size_t index = 0; index + 1 < valueLength; ++index)
                value.push_back(static_cast<char16_t>(U16(resource, cursor + index * 2)));
            parsed.strings.emplace_back(Utf8(key), Utf8(value));
        }
        cursor += valueBytes;
        while (true) {
            cursor = (cursor + 3) & ~std::size_t{3};
            if (cursor >= end) break;
            const std::size_t next = block(cursor, end, depth + 1);
            if (next == 0) return 0;
            cursor = next;
        }
        return end;
    };
    if (resource.size() < 6 || block(0, resource.size(), 0) == 0) return std::nullopt;
    return parsed;
}

std::string InfoPlist(const MacBundleInfo& info) {
    const std::string version = ShortVersion(info.version);
    std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n";
    const auto entry = [&](const std::string_view key, const std::string_view value) {
        plist += "\t<key>" + std::string(key) + "</key>\n\t<string>" + XmlEscape(value) + "</string>\n";
    };
    entry("CFBundleDevelopmentRegion", "en");
    entry("CFBundleDisplayName", info.bundleName);
    entry("CFBundleExecutable", info.executable);
    if (!info.iconFile.empty()) entry("CFBundleIconFile", info.iconFile);
    entry("CFBundleIdentifier", info.identifier);
    entry("CFBundleInfoDictionaryVersion", "6.0");
    entry("CFBundleName", info.bundleName);
    entry("CFBundlePackageType", "APPL");
    entry("CFBundleShortVersionString", version);
    entry("CFBundleVersion", version);
    entry("LSApplicationCategoryType", "public.app-category.games");
    entry("LSMinimumSystemVersion", info.minimumSystemVersion);
    plist += "\t<key>NSHighResolutionCapable</key>\n\t<true/>\n";
    if (!info.copyright.empty()) entry("NSHumanReadableCopyright", info.copyright);
    entry("NSPrincipalClass", "NSApplication");
    // SDL_GetBasePath() is Contents/Resources, where the game data lives.
    entry("SDL_FILESYSTEM_BASE_DIR_TYPE", "resource");
    plist += "</dict>\n</plist>\n";
    return plist;
}

std::optional<std::string> MachOMinimumSystemVersion(const Bytes& executable) {
    if (executable.size() < 32) return std::nullopt;
    const std::uint32_t magicBE = U32BE(executable, 0);
    if (magicBE == 0xCAFEBABE || magicBE == 0xCAFEBABF) {
        const bool wide = magicBE == 0xCAFEBABF;
        if (U32BE(executable, 4) == 0 || executable.size() < 8 + (wide ? 32u : 20u)) return std::nullopt;
        const std::uint64_t offset = wide
            ? (static_cast<std::uint64_t>(U32BE(executable, 16)) << 32) | U32BE(executable, 20)
            : U32BE(executable, 16);
        if (offset == 0 || offset >= executable.size()) return std::nullopt;
        return MachOMinimumSystemVersion(Bytes(executable.begin() + static_cast<std::ptrdiff_t>(offset),
                                               executable.end()));
    }
    const std::uint32_t magic = U32(executable, 0);
    if (magic != 0xFEEDFACF && magic != 0xFEEDFACE) return std::nullopt;
    const std::size_t header = magic == 0xFEEDFACF ? 32 : 28;
    const std::uint32_t commands = U32(executable, 16);
    std::size_t cursor = header;
    const auto format = [](const std::uint32_t encoded) {
        std::string text = std::to_string(encoded >> 16) + '.' + std::to_string((encoded >> 8) & 0xff);
        if ((encoded & 0xff) != 0) text += '.' + std::to_string(encoded & 0xff);
        return text;
    };
    for (std::uint32_t index = 0; index < commands && index < 4096; ++index) {
        if (cursor + 8 > executable.size()) return std::nullopt;
        const std::uint32_t command = U32(executable, cursor), size = U32(executable, cursor + 4);
        if (size < 8) return std::nullopt;
        if (command == 0x32 && cursor + 16 <= executable.size() && U32(executable, cursor + 8) == 1)
            return format(U32(executable, cursor + 12));  // LC_BUILD_VERSION, PLATFORM_MACOS
        if (command == 0x24 && cursor + 12 <= executable.size())
            return format(U32(executable, cursor + 8));   // LC_VERSION_MIN_MACOSX
        cursor += size;
    }
    return std::nullopt;
}

std::string DefaultBundleIdentifier(const std::string_view projectId) {
    std::string id;
    for (const char c : projectId)
        id.push_back(PortableAscii(std::string_view(&c, 1), ".-") ? c : '-');
    while (!id.empty() && id.front() == '.') id.erase(id.begin());
    while (!id.empty() && id.back() == '.') id.pop_back();
    if (id.empty()) id = "game";
    return id.find('.') == std::string::npos ? "com.example." + id : id;
}

bool ValidBundleIdentifier(const std::string_view identifier) {
    if (identifier.empty() || identifier.size() > 155 || !PortableAscii(identifier, ".-") ||
        identifier.find('.') == std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= identifier.size()) {
        const auto end = identifier.find('.', start);
        const auto part = identifier.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty()) return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::string DesktopEntry(const DesktopEntryInfo& info) {
    // Exec quoting (Desktop Entry spec): ", `, $ and \ are escaped inside quotes.
    std::string exec = "\"";
    for (const char c : info.executable) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') exec.push_back('\\');
        exec.push_back(c);
    }
    exec += "\"";
    std::string entry = "[Desktop Entry]\nType=Application\nVersion=1.0\n";
    entry += "Name=" + DesktopEscape(info.name) + "\n";
    if (!info.comment.empty()) entry += "Comment=" + DesktopEscape(info.comment) + "\n";
    entry += "Exec=" + DesktopEscape(exec) + "\n";
    if (!info.icon.empty()) entry += "Icon=" + DesktopEscape(info.icon) + "\n";
    entry += "Terminal=false\nCategories=Game;\n";
    return entry;
}

bool ValidEngineFolder(const std::string_view folder) {
    if (folder.empty() || folder.size() > 64 || folder.front() == '.' || folder.front() == '-' ||
        folder.back() == '.' || !PortableAscii(folder, "._-"))
        return false;
    std::string lower(folder);
    std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string stem = lower.substr(0, lower.find('.'));
    static constexpr std::array<std::string_view, 7> reserved{"con", "prn", "aux", "nul", "package", "save", "updates"};
    if (std::ranges::find(reserved, stem) != reserved.end() || lower == "notices") return false;
    if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9')
        return false;
    return true;
}

std::string AssemblyManifest(const std::string_view folder, const std::string_view architecture,
                             const std::vector<std::string>& files) {
    std::string manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <assemblyIdentity type=\"win32\" name=\"" + XmlEscape(folder) +
        "\" version=\"1.0.0.0\" processorArchitecture=\"" + XmlEscape(architecture) + "\"/>\n";
    for (const auto& file : files) manifest += "  <file name=\"" + XmlEscape(file) + "\"/>\n";
    manifest += "</assembly>\n";
    return manifest;
}

std::optional<std::string> AddAssemblyDependency(const std::string_view manifest, const std::string_view folder,
                                                 const std::string_view architecture) {
    const std::string dependency =
        "  <dependency>\n    <dependentAssembly>\n"
        "      <assemblyIdentity type=\"win32\" name=\"" + XmlEscape(folder) +
        "\" version=\"1.0.0.0\" processorArchitecture=\"" + XmlEscape(architecture) + "\"/>\n"
        "    </dependentAssembly>\n  </dependency>\n";
    if (manifest.empty())
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
               "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n" +
               dependency + "</assembly>\n";
    // Branding an already branded Player keeps a single dependency.
    if (manifest.find(dependency) != std::string_view::npos) return std::string(manifest);
    // The root element may carry a namespace prefix (<asmv1:assembly>).
    const auto close = manifest.rfind("assembly>");
    if (close == std::string_view::npos) return std::nullopt;
    const auto open = manifest.rfind("</", close);
    if (open == std::string_view::npos || manifest.substr(open, close - open).find('>') != std::string_view::npos)
        return std::nullopt;
    std::string result(manifest.substr(0, open));
    result += dependency;
    result += manifest.substr(open);
    return result;
}

std::optional<std::string> PeArchitecture(const Bytes& executable) {
    if (executable.size() < 0x40 || executable[0] != 'M' || executable[1] != 'Z') return std::nullopt;
    const std::size_t pe = U32(executable, 0x3C);
    if (pe + 6 > executable.size() || std::memcmp(&executable[pe], "PE\0\0", 4) != 0) return std::nullopt;
    switch (U16(executable, pe + 4)) {
        case 0x8664: return "amd64";
        case 0x014c: return "x86";
        case 0xAA64: return "arm64";
        default: return std::nullopt;
    }
}

std::optional<std::uint16_t> PeSubsystem(const Bytes& executable) {
    if (executable.size() < 0x40 || executable[0] != 'M' || executable[1] != 'Z') return std::nullopt;
    const std::size_t pe = U32(executable, 0x3C);
    if (pe + 24 + 70 > executable.size() || std::memcmp(&executable[pe], "PE\0\0", 4) != 0) return std::nullopt;
    return U16(executable, pe + 24 + 68);
}

std::string LayoutResource(const std::string_view dataDirectory) {
    return nlohmann::json{{"format", "PrismatiXLayout"}, {"schemaRevision", 1},
                          {"dataDirectory", std::string(dataDirectory)}}.dump();
}

std::optional<std::string> LayoutDataDirectory(const std::string_view resource) {
    const auto parsed = nlohmann::json::parse(resource, nullptr, false);
    if (!parsed.is_object() || parsed.value("format", "") != "PrismatiXLayout") return std::nullopt;
    const auto found = parsed.find("dataDirectory");
    if (found == parsed.end() || !found->is_string() || !ValidEngineFolder(found->get<std::string>()))
        return std::nullopt;
    return found->get<std::string>();
}

namespace {

// Section and header fields of a PE image this editor reads and rewrites.
struct PeSection {
    std::size_t header = 0;  // file offset of the 40-byte section header
    std::string name;
    std::uint32_t virtualSize = 0;
    std::uint32_t virtualAddress = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t rawPointer = 0;
    std::uint32_t characteristics = 0;
};
struct PeLayout {
    std::size_t optional = 0;      // file offset of the optional header
    std::size_t directories = 0;   // file offset of the data directory array
    std::uint32_t directoryCount = 0;
    std::uint32_t sectionAlignment = 0;
    std::uint32_t fileAlignment = 0;
    std::vector<PeSection> sections;  // in address order
};

constexpr std::size_t kResourceDirectory = 2;
constexpr std::size_t kSecurityDirectory = 4;
constexpr std::size_t kRelocationDirectory = 5;
constexpr std::size_t kDebugDirectory = 6;

void SetU32(Bytes& out, const std::size_t at, const std::uint32_t value) {
    SetU16(out, at, value & 0xffff);
    SetU16(out, at + 2, value >> 16);
}

std::uint32_t AlignUp(const std::uint64_t value, const std::uint32_t alignment) {
    return static_cast<std::uint32_t>((value + alignment - 1) / alignment * alignment);
}

std::optional<PeLayout> ParsePeLayout(const Bytes& image, std::string& error) {
    error = "not a PE image";
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return std::nullopt;
    const std::size_t pe = U32(image, 0x3C);
    if (pe > image.size() || image.size() - pe < 24 || std::memcmp(&image[pe], "PE\0\0", 4) != 0) return std::nullopt;
    PeLayout layout;
    const std::uint16_t sectionCount = U16(image, pe + 6);
    const std::uint16_t optionalSize = U16(image, pe + 20);
    layout.optional = pe + 24;
    if (image.size() < layout.optional + optionalSize || optionalSize < 96) return std::nullopt;
    const std::uint16_t magic = U16(image, layout.optional);
    if (magic != 0x10b && magic != 0x20b) return std::nullopt;
    const std::size_t countAt = layout.optional + (magic == 0x20b ? 108 : 92);
    layout.directories = countAt + 4;
    if (countAt + 4 > layout.optional + optionalSize) return std::nullopt;
    layout.directoryCount = std::min<std::uint32_t>(U32(image, countAt), 16);
    if (layout.directories + 8ull * layout.directoryCount > layout.optional + optionalSize) return std::nullopt;
    layout.sectionAlignment = U32(image, layout.optional + 32);
    layout.fileAlignment = U32(image, layout.optional + 36);
    const auto powerOfTwo = [](const std::uint32_t value) { return value != 0 && (value & (value - 1)) == 0; };
    if (!powerOfTwo(layout.sectionAlignment) || !powerOfTwo(layout.fileAlignment) || sectionCount == 0 ||
        sectionCount > 96) {
        error = "the PE headers are invalid";
        return std::nullopt;
    }
    const std::size_t table = layout.optional + optionalSize;
    if (table + 40ull * sectionCount > image.size()) return std::nullopt;
    for (std::uint16_t index = 0; index < sectionCount; ++index) {
        const std::size_t at = table + 40ull * index;
        PeSection section;
        section.header = at;
        section.name.assign(reinterpret_cast<const char*>(&image[at]), 8);
        section.name.erase(std::find(section.name.begin(), section.name.end(), '\0'), section.name.end());
        section.virtualSize = U32(image, at + 8);
        section.virtualAddress = U32(image, at + 12);
        section.rawSize = U32(image, at + 16);
        section.rawPointer = U32(image, at + 20);
        section.characteristics = U32(image, at + 36);
        if (section.rawSize != 0 &&
            (section.rawPointer > image.size() || section.rawSize > image.size() - section.rawPointer)) {
            error = "a PE section lies outside the file";
            return std::nullopt;
        }
        layout.sections.push_back(std::move(section));
    }
    std::ranges::sort(layout.sections, {}, &PeSection::virtualAddress);
    error.clear();
    return layout;
}

std::pair<std::uint32_t, std::uint32_t> Directory(const Bytes& image, const PeLayout& layout, const std::size_t index) {
    if (index >= layout.directoryCount) return {0, 0};
    const std::size_t at = layout.directories + 8 * index;
    return {U32(image, at), U32(image, at + 4)};
}

// The file offset of `size` bytes at `rva`, when they lie in one section's raw data.
std::optional<std::size_t> RvaOffset(const PeLayout& layout, const std::uint32_t rva, const std::uint32_t size) {
    for (const auto& section : layout.sections) {
        if (rva < section.virtualAddress) continue;
        const std::uint64_t within = rva - section.virtualAddress;
        if (within + size <= section.rawSize) return section.rawPointer + static_cast<std::size_t>(within);
    }
    return std::nullopt;
}

bool ReadResourceTree(const Bytes& image, const PeLayout& layout, const std::uint32_t base, const std::uint32_t limit,
                      const std::uint32_t directory, const int level, const PeResource& partial,
                      std::vector<PeResource>& out, std::string& error) {
    const auto at = RvaOffset(layout, base + directory, 16);
    if (!at || directory >= limit) {
        error = "a resource directory lies outside the resource section";
        return false;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(U16(image, *at + 12)) + U16(image, *at + 14);
    if (count > 4096 || !RvaOffset(layout, base + directory + 16, 8 * count)) {
        error = "a resource directory is too large";
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry = *at + 16 + 8ull * index;
        const std::uint32_t name = U32(image, entry);
        const std::uint32_t offset = U32(image, entry + 4);
        PeResourceKey key;
        if ((name & 0x80000000u) != 0) {
            const std::uint32_t stringOffset = name & 0x7fffffffu;
            const auto length = RvaOffset(layout, base + stringOffset, 2);
            const std::uint16_t characters = length ? U16(image, *length) : 0;
            const auto text = length ? RvaOffset(layout, base + stringOffset + 2, 2u * characters) : std::nullopt;
            if (!text || characters == 0) {
                error = "a resource name lies outside the resource section";
                return false;
            }
            for (std::uint16_t character = 0; character < characters; ++character)
                key.name.push_back(static_cast<char16_t>(U16(image, *text + 2ull * character)));
        } else {
            key.id = static_cast<std::uint16_t>(name & 0xffff);
        }
        PeResource next = partial;
        if (level == 0) next.type = key;
        else if (level == 1) next.name = key;
        else next.language = key.id;
        const bool subdirectory = (offset & 0x80000000u) != 0;
        if (level < 2) {
            if (!subdirectory) {
                error = "a resource directory entry is not a directory";
                return false;
            }
            if (!ReadResourceTree(image, layout, base, limit, offset & 0x7fffffffu, level + 1, next, out, error))
                return false;
            continue;
        }
        const auto dataEntry = subdirectory ? std::nullopt : RvaOffset(layout, base + offset, 16);
        if (!dataEntry) {
            error = "a resource data entry lies outside the resource section";
            return false;
        }
        const std::uint32_t dataRva = U32(image, *dataEntry);
        const std::uint32_t size = U32(image, *dataEntry + 4);
        const auto data = size > 256u * 1024u * 1024u ? std::nullopt : RvaOffset(layout, dataRva, size);
        if (!data) {
            error = "resource data lies outside the image";
            return false;
        }
        next.data.assign(image.begin() + static_cast<std::ptrdiff_t>(*data),
                         image.begin() + static_cast<std::ptrdiff_t>(*data + size));
        next.codePage = U32(image, *dataEntry + 8);
        out.push_back(std::move(next));
        if (out.size() > 65536) {
            error = "the image has too many resources";
            return false;
        }
    }
    return true;
}

// Named entries first, ordered case-insensitively (the loader's binary
// search order), then ids in ascending order.
bool KeyBefore(const PeResourceKey& left, const PeResourceKey& right) {
    if (left.name.empty() != right.name.empty()) return !left.name.empty();
    if (left.name.empty()) return left.id < right.id;
    const auto upper = [](const char16_t c) { return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - 32) : c; };
    return std::ranges::lexicographical_compare(left.name, right.name, {}, upper, upper);
}

// A resource section for `resources` at `baseRva`: the directory tables
// breadth first, then the names, the data entries and the 8-byte aligned data.
Bytes BuildResourceSection(const std::vector<PeResource>& resources, const std::uint32_t baseRva) {
    struct Language { std::uint16_t id; const PeResource* resource; };
    struct Name { PeResourceKey key; std::vector<Language> languages; };
    struct Type { PeResourceKey key; std::vector<Name> names; };
    std::vector<Type> types;
    for (const auto& resource : resources) {
        auto type = std::ranges::find(types, resource.type, &Type::key);
        if (type == types.end()) type = types.insert(types.end(), Type{resource.type, {}});
        auto name = std::ranges::find(type->names, resource.name, &Name::key);
        if (name == type->names.end()) name = type->names.insert(type->names.end(), Name{resource.name, {}});
        name->languages.push_back({resource.language, &resource});
    }
    std::ranges::sort(types, KeyBefore, &Type::key);
    for (auto& type : types) {
        std::ranges::sort(type.names, KeyBefore, &Name::key);
        for (auto& name : type.names) std::ranges::sort(name.languages, {}, &Language::id);
    }
    const auto directorySize = [](const std::size_t entries) { return 16 + 8 * entries; };
    std::size_t size = directorySize(types.size());
    std::vector<std::size_t> typeDirectories;
    for (const auto& type : types) {
        typeDirectories.push_back(size);
        size += directorySize(type.names.size());
    }
    std::vector<std::vector<std::size_t>> nameDirectories;
    for (const auto& type : types) {
        nameDirectories.emplace_back();
        for (const auto& name : type.names) {
            nameDirectories.back().push_back(size);
            size += directorySize(name.languages.size());
        }
    }
    std::vector<std::pair<const PeResourceKey*, std::size_t>> strings;
    const auto addString = [&](const PeResourceKey& key) {
        if (key.name.empty()) return;
        strings.emplace_back(&key, size);
        size += 2 + 2 * key.name.size();
    };
    for (const auto& type : types) {
        addString(type.key);
        for (const auto& name : type.names) addString(name.key);
    }
    size = (size + 3) & ~std::size_t{3};
    const std::size_t dataEntries = size;
    size += 16 * resources.size();
    Bytes out(size, 0);
    const auto keyField = [&](const PeResourceKey& key) {
        if (key.name.empty()) return std::uint32_t{key.id};
        for (const auto& [candidate, offset] : strings)
            if (candidate == &key) return 0x80000000u | static_cast<std::uint32_t>(offset);
        return std::uint32_t{0};
    };
    const auto header = [&](const std::size_t at, const std::size_t named, const std::size_t ids) {
        SetU16(out, at + 12, static_cast<std::uint32_t>(named));
        SetU16(out, at + 14, static_cast<std::uint32_t>(ids));
    };
    const auto namedCount = [](const auto& entries) {
        return static_cast<std::size_t>(std::ranges::count_if(entries, [](const auto& entry) { return !entry.key.name.empty(); }));
    };
    header(0, namedCount(types), types.size() - namedCount(types));
    std::size_t nextData = 0;
    for (std::size_t typeIndex = 0; typeIndex < types.size(); ++typeIndex) {
        const auto& type = types[typeIndex];
        SetU32(out, 16 + 8 * typeIndex, keyField(type.key));
        SetU32(out, 16 + 8 * typeIndex + 4, 0x80000000u | static_cast<std::uint32_t>(typeDirectories[typeIndex]));
        const std::size_t typeAt = typeDirectories[typeIndex];
        header(typeAt, namedCount(type.names), type.names.size() - namedCount(type.names));
        for (std::size_t nameIndex = 0; nameIndex < type.names.size(); ++nameIndex) {
            const auto& name = type.names[nameIndex];
            const std::size_t nameAt = nameDirectories[typeIndex][nameIndex];
            SetU32(out, typeAt + 16 + 8 * nameIndex, keyField(name.key));
            SetU32(out, typeAt + 16 + 8 * nameIndex + 4, 0x80000000u | static_cast<std::uint32_t>(nameAt));
            header(nameAt, 0, name.languages.size());
            for (std::size_t index = 0; index < name.languages.size(); ++index) {
                const std::size_t entry = dataEntries + 16 * nextData++;
                SetU32(out, nameAt + 16 + 8 * index, name.languages[index].id);
                SetU32(out, nameAt + 16 + 8 * index + 4, static_cast<std::uint32_t>(entry));
                const PeResource& resource = *name.languages[index].resource;
                out.resize((out.size() + 7) & ~std::size_t{7}, 0);
                SetU32(out, entry, baseRva + static_cast<std::uint32_t>(out.size()));
                SetU32(out, entry + 4, static_cast<std::uint32_t>(resource.data.size()));
                SetU32(out, entry + 8, resource.codePage);
                out.insert(out.end(), resource.data.begin(), resource.data.end());
            }
        }
    }
    for (const auto& [key, offset] : strings) {
        SetU16(out, offset, static_cast<std::uint32_t>(key->name.size()));
        for (std::size_t index = 0; index < key->name.size(); ++index)
            SetU16(out, offset + 2 + 2 * index, key->name[index]);
    }
    return out;
}

}  // namespace

std::optional<std::vector<PeResource>> ReadPeResources(const Bytes& image, std::string& error) {
    const auto layout = ParsePeLayout(image, error);
    if (!layout) return std::nullopt;
    const auto [rva, size] = Directory(image, *layout, kResourceDirectory);
    std::vector<PeResource> resources;
    if (rva == 0 || size == 0) return resources;
    if (!ReadResourceTree(image, *layout, rva, size, 0, 0, {}, resources, error)) return std::nullopt;
    return resources;
}

std::optional<std::uint32_t> PeChecksum(const Bytes& image) {
    std::string ignored;
    const auto layout = ParsePeLayout(image, ignored);
    if (!layout) return std::nullopt;
    const std::size_t field = layout->optional + 64;
    std::uint64_t sum = 0;
    for (std::size_t at = 0; at < image.size(); at += 2) {
        if (at == field || at == field + 2) continue;
        sum += at + 1 < image.size() ? U16(image, at) : image[at];
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<std::uint32_t>(sum + image.size());
}

std::optional<Bytes> ReplacePeResources(const Bytes& image, const std::vector<PeResource>& resources,
                                        std::string& error) {
    const auto parsed = ParsePeLayout(image, error);
    if (!parsed) return std::nullopt;
    const PeLayout& layout = *parsed;
    if (layout.directoryCount <= kDebugDirectory) {
        error = "the PE image has too few data directories";
        return std::nullopt;
    }
    const auto [resourceRva, resourceSize] = Directory(image, layout, kResourceDirectory);
    std::size_t index = layout.sections.size();
    for (std::size_t candidate = 0; candidate < layout.sections.size(); ++candidate) {
        const auto& section = layout.sections[candidate];
        if (resourceRva != 0 && resourceSize != 0 ? section.virtualAddress == resourceRva : section.name == ".rsrc")
            index = candidate;
    }
    if (index == layout.sections.size()) {
        error = "the executable has no resource section";
        return std::nullopt;
    }
    const PeSection& resource = layout.sections[index];
    const auto [relocationRva, relocationSize] = Directory(image, layout, kRelocationDirectory);
    // Only the base relocation table may follow: nothing refers to its
    // address except its data directory, so it can move.
    std::optional<PeSection> relocation;
    for (std::size_t later = index + 1; later < layout.sections.size(); ++later) {
        const auto& section = layout.sections[later];
        const bool relocations = !relocation && relocationRva != 0 && relocationRva >= section.virtualAddress &&
            std::uint64_t{relocationRva} + relocationSize <=
                std::uint64_t{section.virtualAddress} + std::max(section.virtualSize, section.rawSize);
        if (!relocations) {
            error = "the executable's resource section is followed by " + section.name +
                    ", which cannot be moved to make room for new resources";
            return std::nullopt;
        }
        relocation = section;
    }
    for (const auto& section : layout.sections)
        if (section.header != resource.header && (!relocation || section.header != relocation->header) &&
            section.rawSize != 0 && section.rawPointer >= resource.rawPointer) {
            error = "the executable stores " + section.name + " after its resources";
            return std::nullopt;
        }
    if (relocation && relocation->rawSize != 0 &&
        relocation->rawPointer < std::uint64_t{resource.rawPointer} + resource.rawSize) {
        error = "the executable's relocations overlap its resources";
        return std::nullopt;
    }
    // Debug data is addressed by file offset; it must not sit in what moves.
    if (const auto [debugRva, debugSize] = Directory(image, layout, kDebugDirectory); debugRva != 0 && debugSize != 0) {
        const auto debug = RvaOffset(layout, debugRva, debugSize);
        if (!debug || debugSize % 28 != 0) {
            error = "the executable's debug directory is invalid";
            return std::nullopt;
        }
        for (std::size_t at = *debug; at < *debug + debugSize; at += 28) {
            const std::uint32_t pointer = U32(image, at + 24);
            if (pointer != 0 && pointer >= resource.rawPointer) {
                error = "the executable stores debug data after its resources";
                return std::nullopt;
            }
        }
    }
    std::uint64_t sectionsEnd = 0;
    for (const auto& section : layout.sections)
        sectionsEnd = std::max<std::uint64_t>(sectionsEnd, std::uint64_t{section.rawPointer} + section.rawSize);
    // Bytes after the last section (an overlay) stay, without a signature.
    Bytes overlay(image.begin() + static_cast<std::ptrdiff_t>(sectionsEnd), image.end());
    const auto [certificate, certificateSize] = Directory(image, layout, kSecurityDirectory);
    if (certificate != 0 && certificateSize != 0) {
        if (certificate < sectionsEnd || std::uint64_t{certificate} + certificateSize > image.size()) {
            error = "the executable's signature lies outside its overlay";
            return std::nullopt;
        }
        const auto from = overlay.begin() + static_cast<std::ptrdiff_t>(certificate - sectionsEnd);
        overlay.erase(from, from + static_cast<std::ptrdiff_t>(certificateSize));
    }

    const Bytes section = BuildResourceSection(resources, resource.virtualAddress);
    const std::uint32_t rawSize = AlignUp(section.size(), layout.fileAlignment);
    Bytes out(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(resource.rawPointer));
    out.insert(out.end(), section.begin(), section.end());
    out.resize(std::size_t{resource.rawPointer} + rawSize, 0);
    SetU32(out, resource.header + 8, static_cast<std::uint32_t>(section.size()));
    SetU32(out, resource.header + 16, rawSize);
    std::uint32_t imageEnd = AlignUp(std::uint64_t{resource.virtualAddress} + section.size(), layout.sectionAlignment);
    if (relocation) {
        const auto pointer = static_cast<std::uint32_t>(out.size());
        out.insert(out.end(), image.begin() + static_cast<std::ptrdiff_t>(relocation->rawPointer),
                   image.begin() + static_cast<std::ptrdiff_t>(std::size_t{relocation->rawPointer} + relocation->rawSize));
        const std::uint32_t address = imageEnd;
        SetU32(out, relocation->header + 12, address);
        SetU32(out, relocation->header + 20, relocation->rawSize == 0 ? 0 : pointer);
        SetU32(out, layout.directories + 8 * kRelocationDirectory, address + (relocationRva - relocation->virtualAddress));
        imageEnd = AlignUp(std::uint64_t{address} + std::max(relocation->virtualSize, relocation->rawSize),
                           layout.sectionAlignment);
    }
    out.insert(out.end(), overlay.begin(), overlay.end());
    SetU32(out, layout.directories + 8 * kResourceDirectory, resource.virtualAddress);
    SetU32(out, layout.directories + 8 * kResourceDirectory + 4, static_cast<std::uint32_t>(section.size()));
    SetU32(out, layout.directories + 8 * kSecurityDirectory, 0);
    SetU32(out, layout.directories + 8 * kSecurityDirectory + 4, 0);
    SetU32(out, layout.optional + 56, imageEnd);  // SizeOfImage
    if ((resource.characteristics & 0x40u) != 0) {  // IMAGE_SCN_CNT_INITIALIZED_DATA
        const std::int64_t initialized =
            std::int64_t{U32(out, layout.optional + 8)} + std::int64_t{rawSize} - std::int64_t{resource.rawSize};
        SetU32(out, layout.optional + 8, static_cast<std::uint32_t>(std::max<std::int64_t>(0, initialized)));
    }
    SetU32(out, layout.optional + 64, PeChecksum(out).value_or(0));
    return out;
}

std::optional<Bytes> FindPeResource(const std::vector<PeResource>& resources, const PeResourceKey& type,
                                    const PeResourceKey& name) {
    for (const auto& resource : resources)
        if (resource.type == type && resource.name == name) return resource.data;
    return std::nullopt;
}

bool WriteWindowsResources(const std::filesystem::path& executable, const WindowsResources& resources,
                           std::string& error) {
    const PeResourceKey icon{3, {}}, groupIcon{14, {}}, version{16, {}}, manifestType{24, {}}, rcData{10, {}};
    const PeResourceKey layoutName{0, u"PRISMATIX_LAYOUT"};
    constexpr std::uint16_t neutral = 0;
    constexpr std::uint16_t english = 0x0409;

    struct Image { std::size_t entry, offset, size; };
    std::vector<Image> images;
    const Bytes& ico = resources.ico;
    if (!ico.empty()) {
        if (ico.size() < 6 || U16(ico, 0) != 0 || U16(ico, 2) != 1 || U16(ico, 4) == 0 ||
            U16(ico, 4) > 64 || ico.size() < 6 + 16 * static_cast<std::size_t>(U16(ico, 4))) {
            error = "icon is not a Windows .ico file";
            return false;
        }
        for (std::uint16_t index = 0; index < U16(ico, 4); ++index) {
            const std::size_t at = 6 + 16 * static_cast<std::size_t>(index);
            const std::size_t size = U32(ico, at + 8), offset = U32(ico, at + 12);
            if (size == 0 || offset > ico.size() || size > ico.size() - offset) {
                error = "icon image entry lies outside the file";
                return false;
            }
            images.push_back({at, offset, size});
        }
    }
    std::error_code code;
    const auto fileSize = std::filesystem::file_size(executable, code);
    if (code || fileSize > 1024ull * 1024ull * 1024ull) {
        error = "executable resources could not be read";
        return false;
    }
    Bytes image(static_cast<std::size_t>(fileSize));
    {
        std::ifstream input(executable, std::ios::binary);
        input.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!input) {
            error = "executable resources could not be read";
            return false;
        }
    }
    std::string readError;
    auto existing = ReadPeResources(image, readError);
    if (!existing) {
        error = "executable resources could not be read: " + readError;
        return false;
    }
    std::string manifest;
    std::uint16_t manifestLanguage = english;
    // Resources being replaced are removed first, in every language, so the
    // written icon group is the executable's only one and there is a single
    // version resource.
    std::erase_if(*existing, [&](const PeResource& resource) {
        if (!ico.empty() && (resource.type == icon || resource.type == groupIcon)) return true;
        if (!resources.version.empty() && resource.type == version) return true;
        if (resources.engineFolder.empty()) return false;
        if (resource.type == manifestType && resource.name == PeResourceKey{1, {}}) {
            manifest.assign(resource.data.begin(), resource.data.end());
            manifestLanguage = resource.language;
            return true;
        }
        return resource.type == rcData && resource.name == layoutName;
    });
    if (!ico.empty()) {
        // GRPICONDIR followed by one 14-byte GRPICONDIRENTRY per RT_ICON image.
        const auto count = static_cast<std::uint16_t>(images.size());
        Bytes group{0, 0, 1, 0, static_cast<std::uint8_t>(count & 0xff), static_cast<std::uint8_t>(count >> 8)};
        for (std::uint16_t index = 0; index < count; ++index) {
            const auto& entry = images[index];
            const auto id = static_cast<std::uint16_t>(index + 1);
            existing->push_back({icon, {id, {}}, neutral,
                                 Bytes(ico.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                                       ico.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.size))});
            group.insert(group.end(), ico.begin() + static_cast<std::ptrdiff_t>(entry.entry),
                         ico.begin() + static_cast<std::ptrdiff_t>(entry.entry + 12));
            group.push_back(static_cast<std::uint8_t>(id & 0xff));
            group.push_back(static_cast<std::uint8_t>(id >> 8));
        }
        existing->push_back({groupIcon, {1, {}}, neutral, std::move(group)});
    }
    if (!resources.version.empty()) existing->push_back({version, {1, {}}, english, resources.version});
    if (!resources.engineFolder.empty()) {
        // A UTF-8 byte order mark is legal before the XML declaration.
        if (manifest.starts_with("\xEF\xBB\xBF")) manifest.erase(0, 3);
        const auto merged = AddAssemblyDependency(manifest, resources.engineFolder, resources.architecture);
        if (!merged) {
            error = "the Player's application manifest has no closing assembly element";
            return false;
        }
        existing->push_back({manifestType, {1, {}}, manifestLanguage, Bytes(merged->begin(), merged->end())});
        const std::string layout = LayoutResource(resources.engineFolder);
        existing->push_back({rcData, layoutName, neutral, Bytes(layout.begin(), layout.end())});
    }
    std::string replaceError;
    const auto rewritten = ReplacePeResources(image, *existing, replaceError);
    if (!rewritten) {
        error = "executable resources could not be written: " + replaceError;
        return false;
    }
    std::ofstream output(executable, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(rewritten->data()), static_cast<std::streamsize>(rewritten->size()));
    if (!output) {
        error = "executable resources could not be written";
        return false;
    }
    return true;
}

LayoutPlatform HostLayoutPlatform() {
#if defined(_WIN32)
    return LayoutPlatform::Windows;
#elif defined(__APPLE__)
    return LayoutPlatform::MacOS;
#elif defined(__linux__) && !defined(__ANDROID__)
    return LayoutPlatform::Linux;
#else
    return LayoutPlatform::Other;
#endif
}

std::optional<LayoutPlatform> TargetLayoutPlatform(const std::string_view target) {
    if (target == "win32-x64") return LayoutPlatform::Windows;
    if (target == "linux-x64") return LayoutPlatform::Linux;
    if (target == "darwin-x64" || target == "darwin-arm64") return LayoutPlatform::MacOS;
    return std::nullopt;
}

std::optional<std::string> ExecutableTargetMismatch(const Bytes& executable, const std::string_view target) {
    const std::string wanted(target);
    if (executable.size() >= 2 && executable[0] == 'M' && executable[1] == 'Z') {
        const auto architecture = PeArchitecture(executable);
        if (target == "win32-x64" && architecture == "amd64") return std::nullopt;
        return "the Player is a Windows " + architecture.value_or("unknown") + " executable, not a " + wanted + " one";
    }
    if (executable.size() >= 20 && U32BE(executable, 0) == 0x7F454C46) {  // ELF
        const bool x64 = executable[4] == 2 && executable[5] == 1 && U16(executable, 18) == 0x3E;
        if (target == "linux-x64" && x64) return std::nullopt;
        return std::string("the Player is a Linux ") + (x64 ? "x64" : "non-x64") + " executable, not a " + wanted + " one";
    }
    if (executable.size() >= 8) {
        const std::uint32_t magicBE = U32BE(executable, 0);
        std::vector<std::uint32_t> cpus;
        if (magicBE == 0xCAFEBABE || magicBE == 0xCAFEBABF) {
            const bool wide = magicBE == 0xCAFEBABF;
            const std::uint32_t count = U32BE(executable, 4);
            const std::size_t stride = wide ? 32 : 20;
            for (std::uint32_t index = 0; index < std::min<std::uint32_t>(count, 16); ++index)
                if (8 + stride * (index + 1) <= executable.size()) cpus.push_back(U32BE(executable, 8 + stride * index));
        } else if (U32(executable, 0) == 0xFEEDFACF) {
            cpus.push_back(U32(executable, 4));
        }
        if (!cpus.empty()) {
            const std::uint32_t wantedCpu = target == "darwin-arm64" ? 0x0100000Cu : 0x01000007u;  // ARM64, X86_64
            const bool mac = target == "darwin-arm64" || target == "darwin-x64";
            if (mac && std::ranges::find(cpus, wantedCpu) != cpus.end()) return std::nullopt;
            return mac ? "the Player is a macOS executable without " + wanted + " code"
                       : "the Player is a macOS executable, not a " + wanted + " one";
        }
    }
    return "the Player is not a native executable for " + wanted;
}

namespace {

std::optional<Bytes> ReadFileBytes(const std::filesystem::path& path, const std::uintmax_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    Bytes bytes(static_cast<std::size_t>(size));
    if (size > 0) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) return std::nullopt;
    return bytes;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

std::string Lowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsLibrary(const std::string& name, const LayoutPlatform platform) {
    const std::string lower = Lowercase(name);
    switch (platform) {
        case LayoutPlatform::Windows: return lower.ends_with(".dll");
        case LayoutPlatform::MacOS: return lower.ends_with(".dylib");
        case LayoutPlatform::Linux: return lower.ends_with(".so") || lower.find(".so.") != std::string::npos;
        default: return false;
    }
}

std::string Stem(const std::string& file) {
    const auto dot = file.rfind('.');
    return dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
}

// The icon as RGBA for platforms that need raster sizes (macOS, Linux).
std::optional<RgbaImage> IconImage(const std::filesystem::path& icon, std::string& error) {
    const auto bytes = ReadFileBytes(icon, 4u * 1024u * 1024u);
    if (!bytes) {
        error = "icon could not be read";
        return std::nullopt;
    }
    return Lowercase(px::PathToUtf8(icon.extension())) == ".png" ? DecodePng(*bytes, error) : DecodeIcoLargest(*bytes, error);
}

// Moves every entry of `staging` except `keep` into `target` (same volume).
bool MoveEntries(const std::filesystem::path& staging, const std::filesystem::path& target,
                 const std::vector<std::filesystem::path>& keep, std::string& error) {
    std::error_code code;
    std::filesystem::create_directories(target, code);
    if (code) {
        error = "layout folder could not be created: " + code.message();
        return false;
    }
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator iterator(staging, code);
         !code && iterator != std::filesystem::directory_iterator(); iterator.increment(code)) {
        const auto path = iterator->path();
        if (path == target || std::ranges::find(keep, path) != keep.end()) continue;
        entries.push_back(path);
    }
    if (code) {
        error = "staged distribution could not be listed: " + code.message();
        return false;
    }
    for (const auto& path : entries) {
        std::filesystem::rename(path, target / path.filename(), code);
        if (code) {
            error = "staged file could not be moved into the layout: " + code.message();
            return false;
        }
    }
    return true;
}

void MarkExecutable(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::permissions(path,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, ignored);
}

bool ApplyWindowsLayout(const std::filesystem::path& staging, const LayoutRequest& request,
                        LayoutOutcome& outcome, std::string& error) {
    const auto player = staging / px::PathFromUtf8(request.playerFile);
    const auto executable = ReadFileBytes(player, 1024ull * 1024ull * 1024ull);
    const auto architecture = executable ? PeArchitecture(*executable) : std::nullopt;
    if (!architecture) {
        // Fixtures and script stand-ins are not PE images; a real branded
        // or bundled distribution needs one.
        if (!request.icon.empty() || !request.engineFolder.empty()) {
            error = "the Player is not a PE executable of a known architecture";
            return false;
        }
        outcome.notes.push_back("The Player is not a PE executable; version information skipped");
        outcome.player = player;
        return true;
    }
    WindowsResources resources;
    if (!request.icon.empty()) {
        if (Lowercase(px::PathToUtf8(request.icon.extension())) == ".png") {
            const auto bytes = ReadFileBytes(request.icon, 4u * 1024u * 1024u);
            std::string decodeError;
            const auto image = bytes ? DecodePng(*bytes, decodeError) : std::nullopt;
            if (!image) {
                error = "icon " + (bytes ? decodeError : std::string("could not be read"));
                return false;
            }
            resources.ico = BuildIco(*image, error);
            if (resources.ico.empty()) return false;
        } else {
            const auto bytes = ReadFileBytes(request.icon, 4u * 1024u * 1024u);
            if (!bytes) {
                error = "icon could not be read";
                return false;
            }
            resources.ico = *bytes;
        }
    }
    resources.version = BuildVersionResource({
        .productName = request.displayName,
        .fileDescription = request.displayName,
        .companyName = request.companyName,
        .legalCopyright = request.copyright,
        .productVersion = request.version,
        .originalFilename = request.playerFile,
        .internalName = Stem(request.playerFile)});
    if (!request.engineFolder.empty()) {
        resources.engineFolder = request.engineFolder;
        resources.architecture = *architecture;
        std::vector<std::string> libraries;
        std::error_code code;
        for (std::filesystem::directory_iterator iterator(staging, code);
             !code && iterator != std::filesystem::directory_iterator(); iterator.increment(code)) {
            const std::string name = px::PathToUtf8(iterator->path().filename());
            if (iterator->is_regular_file() && IsLibrary(name, LayoutPlatform::Windows)) libraries.push_back(name);
        }
        std::ranges::sort(libraries);
        const auto folder = staging / px::PathFromUtf8(request.engineFolder);
        if (!MoveEntries(staging, folder, {player}, error)) return false;
        if (!WriteFileBytes(folder / px::PathFromUtf8(request.engineFolder + ".manifest"),
                            AssemblyManifest(request.engineFolder, *architecture, libraries))) {
            error = "the engine folder's assembly manifest could not be written";
            return false;
        }
        outcome.notes.push_back("Runtime libraries and game data are in " + request.engineFolder + "/");
    }
    if (!WriteWindowsResources(player, resources, error)) return false;
    outcome.player = player;
    return true;
}

bool ApplyMacLayout(const std::filesystem::path& staging, const LayoutRequest& request,
                    LayoutOutcome& outcome, std::string& error) {
    const auto contents = staging / px::PathFromUtf8(request.appName + ".app") / "Contents";
    const auto macos = contents / "MacOS";
    const auto frameworks = contents / "Frameworks";
    const auto resourcesDirectory = contents / "Resources";
    const auto player = staging / px::PathFromUtf8(request.playerFile);
    std::error_code code;
    for (const auto& directory : {macos, frameworks, resourcesDirectory}) {
        std::filesystem::create_directories(directory, code);
        if (code) {
            error = "app bundle folder could not be created: " + code.message();
            return false;
        }
    }
    const auto bundleRoot = contents.parent_path();
    std::vector<std::filesystem::path> libraries;
    for (std::filesystem::directory_iterator iterator(staging, code);
         !code && iterator != std::filesystem::directory_iterator(); iterator.increment(code))
        if (iterator->is_regular_file() && IsLibrary(px::PathToUtf8(iterator->path().filename()), LayoutPlatform::MacOS))
            libraries.push_back(iterator->path());
    for (const auto& library : libraries) {
        std::filesystem::rename(library, frameworks / library.filename(), code);
        if (code) {
            error = "runtime library could not be moved into Frameworks: " + code.message();
            return false;
        }
    }
    const auto bundledPlayer = macos / px::PathFromUtf8(request.playerFile);
    std::filesystem::rename(player, bundledPlayer, code);
    if (code) {
        error = "the Player could not be moved into the app bundle: " + code.message();
        return false;
    }
    MarkExecutable(bundledPlayer);
    if (!MoveEntries(staging, resourcesDirectory, {bundleRoot}, error)) return false;
    std::string iconFile;
    if (!request.icon.empty()) {
        std::string iconError;
        const auto image = IconImage(request.icon, iconError);
        const Bytes icns = image ? BuildIcns(*image, iconError) : Bytes{};
        if (icns.empty()) {
            error = "icon could not be converted to .icns: " + iconError;
            return false;
        }
        iconFile = Stem(request.playerFile) + ".icns";
        if (!WriteFileBytes(resourcesDirectory / px::PathFromUtf8(iconFile),
                            std::string_view(reinterpret_cast<const char*>(icns.data()), icns.size()))) {
            error = "the .icns icon could not be written";
            return false;
        }
    }
    const auto executable = ReadFileBytes(bundledPlayer, 1024ull * 1024ull * 1024ull);
    const auto minimum = executable ? MachOMinimumSystemVersion(*executable) : std::nullopt;
    if (!minimum) outcome.notes.push_back("The Player has no macOS deployment target; LSMinimumSystemVersion is 11.0");
    const std::string plist = InfoPlist({
        .bundleName = request.displayName,
        .executable = request.playerFile,
        .identifier = request.bundleIdentifier,
        .version = request.version,
        .iconFile = iconFile,
        .minimumSystemVersion = minimum.value_or("11.0"),
        .copyright = request.copyright});
    if (!WriteFileBytes(contents / "Info.plist", plist) || !WriteFileBytes(contents / "PkgInfo", "APPL????")) {
        error = "Info.plist could not be written";
        return false;
    }
    std::filesystem::remove(frameworks, code);  // only when empty
    outcome.player = bundledPlayer;
    return true;
}

bool ApplyLinuxLayout(const std::filesystem::path& staging, const LayoutRequest& request,
                      LayoutOutcome& outcome, std::string& error) {
    const auto player = staging / px::PathFromUtf8(request.playerFile);
    std::error_code code;
    std::vector<std::filesystem::path> libraries;
    for (std::filesystem::directory_iterator iterator(staging, code);
         !code && iterator != std::filesystem::directory_iterator(); iterator.increment(code))
        if (iterator->is_regular_file() && IsLibrary(px::PathToUtf8(iterator->path().filename()), LayoutPlatform::Linux))
            libraries.push_back(iterator->path());
    if (!libraries.empty()) {
        std::filesystem::create_directories(staging / "lib", code);
        for (const auto& library : libraries) {
            if (!code) std::filesystem::rename(library, staging / "lib" / library.filename(), code);
        }
        if (code) {
            error = "runtime libraries could not be moved into lib/: " + code.message();
            return false;
        }
    }
    MarkExecutable(player);
    const std::string stem = Stem(request.playerFile);
    if (!request.icon.empty()) {
        std::string iconError;
        const auto image = IconImage(request.icon, iconError);
        if (!image) {
            error = "icon could not be read for the Linux icon theme: " + iconError;
            return false;
        }
        for (const int size : kLinuxIconSizes) {
            const auto directory = staging / "icons/hicolor" /
                (std::to_string(size) + "x" + std::to_string(size)) / "apps";
            std::filesystem::create_directories(directory, code);
            const Bytes png = code ? Bytes{} : EncodePng(ResizeSquare(*image, size), iconError);
            if (png.empty() ||
                !WriteFileBytes(directory / px::PathFromUtf8(stem + ".png"),
                                std::string_view(reinterpret_cast<const char*>(png.data()), png.size()))) {
                error = "Linux icon could not be written: " + iconError;
                return false;
            }
        }
    }
    if (!WriteFileBytes(staging / px::PathFromUtf8(stem + ".desktop"),
                        DesktopEntry({.name = request.displayName, .executable = request.playerFile,
                                      .icon = request.icon.empty() ? std::string{} : stem}))) {
        error = "the .desktop entry could not be written";
        return false;
    }
    outcome.player = player;
    return true;
}

}  // namespace

std::filesystem::path LaidOutPlayer(const LayoutRequest& request) {
    const auto file = px::PathFromUtf8(request.playerFile);
    if (request.platform == LayoutPlatform::MacOS && request.platformLayout)
        return px::PathFromUtf8(request.appName + ".app") / "Contents" / "MacOS" / file;
    return file;
}

bool ApplyLayout(const std::filesystem::path& staging, const LayoutRequest& request, LayoutOutcome& outcome,
                 std::string& error) {
    if (request.platform != LayoutPlatform::Windows && !request.engineFolder.empty())
        outcome.notes.push_back("layout \"bundled\" applies to Windows; this platform uses its own layout");
    switch (request.platform) {
        case LayoutPlatform::Windows: return ApplyWindowsLayout(staging, request, outcome, error);
        case LayoutPlatform::MacOS:
            if (request.platformLayout) return ApplyMacLayout(staging, request, outcome, error);
            break;
        case LayoutPlatform::Linux:
            if (request.platformLayout) return ApplyLinuxLayout(staging, request, outcome, error);
            break;
        default: break;
    }
    if (!request.icon.empty()) outcome.notes.push_back("Executable icons need a platform layout here; icon ignored");
    outcome.player = staging / px::PathFromUtf8(request.playerFile);
    return true;
}

}  // namespace px::sdk::branding
