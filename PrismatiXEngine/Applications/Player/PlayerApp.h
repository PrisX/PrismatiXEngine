#pragma once

#include "Applications/Player/PlayerNavigation.h"
#include "Applications/Player/QaIpc.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/ProfileRules.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/SDK/Motion.h"
#include "Engine/Script/ScriptHost.h"
#include "Engine/Runtime.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/Startup/SplashSequencePlayer.h"
#include "Engine/VN/GameCatalog.h"
#include "Engine/Video/VideoPlayer.h"
#include "Engine/Accessibility/SpeechService.h"
#include "Engine/Accessibility/PlatformSemanticAdapter.h"
#include "Engine/Platform/ProcessWatch.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>

#include <nlohmann/json.hpp>
#include <memory>
#include <optional>
#include <set>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::player {

// The shipped game application: title screen, VN playback, overlay screens,
// saves, and session modes (auto / skip / backlog / hide-UI).
class PlayerApp {
public:
    ~PlayerApp();
    // Returns 0 after a normal exit and 1 when the game could not start; a
    // failed boot logs PXPLAYER5031 with its cause and releases what it built.
    int Run(int argc, char* argv[]);
    // Whether a failed boot also tells the player in a message box. The host
    // decides: a desktop launch has nowhere else to show it, while tools read
    // the log and must not be blocked by a modal dialog. The QA Player never
    // shows one.
    void SetBootFailureDialog(const bool enabled) { m_bootFailureDialog = enabled; }

private:
    enum class AppState { BootSplash, Title, Game };
    struct PresentationTransition {
        std::string preset = "crossfade";
        float duration = 0.3f;
    };
    struct SplashTransitionOverride {
        std::optional<std::string> preset;
        std::optional<float> duration;
    };

    // Boot configuration shared by typed dev projects and packaged builds.
    struct Boot {
        RuntimeConfig config;
        std::string startScript = "Content/Scenario/start.pxscenario";
        std::string sourceMap;
        std::string startRoute = "title";
        std::unordered_map<std::string,std::string> routeScenes;
        std::vector<ui::startup::SplashScreenEntry> splashes;
        std::string saveSecret;  // legacy build-derived persistence secret
        std::string persistenceDataId;
        std::string persistenceKeyId;
        std::vector<std::string> legacyPersistenceSecrets;
        std::string gameId;
        std::string packageFingerprint;
        std::vector<std::string> compatibleSaveFingerprints;
        std::string contentVersion;
        std::uint32_t saveVersion = 1;
        std::vector<progress::SaveMigrationDescriptor> saveMigrations;
        std::vector<std::string> extensions;
        std::string graphicsTier = "basic";
        bool packaged = false;
        bool overlayApplied = false;
        bool optionalPackApplied = false;
        bool valid = false;
    };
    static Boot LoadBootConfig(bool allowOverlay = true,
                               bool allowOptionalPack = true);

    bool Init(int argc, char* argv[]);
    void MainLoop();
    void WatchOwnerProcess();
    void Shutdown();
    // Releases the script host, splash, video, session and runtime in
    // dependency order; safe after a boot that stopped at any step.
    void ReleaseSubsystems();
    // Reports a boot that stopped (diagnostics emitted from firstDiagnostic
    // on name the cause) and tears it down.
    void FailBoot(std::size_t firstDiagnostic);
    bool SaveProfileIfWritable();
    // Progress writes are coalesced: unlocks flush within a few seconds,
    // read-state marks at the next safe point or after a longer interval.
    void MarkProfileDirty(bool urgent);
    void FlushProfileIfDue();
    // Writes settings on a worker; used where the UI must not wait on disk.
    void SaveSettingsInBackground();
    // Autosave and quicksave: state and pixels are captured now, the thumbnail
    // encode and encrypted write happen on a worker.
    void SaveSlotInBackground(int slot);
    void HarvestBackgroundSave(bool wait);
    bool SaveSettingsIfWritable();
    bool ApplyFullscreenSetting(bool fullscreen);
    void BeginRuntimeIrPreload();
    void FinishBootPresentation();
    [[nodiscard]] const std::string& TitleBgmPath() const;
    void RefreshTitleProgressUi();
    [[nodiscard]] progress::ProfileFlagReader ProfileFlags() const;
    [[nodiscard]] bool TitleVisualsReady();
    void SplashFrame(float dt);
    void PlayMotionMarker(const sdk::MotionAudioMarker& marker);
    void BeginPresentationTransition(const PresentationTransition& transition);

