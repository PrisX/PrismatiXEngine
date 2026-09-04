#include "Engine/IO/VFS.h"
#include "Engine/Video/VideoPlayer.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef PRISMATIX_VIDEO_FIXTURE_BASE64
#error PRISMATIX_VIDEO_FIXTURE_BASE64 must name the checked-in H.264/AAC fixture
#endif

namespace {

std::vector<std::uint8_t> DecodeBase64(const std::string& encoded) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> values{};
    values.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index)
        values[static_cast<unsigned char>(alphabet[index])] =
            static_cast<int>(index);

    std::vector<std::uint8_t> decoded;
    int accumulator = 0;
    int bits = -8;
    for (const unsigned char character : encoded) {
        if (std::isspace(character)) continue;
        if (character == '=') break;
        const int value = values[character];
        if (value < 0) return {};
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(
                static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return decoded;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void AppendFreeBox(std::vector<std::uint8_t>& bytes,
                   const std::uint32_t boxSize) {
    if (boxSize < 8) throw std::runtime_error("invalid MP4 free box size");
    bytes.push_back(static_cast<std::uint8_t>((boxSize >> 24u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((boxSize >> 16u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((boxSize >> 8u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>(boxSize & 0xffu));
    bytes.insert(bytes.end(), {'f', 'r', 'e', 'e'});
    bytes.resize(bytes.size() + boxSize - 8u, 0);
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes,
                const std::size_t count) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(std::min(count, bytes.size())));
    if (!output) throw std::runtime_error("could not write video fixture");
}

}  // namespace

int main() {
    px::test::Suite suite("VideoPlayerAcceptance");
    suite.Run("Mp4H264AacStreamingLifecycle", [&] {
        px::test::TempDirectory temp("video-player");
        const auto fixtureBytes = DecodeBase64(
            ReadText(std::filesystem::path(PRISMATIX_VIDEO_FIXTURE_BASE64)));
        suite.Require(fixtureBytes.size() > 8'000,
                      "checked-in MP4 fixture must decode from base64");
        auto bytes = fixtureBytes;
        AppendFreeBox(bytes, 4u * 1024u * 1024u);
        WriteBytes(temp.path / "h264-aac.mp4", bytes, bytes.size());
        WriteBytes(temp.path / "truncated.mp4", fixtureBytes, 512);
        px::io::ArchiveWriter archiveWriter;
        archiveWriter.SetCompression(true);
        archiveWriter.Add("Video/h264-aac.mp4", bytes);
        suite.Require(archiveWriter.Write((temp.path / "video.pdx").string()),
                      "compressed package mode should create a video archive");
        px::io::Archive archive;
        suite.Require(archive.Open((temp.path / "video.pdx").string()) &&
                          archive.Entries().size() == 1 &&
                          archive.Entries().front().flags == 0,
                      "video containers must remain stored for direct archive streaming");

        const auto key = px::crypto::DeriveKey("encrypted-video-acceptance");
        const auto encryptedPath = temp.path / "encrypted-video.pdx";
        px::io::ArchiveWriter encryptedWriter;
        encryptedWriter.SetKey(key);
        encryptedWriter.SetCompression(true);
        encryptedWriter.Add("Video/h264-aac.mp4", bytes);
        suite.Require(encryptedWriter.Write(encryptedPath.string()),
                      "production package should encrypt native video");
        px::io::Archive encryptedArchive;
        suite.Require(encryptedArchive.Open(encryptedPath.string(), &key) &&
                          encryptedArchive.Entries().size() == 1,
                      "encrypted production video archive should mount");
        const auto& encryptedEntry = encryptedArchive.Entries().front();
        suite.Require((encryptedEntry.flags & 0x05u) == 0x05u &&
                          encryptedEntry.chunkSize == 256u * 1024u &&
                          encryptedEntry.chunkCount > 1,
                      "encrypted video must use independently authenticated PDX records");
        auto encryptedStream =
            encryptedArchive.OpenStream("Video/h264-aac.mp4");
        std::array<std::uint8_t, 64> streamProbe{};
        suite.Require(encryptedStream &&
                          encryptedStream->Seek(128, px::io::SeekOrigin::Begin) &&
                          encryptedStream->Read(streamProbe.data(),
                                                streamProbe.size()) ==
                              streamProbe.size() &&
                          std::equal(streamProbe.begin(), streamProbe.end(),
                                     bytes.begin() + 128) &&
                          encryptedStream->Seek(-32, px::io::SeekOrigin::End) &&
                          encryptedStream->Read(streamProbe.data(), 32) == 32 &&
                          std::equal(streamProbe.begin(), streamProbe.begin() + 32,
                                     bytes.end() - 32),
                      "encrypted records must preserve random seek semantics");
        suite.Expect(encryptedStream->PeakBufferedBytes() <=
                         2u * (256u * 1024u + 28u) &&
                         encryptedStream->PeakBufferedBytes() * 4u < bytes.size(),
                     "encrypted video stream memory must remain bounded independently of file size");

        const auto corruptPath = temp.path / "corrupt-encrypted-video.pdx";
        std::filesystem::copy_file(encryptedPath, corruptPath);
        {
            std::fstream corrupt(corruptPath,
                                 std::ios::binary | std::ios::in | std::ios::out);
            const auto corruptOffset = encryptedEntry.offset + 128u;
            corrupt.seekg(static_cast<std::streamoff>(corruptOffset));
            char value = 0;
            corrupt.read(&value, 1);
            value ^= 0x5a;
            corrupt.seekp(static_cast<std::streamoff>(corruptOffset));
            corrupt.write(&value, 1);
        }
        px::io::Archive corruptArchive;
        suite.Require(corruptArchive.Open(corruptPath.string(), &key),
                      "corrupted encrypted payload should leave the authenticated index readable");
        auto corruptStream = corruptArchive.OpenStream("Video/h264-aac.mp4");
        suite.Require(corruptStream != nullptr,
                      "corrupted encrypted payload should fail on demand, not preload");
        suite.Expect(corruptStream->Read(streamProbe.data(), streamProbe.size()) == 0 &&
                         corruptStream->Failed(),
                     "tampered encrypted video records must fail authentication before exposing plaintext");

        px::io::VFS vfs;
        vfs.MountDirectory(temp.path.string());
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        suite.Require(SDL_Init(SDL_INIT_AUDIO),
                      "SDL audio subsystem should initialize for acceptance playback");
        SDL_Surface* surface = SDL_CreateSurface(
            96, 96, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr,
                      "software render target should initialize headlessly");
        SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
        suite.Require(renderer != nullptr,
                      "software renderer should initialize headlessly");

        {
            px::video::VideoPlayer player(renderer, vfs);
            suite.Require(player.Open("h264-aac.mp4", 0.25f) &&
                              player.Width() == 64 && player.Height() == 64,
                          "FFmpeg must open native MP4/H.264/AAC through VFS AVIO");
            suite.Require(player.Pause() && player.Paused(),
                          "video lifecycle must pause from playing state");
            SDL_Delay(40);
            const auto pausedQueues = player.Queues();
            suite.Expect(pausedQueues.videoFrames <= pausedQueues.videoCapacity &&
                             pausedQueues.audioFrames <= pausedQueues.audioCapacity &&
                             pausedQueues.videoBytes <=
                                 pausedQueues.videoByteCapacity &&
                             pausedQueues.audioBytes <=
                                 pausedQueues.audioByteCapacity &&
                             pausedQueues.videoCapacity == 8 &&
                             pausedQueues.audioCapacity == 32,
                         "decoder queues must remain bounded while presentation is paused");
            suite.Require(player.Resume() && player.Playing(),
                          "video lifecycle must resume a paused stream");

            for (int iteration = 0; iteration < 3'000 && player.Playing();
                 ++iteration) {
                player.Update(1.0f / 240.0f);
                player.Render(96, 96);
                SDL_Delay(1);
            }
            suite.Expect(player.Finished() && player.LastError().empty(),
                         "valid native playback must drain audio/video and reach EOF");

            suite.Require(player.Open("h264-aac.mp4"),
                          "player must support reopening after natural EOF");
            player.Stop();
            suite.Expect(player.State() == px::video::PlaybackState::Stopped &&
                             !player.Playing(),
                         "stop must synchronously join decoder and release playback state");
            suite.Require(player.Open("h264-aac.mp4"),
                          "player must reopen after stop");
            player.Skip();
            suite.Expect(player.State() == px::video::PlaybackState::Stopped,
                         "skip must terminate the active playback lifecycle");

            suite.Expect(!player.Open("truncated.mp4") &&
                             player.State() == px::video::PlaybackState::Failed &&
                             !player.LastError().empty(),
                         "truncated MP4 must fail closed with an actionable error");
            suite.Expect(!player.Open("missing.mp4") &&
                             player.State() == px::video::PlaybackState::Failed,
                         "missing media must fail without leaving a worker alive");
        }

        {
            px::io::VFS archiveVfs;
            suite.Require(
                archiveVfs.MountArchive(encryptedPath.string(), &key),
                "encrypted video archive should mount through VFS");
            px::video::VideoPlayer player(renderer, archiveVfs);
            suite.Require(player.Open("Video/h264-aac.mp4"),
                          "MP4/H.264/AAC must open from encrypted PDX AVIO");
            suite.Require(player.Pause() && player.Resume(),
                          "encrypted playback must preserve pause/resume lifecycle");
            for (int iteration = 0; iteration < 3'000 && player.Playing();
                 ++iteration) {
                player.Update(1.0f / 240.0f);
                SDL_Delay(1);
            }
            suite.Expect(player.Finished(),
                         "encrypted archive-backed video must drain to native EOF");
            suite.Require(player.Open("Video/h264-aac.mp4"),
                          "encrypted playback must reopen after EOF");
            player.Stop();
            suite.Require(player.Open("Video/h264-aac.mp4"),
                          "encrypted playback must reopen after stop");
            player.Skip();
            suite.Expect(player.State() == px::video::PlaybackState::Stopped,
                         "encrypted playback skip must release decoder state");
        }

        {
            px::io::VFS corruptVfs;
            suite.Require(corruptVfs.MountArchive(corruptPath.string(), &key),
                          "corrupted encrypted archive index should still mount");
            px::video::VideoPlayer player(renderer, corruptVfs);
            suite.Expect(!player.Open("Video/h264-aac.mp4") &&
                             player.State() == px::video::PlaybackState::Failed &&
                             !player.LastError().empty(),
                         "authenticated media corruption must fail FFmpeg startup without leaving a decoder worker");
        }

        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
        SDL_Quit();
    });
    return suite.Finish();
}
