#include "Applications/Packager/MediaTranscoder.h"
#include "Engine/Support/Utf8Path.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace transcode = px::sdk::transcode;

// A sprite-like image: smooth colour, a little grain, an opaque body, a soft
// edge and a fully transparent corner whose hidden RGB is not black.
SDL_Surface* SpriteSurface(const int width, const int height) {
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    std::uint32_t state = 12345;
    for (int y = 0; y < height; ++y) {
        auto* row = static_cast<std::uint8_t*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < width; ++x) {
            state = state * 1664525u + 1013904223u;
            const int grain = static_cast<int>(state >> 29);
            row[x * 4 + 0] = static_cast<std::uint8_t>((x * 255) / width);
            row[x * 4 + 1] = static_cast<std::uint8_t>((y * 255) / height);
            row[x * 4 + 2] = static_cast<std::uint8_t>(128 + grain);
            const int edge = x + y;
            row[x * 4 + 3] = edge < 24 ? 0 : edge < 56 ? static_cast<std::uint8_t>((edge - 24) * 8) : 255;
        }
    }
    return surface;
}

Bytes EncodePng(SDL_Surface* surface) {
    SDL_IOStream* io = SDL_IOFromDynamicMem();
    Bytes bytes;
    if (io && IMG_SavePNG_IO(surface, io, false)) {
        const auto size = SDL_GetIOSize(io);
        bytes.resize(static_cast<std::size_t>(size));
        SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
        SDL_ReadIO(io, bytes.data(), bytes.size());
    }
    if (io) SDL_CloseIO(io);
    return bytes;
}

// What AssetCache does with a resource: decode by content, not extension.
SDL_Surface* DecodeLikeRuntime(const Bytes& bytes) {
    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    SDL_Surface* decoded = io ? IMG_Load_IO(io, true) : nullptr;
    if (!decoded) return nullptr;
    SDL_Surface* rgba = SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(decoded);
    return rgba;
}

const std::uint8_t* Pixel(const SDL_Surface* surface, const int x, const int y) {
    return static_cast<const std::uint8_t*>(surface->pixels) + y * surface->pitch + x * 4;
}

void PutLe(Bytes& bytes, const std::uint32_t value, const int count) {
    for (int index = 0; index < count; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
}

// 16-bit PCM WAV: a chord with some noise, so codecs have real work to do.
Bytes Wav(const int rate, const int channels, const double seconds) {
    const auto frames = static_cast<std::uint32_t>(rate * seconds);
    const std::uint32_t dataBytes = frames * static_cast<std::uint32_t>(channels) * 2u;
    Bytes bytes;
    const auto text = [&bytes](const char* value) { bytes.insert(bytes.end(), value, value + 4); };
    text("RIFF");
    PutLe(bytes, 36 + dataBytes, 4);
    text("WAVE");
    text("fmt ");
    PutLe(bytes, 16, 4);
    PutLe(bytes, 1, 2);
    PutLe(bytes, static_cast<std::uint32_t>(channels), 2);
    PutLe(bytes, static_cast<std::uint32_t>(rate), 4);
    PutLe(bytes, static_cast<std::uint32_t>(rate * channels * 2), 4);
    PutLe(bytes, static_cast<std::uint32_t>(channels * 2), 2);
    PutLe(bytes, 16, 2);
    text("data");
    PutLe(bytes, dataBytes, 4);
    std::uint32_t state = 99;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            state = state * 1664525u + 1013904223u;
            const double t = static_cast<double>(frame) / rate;
            const double value = 0.3 * std::sin(2 * 3.14159265 * 440 * t) +
                                 0.2 * std::sin(2 * 3.14159265 * (660 + channel * 2) * t) +
                                 0.02 * (static_cast<double>(state >> 16) / 65535.0 - 0.5);
            PutLe(bytes, static_cast<std::uint32_t>(static_cast<std::int16_t>(value * 32767)), 2);
        }
    }
    return bytes;
}

// Decodes the way AudioEngine does (MIX_LoadAudio_IO, no type hint) and
// returns the duration in seconds, or a negative value when it fails.
double RuntimeDuration(const Bytes& bytes, SDL_AudioSpec* spec = nullptr) {
    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    MIX_Audio* audio = io ? MIX_LoadAudio_IO(nullptr, io, true, true) : nullptr;
    if (!audio) return -1.0;
    SDL_AudioSpec format{};
    MIX_GetAudioFormat(audio, &format);
    if (spec) *spec = format;
    const auto frames = MIX_GetAudioDuration(audio);
    MIX_DestroyAudio(audio);
    return frames < 0 || format.freq <= 0 ? -1.0 : static_cast<double>(frames) / format.freq;
}

