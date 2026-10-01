#include "Applications/Packager/MediaTranscoder.h"

#include "Engine/Support/Utf8Path.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#if PRISMATIX_MEDIA_ENCODERS
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <vorbis/vorbisenc.h>
// Only the memory callbacks below are used; skip vorbisfile's static stdio ones.
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>
#include <webp/decode.h>
#include <webp/encode.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace px::sdk::transcode {

namespace {

using Bytes = std::vector<std::uint8_t>;

Outcome Kept(std::string reason, std::string detail) {
    Outcome outcome;
    outcome.reason = std::move(reason);
    outcome.detail = std::move(detail);
    return outcome;
}

// A re-encoding only replaces the original when it is smaller.
[[maybe_unused]] Outcome Finish(Bytes bytes, const std::size_t inputSize) {
    if (bytes.empty()) return Kept("failed", "encoder produced no data");
    if (bytes.size() >= inputSize)
        return Kept("larger", std::to_string(bytes.size()) + " bytes re-encoded, " +
                                  std::to_string(inputSize) + " original");
    Outcome outcome;
    outcome.encoded = true;
    outcome.bytes = std::move(bytes);
    return outcome;
}

[[maybe_unused]] bool StartsWith(std::span<const std::uint8_t> data, const std::size_t offset,
                                const std::string_view text) {
    return data.size() >= offset + text.size() &&
           std::memcmp(data.data() + offset, text.data(), text.size()) == 0;
}

[[maybe_unused]] bool ContainsWithin(std::span<const std::uint8_t> data,
                                    const std::string_view text, const std::size_t limit) {
    const auto end = data.begin() + static_cast<std::ptrdiff_t>(std::min(limit, data.size()));
    return std::search(data.begin(), end, text.begin(), text.end()) != end;
}

// FNV-1a: a deterministic Ogg serial number, so identical inputs give
// byte-identical outputs (and package diffs stay empty).
[[maybe_unused]] std::uint32_t StreamSerial(std::span<const std::uint8_t> data) {
    std::uint32_t hash = 2166136261u;
    for (const auto byte : data.first(std::min<std::size_t>(data.size(), 65536))) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash ^ static_cast<std::uint32_t>(data.size());
}

}  // namespace

#if PRISMATIX_MEDIA_ENCODERS
Outcome TranscodeImage(std::span<const std::uint8_t> input, const ImageOptions& options) {
    if (StartsWith(input, 0, "RIFF") && StartsWith(input, 8, "WEBP"))
        return Kept("unsupported", "already WebP");
    SDL_IOStream* stream = SDL_IOFromConstMem(input.data(), input.size());
    SDL_Surface* decoded = stream ? IMG_Load_IO(stream, true) : nullptr;
    if (!decoded) return Kept("failed", std::string("image did not decode: ") + SDL_GetError());
    SDL_Surface* rgba = SDL_ConvertSurface(decoded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(decoded);
    if (!rgba) return Kept("failed", std::string("image did not convert: ") + SDL_GetError());
    if (rgba->w > WEBP_MAX_DIMENSION || rgba->h > WEBP_MAX_DIMENSION) {
        const auto size = std::to_string(rgba->w) + "x" + std::to_string(rgba->h);
        SDL_DestroySurface(rgba);
        return Kept("unsupported", size + " exceeds WebP's 16383 pixel limit");
    }

    WebPConfig config;
    WebPPicture picture;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&picture)) {
        SDL_DestroySurface(rgba);
        return Kept("failed", "libwebp version mismatch");
    }
    const int effort = std::clamp(options.effort, 0, 6);
    if (options.lossless) {
        WebPConfigLosslessPreset(&config, effort);
    } else {
        config.quality = static_cast<float>(std::clamp(options.quality, 1, 100));
        config.method = effort;
        // Lossy colour, exact alpha: hit masks and soft edges stay as drawn.
        config.alpha_compression = 1;
        config.alpha_quality = 100;
        config.use_sharp_yuv = 1;
    }
    // Keep the RGB under transparent pixels. Textures are filtered with
    // straight alpha, so zeroed colours there would darken sprite outlines.
    config.exact = 1;
    picture.use_argb = 1;
    picture.width = rgba->w;
    picture.height = rgba->h;
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    bool encoded = WebPValidateConfig(&config) &&
                   WebPPictureImportRGBA(&picture, static_cast<const std::uint8_t*>(rgba->pixels),
                                         rgba->pitch) &&
                   WebPEncode(&config, &picture);
    const int errorCode = picture.error_code;
    WebPPictureFree(&picture);
    SDL_DestroySurface(rgba);
    Bytes bytes;
    if (encoded) bytes.assign(writer.mem, writer.mem + writer.size);
    WebPMemoryWriterClear(&writer);
    if (!encoded) return Kept("failed", "WebP encoding failed (libwebp error " +
                                            std::to_string(errorCode) + ")");
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height))
        return Kept("failed", "WebP output did not parse");
    return Finish(std::move(bytes), input.size());
}

