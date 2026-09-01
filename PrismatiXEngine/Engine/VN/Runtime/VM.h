#pragma once

#include "Engine/Core/Types.h"
#include "Engine/VN/GameCatalog.h"
#include "Engine/VN/Runtime/Program.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
namespace px::diag {
struct Diagnostic;
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
    WaitingExternal, // an awaitable extension command resumes at a safe point
    Paused,        // stopped at a breakpoint (debugger)
    Finished,
};

enum class ProgramPatchStatus {
    Applied,
    InvalidProgram,
    ScriptMismatch,
    UnsupportedState,
    MissingAnchor,
    StructuralChange,
};

enum class ProgramSeekStatus {
    Applied,
    InvalidProgram,
    InvalidOperation,
    ChoicePathRequired,
    UnsupportedBlockingState,
    UnsafeOperation,
    Unreachable,
};

enum class VMRunStatus {
    Yielded,
    AwaitingInput,
    Completed,
    Faulted,
};

struct VMRunResult {
    VMRunStatus status = VMRunStatus::Completed;
    std::size_t instructions = 0;
};

struct Choice {
    std::string text;
    std::string target;
    std::string sourceId;
    std::string operationId;
};

struct VMCallFrameState {
    std::string script;
    int pc = 0;
};

struct VMRuntimeState {
    std::string scriptPath;
    int pc = 0;
    VMState state = VMState::Idle;
    std::vector<VMCallFrameState> callStack;
    std::vector<Choice> choices;
    std::string speaker;
    std::string pendingVoice;
    Color textColor{245,248,255,255};
    Color outlineColor{0,0,0,255};
    int textSpeed = 28;
    std::string textEffect;
    std::string chapter;
    std::string currentBgm;
    std::uint64_t timerRemainingMs = 0;
    bool currentLineSeen = true;
};

struct VMConfig {
    std::string bgDir = "Content/Images/Background/";
    std::string ruleDir = "Content/Images/Rules/";
    std::string charDir = "Content/Images/Character/";
    std::string cgDir = "Content/Images/CG/";
    std::string bgmDir = "Content/Audio/Music/";
    std::string seDir = "Content/Audio/SFX/";
    std::string voiceDir = "Content/Audio/Voice/";
    std::string videoDir = "Content/Video/";
    std::string scriptDir = "Content/Scenario/";
    int defaultTextSpeed = 28;
    std::size_t maxInstructionsPerTick = 10'000;
    std::uint64_t maxInstructionsWithoutYield = 1'000'000;
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
    void SetExecutionSafetyHook(
        std::function<bool(const Command&, bool seeking)> hook) {
        m_executionSafetyHook = std::move(hook);
    }
    void SetDiagnosticSourceResolver(
        std::function<void(diag::Diagnostic&, const Command&)> resolver) {
        m_diagnosticSourceResolver = std::move(resolver);
    }
    // Localization: maps source text to the active language. Applied to dialogue
    // lines and choice labels after variable substitution.
    void SetTextFilter(std::function<std::string(const std::string&, const std::string&)> filter) {
        m_textFilter = std::move(filter);
    }
    // Read-text tracking: called with "script:line" for every dialogue line;
    // returns whether that line was already seen (既讀). Drives skip-read-only.
    void SetSeenHook(std::function<bool(const std::string& key)> hook) {
        m_seenHook = std::move(hook);
    }
    void SetChoiceSeenHook(std::function<void(const std::string& key)> hook) {
        m_choiceSeenHook = std::move(hook);
    }
    // Auto-voice convention: character id/display-name -> voice directory.
    // A say without an explicit voice tries "<dir><scriptStem>_<line>.{ogg,wav,mp3}".
    void SetVoiceDirs(std::unordered_map<std::string, std::string> dirs) {
        m_voiceDirs = std::move(dirs);
    }
    // Character ids and expression images are resolved centrally from the
    // typed GameCatalog. Explicit command file= still takes precedence.
    void SetGameCatalog(const GameCatalog& catalog) {
        m_catalog = catalog;
        m_voiceDirs.clear();
        for (const auto& character : m_catalog.Characters()) {
            if (character.voiceDirectory.empty()) continue;
            if (!character.id.empty()) m_voiceDirs[character.id] = character.voiceDirectory;
            if (!character.name.empty()) m_voiceDirs[character.name] = character.voiceDirectory;
        }
    }
    [[nodiscard]] bool CurrentLineSeen() const { return m_currentLineSeen; }
    // Video: [video file="op.mpg" skippable="true"] suspends the VM and hands the
    // resolved path to the host. The host plays it and calls NotifyVideoDone().
    void SetVideoHook(std::function<void(const std::string& path, bool skippable)> hook) {
        m_videoHook = std::move(hook);
    }
    void NotifyVideoDone();
    void WaitExternal() { m_state = VMState::WaitingExternal; }
    void NotifyExternalDone();
    void SetUnlockHook(std::function<void(const std::string& kind, const std::string& id)> hook) {
        m_unlockHook = std::move(hook);
    }

