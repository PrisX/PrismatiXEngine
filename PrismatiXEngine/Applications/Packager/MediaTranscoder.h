#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

// Re-encodes a game's media for smaller distributions (distribution.transcode
// in prismatix.json). The Player keeps reading the original logical paths:
// SDL_image and SDL_mixer recognise WebP and Ogg Opus by their content, not by
// the file extension, so a ".png" that now holds WebP still decodes.
namespace px::sdk::transcode {

struct ImageOptions {
    bool lossless = false;
    // 1-100 (lossy only).
    int quality = 90;
    // 0 (fastest) to 6 (smallest): libwebp's method, or its lossless level.
    int effort = 4;
};

enum class AudioCodec : std::uint8_t { Opus, Vorbis };

struct AudioOptions {
    AudioCodec codec = AudioCodec::Opus;
    // Opus target in bits per second for the whole stream (all channels).
    int bitrate = 96000;
    // Vorbis VBR quality on oggenc's -1..10 scale.
    double quality = 4.0;
};

struct Outcome {
    // True when `bytes` holds a smaller re-encoding of the input. Otherwise
    // the original stays in the package and `reason` says why.
    bool encoded = false;
    // "larger", "unsupported" or "failed": only "failed" is worth a warning.
    std::string reason;
    std::string detail;
    std::vector<std::uint8_t> bytes;
};

// PNG (or any image SDL_image decodes) to WebP. Alpha is kept exactly: lossy
// colour uses lossless alpha, and colours under transparent pixels are not
// rewritten, so filtered edges look as they did.
[[nodiscard]] Outcome TranscodeImage(std::span<const std::uint8_t> input,
                                     const ImageOptions& options);
// Ogg Vorbis or WAV to Ogg Opus or Ogg Vorbis. Vorbis comments carry over.
[[nodiscard]] Outcome TranscodeAudio(std::span<const std::uint8_t> input,
                                     const AudioOptions& options);

// `PrismatiXPackager --transcode <request.json>`: encodes every job of the
// request on all cores and prints one NDJSON event per job. Returns the
// process exit code.
int RunTranscodeRequest(const std::filesystem::path& request,
                        const std::function<void(const std::string&)>& emit);

}  // namespace px::sdk::transcode