namespace {

struct Pcm {
    int channels = 0;
    int rate = 0;
    std::vector<float> samples;  // interleaved
    std::vector<std::string> comments;
};

struct MemoryReader {
    std::span<const std::uint8_t> data;
    std::size_t position = 0;
};

std::size_t ReadMemory(void* destination, std::size_t size, std::size_t count, void* source) {
    auto& reader = *static_cast<MemoryReader*>(source);
    if (size == 0) return 0;
    const std::size_t items = std::min(count, (reader.data.size() - reader.position) / size);
    std::memcpy(destination, reader.data.data() + reader.position, items * size);
    reader.position += items * size;
    return items;
}

int SeekMemory(void* source, ogg_int64_t offset, int whence) {
    auto& reader = *static_cast<MemoryReader*>(source);
    ogg_int64_t base = 0;
    if (whence == SEEK_CUR) base = static_cast<ogg_int64_t>(reader.position);
    else if (whence == SEEK_END) base = static_cast<ogg_int64_t>(reader.data.size());
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(reader.data.size())) return -1;
    reader.position = static_cast<std::size_t>(target);
    return 0;
}

long TellMemory(void* source) {
    return static_cast<long>(static_cast<MemoryReader*>(source)->position);
}

std::optional<Pcm> DecodeVorbis(std::span<const std::uint8_t> input, std::string& error) {
    MemoryReader reader{input, 0};
    OggVorbis_File file;
    const ov_callbacks callbacks{ReadMemory, SeekMemory, nullptr, TellMemory};
    if (ov_open_callbacks(&reader, &file, nullptr, 0, callbacks) != 0) {
        error = "not an Ogg Vorbis stream";
        return std::nullopt;
    }
    std::optional<Pcm> pcm;
    if (ov_streams(&file) != 1) {
        error = "chained Ogg streams are not re-encoded";
    } else if (const vorbis_info* info = ov_info(&file, -1); !info) {
        error = "Vorbis stream has no header";
    } else {
        Pcm value;
        value.channels = info->channels;
        value.rate = static_cast<int>(info->rate);
        if (const vorbis_comment* comment = ov_comment(&file, -1)) {
            for (int index = 0; index < comment->comments; ++index)
                value.comments.emplace_back(comment->user_comments[index],
                                            static_cast<std::size_t>(comment->comment_lengths[index]));
        }
        const auto total = ov_pcm_total(&file, -1);
        if (total > 0)
            value.samples.reserve(static_cast<std::size_t>(total) *
                                  static_cast<std::size_t>(value.channels));
        bool failed = false;
        for (;;) {
            float** planes = nullptr;
            int section = 0;
            const long count = ov_read_float(&file, &planes, 4096, &section);
            if (count == 0) break;
            if (count < 0) { failed = true; break; }
            for (long frame = 0; frame < count; ++frame)
                for (int channel = 0; channel < value.channels; ++channel)
                    value.samples.push_back(planes[channel][frame]);
        }
        if (failed) error = "Vorbis stream is damaged";
        else pcm = std::move(value);
    }
    ov_clear(&file);
    return pcm;
}

