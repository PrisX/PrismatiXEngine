#include "Applications/Player/PlayerApp.h"
#include "Applications/Player/PlayerControls.h"
#include "Applications/Player/PlayerSupport.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Logger.h"
#include "Engine/Support/Utf8Path.h"
#include "Engine/UI/Actions/PlayerActionContract.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/Widgets.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <utility>

namespace px::player {

namespace {

std::optional<int> IntegerArgument(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return static_cast<int>(value);
}

// Trims surrounding spaces, then cuts at maxLength code points.
std::string TrimmedInput(std::string text, const std::size_t maxLength) {
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = text.find_first_not_of(kSpace);
    text = first == std::string::npos ? std::string{}
        : text.substr(first, text.find_last_not_of(kSpace) - first + 1);
    std::size_t points = 0, end = 0;
    while (end < text.size() && points < maxLength) {
        ++points;
        ++end;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
    }
    text.resize(end);
    return text;
}

using Settings = progress::GameSettings;

struct AudioBus {
    std::string_view name;
    int Settings::* volume;
    bool Settings::* muted;
    void (audio::AudioEngine::*apply)(int);
    std::string_view muteLabel;
};

const AudioBus* FindAudioBus(const std::string_view name) {
    static constexpr AudioBus buses[] = {
        {"master", &Settings::masterVolume, &Settings::masterMuted,
         &audio::AudioEngine::SetMainVolume, "MuteMasterLabel"},
        {"bgm", &Settings::bgmVolume, &Settings::bgmMuted,
         &audio::AudioEngine::SetBGMVolume, "MuteBGMLabel"},
        {"se", &Settings::seVolume, &Settings::seMuted,
         &audio::AudioEngine::SetSEVolume, "MuteSELabel"},
        {"voice", &Settings::voiceVolume, &Settings::voiceMuted,
         &audio::AudioEngine::SetVoiceVolume, "MuteVoiceLabel"},
    };
    const auto found = std::ranges::find(buses, name, &AudioBus::name);
    return found == std::end(buses) ? nullptr : &*found;
}

}  // namespace

// The settings-screen state of one audio bus: its level, its MUTE checkbox
// and the number shown next to the slider.
void PlayerApp::SetBusVolume(const std::string_view bus, const int volume) {
    const AudioBus* found = FindAudioBus(bus);
    if (!found) return;
    m_settings.*found->volume = std::clamp(volume, 0, 128);
    m_settings.*found->muted = false;
    (m_runtime.Audio().*found->apply)(m_settings.*found->volume);
    if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), found->muteLabel))
        label->SetText("□MUTE");
}

void PlayerApp::ToggleBusMute(const std::string_view bus) {
    const AudioBus* found = FindAudioBus(bus);
    if (!found) return;
    bool& muted = m_settings.*found->muted;
    const int volume = m_settings.*found->volume;
    muted = !muted;
    (m_runtime.Audio().*found->apply)(muted ? 0 : volume);
    if (auto* control = FindUiControlNamed<ui::Label>(m_ui.Root(), found->muteLabel))
        control->SetText(muted ? "☑MUTE" : "□MUTE");
    // The slider keeps the level; only the number reads "mute" (conf_mutetext).
    (void)m_ui.UpdateUiData("settings." + std::string(bus) + "Value", Variant(muted ? "mute" :
        std::to_string(static_cast<int>(std::round(volume * 100.0 / 128.0)))));
}

void PlayerApp::SetTextSpeed(const int milliseconds) {
    m_settings.textSpeedMs = std::clamp(milliseconds, 0, 120);
    m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
}

void PlayerApp::ResetSettings() {
    auto& audio = m_runtime.Audio();
    const std::string locale = m_settings.language;
    const bool fullscreen = m_settings.fullscreen;
    m_settings = m_settingsDefaults;
    m_runtime.GetInput().ResetKeyboardBindings();
    m_runtime.GetInput().ResetWheelBindings();
    m_settings.language = locale;
    m_settings.fullscreen = fullscreen;
    audio.SetMainVolume(m_settings.masterVolume);
    audio.SetBGMVolume(m_settings.bgmVolume);
    audio.SetSEVolume(m_settings.seVolume);
    audio.SetVoiceVolume(m_settings.voiceVolume);
    m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    ApplyAllOptions();
    (void)m_ui.ShowSettings(SettingsUI());
}

bool PlayerApp::MusicUnlocked(const std::size_t index) const {
    return index < m_musicTracks.size() && m_profile.MusicUnlocked(
        px::PathStemUtf8(m_musicTracks[index]));
}

void PlayerApp::PlayMusicAt(const std::size_t index) {
    if (m_musicTracks.empty()) return;
    const std::size_t selected = index % m_musicTracks.size();
    if (!MusicUnlocked(selected)) return;
    m_musicTrackIndex = selected;
    auto& audio = m_runtime.Audio();
    const auto handle = audio.PlayBGM(m_musicTracks[m_musicTrackIndex], true, 250);
    if (audio.Status(handle) == px::audio::PlaybackStatus::Failed) {
        PX_LOG_ERROR("Player music gallery failed path={}", m_musicTracks[m_musicTrackIndex]);
        return;
    }
    m_musicTrackSelected = true;
}

// Steps through the music room from the current track, skipping locked ones.
void PlayerApp::StepMusic(const int direction) {
    const std::size_t count = m_musicTracks.size();
    for (std::size_t step = 1; step <= count; ++step) {
        const std::size_t index = direction > 0
            ? (m_musicTrackIndex + step) % count
            : (m_musicTrackIndex + count - step % count) % count;
        if (MusicUnlocked(index)) {
            PlayMusicAt(index);
            return;
        }
    }
}

