#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/AppLifecycle.h"
#include "Engine/Support/Utf8Path.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>

#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using px::platform::AppLifecycle;
using px::platform::LifecycleEvent;
using px::platform::PersistReason;

// Records the order in which the lifecycle calls into the engine and game.
struct Recorder {
    std::vector<std::string> calls;
    AppLifecycle lifecycle;

    Recorder() {
        lifecycle.SetActions({
            .suspendMedia = [this] { calls.emplace_back("suspend"); },
            .resumeMedia = [this] { calls.emplace_back("resume"); },
            .trimCaches = [this] { calls.emplace_back("trim"); },
        });
        lifecycle.SetPersistHandler([this](const PersistReason reason) {
            calls.emplace_back(reason == PersistReason::Background
                                   ? "persist:background"
                                   : "persist:terminating");
        });
    }
};

std::string Join(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        if (!joined.empty()) joined += ',';
        joined += value;
    }
    return joined;
}

void WriteSilentWav(const std::filesystem::path& path,
                    const std::uint32_t milliseconds) {
    constexpr std::uint32_t sampleRate = 8000;
    const std::uint32_t dataSize = sampleRate * milliseconds / 1000 * 2;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const auto put = [&output](const std::uint32_t value, const int bytes) {
        for (int index = 0; index < bytes; ++index)
            output.put(static_cast<char>((value >> (8 * index)) & 0xffu));
    };
    output.write("RIFF", 4);
    put(36 + dataSize, 4);
    output.write("WAVEfmt ", 8);
    put(16, 4);
    put(1, 2);
    put(1, 2);
    put(sampleRate, 4);
    put(sampleRate * 2, 4);
    put(2, 2);
    put(16, 2);
    output.write("data", 4);
    put(dataSize, 4);
    const std::vector<char> silence(dataSize, 0);
    output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
}

bool WriteBitmap(const std::filesystem::path& path) {
    SDL_Surface* surface = SDL_CreateSurface(32, 16, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    const bool saved = SDL_FillSurfaceRect(surface, nullptr, 0xff3366ffu) &&
                       SDL_SaveBMP(surface, px::PathToUtf8(path).c_str());
    SDL_DestroySurface(surface);
    return saved;
}

}  // namespace