// Converts with SDL's resampler; `rate` 0 keeps the source rate.
bool ConvertPcm(const SDL_AudioSpec& source, const void* data, const int bytes,
                const int rate, Pcm& output) {
    const SDL_AudioSpec target{SDL_AUDIO_F32, source.channels, rate > 0 ? rate : source.freq};
    SDL_AudioStream* stream = SDL_CreateAudioStream(&source, &target);
    if (!stream) return false;
    bool ok = SDL_PutAudioStreamData(stream, data, bytes) && SDL_FlushAudioStream(stream);
    std::vector<float> samples;
    std::vector<float> chunk(16384);
    while (ok) {
        const int got = SDL_GetAudioStreamData(stream, chunk.data(),
                                               static_cast<int>(chunk.size() * sizeof(float)));
        if (got < 0) ok = false;
        if (got <= 0) break;
        samples.insert(samples.end(), chunk.begin(),
                       chunk.begin() + got / static_cast<int>(sizeof(float)));
    }
    SDL_DestroyAudioStream(stream);
    if (!ok) return false;
    output.channels = target.channels;
    output.rate = target.freq;
    output.samples = std::move(samples);
    return true;
}

std::optional<Pcm> DecodeWav(std::span<const std::uint8_t> input, std::string& error) {
    SDL_IOStream* stream = SDL_IOFromConstMem(input.data(), input.size());
    SDL_AudioSpec spec{};
    Uint8* buffer = nullptr;
    Uint32 length = 0;
    if (!stream || !SDL_LoadWAV_IO(stream, true, &spec, &buffer, &length)) {
        error = std::string("WAV did not decode: ") + SDL_GetError();
        return std::nullopt;
    }
    Pcm pcm;
    const bool converted = ConvertPcm(spec, buffer, static_cast<int>(length), 0, pcm);
    SDL_free(buffer);
    if (!converted) {
        error = std::string("WAV did not convert: ") + SDL_GetError();
        return std::nullopt;
    }
    return pcm;
}

void AppendPage(Bytes& output, const ogg_page& page) {
    output.insert(output.end(), page.header, page.header + page.header_len);
    output.insert(output.end(), page.body, page.body + page.body_len);
}

void FlushPages(ogg_stream_state& stream, Bytes& output) {
    ogg_page page;
    while (ogg_stream_flush(&stream, &page) != 0) AppendPage(output, page);
}

