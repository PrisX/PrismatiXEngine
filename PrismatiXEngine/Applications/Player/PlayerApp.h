#pragma once

#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/SaveSystem.h"
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
#include <memory>
#include <string>
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

    // Boot configuration shared by typed dev projects and packaged builds.
    struct Boot {
        RuntimeConfig config;
        std::string startScript = "Content/Scenario/start.pxscenario";
        std::string sourceMap;
        std::string startRoute = "title";
        std::unordered_map<std::string,std::string> routeScenes;
        std::vector<ui::startup::SplashScreenEntry> splashes;
        std::string saveSecret;  // per-project secret for save encryption
        std::string gameId;
        std::string packageFingerprint;
        std::string contentVersion;
        std::uint32_t saveVersion = 1;
        std::vector<progress::SaveMigrationDescriptor> saveMigrations;
        std::vector<std::string> extensions;
        std::string graphicsTier = "basic";
        bool packaged = false;
        bool valid = false;
    };
    static Boot LoadBootConfig();

    bool Init(int argc, char* argv[]);
    void MainLoop();
    void Shutdown();
    void FinishBootPresentation();
    void SplashFrame(float dt);

    void StartGame();
    bool LoadSlot(int slot);
    void SaveSlot(int slot, std::vector<std::uint8_t> thumbnail);
    [[nodiscard]] progress::SaveSnapshot MakeSnapshot(bool includeBacklog);
    bool LoadLocale(std::string locale, bool refreshPresentation);

    // --- Rollback (per-line snapshot ring) ---
    struct RollbackEntry {
        progress::SaveSnapshot snap;
        std::size_t backlogSize = 0;  // backlog length including this line
    };
    [[nodiscard]] bool ApplyRollback(const RollbackEntry& entry);
    void RollbackOneLine();
    bool RollbackToBacklogIndex(std::size_t index);

    void OpenScreen(const std::string& route);
    void PresentRoute(const std::string& route, const std::string& operation);
    [[nodiscard]] std::vector<ui::GalgameItem> GalleryItems();
    [[nodiscard]] std::vector<ui::GalgameItem> SaveItems(bool saveMode);
    [[nodiscard]] std::vector<ui::GalgameItem> BacklogItems();
    [[nodiscard]] ui::DialoguePresentation DialogueUI() const;
    [[nodiscard]] ui::SettingsPresentation SettingsUI() const;
    void HandleUIAction(const ui::GalgameAction& action);

    void TitleFrame(float dt);
    void GameFrame(float dt, std::uint64_t now);
    void ScreensFrame(float dt);
    // Returns true when it consumed the frame (a movie is on screen).
    bool VideoFrame(float dt);
    void ConfigureE2EJourney();
    void DriveE2EJourney();
    void CaptureE2EGalleryFrame();

    Runtime m_runtime;
    Boot m_boot;
    crypto::Key m_saveKey{};
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

    ui::GalgameUI m_ui;
    std::unique_ptr<ui::startup::SplashSequencePlayer> m_splash;
    std::vector<std::string> m_choiceTexts;
    vn::GameCatalog m_catalog;
    std::unordered_map<int, std::string> m_screenTriggers;

    AppState m_appState = AppState::BootSplash;
    std::string m_script;

    bool m_autoMode = false;
    bool m_skipMode = false;
    bool m_hudHidden = false;
    std::uint64_t m_autoTimerStart = 0;
    std::uint64_t m_playtimeBaseMs = 0;
    std::uint64_t m_playtimeStartedAtMs = 0;

    bool m_nvlMode = false;
    std::vector<vn::BacklogEntry> m_nvlLines;
    std::deque<RollbackEntry> m_rollback;
    std::size_t m_lastBacklogSize = 0;

    bool m_slotSaveMode = false;
    bool m_autoSavedChoice = false;
    bool m_saveRequested = false;
    bool m_pendingSaveScreen = false;
    std::vector<std::uint8_t> m_menuThumb;
    std::string m_viewingCG;

    std::unique_ptr<video::VideoPlayer> m_video;
    std::string m_pendingVideo;
    bool m_videoSkippable = true;

    bool m_quitRequested = false;
    enum class E2EStage {
        Disabled,
        AwaitTitle,
        AwaitGameReady,
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
    accessibility::SpeechService m_speech;
    std::shared_ptr<accessibility::SemanticAdapter> m_accessibilityAdapter;
};

}
