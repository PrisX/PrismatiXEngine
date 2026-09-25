#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Preview/PreviewSessionFactory.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using Json = nlohmann::json;

Json Operation(const std::string& id, const std::string& kind,
               Json arguments = Json::object()) {
    return {{"operationId", id},
            {"sourceId", id},
            {"sourceLine", 1},
            {"kind", kind},
            {"text", ""},
            {"arguments", std::move(arguments)}};
}

std::string Document(const std::string& id, const std::uint64_t revision,
                     Json operations) {
    return Json{{"format", "PrismatiXRuntimeIR"},
                {"schemaRevision", 2},
                {"documentId", id},
                {"committedRevision", revision},
                {"operations", std::move(operations)}}
        .dump();
}

struct RuntimeFixture {
    px::io::VFS vfs;
    px::audio::AudioEngine audio{vfs};
    px::graphics::AssetCache assets{nullptr, vfs};
    px::graphics::Renderer2D renderer{nullptr, assets};
    px::RuntimeSession runtime{{vfs, audio, renderer, assets}};
};

void WriteSilentWav(const std::filesystem::path& path,
                    const std::uint32_t milliseconds) {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    const std::uint32_t sampleCount = sampleRate * milliseconds / 1000;
    const std::uint32_t dataSize =
        sampleCount * channels * (bitsPerSample / 8);
    const auto write16 = [](std::ofstream& output, const std::uint16_t value) {
        const std::array bytes{static_cast<char>(value & 0xffu),
                               static_cast<char>((value >> 8u) & 0xffu)};
        output.write(bytes.data(), bytes.size());
    };
    const auto write32 = [](std::ofstream& output, const std::uint32_t value) {
        const std::array bytes{
            static_cast<char>(value & 0xffu),
            static_cast<char>((value >> 8u) & 0xffu),
            static_cast<char>((value >> 16u) & 0xffu),
            static_cast<char>((value >> 24u) & 0xffu)};
        output.write(bytes.data(), bytes.size());
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("RIFF", 4);
    write32(output, 36 + dataSize);
    output.write("WAVEfmt ", 8);
    write32(output, 16);
    write16(output, 1);
    write16(output, channels);
    write32(output, sampleRate);
    write32(output, sampleRate * channels * (bitsPerSample / 8));
    write16(output, channels * (bitsPerSample / 8));
    write16(output, bitsPerSample);
    output.write("data", 4);
    write32(output, dataSize);
    const std::vector<char> silence(dataSize, 0);
    output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
}

}  // namespace