void PutLe16(Bytes& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutLe32(Bytes& bytes, const std::uint32_t value) {
    PutLe16(bytes, value & 0xffffu);
    PutLe16(bytes, value >> 16);
}

void PutText(Bytes& bytes, const std::string_view text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
}

constexpr std::string_view kVendor = "PrismatiX media transcoder";

// RFC 7845: Ogg Opus with an OpusHead and an OpusTags header page, granule
// positions in 48 kHz samples including the encoder's pre-skip, and an end
// trim on the last page so the decoded length equals the input.
Outcome EncodeOpus(const Pcm& source, const AudioOptions& options, const std::size_t inputSize,
                   const std::uint32_t serial) {
    if (source.channels < 1 || source.channels > 2)
        return Kept("unsupported", std::to_string(source.channels) +
                                       " channels; Opus re-encoding takes mono or stereo");
    Pcm resampled;
    const Pcm* pcm = &source;
    if (source.rate != 48000) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, source.channels, source.rate};
        if (!ConvertPcm(spec, source.samples.data(),
                        static_cast<int>(source.samples.size() * sizeof(float)), 48000, resampled))
            return Kept("failed", std::string("resampling failed: ") + SDL_GetError());
        resampled.comments = source.comments;
        pcm = &resampled;
    }
    const auto channels = static_cast<std::size_t>(pcm->channels);
    const std::size_t total = pcm->samples.size() / channels;
    if (total == 0) return Kept("unsupported", "no audio samples");

    int error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(48000, pcm->channels, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) return Kept("failed", opus_strerror(error));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(std::clamp(options.bitrate, 6000, 510000)));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    opus_int32 preSkip = 0;
    opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&preSkip));

    Bytes output;
    ogg_stream_state stream;
    ogg_stream_init(&stream, static_cast<int>(serial));
    Bytes head;
    PutText(head, "OpusHead");
    head.push_back(1);
    head.push_back(static_cast<std::uint8_t>(pcm->channels));
    PutLe16(head, static_cast<std::uint32_t>(preSkip));
    PutLe32(head, static_cast<std::uint32_t>(source.rate));
    PutLe16(head, 0);
    head.push_back(0);
    Bytes tags;
    PutText(tags, "OpusTags");
    PutLe32(tags, static_cast<std::uint32_t>(kVendor.size()));
    PutText(tags, kVendor);
    PutLe32(tags, static_cast<std::uint32_t>(pcm->comments.size()));
    for (const auto& comment : pcm->comments) {
        PutLe32(tags, static_cast<std::uint32_t>(comment.size()));
        PutText(tags, comment);
    }
    ogg_packet packet{};
    packet.packet = head.data();
    packet.bytes = static_cast<long>(head.size());
    packet.b_o_s = 1;
    ogg_stream_packetin(&stream, &packet);
    FlushPages(stream, output);
    packet = {};
    packet.packet = tags.data();
    packet.bytes = static_cast<long>(tags.size());
    packet.packetno = 1;
    ogg_stream_packetin(&stream, &packet);
    FlushPages(stream, output);

    constexpr std::size_t kFrame = 960;  // 20 ms
    const std::size_t end = total + static_cast<std::size_t>(preSkip);
    std::vector<float> frame(kFrame * channels);
    std::vector<unsigned char> encoded(4000);
    bool ok = true;
    std::int64_t packetNumber = 2;
    for (std::size_t start = 0; start < end && ok; start += kFrame) {
        std::fill(frame.begin(), frame.end(), 0.0f);
        if (start < total) {
            const std::size_t count = std::min(kFrame, total - start);
            std::copy_n(pcm->samples.begin() + static_cast<std::ptrdiff_t>(start * channels),
                        count * channels, frame.begin());
        }
        const int bytes = opus_encode_float(encoder, frame.data(), static_cast<int>(kFrame),
                                            encoded.data(), static_cast<opus_int32>(encoded.size()));
        if (bytes < 0) { ok = false; break; }
        const bool last = start + kFrame >= end;
        packet = {};
        packet.packet = encoded.data();
        packet.bytes = bytes;
        packet.e_o_s = last ? 1 : 0;
        packet.granulepos = static_cast<ogg_int64_t>(std::min(start + kFrame, end));
        packet.packetno = packetNumber++;
        ogg_stream_packetin(&stream, &packet);
        ogg_page page;
        while (ogg_stream_pageout(&stream, &page) != 0) AppendPage(output, page);
    }
    FlushPages(stream, output);
    ogg_stream_clear(&stream);
    opus_encoder_destroy(encoder);
    if (!ok) return Kept("failed", "Opus encoding failed");
    return Finish(std::move(output), inputSize);
}

Outcome EncodeVorbis(const Pcm& pcm, const AudioOptions& options, const std::size_t inputSize,
                     const std::uint32_t serial) {
    if (pcm.channels < 1 || pcm.channels > 255 || pcm.samples.empty())
        return Kept("unsupported", "no audio samples");
    vorbis_info info;
    vorbis_info_init(&info);
    const float quality = static_cast<float>(std::clamp(options.quality, -1.0, 10.0) / 10.0);
    if (vorbis_encode_init_vbr(&info, pcm.channels, pcm.rate, quality) != 0) {
        vorbis_info_clear(&info);
        return Kept("unsupported", "Vorbis cannot encode " + std::to_string(pcm.channels) +
                                       " channels at " + std::to_string(pcm.rate) + " Hz");
    }
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    for (const auto& text : pcm.comments) vorbis_comment_add(&comment, text.c_str());
    vorbis_dsp_state dsp;
    vorbis_block block;
    vorbis_analysis_init(&dsp, &info);
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream;
    ogg_stream_init(&stream, static_cast<int>(serial));
    ogg_packet identification;
    ogg_packet comments;
    ogg_packet codebooks;
    vorbis_analysis_headerout(&dsp, &comment, &identification, &comments, &codebooks);
    ogg_stream_packetin(&stream, &identification);
    ogg_stream_packetin(&stream, &comments);
    ogg_stream_packetin(&stream, &codebooks);
    Bytes output;
    FlushPages(stream, output);

    const auto drain = [&] {
        ogg_packet packet;
        ogg_page page;
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            while (vorbis_bitrate_flushpacket(&dsp, &packet) != 0) {
                ogg_stream_packetin(&stream, &packet);
                while (ogg_stream_pageout(&stream, &page) != 0) AppendPage(output, page);
            }
        }
    };
    const auto channels = static_cast<std::size_t>(pcm.channels);
    const std::size_t total = pcm.samples.size() / channels;
    constexpr std::size_t kChunk = 1024;
    for (std::size_t start = 0; start < total; start += kChunk) {
        const std::size_t count = std::min(kChunk, total - start);
        float** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(count));
        for (std::size_t frame = 0; frame < count; ++frame)
            for (std::size_t channel = 0; channel < channels; ++channel)
                buffer[channel][frame] = pcm.samples[(start + frame) * channels + channel];
        vorbis_analysis_wrote(&dsp, static_cast<int>(count));
        drain();
    }
    vorbis_analysis_wrote(&dsp, 0);
    drain();
    FlushPages(stream, output);
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return Finish(std::move(output), inputSize);
}

}  // namespace

