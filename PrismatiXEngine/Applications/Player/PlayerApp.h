#pragma once

#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Project/Database.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Runtime.h"
#include "Engine/UI/ScreenManager.h"
#include "Engine/UI/UIStage.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"
#include "Engine/Video/VideoPlayer.h"

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
    int Run(int argc, char* argv[]);

private:
    enum class AppState { Title, Game };

    // Boot configuration shared by dev runs (project.prismatix.json) and
    // packaged builds (game.prismatix).
    struct Boot {
        RuntimeConfig config;
        std::string startUI = "Data/UI/title.pxui";
        std::string startScript = "test_scene.pds";
        std::string saveSecret;  // per-project secret for save encryption
        bool packaged = false;
    };
    static Boot LoadBootConfig();

    bool Init(int argc, char* argv[]);
    void MainLoop();
    void Shutdown();

    void StartGame();
    bool LoadSlot(int slot);
    void SaveSlot(int slot, std::vector<std::uint8_t> thumbnail);
    [[nodiscard]] progress::SaveSnapshot MakeSnapshot(bool includeBacklog);

    // --- Rollback (per-line snapshot ring) ---
    struct RollbackEntry {
        progress::SaveSnapshot snap;
        std::size_t backlogSize = 0;  // backlog length including this line
    };
    void ApplyRollback(const RollbackEntry& entry);
    void RollbackOneLine();
    bool RollbackToBacklogIndex(std::size_t index);

    void RenderNVL();

    void OpenScreen(const std::string& path);
    void PopulateGallery(ui::UIStage& stage);
    void PopulateSaves(ui::UIStage& stage);
    void RefreshSettingsScreen(ui::UIStage& stage);
    // Returns false when the app should quit.
    bool HandleScreenAction(const ui::UIAction& action);

    void TitleFrame(float dt);
    void GameFrame(float dt, std::uint64_t now);
    void ScreensFrame(float dt);
    void BacklogFrame();
    // Returns true when it consumed the frame (a movie is on screen).
    bool VideoFrame(float dt);

    Runtime m_runtime;
    Boot m_boot;
    crypto::Key m_saveKey{};
    progress::GlobalProfile m_profile;
    progress::GameSettings m_settings;
    progress::SaveSystem m_saves;

    vn::Dialogue m_dialogue;
    vn::VariableStore m_vars;
    vn::Backlog m_backlog;
    std::unique_ptr<vn::Stage> m_stage;
    std::unique_ptr<vn::VM> m_vm;
    std::unique_ptr<lua::LuaHost> m_lua;
    lua::LuaServices m_luaServices;
    std::unordered_map<std::string, std::string> m_langTable;

    ui::UIStage m_titleStage;
    ui::UIStage m_hud;
    std::unique_ptr<ui::ScreenManager> m_screens;
    std::vector<std::string> m_choiceTexts;
    project::Database m_db;

    AppState m_appState = AppState::Title;
    bool m_titleOk = false;
    std::string m_script;

    bool m_autoMode = false;
    bool m_skipMode = false;
    bool m_hudHidden = false;
    bool m_backlogOpen = false;
    float m_backlogScroll = 0.0f;
    std::uint64_t m_autoTimerStart = 0;

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
};

}