int main() {
    px::test::Suite suite("PreviewSessionContract");

    suite.Run("DeviceNeutralInputEventsAndCustomActions", [&] {
        static_assert(std::is_const_v<
                      std::remove_reference_t<decltype(
                          std::declval<px::Input>().FrameEvents().front())>>);
        px::Input input;
        input.NewFrame();
        suite.Require(input.RegisterAction("photo-mode") &&
                          input.BindKey("photo-mode", SDL_SCANCODE_X),
                      "custom string actions register before binding");

        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = 12.0f;
        event.motion.y = 34.0f;
        event.motion.xrel = 2.0f;
        event.motion.yrel = -1.0f;
        input.Process(event);
        event = {};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = 12.0f;
        event.button.y = 34.0f;
        input.Process(event);
        event = {};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.text = "A";
        input.Process(event);
        event = {};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.scancode = SDL_SCANCODE_X;
        input.Process(event);
        event = {};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.x = 1.0f;
        event.wheel.y = -2.0f;
        input.Process(event);

        const auto events = input.FrameEvents();
        suite.Require(events.size() == 8,
                      "pointer, text, wheel, and logical actions share one frame stream");
        bool monotonic = true;
        for (std::size_t index = 1; index < events.size(); ++index)
            monotonic = monotonic && events[index - 1].id < events[index].id;
        const auto custom = std::ranges::find_if(
            events, [](const px::InputEvent& value) {
                return value.type == px::InputEventType::Action &&
                       value.action == "photo-mode";
            });
        suite.Require(custom != events.end(),
                      "a bound platform key emits a device-neutral logical action");
        const auto pointerDown = std::ranges::find_if(
            events, [](const px::InputEvent& value) {
                return value.type == px::InputEventType::PointerDown &&
                       value.button == px::PointerButton::Primary;
            });
        suite.Require(pointerDown != events.end(),
                      "pointer down is represented in the frame stream");
        const std::uint64_t customId = custom->id;
        const std::uint64_t lastId = events.back().id;
        suite.Expect(monotonic && input.ActionPressed("photo-mode") &&
                         input.ActionDown("photo-mode") &&
                         input.KeyPressed(SDL_SCANCODE_X),
                     "new action events and legacy polling observe the same input");
        suite.Expect(input.ConsumeEvent(customId) &&
                         input.SuppressDefault(customId) &&
                         input.EventConsumed(customId) &&
                         input.DefaultSuppressed(customId) &&
                         custom->action == "photo-mode" &&
                         !input.ActionPressed("photo-mode") &&
                         !input.ActionDown("photo-mode") &&
                         !input.KeyPressed(SDL_SCANCODE_X),
                     "consume/default state is separate from immutable event payloads");
        suite.Expect(input.SuppressDefault(pointerDown->id) &&
                         !input.LeftClick() && !input.LeftDown() &&
                         !input.ActionPressed(px::InputAction::Accept) &&
                         input.ActionPressed(px::InputAction::Advance),
                     "suppressing a pointer source leaves an independent wheel action active");
        const auto wheel = std::ranges::find_if(events, [](const px::InputEvent& value) {
            return value.type == px::InputEventType::PointerWheel;
        });
        suite.Expect(wheel != events.end() && input.SuppressDefault(wheel->id) &&
                         input.WheelY() == 0.0f &&
                         !input.ActionPressed(px::InputAction::Advance),
                     "suppressing wheel also blocks the linked logical action");

        input.NewFrame();
        suite.Expect(input.FrameEvents().empty() &&
                         !input.EventConsumed(customId) &&
                         input.ActionDown("photo-mode"),
                     "frame events and dispositions clear while held polling persists");
        event = {};
        event.type = SDL_EVENT_KEY_UP;
        event.key.scancode = SDL_SCANCODE_X;
        input.Process(event);
        suite.Expect(!input.ActionDown("photo-mode") &&
                         input.InjectAction("photo-mode") &&
                         input.FrameEvents().back().id > lastId,
                     "injected actions use the same monotonic event stream");
        suite.Expect(!input.RegisterAction("photo-mode") &&
                         input.ClearBindings("photo-mode") &&
                         !input.UnregisterAction("accept") &&
                         input.UnregisterAction("photo-mode"),
                     "registration is idempotent and built-in action identity is retained");
    });

    suite.Run("WheelActionsAreDeclarativeAndContextGated", [&] {
        px::Input input;
        suite.Require(input.RegisterAction("screen.open.history") &&
                          input.SetActionContext("screen.open.history", "game") &&
                          input.SetWheelBinding("screen.open.history",
                                                px::WheelDirection::Up, 1.0f) &&
                          input.SetWheelBinding("advance", px::WheelDirection::None),
                      "custom wheel action and disabled built-in gesture register");
        suite.Expect(!input.SetWheelBinding("screen.open.history",
                                            px::WheelDirection::None, 1.0f) &&
                         !input.SetWheelBinding("missing", px::WheelDirection::Up),
                     "invalid or unknown wheel bindings do not replace the current binding");
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.y = 0.4f;
        input.NewFrame();
        input.Process(event);
        suite.Expect(!input.ActionPressed("screen.open.history") &&
                         input.WheelY() == 0.4f,
                     "high-resolution wheel remains visible to UI before threshold");
        event.wheel.y = 0.6f;
        input.Process(event);
        suite.Expect(!input.ActionPressed("screen.open.history"),
                     "inactive game context prevents the threshold action");
        suite.Require(input.SetBaseContext("game"), "game context becomes active");
        suite.Expect(input.ActionPressed("screen.open.history"),
                     "multiple wheel deltas accumulate within the frame");
        const auto action = std::ranges::find_if(input.FrameEvents(),
            [](const px::InputEvent& value) {
                return value.type == px::InputEventType::Action &&
                       value.action == "screen.open.history";
            });
        suite.Expect(action != input.FrameEvents().end() &&
                         input.SuppressDefault(action->id) &&
                         input.WheelY() == 0.0f &&
                         !input.ActionPressed("screen.open.history"),
                     "suppressing a wheel action also suppresses its raw source");
        input.NewFrame();
        event.wheel.y = -1.0f;
        input.Process(event);
        suite.Expect(!input.ActionPressed(px::InputAction::Advance) &&
                         input.WheelY() == -1.0f,
                     "none disables the default gesture without disabling UI scroll");
        suite.Require(input.SetWheelBinding("advance", px::WheelDirection::Down),
                      "gesture can be re-enabled");
        input.NewFrame();
        input.Process(event);
        suite.Expect(input.ActionPressed(px::InputAction::Advance) &&
                         !input.ActionDown(px::InputAction::Advance),
                     "wheel emits a one-frame action, never a held key state");
    });

    suite.Run("KeyRebindReportsConflictsWithoutLosingBindings", [&] {
        px::Input input;
        suite.Require(input.RegisterAction("photo-mode") &&
                          input.BindKey("photo-mode", SDL_SCANCODE_X),
                      "custom action is available for rebinding");
        suite.Expect(!input.BindKey("photo-mode", SDL_SCANCODE_COUNT),
                     "out-of-range bindings fail instead of silently doing nothing");
        const auto conflict = input.RebindKey("photo-mode", SDL_SCANCODE_F5);
        suite.Expect(conflict.status == px::KeyRebindStatus::Conflict &&
                         std::ranges::find(conflict.conflicts, "quick-save") !=
                             conflict.conflicts.end() &&
                         input.KeyBindings("photo-mode") ==
                             std::vector<int>{SDL_SCANCODE_X},
                     "conflicting rebind names the existing action and leaves the old key intact");
        const auto invalid = input.RebindKey("photo-mode", -1);
        suite.Expect(invalid.status == px::KeyRebindStatus::InvalidScancode &&
                         input.KeyBindings("photo-mode") ==
                             std::vector<int>{SDL_SCANCODE_X},
                     "invalid scancodes do not change the action map");
        const auto applied = input.RebindKey("photo-mode", SDL_SCANCODE_F2);
        suite.Expect(applied.status == px::KeyRebindStatus::Applied &&
                         input.KeyBindings("photo-mode") ==
                             std::vector<int>{SDL_SCANCODE_F2} &&
                         px::Input::KeyName(SDL_SCANCODE_F2) == "F2" &&
                         px::Input::KeyCode("F2") == SDL_SCANCODE_F2 &&
                         px::Input::KeyCode("Enter") == SDL_SCANCODE_RETURN,
                     "successful rebind replaces, rather than appends, the keyboard shortcut");
        input.NewFrame();
        input.InjectKeyPress(SDL_SCANCODE_X);
        suite.Expect(!input.ActionPressed("photo-mode"),
                     "old key no longer triggers the action");
        input.NewFrame();
        input.InjectKeyPress(SDL_SCANCODE_F2);
        suite.Expect(input.ActionPressed("photo-mode"),
                     "new key triggers the action");
        const auto shared = input.RebindKey("photo-mode", SDL_SCANCODE_F5, true);
        suite.Expect(shared.status == px::KeyRebindStatus::Applied &&
                         std::ranges::find(shared.conflicts, "quick-save") !=
                             shared.conflicts.end(),
                     "intentional shared bindings still report what overlaps");
        const auto sameShared = input.RebindKey("photo-mode", SDL_SCANCODE_F5);
        suite.Expect(sameShared.status == px::KeyRebindStatus::Applied,
                     "reselecting an already-shared key is not a new conflict");
        suite.Expect(input.RebindKey("not-registered", SDL_SCANCODE_F2).status ==
                         px::KeyRebindStatus::UnknownAction,
                     "unknown actions cannot silently create a binding");

        auto saved = input.CaptureKeyboardBindings();
        saved["later.action"] = {SDL_SCANCODE_F3};
        px::Input restored;
        suite.Require(restored.RestoreKeyboardBindings(saved),
                      "saved keyboard bindings restore atomically");
        suite.Require(restored.RegisterAction("photo-mode") &&
                          restored.RegisterAction("later.action"),
                      "saved custom actions may register after settings load");
        suite.Expect(restored.KeyBindings("photo-mode") ==
                         std::vector<int>{SDL_SCANCODE_F5} &&
                         restored.KeyBindings("later.action") ==
                             std::vector<int>{SDL_SCANCODE_F3},
                     "late action registration receives its persisted key");
        auto invalidSaved = saved;
        invalidSaved["later.action"] = {-1};
        suite.Expect(!restored.RestoreKeyboardBindings(invalidSaved) &&
                         restored.KeyBindings("later.action") ==
                             std::vector<int>{SDL_SCANCODE_F3},
                     "invalid saved map does not partially overwrite live bindings");
        restored.ResetKeyboardBindings();
        suite.Expect(restored.KeyBindings("later.action").empty() &&
                         restored.KeyBindings("quick-save") ==
                             std::vector<int>{SDL_SCANCODE_F5},
                     "reset clears custom overrides and restores engine defaults");
    });

    suite.Run("AddKeyBindingRetainsKeysAndRejectsConflictsAtomically", [&] {
        px::Input input;
        suite.Require(input.RegisterAction("photo-mode") &&
                          input.BindKey("photo-mode", SDL_SCANCODE_P),
                      "additive binding fixture starts with one key");
        const auto conflict = input.AddKeyBinding("photo-mode", SDL_SCANCODE_F5);
        suite.Expect(conflict.status == px::KeyRebindStatus::Conflict &&
                         input.KeyBindings("photo-mode") ==
                             std::vector<int>{SDL_SCANCODE_P},
                     "adding a conflicting key leaves existing keys untouched");
        const auto applied = input.AddKeyBinding("photo-mode", SDL_SCANCODE_F2);
        suite.Expect(applied.status == px::KeyRebindStatus::Applied &&
                         input.KeyBindings("photo-mode") ==
                             (std::vector<int>{SDL_SCANCODE_P, SDL_SCANCODE_F2}),
                     "adding a key preserves authored order and prior keys");
        suite.Expect(input.AddKeyBinding("photo-mode", SDL_SCANCODE_F2).status ==
                         px::KeyRebindStatus::Applied &&
                         input.KeyBindings("photo-mode").size() == 2,
                     "adding an existing key is idempotent");
        suite.Expect(input.AddKeyBinding("photo-mode", SDL_SCANCODE_F5, true).status ==
                         px::KeyRebindStatus::Applied &&
                         input.KeyBindings("photo-mode").back() == SDL_SCANCODE_F5,
                     "explicit shared binding appends without replacing keys");
        px::Input restored;
        suite.Require(restored.RestoreKeyboardBindings(input.CaptureKeyboardBindings()) &&
                          restored.RegisterAction("photo-mode"),
                      "an additive edit survives settings reload");
        suite.Expect(restored.KeyBindings("photo-mode") ==
                         (std::vector<int>{SDL_SCANCODE_P, SDL_SCANCODE_F2,
                                           SDL_SCANCODE_F5}),
                     "saved additive keys keep their authored order");

        px::Input full;
        suite.Require(full.RegisterAction("many-keys"),
                      "limit fixture registers a custom action");
        for (int key = 1; key <= 32; ++key)
            suite.Require(full.BindKey("many-keys", key),
                          "limit fixture fills the persisted key capacity");
        suite.Expect(full.AddKeyBinding("many-keys", 33).status ==
                         px::KeyRebindStatus::LimitReached &&
                         !full.BindKey("many-keys", 34) &&
                         full.KeyBindings("many-keys").size() == 32,
                     "an additive edit cannot create a binding map that saves cannot reload");
    });

    suite.Run("NamedActionContextsGateActivationWithoutDroppingBindings", [&] {
        px::Input input;
        suite.Require(input.RegisterAction("screen.open.gallery") &&
                          input.BindKey("screen.open.gallery", SDL_SCANCODE_G) &&
                          input.SetActionContext("screen.open.gallery", "game"),
                      "authored screen action has a named gameplay context");
        input.NewFrame();
        input.InjectKeyPress(SDL_SCANCODE_G);
        suite.Expect(!input.ActionPressed("screen.open.gallery"),
                     "game shortcut stays inactive in the global context");
        suite.Require(input.SetBaseContext("game"),
                      "host selects the gameplay context");
        suite.Expect(input.ActionPressed("screen.open.gallery"),
                     "context activation applies to current-frame events");
        suite.Require(input.SetBaseContext("ui"),
                      "host selects the menu context");
        suite.Expect(!input.ActionPressed("screen.open.gallery"),
                     "game shortcut does not fire while a menu owns input");
        suite.Require(input.PushContext("game"),
                      "temporary overlay can activate a nested context");
        suite.Expect(input.ActionPressed("screen.open.gallery") &&
                         input.ActiveContexts() ==
                             std::vector<std::string>{"global", "ui", "game"},
                     "overlay activation is visible and preserves the base context");
        suite.Expect(input.PopContext() &&
                         !input.ActionPressed("screen.open.gallery") &&
                         !input.PopContext(),
                     "popping the overlay restores menu input without underflow");
    });

    suite.Run("ResetRestoresProjectAuthoredKeyboardDefaults", [&] {
        px::Input input;
        suite.Require(input.RegisterAction("screen.open.gallery") &&
                          input.SetDefaultKeyboardBindings("screen.open.gallery", {SDL_SCANCODE_G}) &&
                          input.BindKey("screen.open.gallery", SDL_SCANCODE_G) &&
                          input.SetDefaultKeyboardBindings("quick-save", {SDL_SCANCODE_F8}),
                      "game catalog defaults can be retained separately from current keys");
        suite.Expect(!input.SetDefaultKeyboardBindings("screen.open.gallery", {SDL_SCANCODE_COUNT}) &&
                         !input.SetDefaultKeyboardBindings("missing", {SDL_SCANCODE_G}),
                     "invalid authored defaults cannot replace an existing default");
        suite.Require(input.RebindKey("screen.open.gallery", SDL_SCANCODE_F7, true).status ==
                          px::KeyRebindStatus::Applied &&
                          input.RebindKey("quick-save", SDL_SCANCODE_F6, true).status ==
                          px::KeyRebindStatus::Applied,
                      "user may rebind project and engine actions");
        input.ResetKeyboardBindings();
        suite.Expect(input.KeyBindings("screen.open.gallery") ==
                         std::vector<int>{SDL_SCANCODE_G} &&
                         input.KeyBindings("quick-save") ==
                         std::vector<int>{SDL_SCANCODE_F8},
                     "reset restores both authored named-action and overridden built-in defaults");
    });

    suite.Run("OpenSaveShortcutIsContextScopedAndRebindable", [&] {
        px::Input input;
        suite.Expect(input.KeyBindings("save.open") ==
                         std::vector<int>{SDL_SCANCODE_F6},
                     "save menu has a named default instead of a Player-only raw key check");
        suite.Require(input.SetBaseContext("ui"), "menu context is active");
        input.NewFrame();
        input.InjectKeyPress(SDL_SCANCODE_F6);
        suite.Expect(!input.ActionPressed(px::InputAction::OpenSave),
                     "game-only save action cannot leak through a menu shortcut");
        suite.Require(input.SetBaseContext("game"), "game context is active");
        suite.Expect(input.ActionPressed(px::InputAction::OpenSave),
                     "game context sees the named save shortcut");
        suite.Require(input.RebindKey("save.open", SDL_SCANCODE_F7, true).status ==
                          px::KeyRebindStatus::Applied,
                      "save menu shortcut accepts user rebinding");
        input.ResetKeyboardBindings();
        suite.Expect(input.KeyBindings("save.open") ==
                         std::vector<int>{SDL_SCANCODE_F6},
                     "reset restores the engine F6 default");
    });

    suite.Run("AudioHandlesCompletionAndRestorePolicy", [&] {
        suite.Expect(
            px::audio::AudioEngine::RestorePolicyFor(
                px::audio::AudioBus::Music) ==
                    px::audio::AudioEngine::RestorePolicy::Persistent &&
                px::audio::AudioEngine::RestorePolicyFor(
                    px::audio::AudioBus::Voice) ==
                    px::audio::AudioEngine::RestorePolicy::Persistent &&
                px::audio::AudioEngine::RestorePolicyFor(
                    px::audio::AudioBus::SoundEffect) ==
                    px::audio::AudioEngine::RestorePolicy::Transient,
            "persistent buses and transient sound effects have an explicit restore policy");

        px::test::TempDirectory directory("audio-handles");
        WriteSilentWav(directory.path / "intro.wav", 25);
        WriteSilentWav(directory.path / "loop.wav", 250);
        px::io::VFS vfs;
        vfs.MountDirectory(directory.path.string());
        suite.Require(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"),
                      "dummy audio backend is selected before mixer init");
        px::audio::AudioEngine audio(vfs);
        suite.Require(audio.Init(), "device-backed test mixer initializes");

        std::vector<px::audio::AudioHandle> callbacks;
        audio.SetCompletion([&callbacks](const px::audio::AudioHandle handle) {
            callbacks.push_back(handle);
        });
        const auto music = audio.PlayIntroLoop(
            px::audio::AudioBus::Music, "intro.wav", "loop.wav");
        auto playback = audio.Playback(music);
        suite.Require(playback &&
                          playback->phase == px::audio::PlaybackPhase::Intro &&
                          playback->introPath == "intro.wav" &&
                          playback->loopPath == "loop.wav",
                      "intro-loop phases are exposed without changing handle identity");
        const auto handoffDeadline = SDL_GetTicks() + 2'000;
        while (playback && playback->phase != px::audio::PlaybackPhase::Loop &&
               SDL_GetTicks() < handoffDeadline) {
            SDL_Delay(5);
            audio.Update();
            playback = audio.Playback(music);
        }
        if (!playback || playback->phase != px::audio::PlaybackPhase::Loop)
            std::cerr << "intro-loop diagnostic: playback=" << (playback ? 1 : 0)
                      << " status=" << (playback ? static_cast<int>(playback->status) : -1)
                      << " phase=" << (playback ? static_cast<int>(playback->phase) : -1)
                      << " frame=" << (playback ? playback->playbackFrame : -1)
                      << " playing=" << audio.Playing(music) << '\n';
        suite.Require(playback &&
                          playback->phase == px::audio::PlaybackPhase::Loop &&
                          playback->handle == music && audio.Playing(music),
                      "intro naturally hands off to the looping body on the same handle");
        suite.Require(audio.Pause(music) && audio.Paused(music) &&
                          audio.Seek(music, 100),
                      "handle playback supports pause and seek");
        const auto snapshot = audio.CaptureState();
        suite.Require(snapshot.music.handle == music.value &&
                          snapshot.music.paused,
                      "persistent capture includes handle identity and paused state");

        const auto transient = audio.Play(
            {.bus=px::audio::AudioBus::SoundEffect,.path="loop.wav"});
        suite.Require(audio.Active(transient),
                      "transient sound effect starts independently");
        suite.Require(audio.RestoreState(snapshot),
                      "persistent audio snapshot restores");
        suite.Expect(audio.Paused(music) &&
                         audio.Playback(music)->handle == music &&
                         audio.Status(transient) ==
                             px::audio::PlaybackStatus::Stopped,
                     "restore preserves persistent handle and silences transient SE");
        suite.Expect(audio.Resume(music) && audio.Stop(music) &&
                         audio.Status(music) ==
                             px::audio::PlaybackStatus::Stopped,
                     "handle playback supports resume and explicit stop");

        const auto voice = audio.Play(
            {.bus=px::audio::AudioBus::Voice,.path="intro.wav"});
        suite.Require(audio.Active(voice), "one-shot voice starts");
        for (int attempt = 0; attempt < 100 && audio.Active(voice); ++attempt) {
            SDL_Delay(5);
            audio.Update();
        }
        const auto completions = audio.TakeCompletions();
        suite.Expect(audio.IsComplete(voice) && callbacks.size() == 1 &&
                         callbacks.front() == voice && completions.size() == 1 &&
                         completions.front() == voice &&
                         audio.TakeCompletions().empty(),
                     "natural completion is observable once by callback and drain queue");

        px::audio::AudioHandle oldestFailure;
        px::audio::AudioHandle newestFailure;
        for (std::size_t index = 0;
             index < px::audio::AudioEngine::TerminalHistoryLimit + 2; ++index) {
            newestFailure = audio.Play(
                {.bus=px::audio::AudioBus::SoundEffect,.path="missing.wav"});
            if (index == 0) oldestFailure = newestFailure;
        }
        suite.Expect(audio.TerminalHistorySize() ==
                             px::audio::AudioEngine::TerminalHistoryLimit &&
                         audio.Status(oldestFailure) ==
                             px::audio::PlaybackStatus::Invalid &&
                         audio.Status(newestFailure) ==
                             px::audio::PlaybackStatus::Failed,
                     "terminal handle history is bounded and evicts oldest state");
        audio.Shutdown();
    });

    suite.Run("FrontendNeutralSessionState", [&] {
        RuntimeFixture fixture;
        auto preview = px::preview::CreatePreviewSession(fixture.runtime);
        suite.Expect(preview->Tick(1, 0.0f).accepted,
                     "timeline and UI-only sessions tick before story apply");
        const auto checkpoint = preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted && checkpoint.checkpoint.has_value(),
                      "timeline and UI-only sessions can capture state");
        suite.Expect(
            preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted,
            "timeline and UI-only state restores without a VM program");
    });

    suite.Run("RestoreFailureRecoversExternalCheckpoint", [&] {
        RuntimeFixture fixture;
        int external = 1;
        bool rejectRuntime = false;
        bool rejectRollback = false;
        bool rejectExternal = false;
        bool rejectCapture = false;
        fixture.runtime.SetUIStateHandler({}, [&](const auto&) {
            if (rejectRuntime) {
                rejectRuntime = false;
                return px::Status::Fail(px::diag::Diagnostic{
                    .code = "TEST-RESTORE", .message = "Injected runtime commit failure"});
            }
            return px::Status::Ok();
        });
        auto preview = px::preview::CreatePreviewSession(fixture.runtime, {
            .captureExternalState = [&] { return std::make_shared<const int>(external); },
            .restoreExternalState = [&](const std::shared_ptr<const void>& value) {
                const int restored = *std::static_pointer_cast<const int>(value);
                if (rejectRollback && restored == 2)
                    return px::Status::Fail(px::diag::Diagnostic{
                        .code = "TEST-ROLLBACK", .message = "Injected external rollback failure"});
                external = restored;
                if (rejectExternal && restored == 1)
                    return px::Status::Fail(px::diag::Diagnostic{
                        .code = "TEST-EXTERNAL", .message = "Injected partial external restore"});
                return px::Status::Ok();
            },
            .validateExternalState = [&](const std::shared_ptr<const void>&) {
                if (rejectCapture) return px::Status::Fail(px::diag::Diagnostic{
                    .code = "TEST-CAPTURE", .message = "Extension provider capture failed"});
                return px::Status::Ok();
            }});
        const auto checkpoint = preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted && checkpoint.checkpoint.has_value(),
                      "external checkpoint captures");
        external = 2;
        rejectCapture = true;
        suite.Expect(!preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted && external == 2,
                     "failed provider capture rejects restore before changing external state");
        rejectCapture = false;
        rejectRuntime = true;
        suite.Expect(!preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted &&
                         external == 2,
                     "runtime commit failure restores the previous external state");
        rejectExternal = true;
        suite.Expect(!preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted &&
                         external == 2,
                     "partially failed external restore recovers its previous state");
        rejectExternal = false;
        rejectRuntime = true;
        rejectRollback = true;
        const auto failedRollback = preview->RestoreCheckpoint(checkpoint.checkpoint->id);
        suite.Expect(!failedRollback.accepted && std::ranges::any_of(
                         failedRollback.diagnostics, [](const auto& diagnostic) {
                             return diagnostic.code == "PXPREVIEW1304" &&
                                    diagnostic.severity == px::sdk::PreviewDiagnosticSeverity::Fatal;
                         }),
                     "external rollback failure is returned as a fatal diagnostic");
    });

    suite.Run("RuntimeRestoreRetiresPendingTimelineResume", [&] {
        RuntimeFixture fixture;
        suite.Require(fixture.runtime.StartRuntimeIrText(Document("await", 1,
            Json::array({Operation("line", "narration", {{"text", "Hold"}})})), "await"),
            "runtime program loads");
        px::animation::AnimationClip clip;
        clip.id = px::Uuid::FromName("restore.await");
        clip.name = "Restore await";
        clip.duration = 1.0f;
        suite.Require(static_cast<bool>(fixture.runtime.Timeline().Register(clip)),
                      "await fixture clip registers");
        const auto handle = fixture.runtime.Timeline().Play(clip.id, true);
        auto checkpoint = fixture.runtime.CaptureState();
        checkpoint.vm.state = px::vn::VMState::WaitingExternal;
        suite.Require(static_cast<bool>(fixture.runtime.RestoreState(checkpoint)),
                      "awaiting checkpoint restores");
        suite.Require(static_cast<bool>(fixture.runtime.Timeline().Cancel(handle)),
                      "old timeline queues its completion outside the frame update");
        suite.Require(static_cast<bool>(fixture.runtime.RestoreState(checkpoint)),
                      "checkpoint replaces the cancelled timeline");
        fixture.runtime.Update(1, 0.0f);
        suite.Expect(fixture.runtime.VM().State() == px::vn::VMState::WaitingExternal,
                     "old completion must not resume the restored await");
    });

    suite.Run("CommitRevalidatesPublicPreparedRestore", [&] {
        RuntimeFixture fixture;
        auto prepared = fixture.runtime.PrepareRestore(fixture.runtime.CaptureState());
        suite.Require(static_cast<bool>(prepared), "candidate initially validates");
        auto candidate = prepared.TakeValue();
        candidate.state.typedVariables["unvalidated"] = px::Variant(std::int64_t{7});
        fixture.runtime.SetUIStateHandler({}, {}, [](const auto&) {
            return px::Status::Fail(px::diag::Diagnostic{
                .code = "TEST-VALIDATION", .message = "UI topology changed since prepare"});
        });
        suite.Expect(!fixture.runtime.CommitRestore(std::move(candidate)) &&
                         !fixture.runtime.Variables().GetValue("unvalidated"),
                     "public candidate and changed live dependencies revalidate before mutation");
    });

    suite.Run("CompleteUIRuntimeCheckpoint", [&] {
        RuntimeFixture fixture;
        suite.Require(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                      "built-in typed UI properties register for checkpoint testing");
        px::ui::UIContext ui;
        auto root = std::make_unique<px::ui::Control>("CheckpointRoot");
        const px::Uuid node = root->Id();
        suite.Require(static_cast<bool>(ui.SetRoot(std::move(root))),
                      "UI checkpoint root installs");
        px::ui::AnimationClip clip;
        clip.id = px::Uuid::FromName("preview.checkpoint.ui.clip");
        clip.name = "Checkpoint";
        clip.duration = 1.0f;
        clip.tracks.push_back({
            .node=node,.property="opacity",
            .keys={{0.0f,1.0,px::ui::Ease::Linear,
                    px::ui::KeyInterpolation::Linear},
                   {1.0f,0.0,px::ui::Ease::Linear,
                    px::ui::KeyInterpolation::Linear}}});
        px::ui::UIAnimationLibrary library;
        const px::Uuid clipId = clip.id;
        const px::Uuid animationState =
            px::Uuid::FromName("preview.checkpoint.ui.state");
        library.clips.push_back(std::move(clip));
        library.machine.entry = animationState;
        library.machine.states.push_back(
            {animationState,"Default",clipId,{}});
        suite.Require(static_cast<bool>(
                          ui.SetAnimations(std::move(library),true)),
                      "UI animation state machine installs");
        px::ui::VisualStateGroup group;
        group.id = "interaction";
        group.defaultState = "normal";
        group.states = {
            {"normal",{{node,"opacity",px::Variant(1.0)}}},
            {"hover",{{node,"opacity",px::Variant(0.5)}}}};
        group.transitions.push_back(
            {"normal","hover",1.0f,px::ui::VisualStateEase::Linear,
             std::nullopt});
        suite.Require(ui.SetVisualStateGroups({std::move(group)}) &&
                          ui.PreviewAnimation(clipId,0.25f,false) &&
                          ui.SetVisualState("interaction","hover"),
                      "UI animation and Visual State checkpoints become active");
        px::Input input;
        (void)ui.Update(input,640,360,0.4f);
        fixture.runtime.SetUIStateHandler(
            [&ui]{return ui.CaptureRuntimeState();},
            [&ui](const px::ui::UIRuntimeState& state){
                return ui.RestoreRuntimeState(state);
            });
        auto preview=px::preview::CreatePreviewSession(fixture.runtime);
        const auto checkpoint=preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted&&checkpoint.checkpoint,
                      "PreviewSession captures complete UI runtime state");
        (void)ui.PreviewAnimation(clipId,0.9f,false);
        (void)ui.Update(input,640,360,0.6f);
        suite.Require(preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted,
                      "PreviewSession restores complete UI runtime state");
        const auto restored=ui.CaptureRuntimeState();
        suite.Expect(restored.animation&&
                         std::abs(restored.animation->position-0.25f)<0.001f&&
                         restored.visualState&&
                         restored.visualState->groups.size()==1&&
                         restored.visualState->groups.front().state=="hover"&&
                         std::abs(restored.visualState->groups.front().elapsed-0.4f)<0.001f,
                     "checkpoint restores UI Animation and Visual State transition progress exactly");
    });

    suite.Run("ApplyPatchAndMemoryCheckpointRestore", [&] {
        RuntimeFixture fixture;
        auto preview = px::preview::CreatePreviewSession(
            fixture.runtime,
            {.checkpointInterval = 2, .maximumCheckpoints = 8});
        const std::string first = Document(
            "linear", 1,
            Json::array({Operation("line-1", "narration", {{"text", "One"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "7"}}),
                         Operation("line-2", "narration", {{"text", "Two"}})}));
        const std::uint64_t initialAssetSession = fixture.assets.AssetSession();
        const auto applied = preview->Apply({"linear", 1, first});
        suite.Require(applied.accepted &&
                          applied.status == px::sdk::PreviewSessionStatus::Applied &&
                          fixture.assets.AssetSession() == initialAssetSession + 1,
                      "Apply installs Runtime IR and starts a new asset generation");
        const auto checkpoint = preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted && checkpoint.checkpoint.has_value(),
                     "facade captures an opaque checkpoint identity");
        suite.Expect(preview->Advance().accepted && preview->Advance().accepted &&
                         fixture.runtime.Variables().Get("score") == 7 &&
                         fixture.runtime.Dialogue().State().fullText == "Two",
                     "advance executes through the shared RuntimeSession");
        suite.Expect(preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted &&
                         fixture.runtime.Variables().Get("score") == 0 &&
                         fixture.runtime.Dialogue().State().fullText == "One",
                     "memory-applied Runtime IR restores without a VFS script");

        const std::string second = Document(
            "linear", 2,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto patched = preview->Patch({"linear", 2, second});
        suite.Expect(patched.accepted && patched.inPlace &&
                         patched.status == px::sdk::PreviewSessionStatus::Patched &&
                         fixture.assets.AssetSession() == initialAssetSession + 1,
                     "compatible patch preserves execution and asset state in place");
        suite.Expect(preview->Advance().accepted && preview->Advance().accepted &&
                         fixture.runtime.Variables().Get("score") == 9 &&
                         fixture.runtime.Dialogue().State().fullText == "Two+",
                     "patched checkpoint state references the new compiled program");
        const std::string structural = Document(
            "linear", 3,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("inserted", "narration", {{"text", "New"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto restarted = preview->Patch({"linear", 3, structural});
        suite.Expect(restarted.accepted && !restarted.inPlace &&
                         restarted.status ==
                             px::sdk::PreviewSessionStatus::Restarted &&
                         fixture.assets.AssetSession() == initialAssetSession + 2,
                     "structural patch restarts a detached asset generation");
        const std::string staleIr = Document(
            "linear", 5,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto stale = preview->Patch({"linear", 5, staleIr});
        suite.Expect(!stale.accepted &&
                         stale.status ==
                             px::sdk::PreviewSessionStatus::RevisionConflict,
                     "patch revisions are monotonic and fail closed");
    });

    suite.Run("BranchAwareStorySeekAndUnsafeReplay", [&] {
        RuntimeFixture fixture;
        auto preview = px::preview::CreatePreviewSession(fixture.runtime);
        const std::string branching = Document(
            "branching", 1,
            Json::array({
                Operation("intro", "narration", {{"text", "Choose"}}),
                Operation("left-choice", "choiceOption",
                          {{"text", "Left"}, {"target", "left"}}),
                Operation("right-choice", "choiceOption",
                          {{"text", "Right"}, {"target", "right"}}),
                Operation("left-label", "label", {{"target", "left"}}),
                Operation("left-value", "setVariable",
                          {{"name", "route"}, {"value", "1"}}),
                Operation("left-end", "jump", {{"target", "end"}}),
                Operation("right-label", "label", {{"target", "right"}}),
                Operation("right-value", "setVariable",
                          {{"name", "route"}, {"value", "2"}}),
                Operation("end-label", "label", {{"target", "end"}}),
                Operation("ending", "narration", {{"text", "Done"}}),
            }));
        suite.Require(preview->Apply({"branching", 1, branching}).accepted,
                     "branching Runtime IR applies");
        const auto unresolved = preview->SeekStory({9, {}});
        suite.Expect(!unresolved.accepted &&
                         unresolved.status ==
                             px::sdk::PreviewSessionStatus::ChoicePathRequired,
                     "seek reports an unresolved choice instead of guessing");
        const auto right = preview->SeekStory({9, {1}});
        suite.Expect(right.accepted &&
                         fixture.runtime.Variables().Get("route") == 2 &&
                         fixture.runtime.Dialogue().State().fullText == "Done" &&
                         preview->State().operationIndex == 9,
                     "seek restores a checkpoint and replays the selected branch");

        const std::string unsafe = Document(
            "unsafe", 1,
            Json::array({Operation("before", "narration", {{"text", "Before"}}),
                         Operation("external", "customNode",
                                   {{"type", "unknown.external"}}),
                         Operation("after", "narration", {{"text", "After"}})}));
        suite.Require(preview->Apply({"unsafe", 1, unsafe}).accepted,
                     "unsafe operation after the first wait does not prevent apply");
        const auto blocked = preview->SeekStory({2, {}});
        suite.Expect(!blocked.accepted &&
                         blocked.status ==
                             px::sdk::PreviewSessionStatus::UnsafeOperation &&
                         !blocked.diagnostics.empty(),
                     "seek refuses operations without an explicit safety contract");
    });

    suite.Run("TimelineViewportStateAndEvents", [&] {
        RuntimeFixture fixture;
        bool resized = false;
        double sampledOpacity = -1.0;
        fixture.runtime.SetAnimationTargetHandler(
            px::animation::TargetKind::UI,
            [&sampledOpacity](const px::animation::TrackBinding& binding,
                              const px::Variant& value) {
                if (binding.target == "panel" && binding.property == "opacity") {
                    if (const auto* number = value.TryGet<double>())
                        sampledOpacity = *number;
                }
                return px::Status::Ok();
            });
        auto preview = px::preview::CreatePreviewSession(
            fixture.runtime,
            {.resize = [&resized](const int width, const int height,
                                  const float scale) {
                resized = width == 960 && height == 540 && scale == 1.5f;
                return px::Status::Ok();
            }});
        const std::string runtimeIr = Document(
            "timeline", 1,
            Json::array({Operation("line", "narration", {{"text", "Line"}})}));
        suite.Require(preview->Apply({"timeline", 1, runtimeIr}).accepted,
                     "timeline test document applies");
        const auto beforeTimeline=preview->CaptureCheckpoint();
        suite.Require(beforeTimeline.accepted&&beforeTimeline.checkpoint,
                      "pre-Timeline checkpoint captures");
        const std::string timeline = Json{
            {"format", "PrismatiXTimeline"},
            {"schemaRevision", 2},
            {"id", "preview.timeline"},
            {"duration", 4.0},
            {"tracks", Json::array({
                {{"id", "panel-opacity"},
                 {"binding", {{"kind", "ui"}, {"target", "panel"},
                              {"property", "opacity"}}},
                 {"keyframes", Json::array({
                     {{"time", 0.0}, {"value", 0.0}, {"easing", "linear"}},
                     {{"time", 4.0}, {"value", 1.0}, {"easing", "linear"}}
                 })}}
            })},
            {"markers", Json::array()},
            {"nestedClips", Json::array()}}
            .dump();
        const auto timelineApplied = preview->ApplyTimeline(
            {"preview.timeline", 1, timeline,
             "memory://preview/timeline.pxtimeline", 0.25, 1.0});
        suite.Require(timelineApplied.accepted &&
                          timelineApplied.playbackHandle != 0,
                      "frontend-neutral facade applies in-memory canonical Timeline content");
        suite.Expect(!preview->RestoreCheckpoint(
                          beforeTimeline.checkpoint->id).accepted,
                     "Timeline hot apply invalidates checkpoints from the previous clip graph");
        const auto handle = timelineApplied.playbackHandle;
        suite.Expect(sampledOpacity > 0.06 && sampledOpacity < 0.07 &&
                         preview->SeekTimeline({handle, 2.5}).accepted &&
                         sampledOpacity > 0.62 && sampledOpacity < 0.63 &&
                         preview->State().timelines.back().seconds == 2.5,
                     "Timeline apply returns a high-frequency scrub handle distinct from Story seek");
        const auto staleTimeline = preview->ApplyTimeline(
            {"preview.timeline", 1, timeline});
        suite.Expect(!staleTimeline.accepted &&
                         staleTimeline.status ==
                             px::sdk::PreviewSessionStatus::RevisionConflict,
                     "Timeline hot apply rejects stale revisions");
        suite.Expect(preview->Resize(960, 540, 1.5f).accepted && resized &&
                         preview->State().viewportWidth == 960,
                     "resize flows through the frontend-neutral callback");
        suite.Expect(preview->Tick(16, 0.016f).accepted,
                     "tick advances shared Runtime semantics");
        const auto events = preview->Events();
        suite.Expect(!events.empty() && preview->Events().empty(),
                     "events preserve order and drain explicitly");
    });

    return suite.Finish();
}