bool Contains(const Bytes& bytes, const std::string_view text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

void WriteFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main() {
    px::test::Suite suite("MediaTranscoder");
    suite.Require(MIX_Init(), "SDL_mixer initialises", SDL_GetError());

    suite.Run("LossyWebPKeepsAlphaExactly", [&] {
        SDL_Surface* source = SpriteSurface(256, 192);
        const Bytes png = EncodePng(source);
        suite.Require(!png.empty(), "fixture PNG encodes");
        const auto outcome = transcode::TranscodeImage(png, {.lossless = false, .quality = 90, .effort = 4});
        suite.Require(outcome.encoded, "a sprite PNG re-encodes smaller", outcome.reason + ": " + outcome.detail);
        suite.Expect(outcome.bytes.size() < png.size(), "lossy WebP is smaller than the PNG");
        suite.Expect(std::memcmp(outcome.bytes.data(), "RIFF", 4) == 0 &&
                         std::memcmp(outcome.bytes.data() + 8, "WEBP", 4) == 0,
                     "output is a WebP file");
        SDL_Surface* decoded = DecodeLikeRuntime(outcome.bytes);
        suite.Require(decoded && decoded->w == 256 && decoded->h == 192,
                      "the runtime decoder reads the WebP at full size");
        long colourError = 0;
        int alphaMismatches = 0;
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 256; ++x) {
                const auto* before = Pixel(source, x, y);
                const auto* after = Pixel(decoded, x, y);
                alphaMismatches += before[3] != after[3];
                if (before[3] == 255)
                    for (int channel = 0; channel < 3; ++channel)
                        colourError += std::abs(before[channel] - after[channel]);
            }
        suite.Expect(alphaMismatches == 0, "alpha survives lossy colour exactly",
                     std::to_string(alphaMismatches) + " pixels differ");
        suite.Expect(colourError / (256 * 192 * 3) < 4, "opaque colour stays close",
                     std::to_string(colourError / (256 * 192 * 3)) + " mean error");
        SDL_DestroySurface(decoded);
        SDL_DestroySurface(source);
    });

    suite.Run("LosslessWebPIsPixelExactIncludingHiddenColour", [&] {
        SDL_Surface* source = SpriteSurface(128, 96);
        const Bytes png = EncodePng(source);
        const auto outcome = transcode::TranscodeImage(png, {.lossless = true, .quality = 90, .effort = 4});
        suite.Require(outcome.encoded, "lossless re-encoding is smaller", outcome.reason + ": " + outcome.detail);
        SDL_Surface* decoded = DecodeLikeRuntime(outcome.bytes);
        suite.Require(decoded != nullptr, "the runtime decoder reads lossless WebP");
        bool identical = true;
        for (int y = 0; y < 96 && identical; ++y)
            identical = std::memcmp(Pixel(source, 0, y), Pixel(decoded, 0, y), 128 * 4) == 0;
        suite.Expect(identical, "every RGBA byte matches, also under transparent pixels");
        SDL_DestroySurface(decoded);
        SDL_DestroySurface(source);
    });

    suite.Run("ImagesThatCannotShrinkStayOriginal", [&] {
        SDL_Surface* source = SpriteSurface(64, 64);
        const Bytes png = EncodePng(source);
        SDL_DestroySurface(source);
        const auto webp = transcode::TranscodeImage(png, {.lossless = false, .quality = 80, .effort = 0});
        suite.Require(webp.encoded, "fixture WebP");
        const auto again = transcode::TranscodeImage(webp.bytes, {});
        suite.Expect(!again.encoded && again.reason == "unsupported", "WebP is not re-encoded");
        Bytes broken = png;
        broken.resize(40);
        const auto damaged = transcode::TranscodeImage(broken, {});
        suite.Expect(!damaged.encoded && damaged.reason == "failed", "a damaged PNG stays original");
    });

    suite.Run("WavToOpusDecodesAtFullLength", [&] {
        const Bytes wav = Wav(44100, 2, 1.5);
        const auto outcome = transcode::TranscodeAudio(wav, {.codec = transcode::AudioCodec::Opus,
                                                             .bitrate = 96000});
        suite.Require(outcome.encoded, "stereo WAV becomes Opus", outcome.reason + ": " + outcome.detail);
        suite.Expect(Contains(outcome.bytes, "OpusHead") && Contains(outcome.bytes, "OpusTags"),
                     "Ogg Opus headers");
        SDL_AudioSpec spec{};
        const double seconds = RuntimeDuration(outcome.bytes, &spec);
        suite.Expect(spec.channels == 2, "channels kept");
        suite.Expect(std::abs(seconds - 1.5) < 0.002, "pre-skip and end trim keep the exact length",
                     std::to_string(seconds) + " s");
    });

    suite.Run("VorbisReencodesAndFeedsOpus", [&] {
        const Bytes wav = Wav(48000, 1, 2.0);
        const auto vorbis = transcode::TranscodeAudio(wav, {.codec = transcode::AudioCodec::Vorbis,
                                                            .quality = 2.0});
        suite.Require(vorbis.encoded, "WAV becomes Vorbis", vorbis.reason + ": " + vorbis.detail);
        suite.Expect(std::abs(RuntimeDuration(vorbis.bytes) - 2.0) < 0.002, "Vorbis keeps the length");
        const auto opus = transcode::TranscodeAudio(vorbis.bytes, {.codec = transcode::AudioCodec::Opus,
                                                                   .bitrate = 32000});
        suite.Require(opus.encoded, "Ogg Vorbis becomes smaller Opus", opus.reason + ": " + opus.detail);
        suite.Expect(std::abs(RuntimeDuration(opus.bytes) - 2.0) < 0.002, "Opus keeps the length");
        const auto twice = transcode::TranscodeAudio(opus.bytes, {});
        suite.Expect(!twice.encoded && twice.reason == "unsupported", "Opus is not re-encoded");
        const auto bigger = transcode::TranscodeAudio(vorbis.bytes, {.codec = transcode::AudioCodec::Opus,
                                                                     .bitrate = 510000});
        suite.Expect(!bigger.encoded && bigger.reason == "larger",
                     "a re-encoding that grows keeps the original", bigger.reason);
    });

    suite.Run("RequestEncodesEveryJobAndReportsEach", [&] {
        px::test::TempDirectory temp("media-transcoder");
        SDL_Surface* surface = SpriteSurface(200, 120);
        WriteFile(temp.path / "sprite.png", EncodePng(surface));
        SDL_DestroySurface(surface);
        WriteFile(temp.path / "voice.wav", Wav(48000, 1, 1.0));
        const nlohmann::json request{
            {"format", "PrismatiXTranscodeRequest"}, {"schemaRevision", 1}, {"threads", 3},
            {"images", {{"lossless", false}, {"quality", 85}, {"effort", 2}}},
            {"audio", {{"codec", "opus"}, {"bitrate", 48000}}},
            {"jobs", {
                {{"kind", "image"}, {"source", px::PathToUtf8(temp.path / "sprite.png")},
                 {"output", px::PathToUtf8(temp.path / "sprite.out")}},
                {{"kind", "audio"}, {"source", px::PathToUtf8(temp.path / "voice.wav")},
                 {"output", px::PathToUtf8(temp.path / "voice.out")}},
                {{"kind", "image"}, {"source", px::PathToUtf8(temp.path / "missing.png")},
                 {"output", px::PathToUtf8(temp.path / "missing.out")}}}}};
        const auto requestPath = temp.path / "request.json";
        std::ofstream(requestPath) << request.dump();
        std::vector<nlohmann::json> events;
        const int code = transcode::RunTranscodeRequest(
            requestPath, [&](const std::string& line) { events.push_back(nlohmann::json::parse(line)); });
        suite.Expect(code == 0, "request completes");
        suite.Require(events.size() == 4 && events.back()["event"] == "completed",
                      "one event per job and a completion event");
        std::vector<std::string> results(3);
        for (std::size_t index = 0; index + 1 < events.size(); ++index)
            results[events[index]["index"].get<std::size_t>()] = events[index]["result"].get<std::string>();
        suite.Expect(results[0] == "encoded" && results[1] == "encoded" && results[2] == "kept",
                     "image and audio encode; a missing source is kept, not fatal");
        suite.Expect(std::filesystem::exists(temp.path / "sprite.out") &&
                         std::filesystem::exists(temp.path / "voice.out") &&
                         !std::filesystem::exists(temp.path / "missing.out"),
                     "outputs are written only for encoded jobs");
        suite.Expect(events.back()["failed"] == 1, "the missing source counts as failed");
        std::vector<nlohmann::json> rejected;
        std::ofstream(requestPath) << R"({"format":"PrismatiXTranscodeRequest","schemaRevision":1,"audio":{"codec":"mp3"},"jobs":[]})";
        suite.Expect(transcode::RunTranscodeRequest(requestPath, [&](const std::string& line) {
                         rejected.push_back(nlohmann::json::parse(line));
                     }) != 0 && rejected.size() == 1 && rejected[0]["event"] == "failed",
                     "an unknown audio codec is rejected");
    });

    MIX_Quit();
    return suite.Finish();
}