    void StartGame();
    [[nodiscard]] bool CommitStartGame();
    void GameStartTransitionFrame(float dt, std::uint64_t now);
    bool LoadSlot(int slot);
    bool SaveSlot(int slot, std::vector<std::uint8_t> thumbnail);
    // Writes a snapshot taken earlier (SaveSlot takes it now).
    bool WriteSaveSlot(int slot, progress::SaveSnapshot snapshot);
    [[nodiscard]] std::optional<progress::SaveSnapshot> MakeSnapshot(
        bool includeBacklog);
    bool LoadLocale(std::string locale, bool refreshPresentation);

    // --- Rollback (per-line snapshot ring) ---
    struct RollbackEntry {
        progress::SaveSnapshot snap;
        std::uint64_t backlogSequence = 0;
    };
    [[nodiscard]] bool ApplyRollback(const RollbackEntry& entry);
    void RollbackOneLine();
    bool RollbackToBacklogSequence(std::uint64_t sequence);

    void OpenScreen(const std::string& route);
    void PresentRoute(const std::string& route, const std::string& operation);
    void BeginRouteVisualPreload(std::string_view route);
    void FinishRouteVisualPreload(std::string_view route);
    void UpdateRouteVisualPreload();
    [[nodiscard]] std::vector<ui::GalgameItem> GalleryItems();
    [[nodiscard]] std::vector<ui::GalgameItem> SaveItems(bool saveMode);
    [[nodiscard]] std::vector<ui::GalgameItem> BacklogItems();
    [[nodiscard]] std::vector<ui::BacklogEntryPresentation> BacklogEntries();
    [[nodiscard]] ui::DialoguePresentation DialogueUI() const;
    [[nodiscard]] ui::SettingsPresentation SettingsUI() const;
    void HandleUIAction(const ui::GalgameAction& action);
    // UI actions (PlayerActions.cpp): one handler per ui::kPlayerActions id.
    using ActionHandler = void (*)(PlayerApp&, const ui::GalgameAction&);
    [[nodiscard]] static const std::unordered_map<std::string_view, ActionHandler>&
    ActionHandlers();
    [[nodiscard]] bool RegisterUiActions();
    [[nodiscard]] static std::string_view BusOf(std::string_view command);
    [[nodiscard]] int BusVolume(std::string_view bus) const;
    void SetBusVolume(std::string_view bus, int volume);
    void ToggleBusMute(std::string_view bus);
    void SetTextSpeed(int milliseconds);
    void ResetSettings();
    [[nodiscard]] bool MusicUnlocked(std::size_t index) const;
    void PlayMusicAt(std::size_t index);
    void StepMusic(int direction);
    void ScrollNamedList(std::string_view controlName, std::string_view command);
    void CloseAllModals();
    void ActivateSlot(bool saveMode, const std::string& argument);
    // Save management: delete, copy or move a slot, and its comment.
    [[nodiscard]] bool ManagedSlot(int slot) const;
    void RefreshSaveLoad();
    void BeginSlotOperation(bool move, const std::string& argument);
    void FinishSlotOperation(int target);
    void DeleteSlot(const std::string& argument);
    void BeginSlotComment(const std::string& argument);
    void FinishSlotComment(bool accept);
    static constexpr std::size_t kMaxSlotComment = 64;  // code points
    void ReturnToTitle();
    // `player.navigation`; defaults keep the Player's own behaviour.
    NavigationSettings m_navigation;
    // The Story's end was answered (afterStoryEnd); cleared while it runs.
    bool m_storyEndHandled = false;
    // Does what afterStoryEnd asks once the Story has finished; true when it
    // left the game screen (the frame should end here).
    bool FollowStoryEnd();
    // Story [input]: the HUD's TextInput field asks for text, the VM waits,
    // and input.accept stores it in the variable.
    struct TextInputRequest {
        std::string variable;
        std::string fallback;
        std::size_t maxLength = 32;
        bool profile = false;
    };
    std::optional<TextInputRequest> m_textInput;
    bool m_sdlTextInputActive = false;
    void BeginTextInput(const vn::Command& command);
    void FinishTextInput();
    // Starts SDL text input (and the IME) only while a text field has focus.
    void SyncTextInput();
    void RecordScriptVideoTerminal(std::uint64_t handle, std::string status,
                                   std::string error = {});