void PlayerApp::ScrollNamedList(const std::string_view controlName,
                                const std::string_view command) {
    auto* scroll = FindUiControlNamed<ui::ScrollContainer>(m_ui.Root(), controlName);
    if (!scroll) return;
    auto offset = scroll->ScrollOffset();
    if (command.ends_with(".home")) offset.y = 0.0f;
    else if (command.ends_with(".end")) offset.y = 1'000'000.0f;
    else if (command.ends_with(".up")) offset.y = std::max(0.0f, offset.y - 360.0f);
    else if (command.ends_with(".down")) offset.y += 360.0f;
    scroll->SetScrollOffset(offset);
}

void PlayerApp::CloseAllModals() {
    while (!m_session->Routes().CurrentModalRoute().empty())
        (void)m_session->Routes().CloseModal();
}

// A slot the player can manage: one on the save pages, or the autosave.
bool PlayerApp::ManagedSlot(const int slot) const {
    return slot == kAutoSaveSlot ||
           (slot >= m_saveFirstSlot &&
            slot < m_saveFirstSlot + m_savePages * m_saveSlotsPerPage);
}

void PlayerApp::RefreshSaveLoad() {
    const auto& operation = m_slotOperation;
    (void)m_ui.UpdateUiData("saveLoad.pending.visible", Variant(operation.has_value()));
    (void)m_ui.UpdateUiData("saveLoad.pending.copy",
                            Variant(operation.has_value() && !operation->move));
    (void)m_ui.UpdateUiData("saveLoad.pending.move",
                            Variant(operation.has_value() && operation->move));
    (void)m_ui.UpdateUiData("saveLoad.pending.slot",
        Variant(operation ? std::to_string(operation->slot) : std::string{}));
    const auto screen = m_ui.CurrentScreen();
    if (screen != ui::GalgameUI::Screen::Save && screen != ui::GalgameUI::Screen::Load) return;
    const bool saveMode = screen == ui::GalgameUI::Screen::Save;
    (void)m_ui.ShowSaveLoad(saveMode, SaveItems(saveMode));
}

// save.copy / save.move on a slot picks it as the source; the next slot the
// player activates (save.slot, load.slot, or the same action) is the target.
void PlayerApp::BeginSlotOperation(const bool move, const std::string& argument) {
    const auto slot = IntegerArgument(argument);
    if (!slot || !ManagedSlot(*slot)) return;
    if (m_slotOperation) {
        FinishSlotOperation(*slot);
        return;
    }
    // The autosave belongs to the Player: it can be copied, not moved.
    if (!m_saves.Peek(*slot).exists || (move && *slot == kAutoSaveSlot)) return;
    m_slotOperation = SlotOperation{move, *slot};
    RefreshSaveLoad();
}

void PlayerApp::FinishSlotOperation(const int target) {
    if (!m_slotOperation || !ManagedSlot(target) || target == kAutoSaveSlot) return;
    const SlotOperation operation = *m_slotOperation;
    if (target == operation.slot) {
        m_slotOperation.reset();
        RefreshSaveLoad();
        return;
    }
    const auto apply = [this, operation, target] {
        m_slotOperation.reset();
        const bool done = operation.move ? m_saves.Move(operation.slot, target)
                                         : m_saves.Copy(operation.slot, target);
        RefreshSaveLoad();
        if (done) m_ui.RevealSaveSlot(std::to_string(target));
        else (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
    };
    // Only replacing (copy) or swapping (move) an occupied slot needs a yes.
    if (m_saves.Peek(target).exists)
        RequestConfirm(operation.move ? "moveSave" : "copySave", apply);
    else
        apply();
}

void PlayerApp::DeleteSlot(const std::string& argument) {
    const auto slot = IntegerArgument(argument);
    if (!slot || !ManagedSlot(*slot) || !m_saves.Peek(*slot).exists) return;
    m_slotOperation.reset();
    const int number = *slot;
    RequestConfirm("deleteSave", [this, number] {
        if (!m_saves.Delete(number))
            (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
        RefreshSaveLoad();
    });
}

// save.comment fills the screen's SlotComment field with the slot's comment
// and focuses it; Enter or saveLoad.comment.accept stores the text.
void PlayerApp::BeginSlotComment(const std::string& argument) {
    const auto slot = IntegerArgument(argument);
    if (!slot || !ManagedSlot(*slot)) return;
    const auto info = m_saves.Peek(*slot);
    if (!info.exists) return;
    auto* field = FindUiControlNamed<ui::LineEdit>(m_ui.Root(), "SlotComment");
    if (!field) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Warning,
                                    .code = "PXPLAYER5030",
                                    .category = "Player.Save",
                                    .message = "save.comment needs a LineEdit named SlotComment on this screen"});
        return;
    }
    m_commentSlot = *slot;
    field->SetText(info.comment);
    field->SetOnSubmitted([this](const std::string&) {
        HandleUIAction({"saveLoad.comment.accept", {}});
    });
    (void)m_ui.UpdateUiData("saveLoad.comment.slot", Variant(std::to_string(*slot)));
    (void)m_ui.UpdateUiData("saveLoad.comment.visible", Variant(true));
    m_ui.FocusControl(field);
}

