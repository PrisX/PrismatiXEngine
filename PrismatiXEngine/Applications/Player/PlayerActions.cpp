#include "Applications/Player/PlayerApp.h"
#include "Applications/Player/PlayerControls.h"
#include "Applications/Player/PlayerSupport.h"
#include "Applications/Player/QaHooks.h"

#include "Engine/Graphics/Screenshot.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Diagnostics/FrameProfiler.h"
#include "Engine/IO/PackageVerifier.h"
#include "Engine/Progression/AwaitedWork.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/UserDataLocation.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Script/ScriptFrameScheduler.h"
#include "Engine/Script/VnModelSnapshots.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/Support/Logger.h"
#include "Engine/Support/Utf8Path.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Startup/SplashTypes.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace px::player {

// Every UI action the stock Player handles: screens, save/load, settings,
// options, confirmations, music room, replay and flowchart.
void PlayerApp::HandleUIAction(const ui::GalgameAction& action) {
    const std::string& t = action.command;
    auto& audio = m_runtime.Audio();
    // Settings actions keep the MUTE checkboxes' data in step with the model.
    struct MuteSync {
        PlayerApp* app;
        bool active;
        ~MuteSync() { if (active) app->PublishMuteData(); }
    } muteSync{this, t.starts_with("set.") || t.starts_with("settings.") ||
                      t == "system.reset.apply"};
    const auto musicUnlocked = [this](const std::size_t index) {
        return index < m_musicTracks.size() && m_profile.MusicUnlocked(
            std::filesystem::path(m_musicTracks[index]).stem().string());
    };
    const auto playMusicAt = [this, &audio](const std::size_t index) {
        if (m_musicTracks.empty()) return;
        const std::size_t selected = index % m_musicTracks.size();
        if (!m_profile.MusicUnlocked(
                std::filesystem::path(m_musicTracks[selected]).stem().string())) return;
        m_musicTrackIndex = selected;
        const auto handle = audio.PlayBGM(m_musicTracks[m_musicTrackIndex], true, 250);
        if (audio.Status(handle) == px::audio::PlaybackStatus::Failed) {
            PX_LOG_ERROR("Player music gallery failed path={}",
                         m_musicTracks[m_musicTrackIndex]);
            return;
        }
        m_musicTrackSelected = true;
    };
    const auto scrollRoute = [this](const std::string_view controlName,
                                    const std::string_view command) {
        auto* scroll = FindUiControlNamed<ui::ScrollContainer>(
            m_ui.Root(), controlName);
        if (!scroll) return;
        auto offset = scroll->ScrollOffset();
        if (command.ends_with(".home")) offset.y = 0.0f;
        else if (command.ends_with(".end")) offset.y = 1'000'000.0f;
        else if (command.ends_with(".up")) offset.y = std::max(0.0f, offset.y - 360.0f);
        else if (command.ends_with(".down")) offset.y += 360.0f;
        scroll->SetScrollOffset(offset);
    };
    if (t == "saveLoad.page") {
        char* end = nullptr;
        const long page = std::strtol(action.argument.c_str(), &end, 10);
        if (end == action.argument.c_str() || *end != '\0' ||
            page < 1 || page > m_savePages) return;
        m_saveLoadPage = static_cast<int>(page);
        for (int index = 1; index <= m_savePages; ++index)
            (void)m_ui.UpdateUiData("saveLoad.page" + std::to_string(index),
                                    Variant(index == m_saveLoadPage));
        const bool saveMode = m_ui.CurrentScreen() == ui::GalgameUI::Screen::Save;
        (void)m_ui.ShowSaveLoad(saveMode, SaveItems(saveMode));
    } else if (t == "load.slot" || t == "save.slot") {
        const bool saveMode = t == "save.slot";
        if (saveMode ? AsksBefore("save") || AsksBefore("overwrite") : AsksBefore("load")) {
            char* end = nullptr;
            const long slot = std::strtol(action.argument.c_str(), &end, 10);
            if (end == action.argument.c_str() || *end != '\0' ||
                slot < m_saveFirstSlot ||
                slot >= m_saveFirstSlot + m_savePages * m_saveSlotsPerPage ||
                (!saveMode && !m_saves.Peek(static_cast<int>(slot)).exists))
                return;
            const int number = static_cast<int>(slot);
            const std::string_view operation = !saveMode ? "load"
                : m_saves.Peek(number).exists ? "overwrite" : "save";
            RequestConfirm(operation, [this, number, saveMode] {
                if (saveMode) {
                    if (SaveSlot(number, m_menuThumb))
                        (void)m_ui.ShowSaveLoad(true, SaveItems(true));
                } else if (LoadSlot(number)) {
                    BeginPresentationTransition(m_routeTransition);
                } else {
                    (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
                }
            });
        } else if (m_ui.SupportsSaveLoadConfirmation()) {
            char* end = nullptr;
            const long slot = std::strtol(action.argument.c_str(), &end, 10);
            if (end == action.argument.c_str() || *end != '\0' ||
                slot < m_saveFirstSlot ||
                slot >= m_saveFirstSlot + m_savePages * m_saveSlotsPerPage ||
                (!saveMode && !m_saves.Peek(static_cast<int>(slot)).exists))
                return;
            m_pendingSaveLoadSlot = static_cast<int>(slot);
            m_pendingSaveLoadSaveMode = saveMode;
            const auto prompt = !saveMode
                ? ui::GalgameUI::SaveLoadPrompt::Load
                : m_saves.Peek(static_cast<int>(slot)).exists
                    ? ui::GalgameUI::SaveLoadPrompt::Overwrite
                    : ui::GalgameUI::SaveLoadPrompt::Save;
            (void)m_ui.SetSaveLoadConfirmation(prompt);
        } else if (saveMode) {
            SaveSlot(std::atoi(action.argument.c_str()), m_menuThumb);
            m_ui.ShowSaveLoad(true, SaveItems(true));
        } else {
            LoadSlot(std::atoi(action.argument.c_str()));
        }
    } else if (t == "saveLoad.confirm.accept") {
        const auto pending = std::exchange(m_pendingSaveLoadSlot, std::nullopt);
        (void)m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
        if (!pending) return;
        if (m_pendingSaveLoadSaveMode) {
            if (SaveSlot(*pending, m_menuThumb))
                (void)m_ui.ShowSaveLoad(true, SaveItems(true));
        } else {
            if (LoadSlot(*pending))
                BeginPresentationTransition(m_routeTransition);
            else
                (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(true));
        }
    } else if (t == "saveLoad.confirm.cancel") {
        m_pendingSaveLoadSlot.reset();
        (void)m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
    } else if (t == "saveLoad.error.dismiss") {
        (void)m_ui.UpdateUiData("saveLoad.error.visible", Variant(false));
    } else if (t == "set.master.value") {
        m_settings.masterVolume = std::clamp(std::atoi(action.argument.c_str()), 0, 128);
        m_settings.masterMuted = false;
        audio.SetMainVolume(m_settings.masterVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteMasterLabel"))
            label->SetText("□MUTE");
    } else if (t == "set.bgm.value") {
        m_settings.bgmVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);m_settings.bgmMuted=false;audio.SetBGMVolume(m_settings.bgmVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteBGMLabel")) label->SetText("□MUTE");
    } else if (t == "set.se.value") {
        m_settings.seVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);m_settings.seMuted=false;audio.SetSEVolume(m_settings.seVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteSELabel")) label->SetText("□MUTE");
    } else if (t == "set.voice.value") {
        m_settings.voiceVolume=std::clamp(std::atoi(action.argument.c_str()),0,128);m_settings.voiceMuted=false;audio.SetVoiceVolume(m_settings.voiceVolume);
        if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(), "MuteVoiceLabel")) label->SetText("□MUTE");
    } else if (t.starts_with("set.mute.")) {
        const std::string bus = t.substr(std::string("set.mute.").size());
        bool* muted = nullptr;
        int volume = 0;
        std::string label;
        if (bus == "master") { muted = &m_settings.masterMuted; volume = m_settings.masterVolume; label = "MuteMasterLabel"; }
        else if (bus == "bgm") { muted = &m_settings.bgmMuted; volume = m_settings.bgmVolume; label = "MuteBGMLabel"; }
        else if (bus == "se") { muted = &m_settings.seMuted; volume = m_settings.seVolume; label = "MuteSELabel"; }
        else if (bus == "voice") { muted = &m_settings.voiceMuted; volume = m_settings.voiceVolume; label = "MuteVoiceLabel"; }
        if (muted) {
            *muted = !*muted;
            const int effective = *muted ? 0 : volume;
            if (bus == "master") audio.SetMainVolume(effective);
            else if (bus == "bgm") audio.SetBGMVolume(effective);
            else if (bus == "se") audio.SetSEVolume(effective);
            else audio.SetVoiceVolume(effective);
            if (auto* control = FindUiControlNamed<ui::Label>(m_ui.Root(), label))
                control->SetText(*muted ? "☑MUTE" : "□MUTE");
            // The slider keeps the level; only the number reads "mute" (conf_mutetext).
            (void)m_ui.UpdateUiData("settings." + bus + "Value", Variant(*muted ? "mute" :
                std::to_string(static_cast<int>(std::round(volume * 100.0 / 128.0)))));
        }
    } else if (t == "set.duck.on" || t == "set.duck.off") {
        m_settings.duckBgmOnVoice = t.ends_with(".on");
        audio.SetVoiceDuckingEnabled(m_settings.duckBgmOnVoice);
        (void)m_ui.UpdateUiData("settings.duckOn", Variant(m_settings.duckBgmOnVoice));
        (void)m_ui.UpdateUiData("settings.duckOff", Variant(!m_settings.duckBgmOnVoice));
    } else if (t == "set.voiceCarry.on" || t == "set.voiceCarry.off") {
        m_settings.continueVoiceOnAdvance = t.ends_with(".on");
        (void)m_ui.UpdateUiData("settings.voiceCarryOn", Variant(m_settings.continueVoiceOnAdvance));
        (void)m_ui.UpdateUiData("settings.voiceCarryOff", Variant(!m_settings.continueVoiceOnAdvance));
    } else if (t == "set.voiceProfile.value") {
        const auto separator = action.argument.find(':');
        if (separator != std::string::npos && separator > 0 && separator <= 64) {
            const std::string id = action.argument.substr(0, separator);
            const int volume = std::clamp(std::atoi(action.argument.c_str() + separator + 1), 0, 128);
            m_settings.voiceProfileVolumes[id] = volume;
            m_settings.voiceProfileMuted[id] = false;
            audio.SetVoiceProfileVolume(id, volume);
            if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(),
                    "VoiceProfileMute_" + id + "Label")) label->SetText("□MUTE");
        }
    } else if (t == "set.voiceProfile.mute") {
        const std::string& id = action.argument;
        if (!id.empty() && id.size() <= 64) {
            const bool muted = !(m_settings.voiceProfileMuted[id]);
            m_settings.voiceProfileMuted[id] = muted;
            const auto volume = m_settings.voiceProfileVolumes.find(id);
            audio.SetVoiceProfileVolume(id, muted ? 0 :
                volume == m_settings.voiceProfileVolumes.end() ? 128 : volume->second);
            if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(),
                    "VoiceProfileMute_" + id + "Label"))
                label->SetText(muted ? "☑MUTE" : "□MUTE");
            if (auto* label = FindUiControlNamed<ui::Label>(m_ui.Root(),
                    "VoiceProfileValue_" + id))
                label->SetText(muted ? "mute" : std::to_string(static_cast<int>(std::round(
                    (volume == m_settings.voiceProfileVolumes.end() ? 128 : volume->second)
                    * 100.0 / 128.0))));
        }
    } else if (t == "voiceProfile.test") {
        audio.PlayVoice(action.argument);
    } else if (t == "audio.test.se") {
        audio.PlaySE(action.argument);
    } else if (t == "set.speed.value") {
        m_settings.textSpeedMs=std::clamp(std::atoi(action.argument.c_str()),0,120);m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.skipread.value") {
        m_settings.skipReadOnly=action.argument=="true";
    } else if (t == "set.fullscreen.value") {
        (void)ApplyFullscreenSetting(action.argument == "true");
    } else if (t == "set.fullscreen.on" || t == "set.fullscreen.off") {
        (void)ApplyFullscreenSetting(t.ends_with(".on"));
    } else if(t=="set.textscale.value") { m_settings.textScale=std::clamp(std::atoi(action.argument.c_str())/100.0f,.75f,2.0f);
    } else if(t=="set.highcontrast.value") { m_settings.highContrast=action.argument=="true";
    } else if(t=="set.reducedmotion.value") { m_settings.reducedMotion=action.argument=="true";
    } else if(t=="set.reducedmotion.on"||t=="set.reducedmotion.off") {
        m_settings.reducedMotion=t.ends_with(".on");
        (void)m_ui.ShowSettings(SettingsUI());
    } else if(t=="set.selfvoicing.value") { m_settings.selfVoicing=action.argument=="true";if(!m_settings.selfVoicing)m_speech.Stop();
    } else if(t=="set.language.value") { if(LoadLocale(action.argument,true))(void)SaveSettingsIfWritable();
    } else if (t == "set.master.up") {
        m_settings.masterVolume = std::min(128, m_settings.masterVolume + 8);
        audio.SetMainVolume(m_settings.masterVolume);
    } else if (t == "set.master.down") {
        m_settings.masterVolume = std::max(0, m_settings.masterVolume - 8);
        audio.SetMainVolume(m_settings.masterVolume);
    } else if (t == "set.bgm.up") {
        m_settings.bgmVolume = std::min(128, m_settings.bgmVolume + 8);
        audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.bgm.down") {
        m_settings.bgmVolume = std::max(0, m_settings.bgmVolume - 8);
        audio.SetBGMVolume(m_settings.bgmVolume);
    } else if (t == "set.se.up") {
        m_settings.seVolume = std::min(128, m_settings.seVolume + 8);
        audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.se.down") {
        m_settings.seVolume = std::max(0, m_settings.seVolume - 8);
        audio.SetSEVolume(m_settings.seVolume);
    } else if (t == "set.voice.up") {
        m_settings.voiceVolume = std::min(128, m_settings.voiceVolume + 8);
        audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.voice.down") {
        m_settings.voiceVolume = std::max(0, m_settings.voiceVolume - 8);
        audio.SetVoiceVolume(m_settings.voiceVolume);
    } else if (t == "set.speed.up") {
        m_settings.textSpeedMs = std::min(120, m_settings.textSpeedMs + 4);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "set.speed.down") {
        m_settings.textSpeedMs = std::max(0, m_settings.textSpeedMs - 4);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
    } else if (t == "gallery.open") {
        OpenScreen("gallery");
    } else if (t == "load.open") {
        m_slotSaveMode = false;
        OpenScreen("load");
    } else if (t == "save.open") {
        m_slotSaveMode = true;
        if (m_ui.CurrentScreen() == ui::GalgameUI::Screen::Load ||
            m_appState != AppState::Game)
            OpenScreen("save");
        else
            m_pendingSaveScreen = true;
    } else if (t == "settings.open") {
        OpenScreen("settings");
    } else if (t == "settings.reset" && AsksBefore("resetSettings")) {
        RequestConfirm("resetSettings", [this] { HandleUIAction({"system.reset.apply", {}}); });
    } else if (t == "settings.reset" &&
               m_ui.SupportsSystemConfirmation()) {
        m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::Reset;
        (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
    } else if (t == "settings.reset" || t == "system.reset.apply") {
        const std::string locale=m_settings.language;
        const bool fullscreen=m_settings.fullscreen;
        m_settings=m_settingsDefaults;
        m_runtime.GetInput().ResetKeyboardBindings();
        m_runtime.GetInput().ResetWheelBindings();
        m_settings.language=locale;
        m_settings.fullscreen=fullscreen;
        audio.SetMainVolume(m_settings.masterVolume);
        audio.SetBGMVolume(m_settings.bgmVolume);
        audio.SetSEVolume(m_settings.seVolume);
        audio.SetVoiceVolume(m_settings.voiceVolume);
        m_session->VM().SetDefaultTextSpeed(m_settings.textSpeedMs);
        ApplyAllOptions();
        (void)m_ui.ShowSettings(SettingsUI());
    } else if (t.starts_with("settings.page")) {
        const std::string page = t.substr(std::string("settings.page").size());
        if (m_boot.routeScenes.contains("settings") && page.size() == 1 &&
            page[0] >= '1' && page[0] <= '4') {
            m_settingsPage = page[0] - '0';
            for (int index = 1; index <= 4; ++index)
                (void)m_ui.UpdateUiData("settings.page" + std::to_string(index),
                                        Variant(index == m_settingsPage));
            // Pages are bound groups of one installed screen; rebuilding it
            // here would restart every transition on the screen.
            return;
        }
        while (!m_session->Routes().CurrentModalRoute().empty())
            (void)m_session->Routes().CloseModal();
        OpenScreen(page == "0" ? "settings" : "settings-page" + page);
    } else if (t == "settings.noop") {
        // The original setting exists visually but has no matching portable
        // host capability yet. Keep the authored control responsive without
        // mutating an unrelated setting.
    } else if (t == "music.play") {
        const auto found = std::ranges::find(m_musicTracks, action.argument);
        if (found != m_musicTracks.end()) {
            playMusicAt(static_cast<std::size_t>(
                std::distance(m_musicTracks.begin(), found)));
            PX_LOG_INFO("Player music gallery selected path={}", action.argument);
        } else {
            PX_LOG_ERROR("Player music gallery received unknown track path={}",
                         action.argument);
        }
    } else if (t == "music.previous") {
        if (!m_musicTracks.empty())
            for (std::size_t step = 1; step <= m_musicTracks.size(); ++step) {
                const auto index = (m_musicTrackIndex + m_musicTracks.size() -
                                    step % m_musicTracks.size()) % m_musicTracks.size();
                if (musicUnlocked(index)) { playMusicAt(index); break; }
            }
    } else if (t == "music.next") {
        if (!m_musicTracks.empty())
            for (std::size_t step = 1; step <= m_musicTracks.size(); ++step) {
                const auto index = (m_musicTrackIndex + step) % m_musicTracks.size();
                if (musicUnlocked(index)) { playMusicAt(index); break; }
            }
    } else if (t == "music.random") {
        std::vector<std::size_t> available;
        for (std::size_t index = 0; index < m_musicTracks.size(); ++index)
            if (musicUnlocked(index)) available.push_back(index);
        if (!available.empty())
            playMusicAt(available[static_cast<std::size_t>(SDL_GetTicks()) %
                                  available.size()]);
    } else if (t == "music.stop") {
        audio.StopBGM(200);
        m_musicTrackSelected = false;
    } else if (t == "music.resume") {
        if (m_musicTrackSelected) audio.SetMusicPaused(false);
        else if (musicUnlocked(m_musicTrackIndex)) playMusicAt(m_musicTrackIndex);
        else {
            for (std::size_t index = 0; index < m_musicTracks.size(); ++index)
                if (musicUnlocked(index)) { playMusicAt(index); break; }
        }
    } else if (t == "music.about") {
        audio.SetMusicPaused(true);
    } else if (t.starts_with("music.scroll.")) {
        scrollRoute("MusicTracksScroll", t);
    } else if (t == "flowchart.select") {
        const auto selected = nlohmann::json::parse(
            action.argument, nullptr, false);
        if (selected.is_object() && selected.value("entry", "") != "" &&
            selected.value("heading", "").size() < 512 &&
            selected.value("details", "").size() < 2048) {
            m_selectedFlowEntry = selected.value("entry", "");
            if (auto* item = FindUiControlNamed<ui::Label>(
                    m_ui.Root(), "FlowSelectedItem"))
                item->SetText(selected.value("heading", ""));
            if (auto* heading = FindUiControlNamed<ui::Label>(
                    m_ui.Root(), "FlowSelectedHeading"))
                heading->SetText(selected.value("heading", ""));
            if (auto* details = FindUiControlNamed<ui::Label>(
                    m_ui.Root(), "FlowSelectedDetails"))
                details->SetText(selected.value("details", ""));
            if (auto* jump = FindUiControlNamed<ui::Button>(
                    m_ui.Root(), "scenejumpButton"))
                jump->SetEnabled(true);
            PX_LOG_INFO("Player flowchart selected entry={}", m_selectedFlowEntry);
        }
    } else if (t == "flowchart.jump") {
        if (!m_selectedFlowEntry.empty()) {
            m_pendingStoryEntry = std::exchange(m_selectedFlowEntry, {});
            (void)m_session->Routes().CloseModal();
            StartGame();
        }
    } else if (t == "replay.launch") {
        if (!action.argument.empty()) {
            m_pendingStoryEntry = action.argument;
            while (!m_session->Routes().CurrentModalRoute().empty())
                (void)m_session->Routes().CloseModal();
            StartGame();
        }
    } else if (t == "replay.page") {
        const int page = std::atoi(action.argument.c_str());
        if (page >= 1 && page <= 4) {
            m_replayPage = page;
            for (int index = 1; index <= 4; ++index) {
                if (const Status updated = m_ui.UpdateUiData(
                        "replay.page" + std::to_string(index),
                        Variant(index == page)); !updated)
                    for (const auto& diagnostic : updated.Diagnostics())
                        diag::Emit(diagnostic);
            }
        }
    } else if (t == "title.chapter3") {
        OpenScreen("chapter3");
    } else if (t.starts_with("flowchart.scroll.")) {
        scrollRoute("FlowGraphScroll", t);
    } else if (t == "flowchart.filter") {
        if (!action.argument.empty() &&
            (action.argument == "flowchart" ||
             action.argument.starts_with("flowchart-"))) {
            m_selectedFlowEntry.clear();
            while (!m_session->Routes().CurrentModalRoute().empty())
                (void)m_session->Routes().CloseModal();
            OpenScreen(action.argument);
        }
    } else if (t == "cg.view") {
        const auto& entries = m_catalog.Gallery();
        const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
            return entry.id == action.argument;
        });
        if (found != entries.end()) {
            if (m_profile.CGUnlocked(found->id)) m_viewingCG = found->image;
        } else {
            m_viewingCG = action.argument;
        }
    } else if (t == "set.skipread.toggle") {
        m_settings.skipReadOnly = !m_settings.skipReadOnly;
        m_ui.ShowSettings(SettingsUI());
    } else if (t == "set.fullscreen.toggle") {
        (void)ApplyFullscreenSetting(!m_settings.fullscreen);
        if (m_ui.CurrentRoute() == "settings")
            (void)m_ui.ShowSettings(SettingsUI());
    } else if (t == "game.start") {
        StartGame();
    } else if (t == "game.continue") {
        const auto saves = m_saves.List(1000);
        std::optional<int> latest;
        std::uint64_t latestTimestamp = 0;
        for (std::size_t index = 0; index < saves.size(); ++index) {
            if (saves[index].exists &&
                (!latest || saves[index].timestamp >= latestTimestamp)) {
                latest = static_cast<int>(index);
                latestTimestamp = saves[index].timestamp;
            }
        }
        if (latest) (void)LoadSlot(*latest);
        else OpenScreen("load");
    } else if (t == "extra.open") {
        OpenScreen("extra");
    } else if (t == "flowchart.open") {
        OpenScreen("flowchart");
    } else if (t == "replay.open") {
        OpenScreen("replay");
    } else if (t == "music.open") {
        OpenScreen("music");
    } else if (t == "title.confirm.cancel") {
        (void)m_ui.SetTitleConfirmation(false);
    } else if (t == "title.open" && m_appState == AppState::Game &&
               AsksBefore("returnToTitle")) {
        RequestConfirm("returnToTitle", [this] { HandleUIAction({"title.confirm.accept", {}}); });
    } else if (t == "title.open" && m_appState == AppState::Game &&
               m_ui.SupportsTitleConfirmation()) {
        (void)m_ui.SetTitleConfirmation(true);
    } else if (t == "title.open" || t == "title.confirm.accept") {
        (void)m_ui.SetTitleConfirmation(false);
        while (!m_session->Routes().CurrentModalRoute().empty())
            (void)m_session->Routes().CloseModal();
        (void)m_session->Routes().Replace("title");
    } else if (t == "hud.hide") {
        m_hudHidden = true;
    } else if (t == "choice.select") {
        m_session->SelectChoice(std::atoi(action.argument.c_str()));
    } else if (t == "mode.auto") {
        m_autoMode = !m_autoMode; if (m_autoMode) m_skipMode = false;
    } else if (t == "mode.skip") {
        m_skipMode = !m_skipMode; if (m_skipMode) m_autoMode = false;
    } else if (t == "backlog.open") {
        OpenScreen("backlog");
    } else if (t == "save.quick") {
        // Written after the Stage renders, so the thumbnail has no HUD.
        RequestConfirm("quickSave", [this] { m_saveRequested = true; });
    } else if (t == "load.quick") {
        RequestConfirm("quickLoad", [this] {
            if (m_saves.Peek(0).exists) (void)LoadSlot(0);
        });
    } else if (t == "voice.replay") {
        const auto& lines = m_session->Backlog().Entries();
        const auto voiced = std::find_if(lines.rbegin(), lines.rend(), [](const auto& entry) {
            return !entry.voice.empty();
        });
        if (voiced != lines.rend() && voiced == lines.rbegin())
            m_runtime.Audio().PlayVoice(voiced->voice.find('/') != std::string::npos
                ? voiced->voice : m_session->VM().Config().voiceDir + voiced->voice);
    } else if (t == "choice.previous") {
        // The choice autosave is the state at the most recent choice prompt.
        RequestConfirm("previousChoice", [this] {
            if (m_saves.Peek(kAutoSaveSlot).exists) (void)LoadSlot(kAutoSaveSlot);
        });
    } else if (t == "choice.next") {
        // Skip stops at the next choice, and at unread text unless the
        // player allowed skipping it.
        RequestConfirm("nextChoice", [this] {
            m_skipMode = true;
            m_autoMode = false;
        });
    } else if (t == "backlog.voice" || t == "backlog.rollback") {
        std::uint64_t sequence = 0;
        try {
            sequence = std::stoull(action.argument);
        } catch (const std::exception&) {
            sequence = 0;
        }
        if (const auto* entry = m_session->Backlog().Find(sequence)) {
            if (t == "backlog.voice" && !entry->voice.empty())
                m_runtime.Audio().PlayVoice(entry->voice.find('/') != std::string::npos ? entry->voice : m_session->VM().Config().voiceDir + entry->voice);
            else RequestConfirm("backlogJump", [this, sequence = entry->sequence] {
                RollbackToBacklogSequence(sequence);
            });
        }
    } else if (t == "overlay.close") {
        if (m_dialog) {
            HandleUIAction({"dialog.cancel", {}});
            return;
        }
        if (m_pendingSystemPrompt != ui::GalgameUI::SystemPrompt::None) {
            m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
            (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
            return;
        }
        if (m_pendingSaveLoadSlot) {
            m_pendingSaveLoadSlot.reset();
            (void)m_ui.SetSaveLoadConfirmation(ui::GalgameUI::SaveLoadPrompt::None);
            return;
        }
        const bool restoreTitleMusic = m_musicTrackSelected &&
            (m_ui.CurrentRoute() == "music" || m_ui.CurrentRoute() == "extra");
        if (!m_session->Routes().CloseModal()) return;
        if (restoreTitleMusic && !TitleBgmPath().empty()) {
            audio.PlayBGM(TitleBgmPath(), true, 300);
            m_musicTrackSelected = false;
        }
        // Closing a menu must not wait on disk; both files are written by
        // background workers and flushed synchronously at shutdown.
        SaveSettingsInBackground();
        MarkProfileDirty(true);
        PX_LOG_INFO("Player overlay closed modal={} route={}",
                    m_session->Routes().CurrentModalRoute(),
                    m_session->Routes().CurrentRoute());
    } else if (t == "system.confirm.cancel") {
        m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::None;
        (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
    } else if (t == "system.confirm.accept") {
        const auto prompt = std::exchange(
            m_pendingSystemPrompt, ui::GalgameUI::SystemPrompt::None);
        (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
        if (prompt == ui::GalgameUI::SystemPrompt::Reset)
            HandleUIAction({"system.reset.apply", {}});
        else if (prompt == ui::GalgameUI::SystemPrompt::Exit)
            m_quitRequested = true;
    } else if (t == "dialog.accept") {
        if (!m_dialog) return;
        auto dialog = std::move(*m_dialog);
        m_dialog.reset();
        if (m_dialogSkipNext && !dialog.dontAskOption.empty())
            (void)SetOption(dialog.dontAskOption, 1);
        m_dialogSkipNext = false;
        PublishDialog();
        dialog.accept();
    } else if (t == "dialog.cancel") {
        m_dialog.reset();
        m_dialogSkipNext = false;
        PublishDialog();
    } else if (t == "dialog.toggleSkip") {
        if (!m_dialog) return;
        m_dialogSkipNext = !m_dialogSkipNext;
        PublishDialog();
    } else if (t == "option.set") {
        const auto split = action.argument.find('=');
        if (split != std::string::npos) {
            char* end = nullptr;
            const long value = std::strtol(action.argument.c_str() + split + 1, &end, 10);
            if (end != action.argument.c_str() + split + 1 && *end == '\0')
                (void)SetOption(std::string_view(action.argument).substr(0, split),
                                static_cast<int>(value));
        }
    } else if (t == "app.quit" && AsksBefore("exit")) {
        RequestConfirm("exit", [this] { m_quitRequested = true; });
    } else if (t == "app.quit") {
        if (m_ui.SupportsSystemConfirmation()) {
            m_pendingSystemPrompt = ui::GalgameUI::SystemPrompt::Exit;
            (void)m_ui.SetSystemConfirmation(m_pendingSystemPrompt);
        } else m_quitRequested = true;
    } else {
        diag::Emit(diag::Diagnostic{.severity = diag::Severity::Error,
                                    .code = "PXPLAYER5023",
                                    .category = "Player.Input",
                                    .message = "UI command is not handled by this Player",
                                    .details = t});
        return;
    }
    if (t.ends_with(".up") || t.ends_with(".down")) m_ui.ShowSettings(SettingsUI());
}

}  // namespace px::player