    void TitleFrame(float dt);
    void GameFrame(float dt, std::uint64_t now);
    void ScreensFrame(float dt);
    void RenderStageWithScripts();
    void RenderUIWithScripts(float opacity = 1.0f);
    // The message window fades in and out over 200 ms (mw_time / mw_hidetime)
    // when text is hidden by `msgoff`, a right click or a script.
    float m_hudFade = 0.0f;
    bool m_hudFadeSnap = true;
    // `[hud time=]` sets the fade of that one show or hide; later automatic
    // fades (the next line, a right click) use the default again.
    static constexpr float kHudFadeSeconds = 0.2f;
    float m_hudFadeSeconds = kHudFadeSeconds;
    void UpdateScriptScheduler(float dt);
    void ProcessScriptSafePoint();
    // Returns true when it consumed the frame (a movie is on screen).
    bool VideoFrame(float dt);
    void PublishLocaleStrings();
    bool ApplyPlayerSettings(const nlohmann::json& settings);
    // QA IPC (QaIpc.cpp): PrismatiXPlayerQA answers test requests at the
    // start of each frame; both are empty in the shipped Player.
    friend struct QaOps;
    void StartQaIpc();
    void PumpQaIpc();
    // After the Stage is drawn each frame: QA requests waiting for a frame
    // without the UI (a save's thumbnail).
    void QaStageRendered();

    Runtime m_runtime;
    Boot m_boot;
    std::filesystem::path m_saveDirectory = "Save";
    crypto::Key m_saveKey{};
    std::vector<crypto::Key> m_saveFallbackKeys;
    bool m_profileWritable = true;
    bool m_profileTrusted = false;
    bool m_profileDirty = false;
    bool m_profileUrgent = false;
    std::uint64_t m_profileDirtySince = 0;
    // Settings, profile and save writes share one strand: they run on the
    // engine's worker pool one at a time, in the order they were requested.
    jobs::JobSystem::Strand m_persistenceWrites;
    jobs::Job<bool> m_profileWrite;
    jobs::Job<bool> m_settingsWrite;
    jobs::Job<bool> m_saveWrite;
    int m_saveWriteSlot = 0;
    bool m_settingsWritable = true;
    // Settings a fresh install and "reset" start from: engine defaults with the
    // project's `player.defaultVolumes` percentages applied.
    progress::GameSettings m_settingsDefaults;
    bool m_settingsMissing = false;
    progress::GlobalProfile m_profile;
    progress::GameSettings m_settings;
    progress::SaveSystem m_saves;

    // Project-declared integer options (`player.options`). An option may bind
    // one Player behaviour (`"binds"`); the Player acts on `value * scale +
    // offset` in that behaviour's unit and ignores every other option.
    enum class OptionBinding {
        None, TextSpeed, AutoWait, SkipUnread, MessageWindowOpacity, DimReadText,
        UiEffects, Font, KeepSkipAfterChoice, KeepAutoAfterChoice, CursorHideDelay,
        DialogCursorWarp, DockPinned, MessageFace,
    };
    struct OptionSpec {
        int def = 0;
        int min = 0;
        int max = 100;
        std::vector<int> choices;
        OptionBinding binding = OptionBinding::None;
        double scale = 1.0;
        double offset = 0.0;
    };
    std::map<std::string, OptionSpec, std::less<>> m_optionSpecs;
    [[nodiscard]] static std::optional<OptionBinding> ParseOptionBinding(std::string_view name);
    // `player.voiceProfiles.ids`: per-character voice volume groups the
    // settings screen may bind as `settings.profileMuted.<id>`.
    std::vector<std::string> m_voiceProfileIds;
    std::vector<std::string> m_fontChoices;
    // The project set player.uiFont; locale changes keep it.
    bool m_uiFontAuthored = false;
    float m_messageWindowOpacity = 1.0f;
    bool m_dimReadText = false;
    bool m_uiEffects = true;
    bool m_keepSkipAfterChoice = false;
    bool m_keepAutoAfterChoice = false;
    int m_cursorHideDelayMs = 0;
    bool m_dialogCursorWarp = false;
    bool m_dockPinned = true;
    // `[auto on speed= wait= lock=]`: the Story drives auto-reading at its own
    // pace (-1 keeps the player's setting); a locked section ignores player
    // input and menus, like Artemis `autoplay`. `[skip.disable]` refuses skip.
    struct StoryAuto {
        int textSpeedMs = -1;
        int waitMs = -1;
        bool locked = false;
    };
    std::optional<StoryAuto> m_storyAuto;
    bool m_skipDisabled = false;
    void SetStoryAuto(std::optional<StoryAuto> value);
    // Message-window face: 0 never, 1 always, 2 only while the speaker is
    // off stage (Artemis `mwface`).
    int m_messageFaceMode = 1;
    [[nodiscard]] std::string MessageFacePath() const;
    [[nodiscard]] int OptionValue(std::string_view name) const;
    bool SetOption(std::string_view name, int value);
    void ApplyOption(std::string_view name, int value);
    void ApplyAllOptions();
    void PublishOption(std::string_view name);
    Status DefineOptionData();
    // settings.muted.<bus> and settings.profileMuted.<voice> for the MUTE boxes.
    void PublishMuteData();
    // Cursor auto-hide (`cursorHideDelay`, milliseconds; 0 = never) and the
    // dialog warp (`dialogCursorWarp`).
    float m_lastMouseX = -1.0f;
    float m_lastMouseY = -1.0f;
    std::uint64_t m_cursorActiveAt = 0;
    bool m_cursorHidden = false;
    bool m_choiceWasPending = false;
    bool m_resumeSkipAfterChoice = false;
    bool m_resumeAutoAfterChoice = false;
    void UpdateCursorVisibility(std::uint64_t now);
    // `hud.dockShown`: the HUD toolbar (original mw dock) is visible while it
    // is pinned (`dockPinned`), a choice is up or the pointer is in its area.
    struct DockArea { float x = 0, y = 0, width = 0, height = 0; bool valid = false; } m_dockArea;
    bool m_dockShown = true;
    void PublishDock();
    // `settings.preview`: while the settings screen is open, a sample line
    // types at the chosen text speed, holds for the auto-advance wait and
    // starts again (`settings.previewWaiting` during the hold).
    void UpdateSettingsPreview(std::uint64_t now);
    std::uint64_t m_previewStart = 0;
    int m_previewSpeedMs = -1;
    int m_previewWaitMs = -1;
    std::string m_previewShown;
    bool m_previewWaiting = false;
    void WarpCursorTo(float logicalX, float logicalY);