    bool LoadScript(const std::string& scriptPath);
    bool LoadScenarioText(std::string_view text, const std::string& scriptPath);
    bool LoadCompiledProgram(Program program, const std::string& scriptPath);
    ProgramSeekStatus LoadCompiledProgramAt(Program program,
                                            const std::string& scriptPath,
                                            int operationIndex);
    ProgramSeekStatus ReplayCompiledProgramAt(
        int operationIndex, std::span<const int> branchPath = {},
        std::size_t* choicesConsumed = nullptr);
    ProgramPatchStatus PatchCompiledProgram(Program program,
                                            const std::string& scriptPath);
    void Update(std::uint64_t nowMs, float dt);
    [[nodiscard]] VMRunResult RunSlice(std::size_t maxInstructions);
    void OnAdvance();
    void SelectChoice(int index);
    void Resume();

    // --- Debugger ---
    void ToggleBreakpoint(int line) {
        if (!m_breakpoints.erase(line)) m_breakpoints.insert(line);
    }
    void ClearBreakpoints() { m_breakpoints.clear(); }
    [[nodiscard]] const std::set<int>& Breakpoints() const { return m_breakpoints; }
    bool DebugPause();     // pause without discarding the current waiting/running state
    void DebugContinue();  // resume past the current breakpoint
    void DebugStep();      // execute exactly one command, then pause again
    [[nodiscard]] bool ManuallyPaused() const { return m_debugResumeState.has_value(); }

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
    [[nodiscard]] int CurrentSourceLine() const {
        if (m_program.code.empty()) return 0;
        int index = m_pc;
        if (m_state == VMState::WaitingClick && index > 0) --index;
        if (index < 0 || index >= static_cast<int>(m_program.code.size())) return 0;
        return m_program.code[static_cast<std::size_t>(index)].line;
    }
    [[nodiscard]] std::string CurrentSourceId() const {
        if (m_program.code.empty()) return {};
        int index = m_pc;
        if ((m_state == VMState::WaitingClick || m_state == VMState::WaitingTimer ||
             m_state == VMState::WaitingVideo || m_state == VMState::WaitingExternal) &&
            index > 0) --index;
        if (index < 0 || index >= static_cast<int>(m_program.code.size())) return {};
        return m_program.code[static_cast<std::size_t>(index)].sourceId;
    }
    [[nodiscard]] std::string CurrentOperationId() const {
        if (m_program.code.empty()) return {};
        int index = m_pc;
        if ((m_state == VMState::WaitingClick || m_state == VMState::WaitingTimer ||
             m_state == VMState::WaitingVideo || m_state == VMState::WaitingExternal) &&
            index > 0) --index;
        if (index < 0 || index >= static_cast<int>(m_program.code.size())) return {};
        return m_program.code[static_cast<std::size_t>(index)].operationId;
    }
    [[nodiscard]] const std::string& CurrentDocumentId() const {
        return m_program.documentId;
    }
    // pc to store in a save: while waiting for a click it backs up onto the say
    // command so loading the save re-displays the line being read.
    [[nodiscard]] int SavePoint() const {
        return (m_state == VMState::WaitingClick && m_pc > 0) ? m_pc - 1 : m_pc;
    }

    void SeekTo(const std::string& scriptPath, int pc);
    [[nodiscard]] VMRuntimeState CaptureState() const;
    bool RestoreState(const VMRuntimeState& state, std::uint64_t nowMs = 0);
    bool RestoreCompiledState(const VMRuntimeState& state, Program program,
                              std::uint64_t nowMs = 0);
    [[nodiscard]] bool SafetyRejected() const { return m_safetyRejected; }
    void ClearSafetyRejection() { m_safetyRejected = false; }
    [[nodiscard]] bool Seeking() const { return m_seeking; }
    [[nodiscard]] VMRunResult LastRunResult() const { return m_lastRunResult; }

private:
    bool LoadProgram(const std::string& scriptPath);
    VMRunResult Run();
    void ExecuteSimple(const Command& cmd);
    void HandleSay(const Command& cmd, bool recordPlayback = true);
    void CollectChoices();
    bool EvaluateCondition(const Command& cmd) const;
    void ApplyDiagnosticSource(diag::Diagnostic& diagnostic,
                               const Command& command) const;
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
    std::function<std::string(const std::string&, const std::string&)> m_textFilter;
    std::function<bool(const std::string&)> m_seenHook;
    std::function<void(const std::string&)> m_choiceSeenHook;
    std::function<void(const std::string&, bool)> m_videoHook;
    bool m_currentLineSeen = true;

    [[nodiscard]] std::string FilterText(const std::string& text,const std::string& textId={}) const {
        return m_textFilter ? m_textFilter(textId,text) : text;
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
    std::optional<VMState> m_debugResumeState;
    std::optional<int> m_seekTargetPc;
    bool m_seeking = false;
    bool m_safetyRejected = false;
    std::uint64_t m_instructionsWithoutObservableYield = 0;
    VMRunResult m_lastRunResult{};
    std::function<bool(const Command&, bool)> m_executionSafetyHook;
    std::function<void(diag::Diagnostic&, const Command&)>
        m_diagnosticSourceResolver;

    std::string m_speaker;
    std::string m_pendingVoice;  // set by a [text voice=...] header, consumed by the next say
    GameCatalog m_catalog;
    std::unordered_map<std::string, std::string> m_voiceDirs;
    Color m_textColor{ 245, 248, 255, 255 };
    Color m_outlineColor{ 0, 0, 0, 255 };
    int m_textSpeed = 28;
    std::string m_textEffect;
    std::string m_chapter;
    std::string m_currentBgm;

    std::uint64_t m_timerStart = 0;
    std::uint64_t m_timerMs = 0;
    std::uint64_t m_nowMs = 0;
};

}
