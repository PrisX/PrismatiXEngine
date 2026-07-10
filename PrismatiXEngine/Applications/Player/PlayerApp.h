#pragma once

#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Runtime.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Runtime/VM.h"
#include "Engine/VN/GameCatalog.h"
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

    // Boot configuration shared by typed dev projects and packaged builds.
    struct Boot {
        RuntimeConfig config;
        std::string startScript = "Content/Script/start.pds";
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

    void OpenScreen(const std::string& route);
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

    ui::GalgameUI m_ui;
    std::vector<std::string> m_choiceTexts;
    vn::GameCatalog m_catalog;
    std::unordered_map<int, std::string> m_screenTriggers;

    AppState m_appState = AppState::Title;
    std::string m_script;

    bool m_autoMode = false;
    bool m_skipMode = false;
    bool m_hudHidden = false;
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
