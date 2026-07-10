#pragma once

#include "Engine/Core/Types.h"
#include "Engine/VN/PDS/Compiler.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::io {
class VFS;
}
namespace px::audio {
class AudioEngine;
}

namespace px::vn {

class Stage;
class Dialogue;
class VariableStore;
class Backlog;

enum class VMState {
    Idle,
    Running,
    WaitingClick,
    WaitingChoice,
    WaitingTimer,
    WaitingVideo,  // a [video] is playing; host calls NotifyVideoDone()
    Paused,        // stopped at a breakpoint (debugger)
    Finished,
};

struct Choice {
    std::string text;
    std::string target;
};

struct VMConfig {
    std::string bgDir = "Data/Image/Background/";
    std::string ruleDir = "Data/Image/Rule/";
    std::string charDir = "Data/Image/Character/";
    std::string cgDir = "Data/Image/CG/";
    std::string bgmDir = "Data/Audio/Music/";
    std::string seDir = "Data/Audio/SFX/";
    std::string voiceDir = "Data/Audio/Voice/";
    std::string videoDir = "Data/Video/";
    std::string scriptDir = "Data/Script/";
    int defaultTextSpeed = 28;
};

class VM {
public:
    VM(io::VFS& vfs, audio::AudioEngine& audio, Stage& stage, Dialogue& dialogue,
       VariableStore& vars, Backlog& backlog);

    void SetConfig(const VMConfig& config) { m_config = config; }
    // Applies a user-preference text speed (settings screen); overrides any
    // script-set speed for subsequent lines.
    void SetDefaultTextSpeed(int ms) {
        m_config.defaultTextSpeed = ms;
        m_textSpeed = ms;
    }
    void SetCommandHook(std::function<bool(const Command&)> hook) { m_commandHook = std::move(hook); }
    // Localization: maps source text to the active language. Applied to dialogue
    // lines and choice labels after variable substitution.
    void SetTextFilter(std::function<std::string(const std::string&)> filter) {
        m_textFilter = std::move(filter);
    }
    // Read-text tracking: called with "script:line" for every dialogue line;
    // returns whether that line was already seen (既讀). Drives skip-read-only.
    void SetSeenHook(std::function<bool(const std::string& key)> hook) {
        m_seenHook = std::move(hook);
    }
    // Auto-voice convention: character id/display-name -> voice directory.
    // A say without an explicit voice tries "<dir><scriptStem>_<line>.{ogg,wav,mp3}".
    void SetVoiceDirs(std::unordered_map<std::string, std::string> dirs) {
        m_voiceDirs = std::move(dirs);
    }
    [[nodiscard]] bool CurrentLineSeen() const { return m_currentLineSeen; }
    // Video: [video file="op.mpg" skippable="true"] suspends the VM and hands the
    // resolved path to the host. The host plays it and calls NotifyVideoDone().
    void SetVideoHook(std::function<void(const std::string& path, bool skippable)> hook) {
        m_videoHook = std::move(hook);
    }
    void NotifyVideoDone();
    void SetUnlockHook(std::function<void(const std::string& kind, const std::string& id)> hook) {
        m_unlockHook = std::move(hook);
    }

    bool LoadScript(const std::string& scriptPath);
    void Update(std::uint64_t nowMs, float dt);
    void OnAdvance();
    void SelectChoice(int index);
    void Resume();

    // --- Debugger ---
    void ToggleBreakpoint(int line) {
        if (!m_breakpoints.erase(line)) m_breakpoints.insert(line);
    }
    void ClearBreakpoints() { m_breakpoints.clear(); }
    [[nodiscard]] const std::set<int>& Breakpoints() const { return m_breakpoints; }
    void DebugContinue();  // resume past the current breakpoint
    void DebugStep();      // execute exactly one command, then pause again

    [[nodiscard]] VMState State() const { return m_state; }
    [[nodiscard]] bool Blocking() const;
    [[nodiscard]] const VMConfig& Config() const { return m_config; }
    [[nodiscard]] const std::vector<Choice>& Choices() const { return m_choices; }
    [[nodiscard]] const std::string& Chapter() const { return m_chapter; }
    [[nodiscard]] const std::string& CurrentBgm() const { return m_currentBgm; }
    // Rollback restores audio outside the VM; keep the bookkeeping in sync.
    void OverrideCurrentBgm(const std::string& path) { m_currentBgm = path; }
    [[nodiscard]] const std::string& CurrentScript() const { return m_scriptPath; }
    [[nodiscard]] const Program& CurrentProgram() const { return m_program; }
    [[nodiscard]] int ProgramCounter() const { return m_pc; }
    // pc to store in a save: while waiting for a click it backs up onto the say
    // command so loading the save re-displays the line being read.
    [[nodiscard]] int SavePoint() const {
        return (m_state == VMState::WaitingClick && m_pc > 0) ? m_pc - 1 : m_pc;
    }

    void SeekTo(const std::string& scriptPath, int pc);

private:
    bool LoadProgram(const std::string& scriptPath);
    void Run();
    void ExecuteSimple(const Command& cmd);
    void HandleSay(const Command& cmd);
    void CollectChoices();
    bool EvaluateCondition(const Command& cmd) const;
    // Jumps to a label in the current program or loads another script.
    // Returns false (leaving the pc untouched) when the target is empty or
    // cannot be resolved; callers decide how to advance.
    bool JumpToTarget(const std::string& target);

    [[nodiscard]] std::string Resolve(const std::string& dir, const std::string& file) const;

    io::VFS& m_vfs;
    audio::AudioEngine& m_audio;
    Stage& m_stage;
    Dialogue& m_dialogue;
    VariableStore& m_vars;
    Backlog& m_backlog;

    VMConfig m_config;
    std::function<bool(const Command&)> m_commandHook;
    std::function<void(const std::string&, const std::string&)> m_unlockHook;
    std::function<std::string(const std::string&)> m_textFilter;
    std::function<bool(const std::string&)> m_seenHook;
    std::function<void(const std::string&, bool)> m_videoHook;
    bool m_currentLineSeen = true;

    [[nodiscard]] std::string FilterText(const std::string& text) const {
        return m_textFilter ? m_textFilter(text) : text;
    }

    Program m_program;
    std::string m_scriptPath;
    int m_pc = 0;
    VMState m_state = VMState::Idle;

    struct CallFrame {
        std::string script;  // script the [call] lives in, so [return] crosses files
        int pc = 0;
    };

    std::vector<Choice> m_choices;
    std::vector<CallFrame> m_callStack;
    std::set<int> m_breakpoints;
    bool m_skipBacklogOnce = false;  // set when a load re-executes the saved say
    bool m_skipBreakOnce = false;
    bool m_stepping = false;
    int m_stepBudget = 0;

    std::string m_speaker;
    std::string m_pendingVoice;  // set by a [text voice=...] header, consumed by the next say
    std::unordered_map<std::string, std::string> m_voiceDirs;
    Color m_textColor{ 245, 248, 255, 255 };
    Color m_outlineColor{ 0, 0, 0, 255 };
    int m_textSpeed = 28;
    std::string m_chapter;
    std::string m_currentBgm;

    std::uint64_t m_timerStart = 0;
    std::uint64_t m_timerMs = 0;
    std::uint64_t m_nowMs = 0;
};

}