Outcome TranscodeAudio(std::span<const std::uint8_t> input, const AudioOptions& options) {
    const bool ogg = StartsWith(input, 0, "OggS");
    if (ogg && ContainsWithin(input, "OpusHead", 256))
        return Kept("unsupported", "already Opus");
    std::string error;
    std::optional<Pcm> pcm;
    if (ogg) pcm = DecodeVorbis(input, error);
    else if (StartsWith(input, 0, "RIFF") && StartsWith(input, 8, "WAVE")) pcm = DecodeWav(input, error);
    else return Kept("unsupported", "only Ogg Vorbis and WAV audio is re-encoded");
    if (!pcm) return Kept(ogg && error == "not an Ogg Vorbis stream" ? "unsupported" : "failed", error);
    const auto serial = StreamSerial(input);
    return options.codec == AudioCodec::Opus ? EncodeOpus(*pcm, options, input.size(), serial)
                                             : EncodeVorbis(*pcm, options, input.size(), serial);
}

namespace {
nlohmann::json EncoderVersions() {
    return {{"webp", WebPGetEncoderVersion()}, {"opus", opus_get_version_string()},
            {"vorbis", vorbis_version_string()}};
}

constexpr bool kEncodersAvailable = true;
}  // namespace
#else
Outcome TranscodeImage(std::span<const std::uint8_t>, const ImageOptions&) {
    return Kept("unsupported", "this Packager was built without media encoders");
}

Outcome TranscodeAudio(std::span<const std::uint8_t>, const AudioOptions&) {
    return Kept("unsupported", "this Packager was built without media encoders");
}

namespace {
nlohmann::json EncoderVersions() { return nlohmann::json::object(); }

constexpr bool kEncodersAvailable = false;
}  // namespace
#endif

namespace {

struct Job {
    bool image = true;
    std::filesystem::path source;
    std::filesystem::path output;
};

constexpr std::uintmax_t kMaximumInputBytes = 1024ull * 1024ull * 1024ull;

std::optional<Bytes> ReadFile(const std::filesystem::path& path, std::string& error) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code) { error = "cannot read: " + code.message(); return std::nullopt; }
    if (size > kMaximumInputBytes) { error = "larger than 1 GiB"; return std::nullopt; }
    std::ifstream stream(path, std::ios::binary);
    Bytes bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot read the file";
        return std::nullopt;
    }
    return bytes;
}

bool WriteFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (stream) return true;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return false;
}

nlohmann::json Failure(std::string code, std::string message) {
    return {{"event", "failed"}, {"code", std::move(code)}, {"message", std::move(message)}};
}

}  // namespace