void PlayerApp::FinishSlotComment(const bool accept) {
    const auto slot = std::exchange(m_commentSlot, std::nullopt);
    (void)m_ui.UpdateUiData("saveLoad.comment.visible", Variant(false));
    m_ui.FocusControl(nullptr);
    if (!accept || !slot) return;
    std::string text;
    if (const auto* field = FindUiControlNamed<ui::LineEdit>(m_ui.Root(), "SlotComment"))
        text = field->Text();
    if (!m_saves.SetComment(*slot, TrimmedInput(std::move(text), kMaxSlotComment)))
        (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
    RefreshSaveLoad();
}

void PlayerApp::ActivateSlot(const bool saveMode, const std::string& argument) {
    if (m_slotOperation) {
        if (const auto target = IntegerArgument(argument)) FinishSlotOperation(*target);
        return;
    }
    const auto slot = IntegerArgument(argument);
    const bool valid = slot && *slot >= m_saveFirstSlot &&
        *slot < m_saveFirstSlot + m_savePages * m_saveSlotsPerPage &&
        (saveMode || m_saves.Peek(*slot).exists);
    if (saveMode ? AsksBefore("save") || AsksBefore("overwrite") : AsksBefore("load")) {
        if (!valid) return;
        const int number = *slot;
        const std::string_view operation = !saveMode ? "load"
            : m_saves.Peek(number).exists ? "overwrite" : "save";
        RequestConfirm(operation, [this, number, saveMode] {
            if (saveMode) {
                if (SaveSlot(number, m_menuThumb)) {
                    (void)m_ui.ShowSaveLoad(true, SaveItems(true));
                    m_ui.RevealSaveSlot(std::to_string(number));
                }
            } else if (LoadSlot(number)) {
                BeginPresentationTransition(m_loadTransition);
            } else {
                (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
            }
        });
    } else if (m_ui.SupportsSaveLoadConfirmation()) {
        if (!valid) return;
        m_pendingSaveLoadSlot = *slot;
        m_pendingSaveLoadSaveMode = saveMode;
        const auto prompt = !saveMode
            ? ui::GalgameUI::SaveLoadPrompt::Load
            : m_saves.Peek(*slot).exists
                ? ui::GalgameUI::SaveLoadPrompt::Overwrite
                : ui::GalgameUI::SaveLoadPrompt::Save;
        (void)m_ui.SetSaveLoadConfirmation(prompt);
    } else if (saveMode) {
        const int number = std::atoi(argument.c_str());
        const bool saved = SaveSlot(number, m_menuThumb);
        m_ui.ShowSaveLoad(true, SaveItems(true));
        if (saved) m_ui.RevealSaveSlot(std::to_string(number));
    } else {
        if (LoadSlot(std::atoi(argument.c_str())))
            BeginPresentationTransition(m_loadTransition);
    }
}

void PlayerApp::BeginTextInput(const vn::Command& command) {
    TextInputRequest request;
    request.variable = command.Get("variable");
    request.profile = command.Get("scope") == "profile";
    request.fallback = command.Get("default");
    if (const std::string limit = command.Get("maxLength"); !limit.empty())
        request.maxLength = static_cast<std::size_t>(std::clamp(std::atoi(limit.c_str()), 1, 256));
    auto* field = FindUiControlNamed<ui::LineEdit>(m_ui.Root(), "TextInput");
    if (!field) {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Warning,
                                    .code = "PXPLAYER5029",
                                    .category = "Player.Input",
                                    .message = "[input] needs a LineEdit named TextInput in the HUD; "
                                               "the default value was used",
                                    .details = request.variable});
        m_session->Variables().SetValue(request.variable, Variant(request.fallback),
            request.profile ? vn::VariableScope::Profile : vn::VariableScope::Session);
        return;
    }
    m_autoMode = false;
    m_skipMode = false;
    field->SetText(request.fallback);
    field->SetOnSubmitted([this](const std::string&) { HandleUIAction({"input.accept", {}}); });
    (void)m_ui.UpdateUiData("input.prompt", Variant(command.Get("prompt")));
    (void)m_ui.UpdateUiData("input.visible", Variant(true));
    m_ui.FocusControl(field);
    m_textInput = std::move(request);
    m_session->VM().WaitExternal();
    PX_LOG_INFO("Player text input started variable={}", m_textInput->variable);
}

void PlayerApp::FinishTextInput() {
    if (!m_textInput) return;
    const auto request = std::exchange(m_textInput, std::nullopt);
    std::string text;
    if (const auto* field = FindUiControlNamed<ui::LineEdit>(m_ui.Root(), "TextInput"))
        text = field->Text();
    text = TrimmedInput(std::move(text), request->maxLength);
    if (text.empty()) text = request->fallback;
    m_session->Variables().SetValue(request->variable, Variant(text),
        request->profile ? vn::VariableScope::Profile : vn::VariableScope::Session);
    (void)m_ui.UpdateUiData("input.visible", Variant(false));
    m_ui.FocusControl(nullptr);
    PX_LOG_INFO("Player text input finished variable={}", request->variable);
    m_session->VM().NotifyExternalDone();
}

void PlayerApp::SyncTextInput() {
    const bool wanted = m_ui.TextFieldFocused();
    if (wanted == m_sdlTextInputActive) return;
    SDL_Window* window = m_runtime.GetWindow().Handle();
    if (!window) return;
    if (wanted) SDL_StartTextInput(window);
    else SDL_StopTextInput(window);
    m_sdlTextInputActive = wanted;
}

