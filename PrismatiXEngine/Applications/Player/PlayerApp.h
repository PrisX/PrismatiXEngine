#pragma once

#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
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

#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
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
    int Run(int argc, char* argv[]);

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
    void Shutdown();
    bool SaveProfileIfWritable();
    bool SaveSettingsIfWritable();
    bool ApplyFullscreenSetting(bool fullscreen);
    void BeginRuntimeIrPreload();
    void FinishBootPresentation();
    [[nodiscard]] bool TitleVisualsReady();
    void SplashFrame(float dt);
    void PlayMotionMarker(const sdk::MotionAudioMarker& marker);
    void BeginPresentationTransition(const PresentationTransition& transition);

    void StartGame();
    [[nodiscard]] bool CommitStartGame();
    void GameStartTransitionFrame(float dt, std::uint64_t now);
    bool LoadSlot(int slot);
    bool SaveSlot(int slot, std::vector<std::uint8_t> thumbnail);
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
    [[nodiscard]] ui::DialoguePresentation DialogueUI() const;
    [[nodiscard]] ui::SettingsPresentation SettingsUI() const;
    void HandleUIAction(const ui::GalgameAction& action);
    void RecordScriptVideoTerminal(std::uint64_t handle, std::string status,
                                   std::string error = {});

    void TitleFrame(float dt);
    void GameFrame(float dt, std::uint64_t now);
    void ScreensFrame(float dt);
    void RenderStageWithScripts();
    void RenderUIWithScripts();
    void UpdateScriptScheduler(float dt);
    void ProcessScriptSafePoint();
    // Returns true when it consumed the frame (a movie is on screen).
    bool VideoFrame(float dt);
    void ConfigureE2EJourney();
    void DriveE2EJourney();
    void CaptureE2EGalleryFrame();
    void CaptureUIRoutesE2EFrame();
    bool CapturePresentationE2EFrame(std::string_view name);

    Runtime m_runtime;
    Boot m_boot;
    std::filesystem::path m_saveDirectory = "Save";
    crypto::Key m_saveKey{};
    std::vector<crypto::Key> m_saveFallbackKeys;
    bool m_profileWritable = true;
    bool m_settingsWritable = true;
    progress::GlobalProfile m_profile;
    progress::GameSettings m_settings;
    progress::SaveSystem m_saves;

    std::unique_ptr<RuntimeSession> m_session;
    std::unique_ptr<script::ScriptHost> m_scriptHost;
    script::ScriptServices m_scriptServices;
    std::unordered_map<std::string, std::string> m_langTable;
    std::string m_defaultLocale = "zh-TW";
    std::vector<std::string> m_supportedLocales;
    std::unordered_map<std::string, std::string> m_uiAssets;
    std::unordered_map<std::string, std::string> m_uiComponents;
    std::string m_titleBgm;
    std::vector<std::string> m_titleVoices;
    std::string m_defaultUiHoverSound;
    std::string m_defaultUiActivateSound;
    PresentationTransition m_routeTransition;
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

    bool m_autoMode = false;
    bool m_skipMode = false;
    bool m_hudHidden = false;
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
    enum class E2EStage {
        Disabled,
        AwaitTitle,
        AwaitTitleAsync,
        AwaitGameReady,
        AwaitMenuAsync,
        AwaitSaveScreen,
        AwaitPostMenuGameReady,
        VerifyReleaseF2,
        AwaitGallery,
        CaptureGallery,
        CloseGallery,
        AwaitOverlayClosed,
        Complete,
    };
    E2EStage m_e2eStage = E2EStage::Disabled;
    std::uint64_t m_e2eStartedAt = 0;
    bool m_e2eFailed = false;
    bool m_e2eLocaleSwitched = false;
    bool m_e2eCustomTitleRouteSeen = false;
    bool m_e2eCustomMenuRouteSeen = false;
    bool m_presentationE2E = false;
    int m_presentationE2EStage = 0;
    std::uint64_t m_presentationE2EStartedAt = 0;
    std::string m_presentationE2ECaptureDir;
    std::string m_presentationE2ETargetOperationId;
    std::vector<bool> m_presentationE2ESplashCaptured;
    bool m_uiRoutesE2E = false;
    bool m_uiRoutesE2EReadinessLogged = false;
    std::size_t m_uiRoutesE2EIndex = 0;
    bool m_uiRoutesE2EAwaitingCapture = false;
    int m_uiRoutesE2ESaveConfirmStage = 0;
    std::uint64_t m_uiRoutesE2EStartedAt = 0;
    std::uint64_t m_uiRoutesE2ECaptureAt = 0;
    bool m_uiActionsE2E = false;
    int m_uiActionsE2EStage = 0;
    std::uint64_t m_uiActionsE2EStartedAt = 0;
    int m_e2eClearCountBeforeF2 = 0;
    std::optional<Variant> m_e2eSessionValueBeforeF2;
    accessibility::SpeechService m_speech;
    std::shared_ptr<accessibility::SemanticAdapter> m_accessibilityAdapter;
};

}