int RunTranscodeRequest(const std::filesystem::path& requestPath,
                        const std::function<void(const std::string&)>& emit) {
    const auto fail = [&](std::string code, std::string message) {
        emit(Failure(std::move(code), std::move(message)).dump());
        return 1;
    };
    if constexpr (!kEncodersAvailable)
        return fail("PXPKG1600", "This Packager was built without WebP/Opus/Vorbis encoders");
    if (!requestPath.is_absolute()) return fail("PXPKG1601", "Transcode request path must be absolute");
    nlohmann::json request;
    try {
        std::ifstream stream(requestPath, std::ios::binary);
        request = nlohmann::json::parse(stream);
    } catch (const std::exception& error) {
        return fail("PXPKG1602", std::string("Transcode request is not JSON: ") + error.what());
    }
    ImageOptions image;
    AudioOptions audio;
    std::vector<Job> jobs;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    try {
        if (request.value("format", "") != "PrismatiXTranscodeRequest" ||
            request.value("schemaRevision", 0) != 1)
            return fail("PXPKG1603", "Expected a PrismatiXTranscodeRequest, schema revision 1");
        if (const auto images = request.find("images"); images != request.end()) {
            image.lossless = images->value("lossless", false);
            image.quality = images->value("quality", 90);
            image.effort = images->value("effort", 4);
        }
        if (const auto sound = request.find("audio"); sound != request.end()) {
            const auto codec = sound->value("codec", "opus");
            if (codec != "opus" && codec != "vorbis")
                return fail("PXPKG1604", "audio.codec must be opus or vorbis");
            audio.codec = codec == "opus" ? AudioCodec::Opus : AudioCodec::Vorbis;
            audio.bitrate = sound->value("bitrate", 96000);
            audio.quality = sound->value("quality", 4.0);
        }
        if (const auto count = request.find("threads"); count != request.end())
            threads = std::clamp(count->get<unsigned>(), 1u, 256u);
        for (const auto& item : request.at("jobs")) {
            const auto kind = item.at("kind").get<std::string>();
            Job job{kind == "image", PathFromUtf8(item.at("source").get<std::string>()),
                    PathFromUtf8(item.at("output").get<std::string>())};
            if ((kind != "image" && kind != "audio") || !job.source.is_absolute() ||
                !job.output.is_absolute())
                return fail("PXPKG1605", "Each job needs kind image or audio and absolute paths");
            jobs.push_back(std::move(job));
        }
    } catch (const std::exception& error) {
        return fail("PXPKG1605", std::string("Transcode request is invalid: ") + error.what());
    }

    std::mutex emitLock;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> encodedCount{0};
    std::atomic<std::size_t> failedCount{0};
    const auto work = [&] {
        for (std::size_t index = next++; index < jobs.size(); index = next++) {
            const auto& job = jobs[index];
            std::string error;
            Outcome outcome;
            std::size_t inputBytes = 0;
            try {
                if (const auto bytes = ReadFile(job.source, error)) {
                    inputBytes = bytes->size();
                    outcome = job.image ? TranscodeImage(*bytes, image) : TranscodeAudio(*bytes, audio);
                } else {
                    outcome = Kept("failed", error);
                }
                if (outcome.encoded && !WriteFile(job.output, outcome.bytes))
                    outcome = Kept("failed", "cannot write " + PathToUtf8(job.output));
            } catch (const std::exception& exception) {
                outcome = Kept("failed", exception.what());
            }
            nlohmann::json event{{"event", "transcoded"}, {"index", index},
                                 {"result", outcome.encoded ? "encoded" : "kept"},
                                 {"inputBytes", inputBytes}};
            if (outcome.encoded) {
                event["outputBytes"] = outcome.bytes.size();
                ++encodedCount;
            } else {
                event["reason"] = outcome.reason;
                event["detail"] = outcome.detail;
                if (outcome.reason == "failed") ++failedCount;
            }
            const std::lock_guard lock(emitLock);
            emit(event.dump());
        }
    };
    std::vector<std::thread> workers;
    const auto count = std::min<std::size_t>(threads, std::max<std::size_t>(jobs.size(), 1));
    for (std::size_t worker = 1; worker < count; ++worker) workers.emplace_back(work);
    work();
    for (auto& worker : workers) worker.join();
    emit(nlohmann::json{{"event", "completed"}, {"jobs", jobs.size()},
                        {"encoded", encodedCount.load()}, {"failed", failedCount.load()},
                        {"versions", EncoderVersions()}}
             .dump());
    return 0;
}

}  // namespace px::sdk::transcode