// One handler per action in ui::kPlayerActions. Init registers exactly the
// Player-owned ids of that table and fails if this map and the table differ,
// so a TSX action can never install fine and then do nothing when clicked.
const std::unordered_map<std::string_view, PlayerApp::ActionHandler>&
PlayerApp::ActionHandlers() {
    using A = const ui::GalgameAction&;
    static const std::unordered_map<std::string_view, ActionHandler> handlers = [] {
        std::unordered_map<std::string_view, ActionHandler> map;
        const auto on = [&map](const std::string_view id, const ActionHandler handler) {
            map.emplace(id, handler);
        };
        // --- Game ---
        on("game.start", [](PlayerApp& app, A) { app.StartGame(); });
        on("game.continue", [](PlayerApp& app, A) {
            const auto saves = app.m_saves.List(1000);
            std::optional<int> latest;
            std::uint64_t latestTimestamp = 0;
            for (std::size_t index = 0; index < saves.size(); ++index) {
                if (saves[index].exists &&
                    (!latest || saves[index].timestamp >= latestTimestamp)) {
                    latest = static_cast<int>(index);
                    latestTimestamp = saves[index].timestamp;
                }
            }
            if (latest) (void)app.LoadSlot(*latest);
            else app.OpenScreen("load");
        });
        on("app.quit", [](PlayerApp& app, A) {
            if (app.AsksBefore("exit")) {
                app.RequestConfirm("exit", [&app] { app.m_quitRequested = true; });
            } else if (app.m_ui.SupportsSystemConfirmation()) {
                app.m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::Exit;
                (void)app.m_ui.SetSystemConfirmation(app.m_pendingSystemPrompt);
            } else {
                app.m_quitRequested = true;
            }
        });
        on("hud.hide", [](PlayerApp& app, A) { app.m_hudHidden = true; });
        on("input.accept", [](PlayerApp& app, A) { app.FinishTextInput(); });
        on("mode.auto", [](PlayerApp& app, A) {
            app.m_autoMode = !app.m_autoMode;
            if (app.m_autoMode) app.m_skipMode = false;
        });
        on("mode.skip", [](PlayerApp& app, A) {
            app.m_skipMode = !app.m_skipMode;
            if (app.m_skipMode) app.m_autoMode = false;
        });
        on("choice.select", [](PlayerApp& app, A action) {
            app.m_session->SelectChoice(std::atoi(action.argument.c_str()));
        });
        on("choice.previous", [](PlayerApp& app, A) {
            // The choice autosave is the state at the most recent choice prompt.
            app.RequestConfirm("previousChoice", [&app] {
                if (app.m_saves.Peek(kAutoSaveSlot).exists) (void)app.LoadSlot(kAutoSaveSlot);
            });
        });
        on("choice.next", [](PlayerApp& app, A) {
            // Skip stops at the next choice, and at unread text unless the
            // player allowed skipping it.
            app.RequestConfirm("nextChoice", [&app] {
                app.m_skipMode = true;
                app.m_autoMode = false;
            });
        });
        on("voice.replay", [](PlayerApp& app, A) {
            const auto& lines = app.m_session->Backlog().Entries();
            const auto voiced = std::find_if(lines.rbegin(), lines.rend(), [](const auto& entry) {
                return !entry.voice.empty();
            });
            if (voiced != lines.rend() && voiced == lines.rbegin())
                app.m_runtime.Audio().PlayVoice(voiced->voice.find('/') != std::string::npos
                    ? voiced->voice : app.m_session->VM().Config().voiceDir + voiced->voice);
        });
        const auto backlogEntry = [](PlayerApp& app, A action) {
            std::uint64_t sequence = 0;
            try {
                sequence = std::stoull(action.argument);
            } catch (const std::exception&) {
                sequence = 0;
            }
            const auto* entry = app.m_session->Backlog().Find(sequence);
            if (!entry) return;
            if (action.command == "backlog.voice" && !entry->voice.empty())
                app.m_runtime.Audio().PlayVoice(entry->voice.find('/') != std::string::npos
                    ? entry->voice : app.m_session->VM().Config().voiceDir + entry->voice);
            else
                app.RequestConfirm("backlogJump", [&app, sequence = entry->sequence] {
                    app.RollbackToBacklogSequence(sequence);
                });
        };
        on("backlog.voice", backlogEntry);
        on("backlog.rollback", backlogEntry);

        // --- Screens ---
        on("screen.open", [](PlayerApp& app, A action) {
            if (!action.argument.empty()) app.OpenScreen(action.argument);
        });
        on("title.open", [](PlayerApp& app, A) {
            if (app.m_appState == AppState::Game && app.AsksBefore("returnToTitle"))
                app.RequestConfirm("returnToTitle", [&app] { app.ReturnToTitle(); });
            else if (app.m_appState == AppState::Game && app.m_ui.SupportsTitleConfirmation())
                (void)app.m_ui.SetTitleConfirmation(true);
            else
                app.ReturnToTitle();
        });
        on("save.open", [](PlayerApp& app, A) {
            app.m_slotSaveMode = true;
            if (app.m_ui.CurrentScreen() == ui::GalgameUI::Screen::Load ||
                app.m_appState != AppState::Game)
                app.OpenScreen("save");
            else
                app.m_pendingSaveScreen = true;
        });
        on("load.open", [](PlayerApp& app, A) {
            app.m_slotSaveMode = false;
            app.OpenScreen("load");
        });
        for (const std::string_view route :
             {"settings", "backlog", "gallery", "extra", "replay", "music", "flowchart"}) {
            // The id outlives the map: these are literals in kPlayerActions.
            const auto* spec = ui::FindPlayerAction(std::string(route) + ".open");
            on(spec->id, [](PlayerApp& app, A action) {
                app.OpenScreen(action.command.substr(0, action.command.size() - 5));
            });
        }
        on("overlay.close", [](PlayerApp& app, A) {
            if (app.m_dialog) {
                app.HandleUIAction({"dialog.cancel", {}});
                return;
            }
            if (app.m_pendingSystemPrompt != ui::GalgameUI::SystemPrompt::None) {
                app.m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
                (void)app.m_ui.SetSystemConfirmation(app.m_pendingSystemPrompt);
                return;
            }
            if (app.m_pendingSaveLoadSlot) {
                app.m_pendingSaveLoadSlot.reset();
                (void)app.m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
                return;
            }
            const bool restoreTitleMusic = app.m_musicTrackSelected &&
                (app.m_ui.CurrentRoute() == "music" || app.m_ui.CurrentRoute() == "extra");
            if (!app.m_session->Routes().CloseModal()) return;
            if (restoreTitleMusic && !app.TitleBgmPath().empty()) {
                app.m_runtime.Audio().PlayBGM(app.TitleBgmPath(), true, 300);
                app.m_musicTrackSelected = false;
            }
            // Closing a menu must not wait on disk; both files are written by
            // background workers and flushed synchronously at shutdown.
            app.SaveSettingsInBackground();
            app.MarkProfileDirty(true);
            PX_LOG_INFO("Player overlay closed modal={} route={}",
                        app.m_session->Routes().CurrentModalRoute(),
                        app.m_session->Routes().CurrentRoute());
        });
        for (int page = 0; page < 8; ++page) {
            const auto* spec = ui::FindPlayerAction("settings.page" + std::to_string(page));
            on(spec->id, [](PlayerApp& app, A action) {
                const char page = action.command.back();
                if (app.m_boot.routeScenes.contains("settings") && page >= '1' && page <= '4') {
                    app.m_settingsPage = page - '0';
                    for (int index = 1; index <= 4; ++index)
                        (void)app.m_ui.UpdateUiData("settings.page" + std::to_string(index),
                                                    Variant(index == app.m_settingsPage));
                    // Pages are bound groups of one installed screen; rebuilding it
                    // here would restart every transition on the screen.
                    return;
                }
                app.CloseAllModals();
                app.OpenScreen(page == '0' ? "settings" : "settings-page" + std::string(1, page));
            });
        }

        // --- Confirmations ---
        on("dialog.accept", [](PlayerApp& app, A) {
            if (!app.m_dialog) return;
            auto dialog = std::move(*app.m_dialog);
            app.m_dialog.reset();
            if (app.m_dialogSkipNext && !dialog.dontAskOption.empty())
                (void)app.SetOption(dialog.dontAskOption, 1);
            app.m_dialogSkipNext = false;
            app.PublishDialog();
            dialog.accept();
        });
        on("dialog.cancel", [](PlayerApp& app, A) {
            app.m_dialog.reset();
            app.m_dialogSkipNext = false;
            app.PublishDialog();
        });
        on("dialog.toggleSkip", [](PlayerApp& app, A) {
            if (!app.m_dialog) return;
            app.m_dialogSkipNext = !app.m_dialogSkipNext;
            app.PublishDialog();
        });
        on("title.confirm.accept", [](PlayerApp& app, A) { app.ReturnToTitle(); });
        on("title.confirm.cancel", [](PlayerApp& app, A) {
            (void)app.m_ui.SetTitleConfirmation(false);
        });
        on("system.confirm.cancel", [](PlayerApp& app, A) {
            app.m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
            (void)app.m_ui.SetSystemConfirmation(app.m_pendingSystemPrompt);
        });
        on("system.confirm.accept", [](PlayerApp& app, A) {
            const auto prompt = std::exchange(
                app.m_pendingSystemPrompt, ui::GalgameUI::SystemPrompt::None);
            (void)app.m_ui.SetSystemConfirmation(app.m_pendingSystemPrompt);
            if (prompt == ui::GalgameUI::SystemPrompt::Reset) app.ResetSettings();
            else if (prompt == ui::GalgameUI::SystemPrompt::Exit) app.m_quitRequested = true;
        });

        // --- Save and load ---
        on("save.slot", [](PlayerApp& app, A action) { app.ActivateSlot(true, action.argument); });
        on("load.slot", [](PlayerApp& app, A action) { app.ActivateSlot(false, action.argument); });
        on("saveLoad.page", [](PlayerApp& app, A action) {
            const auto page = IntegerArgument(action.argument);
            if (!page || *page < 1 || *page > app.m_savePages) return;
            app.m_saveLoadPage = *page;
            for (int index = 1; index <= app.m_savePages; ++index)
                (void)app.m_ui.UpdateUiData("saveLoad.page" + std::to_string(index),
                                            Variant(index == app.m_saveLoadPage));
            const bool saveMode = app.m_ui.CurrentScreen() == ui::GalgameUI::Screen::Save;
            (void)app.m_ui.ShowSaveLoad(saveMode, app.SaveItems(saveMode));
        });
        on("saveLoad.confirm.accept", [](PlayerApp& app, A) {
            const auto pending = std::exchange(app.m_pendingSaveLoadSlot, std::nullopt);
            (void)app.m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
            if (!pending) return;
            if (app.m_pendingSaveLoadSaveMode) {
                if (app.SaveSlot(*pending, app.m_menuThumb)) {
                    (void)app.m_ui.ShowSaveLoad(true, app.SaveItems(true));
                    app.m_ui.RevealSaveSlot(std::to_string(*pending));
                }
            } else if (app.LoadSlot(*pending)) {
                app.BeginPresentationTransition(app.m_loadTransition);
            } else {
                (void)app.m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
            }
        });
        on("save.delete", [](PlayerApp& app, A action) { app.DeleteSlot(action.argument); });
        on("save.copy", [](PlayerApp& app, A action) {
            app.BeginSlotOperation(false, action.argument);
        });
        on("save.move", [](PlayerApp& app, A action) {
            app.BeginSlotOperation(true, action.argument);
        });
        on("saveLoad.pending.cancel", [](PlayerApp& app, A) {
            if (!app.m_slotOperation) return;
            app.m_slotOperation.reset();
            app.RefreshSaveLoad();
        });
        on("save.comment", [](PlayerApp& app, A action) { app.BeginSlotComment(action.argument); });
        on("saveLoad.comment.accept", [](PlayerApp& app, A) { app.FinishSlotComment(true); });
        on("saveLoad.comment.cancel", [](PlayerApp& app, A) { app.FinishSlotComment(false); });
        on("saveLoad.confirm.cancel", [](PlayerApp& app, A) {
            app.m_pendingSaveLoadSlot.reset();
            (void)app.m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
        });
        on("saveLoad.error.dismiss", [](PlayerApp& app, A) {
            (void)app.m_ui.UpdateUiData("saveLoad.error.visible", Variant(false));
        });
        on("save.quick", [](PlayerApp& app, A) {
            // Written after the Stage renders, so the thumbnail has no HUD.
            app.RequestConfirm("quickSave", [&app] { app.m_saveRequested = true; });
        });
        on("load.quick", [](PlayerApp& app, A) {
            app.RequestConfirm("quickLoad", [&app] {
                if (app.m_saves.Peek(0).exists) (void)app.LoadSlot(0);
            });
        });

        // --- Settings ---
        for (const std::string_view bus : {"master", "bgm", "se", "voice"}) {
            const std::string prefix = "set." + std::string(bus);
            on(ui::FindPlayerAction(prefix + ".value")->id, [](PlayerApp& app, A action) {
                app.SetBusVolume(BusOf(action.command), std::atoi(action.argument.c_str()));
            });
            on(ui::FindPlayerAction(prefix + ".up")->id, [](PlayerApp& app, A action) {
                const auto bus = BusOf(action.command);
                app.SetBusVolume(bus, app.BusVolume(bus) + 8);
            });
            on(ui::FindPlayerAction(prefix + ".down")->id, [](PlayerApp& app, A action) {
                const auto bus = BusOf(action.command);
                app.SetBusVolume(bus, app.BusVolume(bus) - 8);
            });
            on(ui::FindPlayerAction("set.mute." + std::string(bus))->id,
               [](PlayerApp& app, A action) {
                   app.ToggleBusMute(std::string_view(action.command).substr(9));
               });
        }
        on("set.speed.value", [](PlayerApp& app, A action) {
            app.SetTextSpeed(std::atoi(action.argument.c_str()));
        });
        on("set.speed.up", [](PlayerApp& app, A) {
            app.SetTextSpeed(app.m_settings.textSpeedMs + 4);
        });
        on("set.speed.down", [](PlayerApp& app, A) {
            app.SetTextSpeed(app.m_settings.textSpeedMs - 4);
        });
        const auto duck = [](PlayerApp& app, A action) {
            app.m_settings.duckBgmOnVoice = action.command.ends_with(".on");
            app.m_runtime.Audio().SetVoiceDuckingEnabled(app.m_settings.duckBgmOnVoice);
            (void)app.m_ui.UpdateUiData("settings.duckOn", Variant(app.m_settings.duckBgmOnVoice));
            (void)app.m_ui.UpdateUiData("settings.duckOff", Variant(!app.m_settings.duckBgmOnVoice));
        };
        on("set.duck.on", duck);
        on("set.duck.off", duck);
        const auto voiceCarry = [](PlayerApp& app, A action) {
            app.m_settings.continueVoiceOnAdvance = action.command.ends_with(".on");
            (void)app.m_ui.UpdateUiData("settings.voiceCarryOn",
                                        Variant(app.m_settings.continueVoiceOnAdvance));
            (void)app.m_ui.UpdateUiData("settings.voiceCarryOff",
                                        Variant(!app.m_settings.continueVoiceOnAdvance));
        };
        on("set.voiceCarry.on", voiceCarry);
        on("set.voiceCarry.off", voiceCarry);
        on("set.voiceProfile.value", [](PlayerApp& app, A action) {
            const auto separator = action.argument.find(':');
            if (separator == std::string::npos || separator == 0 || separator > 64) return;
            const std::string id = action.argument.substr(0, separator);
            const int volume = std::clamp(std::atoi(action.argument.c_str() + separator + 1), 0, 128);
            app.m_settings.voiceProfileVolumes[id] = volume;
            app.m_settings.voiceProfileMuted[id] = false;
            app.m_runtime.Audio().SetVoiceProfileVolume(id, volume);
            if (auto* label = FindUiControlNamed<ui::Label>(app.m_ui.Root(),
                    "VoiceProfileMute_" + id + "Label"))
                label->SetText("□MUTE");
        });
        on("set.voiceProfile.mute", [](PlayerApp& app, A action) {
            const std::string& id = action.argument;
            if (id.empty() || id.size() > 64) return;
            const bool muted = !app.m_settings.voiceProfileMuted[id];
            app.m_settings.voiceProfileMuted[id] = muted;
            const auto found = app.m_settings.voiceProfileVolumes.find(id);
            const int volume = found == app.m_settings.voiceProfileVolumes.end() ? 128 : found->second;
            app.m_runtime.Audio().SetVoiceProfileVolume(id, muted ? 0 : volume);
            if (auto* label = FindUiControlNamed<ui::Label>(app.m_ui.Root(),
                    "VoiceProfileMute_" + id + "Label"))
                label->SetText(muted ? "☑MUTE" : "□MUTE");
            if (auto* label = FindUiControlNamed<ui::Label>(app.m_ui.Root(),
                    "VoiceProfileValue_" + id))
                label->SetText(muted ? "mute" : std::to_string(
                    static_cast<int>(std::round(volume * 100.0 / 128.0))));
        });
        on("voiceProfile.test", [](PlayerApp& app, A action) {
            app.m_runtime.Audio().PlayVoice(action.argument);
        });
        on("audio.test.se", [](PlayerApp& app, A action) {
            app.m_runtime.Audio().PlaySE(action.argument);
        });
        on("set.skipread.value", [](PlayerApp& app, A action) {
            app.m_settings.skipReadOnly = action.argument == "true";
        });
        on("set.skipread.toggle", [](PlayerApp& app, A) {
            app.m_settings.skipReadOnly = !app.m_settings.skipReadOnly;
            (void)app.m_ui.ShowSettings(app.SettingsUI());
        });
        on("set.fullscreen.value", [](PlayerApp& app, A action) {
            (void)app.ApplyFullscreenSetting(action.argument == "true");
        });
        const auto fullscreen = [](PlayerApp& app, A action) {
            (void)app.ApplyFullscreenSetting(action.command.ends_with(".on"));
        };
        on("set.fullscreen.on", fullscreen);
        on("set.fullscreen.off", fullscreen);
        on("set.fullscreen.toggle", [](PlayerApp& app, A) {
            (void)app.ApplyFullscreenSetting(!app.m_settings.fullscreen);
            if (app.m_ui.CurrentRoute() == "settings") (void)app.m_ui.ShowSettings(app.SettingsUI());
        });
        on("set.language.value", [](PlayerApp& app, A action) {
            if (app.LoadLocale(action.argument, true)) (void)app.SaveSettingsIfWritable();
        });
        on("set.textscale.value", [](PlayerApp& app, A action) {
            app.m_settings.textScale =
                std::clamp(std::atoi(action.argument.c_str()) / 100.0f, .75f, 2.0f);
        });
        on("set.highcontrast.value", [](PlayerApp& app, A action) {
            app.m_settings.highContrast = action.argument == "true";
        });
        on("set.reducedmotion.value", [](PlayerApp& app, A action) {
            app.m_settings.reducedMotion = action.argument == "true";
        });
        const auto reducedMotion = [](PlayerApp& app, A action) {
            app.m_settings.reducedMotion = action.command.ends_with(".on");
            (void)app.m_ui.ShowSettings(app.SettingsUI());
        };
        on("set.reducedmotion.on", reducedMotion);
        on("set.reducedmotion.off", reducedMotion);
        on("set.selfvoicing.value", [](PlayerApp& app, A action) {
            app.m_settings.selfVoicing = action.argument == "true";
            if (!app.m_settings.selfVoicing) app.m_speech.Stop();
        });
        on("settings.reset", [](PlayerApp& app, A) {
            if (app.AsksBefore("resetSettings")) {
                app.RequestConfirm("resetSettings", [&app] { app.ResetSettings(); });
            } else if (app.m_ui.SupportsSystemConfirmation()) {
                app.m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::Reset;
                (void)app.m_ui.SetSystemConfirmation(app.m_pendingSystemPrompt);
            } else {
                app.ResetSettings();
            }
        });
        on("settings.noop", [](PlayerApp&, A) {});
        on("option.set", [](PlayerApp& app, A action) {
            const auto split = action.argument.find('=');
            if (split == std::string::npos) return;
            if (const auto value = IntegerArgument(action.argument.substr(split + 1)))
                (void)app.SetOption(std::string_view(action.argument).substr(0, split), *value);
        });

        // --- Music room ---
        on("music.play", [](PlayerApp& app, A action) {
            const auto found = std::ranges::find(app.m_musicTracks, action.argument);
            if (found == app.m_musicTracks.end()) {
                PX_LOG_ERROR("Player music gallery received unknown track path={}", action.argument);
                return;
            }
            app.PlayMusicAt(static_cast<std::size_t>(std::distance(app.m_musicTracks.begin(), found)));
            PX_LOG_INFO("Player music gallery selected path={}", action.argument);
        });
        on("music.previous", [](PlayerApp& app, A) { app.StepMusic(-1); });
        on("music.next", [](PlayerApp& app, A) { app.StepMusic(1); });
        on("music.random", [](PlayerApp& app, A) {
            std::vector<std::size_t> available;
            for (std::size_t index = 0; index < app.m_musicTracks.size(); ++index)
                if (app.MusicUnlocked(index)) available.push_back(index);
            if (!available.empty())
                app.PlayMusicAt(available[static_cast<std::size_t>(SDL_GetTicks()) % available.size()]);
        });
        on("music.stop", [](PlayerApp& app, A) {
            app.m_runtime.Audio().StopBGM(200);
            app.m_musicTrackSelected = false;
        });
        on("music.resume", [](PlayerApp& app, A) {
            if (app.m_musicTrackSelected) {
                app.m_runtime.Audio().SetMusicPaused(false);
            } else if (app.MusicUnlocked(app.m_musicTrackIndex)) {
                app.PlayMusicAt(app.m_musicTrackIndex);
            } else {
                for (std::size_t index = 0; index < app.m_musicTracks.size(); ++index)
                    if (app.MusicUnlocked(index)) { app.PlayMusicAt(index); break; }
            }
        });
        on("music.about", [](PlayerApp& app, A) { app.m_runtime.Audio().SetMusicPaused(true); });
        for (const std::string_view direction : {"home", "up", "down", "end"}) {
            on(ui::FindPlayerAction("music.scroll." + std::string(direction))->id,
               [](PlayerApp& app, A action) { app.ScrollNamedList("MusicTracksScroll", action.command); });
            on(ui::FindPlayerAction("flowchart.scroll." + std::string(direction))->id,
               [](PlayerApp& app, A action) { app.ScrollNamedList("FlowGraphScroll", action.command); });
        }

        // --- Gallery, replay and flowchart ---
        on("cg.view", [](PlayerApp& app, A action) {
            const auto& entries = app.m_catalog.Gallery();
            const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
                return entry.id == action.argument;
            });
            if (found == entries.end()) app.m_viewingCG = action.argument;
            else if (app.m_profile.CGUnlocked(found->id)) app.m_viewingCG = found->image;
        });
        on("replay.launch", [](PlayerApp& app, A action) {
            if (action.argument.empty()) return;
            app.m_pendingStoryEntry = action.argument;
            app.CloseAllModals();
            app.StartGame();
        });
        on("replay.page", [](PlayerApp& app, A action) {
            const auto page = IntegerArgument(action.argument);
            if (!page || *page < 1 || *page > 4) return;
            app.m_replayPage = *page;
            for (int index = 1; index <= 4; ++index)
                if (const Status updated = app.m_ui.UpdateUiData(
                        "replay.page" + std::to_string(index), Variant(index == *page)); !updated)
                    for (const auto& diagnostic : updated.Diagnostics()) diag::Emit(diagnostic);
        });
        on("flowchart.select", [](PlayerApp& app, A action) {
            const auto selected = nlohmann::json::parse(action.argument, nullptr, false);
            if (!selected.is_object() || selected.value("entry", "").empty() ||
                selected.value("heading", "").size() >= 512 ||
                selected.value("details", "").size() >= 2048)
                return;
            app.m_selectedFlowEntry = selected.value("entry", "");
            const std::string heading = selected.value("heading", "");
            for (const std::string_view name : {"FlowSelectedItem", "FlowSelectedHeading"})
                if (auto* label = FindUiControlNamed<ui::Label>(app.m_ui.Root(), name))
                    label->SetText(heading);
            if (auto* details = FindUiControlNamed<ui::Label>(app.m_ui.Root(), "FlowSelectedDetails"))
                details->SetText(selected.value("details", ""));
            if (auto* jump = FindUiControlNamed<ui::Button>(app.m_ui.Root(), "scenejumpButton"))
                jump->SetEnabled(true);
            PX_LOG_INFO("Player flowchart selected entry={}", app.m_selectedFlowEntry);
        });
        on("flowchart.jump", [](PlayerApp& app, A) {
            if (app.m_selectedFlowEntry.empty()) return;
            app.m_pendingStoryEntry = std::exchange(app.m_selectedFlowEntry, {});
            (void)app.m_session->Routes().CloseModal();
            app.StartGame();
        });
        on("flowchart.filter", [](PlayerApp& app, A action) {
            if (action.argument != "flowchart" && !action.argument.starts_with("flowchart-"))
                return;
            app.m_selectedFlowEntry.clear();
            app.CloseAllModals();
            app.OpenScreen(action.argument);
        });
        return map;
    }();
    return handlers;
}