int main() {
    px::test::Suite suite("AppLifecycle");

    suite.Run("BackgroundPersistsBeforeSuspendingOnce", [&] {
        Recorder recorder;
        recorder.lifecycle.Handle(LifecycleEvent::WillEnterBackground);
        recorder.lifecycle.Handle(LifecycleEvent::DidEnterBackground);
        suite.Equal("persist:background,suspend", Join(recorder.calls),
                    "save sees live media; the Will/Did pair acts once");
        suite.Expect(recorder.lifecycle.Suspended(), "suspended in background");
        suite.Expect(!recorder.lifecycle.ConsumeResumed(), "no resume yet");
    });

    suite.Run("DidEnterBackgroundAloneStillSaves", [&] {
        Recorder recorder;
        recorder.lifecycle.Handle(LifecycleEvent::DidEnterBackground);
        suite.Equal("persist:background,suspend", Join(recorder.calls),
                    "a missed Will event does not skip the save");
    });

    suite.Run("ForegroundResumesOnceAndReportsIt", [&] {
        Recorder recorder;
        recorder.lifecycle.Handle(LifecycleEvent::DidEnterForeground);
        suite.Expect(recorder.calls.empty() && !recorder.lifecycle.ConsumeResumed(),
                     "foreground without background does nothing");
        recorder.lifecycle.Handle(LifecycleEvent::WillEnterBackground);
        recorder.lifecycle.Handle(LifecycleEvent::WillEnterForeground);
        suite.Expect(recorder.lifecycle.Suspended(),
                     "still suspended until the app is interactive");
        recorder.lifecycle.Handle(LifecycleEvent::DidEnterForeground);
        recorder.lifecycle.Handle(LifecycleEvent::DidEnterForeground);
        suite.Equal("persist:background,suspend,resume", Join(recorder.calls),
                    "one resume per background");
        suite.Expect(!recorder.lifecycle.Suspended(), "interactive again");
        suite.Expect(recorder.lifecycle.ConsumeResumed() &&
                         !recorder.lifecycle.ConsumeResumed(),
                     "resume is reported exactly once");
        recorder.lifecycle.Handle(LifecycleEvent::WillEnterBackground);
        suite.Equal("persist:background,suspend,resume,persist:background,suspend",
                    Join(recorder.calls), "a second background saves again");
    });

    suite.Run("TerminateSavesUnlessTheBackgroundSaveIsCurrent", [&] {
        Recorder foreground;
        foreground.lifecycle.Handle(LifecycleEvent::Terminating);
        foreground.lifecycle.Handle(LifecycleEvent::Terminating);
        suite.Equal("persist:terminating", Join(foreground.calls),
                    "terminating in the foreground saves once");
        suite.Expect(foreground.lifecycle.TerminateRequested(), "quit requested");

        Recorder background;
        background.lifecycle.Handle(LifecycleEvent::WillEnterBackground);
        background.lifecycle.Handle(LifecycleEvent::Terminating);
        suite.Equal("persist:background,suspend", Join(background.calls),
                    "nothing ran since the background save");

        Recorder resumed;
        resumed.lifecycle.Handle(LifecycleEvent::WillEnterBackground);
        resumed.lifecycle.Handle(LifecycleEvent::DidEnterForeground);
        resumed.lifecycle.Handle(LifecycleEvent::Terminating);
        suite.Equal("persist:background,suspend,resume,persist:terminating",
                    Join(resumed.calls), "play after resuming is saved again");
    });

    suite.Run("LowMemoryTrimsEveryTime", [&] {
        Recorder recorder;
        recorder.lifecycle.Handle(LifecycleEvent::LowMemory);
        recorder.lifecycle.Handle(LifecycleEvent::LowMemory);
        suite.Equal("trim,trim", Join(recorder.calls), "trim per warning");
        suite.Expect(recorder.lifecycle.TrimCount() == 2 &&
                         !recorder.lifecycle.Suspended(),
                     "low memory does not suspend");
    });

    suite.Run("ThrowingPersistStillSuspends", [&] {
        AppLifecycle lifecycle;
        bool suspended = false;
        lifecycle.SetActions({.suspendMedia = [&] { suspended = true; }});
        lifecycle.SetPersistHandler(
            [](PersistReason) { throw std::runtime_error("disk full"); });
        lifecycle.Handle(LifecycleEvent::WillEnterBackground);
        suite.Expect(suspended && lifecycle.PersistCount() == 1,
                     "a failed save cannot leave media playing in background");
        AppLifecycle empty;
        empty.Handle(LifecycleEvent::WillEnterBackground);
        empty.Handle(LifecycleEvent::LowMemory);
        empty.Handle(LifecycleEvent::Terminating);
        suite.Expect(empty.Suspended() && empty.TerminateRequested(),
                     "missing callbacks are optional");
    });

    suite.Run("SdlEventTypesMap", [&] {
        suite.Expect(AppLifecycle::FromSdlEventType(SDL_EVENT_WILL_ENTER_BACKGROUND) ==
                             LifecycleEvent::WillEnterBackground &&
                         AppLifecycle::FromSdlEventType(SDL_EVENT_DID_ENTER_BACKGROUND) ==
                             LifecycleEvent::DidEnterBackground &&
                         AppLifecycle::FromSdlEventType(SDL_EVENT_WILL_ENTER_FOREGROUND) ==
                             LifecycleEvent::WillEnterForeground &&
                         AppLifecycle::FromSdlEventType(SDL_EVENT_DID_ENTER_FOREGROUND) ==
                             LifecycleEvent::DidEnterForeground &&
                         AppLifecycle::FromSdlEventType(SDL_EVENT_LOW_MEMORY) ==
                             LifecycleEvent::LowMemory &&
                         AppLifecycle::FromSdlEventType(SDL_EVENT_TERMINATING) ==
                             LifecycleEvent::Terminating,
                     "every lifecycle event type maps");
        suite.Expect(!AppLifecycle::FromSdlEventType(SDL_EVENT_QUIT) &&
                         !AppLifecycle::FromSdlEventType(SDL_EVENT_KEY_DOWN),
                     "other events are not lifecycle events");
    });

    suite.Run("AudioSuspendResumesOnlyWhatItPaused", [&] {
        px::test::TempDirectory directory("lifecycle-audio");
        WriteSilentWav(directory.path / "music.wav", 2000);
        WriteSilentWav(directory.path / "rain.wav", 2000);
        WriteSilentWav(directory.path / "click.wav", 30);
        px::io::VFS vfs;
        vfs.MountDirectory(px::PathToUtf8(directory.path));
        suite.Require(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"),
                      "dummy audio backend");
        px::audio::AudioEngine audio(vfs);
        suite.Require(audio.Init(), "mixer initializes");
        const auto music = audio.PlayBGM("music.wav", true);
        audio.PlayAmbience("rain.wav", true);
        suite.Require(audio.SetAmbiencePaused(true), "game pauses ambience");
        suite.Expect(!audio.CaptureState().music.paused,
                     "a save taken before suspend records live music");

        audio.SetSuspended(true);
        auto state = audio.CaptureState();
        suite.Expect(audio.Suspended() && state.music.paused && state.music.playing,
                     "suspend pauses the audible track without stopping it");
        suite.Expect(audio.Playing(music), "the handle keeps its identity and status");
        audio.SetSuspended(true);

        audio.SetSuspended(false);
        state = audio.CaptureState();
        suite.Expect(!audio.Suspended() && !state.music.paused && state.music.playing,
                     "resume restarts the track suspend paused");
        suite.Expect(state.ambience.paused,
                     "a track the game paused stays paused");

        audio.PlaySE("click.wav");
        for (int attempt = 0; attempt < 100 && audio.SEChannelPlaying(0); ++attempt)
            SDL_Delay(5);
        SDL_Delay(80);
        audio.Update();
        suite.Expect(audio.TrimReclaimable() >= 1,
                     "finished sound effect audio is released");
        suite.Expect(!audio.CaptureState().music.paused &&
                         audio.CaptureState().music.playing,
                     "trimming never touches playing music");
        audio.Shutdown();
        SDL_Quit();
    });

    suite.Run("AssetTrimKeepsTexturesDrawnLastFrame", [&] {
        suite.Require(SDL_Init(SDL_INIT_VIDEO), "video initializes");
        px::test::TempDirectory directory("lifecycle-assets");
        suite.Require(WriteBitmap(directory.path / "old.bmp") &&
                          WriteBitmap(directory.path / "live.bmp"),
                      "bitmaps written");
        SDL_Window* window =
            SDL_CreateWindow("Lifecycle trim", 64, 64, SDL_WINDOW_HIDDEN);
        suite.Require(window != nullptr, "hidden window");
        SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
        suite.Require(renderer != nullptr, "renderer");
        px::io::VFS vfs;
        vfs.MountDirectory(px::PathToUtf8(directory.path));
        {
            px::graphics::AssetCache assets(renderer, vfs);
            assets.BeginFrame();
            (void)assets.Texture("old.bmp");
            assets.BeginFrame();
            assets.BeginFrame();
            (void)assets.Texture("live.bmp");
            assets.BeginFrame();
            suite.Require(assets.HasResidentTexture("old.bmp") &&
                              assets.HasResidentTexture("live.bmp"),
                          "both textures resident under budget");
            suite.Expect(assets.TrimReclaimable() > 0, "trim releases bytes");
            suite.Expect(!assets.HasResidentTexture("old.bmp") &&
                             assets.HasResidentTexture("live.bmp"),
                         "only textures not drawn last frame are dropped");
            suite.Expect(assets.Texture("old.bmp").Width() == 32,
                         "a trimmed texture reloads on demand");
            assets.ReleaseDeviceTextures();
            suite.Expect(!assets.HasResidentTexture("live.bmp") &&
                             assets.ResidentTextureBytes() == 0,
                         "device reset drops every file-backed texture");
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    });

    return suite.Finish();
}