    // Confirmation dialogs (`player.confirmations`): each Player operation the
    // project lists asks first, showing its `kind`, unless the player chose
    // "do not ask again" (its `dontAskOption` is non-zero). The dialog itself
    // is authored UI, a control named ConfirmDialog driven by `dialog.*` data
    // and actions; unlisted operations run at once.
    struct ConfirmSpec {
        std::string kind;
        std::string dontAskOption;
    };
    std::map<std::string, ConfirmSpec, std::less<>> m_confirmations;
    // Distinct kinds, each published as `dialog.kind.<kind>`.
    std::vector<std::string> m_dialogKinds;
    struct PendingDialog {
        std::string kind;
        std::string dontAskOption;
        std::function<void()> accept;
    };
    std::optional<PendingDialog> m_dialog;
    [[nodiscard]] bool AsksBefore(std::string_view operation) const;
    bool m_dialogSkipNext = false;
    void RequestConfirm(std::string_view operation, std::function<void()> accept);
    void PublishDialog();
    Status DefineDialogData();

    std::unique_ptr<RuntimeSession> m_session;
    std::unique_ptr<script::ScriptHost> m_scriptHost;
    script::ScriptServices m_scriptServices;
    std::unordered_map<std::string, std::string> m_langTable;
    std::set<std::string> m_uiStringPaths;
    std::string m_defaultLocale = "zh-TW";
    std::vector<std::string> m_supportedLocales;
    std::unordered_map<std::string, std::string> m_uiAssets;
    std::unordered_map<std::string, std::string> m_uiComponents;
    // player.titleBgm rules, first match wins (player.titleBgmAfter adds one).
    std::vector<progress::ProfileChoice> m_titleBgmRules;
    // player.profileUiData: UI data computed from global profile flags.
    std::vector<progress::ProfileUiDatum> m_profileUiData;
    std::vector<std::string> m_titleVoices;
    std::string m_defaultUiHoverSound;
    std::string m_defaultUiActivateSound;
    std::string m_defaultUiCancelSound;
    // Sounds controls played as they appeared or hid, latest last (QA state).
    struct UiSoundPlayed {
        std::string phase;
        std::string path;
        std::string control;
        float afterSeconds = 0.0f;
    };
    std::deque<UiSoundPlayed> m_uiSoundsPlayed;
    // showSound/hideSound: plays the control's sound on the SE channel.
    void PlayUiControlSound(const ui::Control& control, bool show);
    // Story `[shake target=window]` offsets only the HUD layer.
    std::chrono::steady_clock::time_point m_windowShakeStart{};
    float m_windowShakeDuration = 0.0f;
    float m_windowShakeAmplitude = 0.0f;
    // Set for `[shake target=window dir=...]`: the Artemis quake curve.
    std::optional<vn::QuakeCurve> m_windowQuake;
    // [act window ...]: a character action preset played on the HUD layer.
    std::optional<vn::ActorActionTrack> m_windowAction;
    std::chrono::steady_clock::time_point m_windowActionStart{};
    PresentationTransition m_routeTransition;
    PresentationTransition m_loadTransition;
    PresentationTransition m_splashTransition;
    bool m_splashTransitionConfigured = false;
    PresentationTransition m_titleTransition{"crossfade", 0.5f};
    std::unordered_map<std::string, PresentationTransition> m_routeTransitionOverrides;
    std::vector<SplashTransitionOverride> m_splashTransitionOverrides;