std::string_view PlayerApp::BusOf(const std::string_view command) {
    // "set.<bus>.<verb>"
    const auto start = command.find('.') + 1;
    return command.substr(start, command.find('.', start) - start);
}

int PlayerApp::BusVolume(const std::string_view bus) const {
    const AudioBus* found = FindAudioBus(bus);
    return found ? m_settings.*found->volume : 0;
}

void PlayerApp::ReturnToTitle() {
    (void)m_ui.SetTitleConfirmation(false);
    CloseAllModals();
    (void)m_session->Routes().Replace("title");
}

// Registers every Player-owned action of ui::kPlayerActions with the UI, and
// checks that each one has a handler here and nothing else does.
bool PlayerApp::RegisterUiActions() {
    const auto& handlers = ActionHandlers();
    std::size_t playerOwned = 0;
    for (const auto& spec : ui::kPlayerActions) {
        if (spec.owner != ui::PlayerActionOwner::Player) continue;
        ++playerOwned;
        if (!handlers.contains(spec.id)) {
            diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                        .code = "PXPLAYER5027",
                                        .category = "Player.Input",
                                        .message = "Player action has no handler",
                                        .details = std::string(spec.id)});
            return false;
        }
        const std::string key = spec.argumentKey.empty() ? "argument" : std::string(spec.argumentKey);
        if (const Status registered = m_ui.RegisterActionCommand(std::string(spec.id), key);
            !registered) {
            for (const auto& diagnostic : registered.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
    }
    if (handlers.size() != playerOwned) {
        for (const auto& [id, handler] : handlers)
            if (!ui::FindPlayerAction(id))
                diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                            .code = "PXPLAYER5027",
                                            .category = "Player.Input",
                                            .message = "Player handles an action missing from the contract",
                                            .details = std::string(id)});
        return false;
    }
    return true;
}

void PlayerApp::HandleUIAction(const ui::GalgameAction& action) {
    const std::string& t = action.command;
    // Settings actions keep the MUTE checkboxes' data in step with the model.
    struct MuteSync {
        PlayerApp* app;
        bool active;
        ~MuteSync() { if (active) app->PublishMuteData(); }
    } muteSync{this, t.starts_with("set.") || t.starts_with("settings.")};
    // A name prompt is modal: only its own button and quitting get through.
    if (m_textInput && t != "input.accept" && t != "app.quit") {
        muteSync.active = false;
        return;
    }
    const auto& handlers = ActionHandlers();
    const auto found = handlers.find(t);
    if (found == handlers.end()) {
        muteSync.active = false;
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                    .code = "PXPLAYER5023",
                                    .category = "Player.Input",
                                    .message = "UI command is not handled by this Player",
                                    .details = t});
        return;
    }
    found->second(*this, action);
    if (t.starts_with("set.") && (t.ends_with(".up") || t.ends_with(".down")))
        (void)m_ui.ShowSettings(SettingsUI());
}

}  // namespace px::player