    ui::GalgameUI m_ui;
    std::unique_ptr<ui::startup::SplashSequencePlayer> m_splash;
    graphics::AssetPreloadBatchId m_titlePreloadBatch = 0;
    graphics::AssetPreloadBatchId m_routePreloadBatch = 0;
    graphics::ScreenEffectHandle m_routePreloadEffect = 0;
    bool m_routePreloadHold = false;
    std::vector<std::string> m_routePreloadPaths;
    std::vector<std::string> m_choiceTexts;
    vn::GameCatalog m_catalog;
    std::map<std::string, std::string, std::less<>> m_screenTriggers;
    std::map<std::string, ui::GalgameAction, std::less<>> m_uiActionTriggers;

    AppState m_appState = AppState::BootSplash;
    bool m_uiTransitionStartedThisFrame = false;
    bool m_gameStartTransitionActive = false;
    bool m_gameStartTransitionCommitted = false;
    float m_gameStartTransitionElapsed = 0.0f;
    std::string m_script;
    std::string m_runtimePreloadPath;
    std::future<std::shared_ptr<const vn::Program>> m_runtimePreload;
    std::string m_pendingStoryEntry;
    std::string m_selectedFlowEntry;
    std::vector<std::string> m_musicTracks;
    std::size_t m_musicTrackIndex = 0;
    bool m_musicTrackSelected = false;
    std::string m_lastStoryBgm;
    int m_replayPage = 1;
    int m_saveLoadPage = 1;
    int m_savePages = 1;
    int m_saveSlotsPerPage = 12;
    int m_saveFirstSlot = 0;
    int m_settingsPage = 1;

    bool m_autoMode = false;
    bool m_skipMode = false;
    bool m_hudHidden = false;
    bool m_storyHudVisible = true;
    std::uint64_t m_autoTimerStart = 0;
    std::uint64_t m_playtimeBaseMs = 0;
    std::uint64_t m_playtimeStartedAtMs = 0;

    bool m_nvlMode = false;
    std::vector<vn::BacklogEntry> m_nvlLines;
    std::deque<RollbackEntry> m_rollback;
    std::uint64_t m_lastBacklogSequence = 0;

    bool m_slotSaveMode = false;
    bool m_autoSavedChoice = false;
    bool m_saveRequested = false;
    bool m_pendingSaveScreen = false;
    struct SavePresentationState {
        ui::UIRuntimeState ui;
        ui::RouteState routes;
    };
    std::optional<SavePresentationState> m_beforeSavePresentation;
    std::optional<int> m_pendingSaveLoadSlot;
    bool m_pendingSaveLoadSaveMode = false;
    // A copy or move waiting for its target slot.
    struct SlotOperation {
        bool move = false;
        int slot = 0;
    };
    std::optional<SlotOperation> m_slotOperation;
    std::optional<int> m_commentSlot;
    ui::GalgameUI::SystemPrompt m_pendingSystemPrompt =
        ui::GalgameUI::SystemPrompt::None;
    std::vector<std::uint8_t> m_menuThumb;
    std::string m_viewingCG;

    std::unique_ptr<video::VideoPlayer> m_video;
    std::string m_pendingVideo;
    bool m_videoSkippable = true;
    std::uint64_t m_nextScriptVideoHandle = 1;
    std::uint64_t m_scriptVideoHandle = 0;
    std::unordered_map<std::uint64_t, std::string> m_scriptVideoStates;
    std::unordered_map<std::uint64_t, std::string> m_scriptVideoErrors;
    std::deque<std::uint64_t> m_scriptVideoTerminalOrder;
    std::optional<int> m_pendingScriptSave;
    std::optional<int> m_pendingScriptLoad;

    bool m_quitRequested = false;
    bool m_bootFailureDialog = false;
    // PRISMATIX_EXIT_WITH_PROCESS=<pid>: `prismatix run` sets it so the game
    // closes when the CLI (an editor's Play/Stop) goes away.
    std::atomic<bool> m_ownerProcessExited{false};
    std::unique_ptr<platform::ProcessExitWatch> m_ownerProcessWatch;
    std::unique_ptr<QaSession> m_qa;
    accessibility::SpeechService m_speech;
    std::shared_ptr<accessibility::SemanticAdapter> m_accessibilityAdapter;
};

}
