#include "Engine/VN/Runtime/VM.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/IO/VFS.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Expression/Expression.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/Support/Logger.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <utility>

namespace px::vn {

namespace {
int ParseInt(const std::string& s, int fallback = 0) {
    if (s.empty()) return fallback;
    int v = fallback;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

float ParseFloat(const std::string& s, float fallback = 0.0f) {
    if (s.empty()) return fallback;
    try {
        return std::stof(s);
    } catch (...) {
        return fallback;
    }
}

Color ParseColor(const std::string& s, Color fallback) {
    if (s.empty()) return fallback;
    Color c = fallback;
    std::stringstream ss(s);
    std::string part;
    int idx = 0;
    while (std::getline(ss, part, ',') && idx < 4) {
        const int v = ParseInt(part, 255);
        const auto b = static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        if (idx == 0) c.r = b;
        else if (idx == 1) c.g = b;
        else if (idx == 2) c.b = b;
        else c.a = b;
        ++idx;
    }
    return c;
}

// Explicit image framing from x/y/zoom/rotate/anchor ("ax,ay"). Omitted
// fields keep `base`, so [bg same x=...] pans without resetting the zoom. The
// from-* form reads from_x/from_y/from_zoom with the target as the default.
std::optional<Stage::Placement> ParsePlacement(
    const Command& cmd, const std::optional<Stage::Placement>& base = std::nullopt) {
    if (!cmd.Has("x") && !cmd.Has("y") && !cmd.Has("zoom") &&
        !cmd.Has("rotate") && !cmd.Has("anchor"))
        return std::nullopt;
    Stage::Placement placement = base.value_or(Stage::Placement{});
    if (cmd.Has("x")) placement.x = ParseFloat(cmd.Get("x"));
    if (cmd.Has("y")) placement.y = ParseFloat(cmd.Get("y"));
    if (cmd.Has("zoom")) placement.zoom = std::max(0.001f, ParseFloat(cmd.Get("zoom"), 1.0f));
    if (cmd.Has("rotate")) placement.rotation = ParseFloat(cmd.Get("rotate"));
    const std::string anchor = cmd.Get("anchor");
    if (const auto comma = anchor.find(','); comma != std::string::npos) {
        placement.anchored = true;
        placement.anchorX = ParseFloat(anchor.substr(0, comma));
        placement.anchorY = ParseFloat(anchor.substr(comma + 1));
    }
    return placement;
}

std::optional<Stage::Placement> ParseFromPlacement(
    const Command& cmd, const std::optional<Stage::Placement>& target) {
    if (!target || (!cmd.Has("from_x") && !cmd.Has("from_y") && !cmd.Has("from_zoom")))
        return std::nullopt;
    Stage::Placement from = *target;
    if (cmd.Has("from_x")) from.x = ParseFloat(cmd.Get("from_x"));
    if (cmd.Has("from_y")) from.y = ParseFloat(cmd.Get("from_y"));
    if (cmd.Has("from_zoom"))
        from.zoom = std::max(0.001f, ParseFloat(cmd.Get("from_zoom"), 1.0f));
    return from;
}

bool TruthyFlag(const Command& cmd) {
    const std::string v = cmd.Get("transition", "true");
    return v != "none" && v != "false" && v != "0";
}
}

VM::VM(io::VFS& vfs, audio::AudioEngine& audio, Stage& stage, Dialogue& dialogue,
       VariableStore& vars, Backlog& backlog)
    : m_vfs(vfs), m_audio(audio), m_stage(stage), m_dialogue(dialogue), m_vars(vars),
      m_backlog(backlog) {
    m_textSpeed = m_config.defaultTextSpeed;
}

std::string VM::Resolve(const std::string& dir, const std::string& file) const {
    if (file.empty()) return "";
    if (file.find('/') != std::string::npos || file.find(':') != std::string::npos) {
        return file;
    }
    return dir + file;
}

bool VM::LoadProgram(const std::string& scriptPath) {
    const std::string path = Resolve(m_config.scriptDir, scriptPath);
    auto text = m_vfs.ReadText(path);
    if (!text) {
        PX_LOG_ERROR("VM: script not found '{}'", path);
        return false;
    }
    if (!path.ends_with(".pxscenario")) {
        PX_LOG_ERROR("VM: only strict Typed Format v4 .pxscenario files are executable: '{}'", path);
        return false;
    }
    auto document = scenario::ParseScenario(*text, path);
    if (!document) {
        m_program = {};
        for (const auto& diagnostic : document.Diagnostics()) {
            m_program.errors.push_back(diag::Describe(diagnostic));
        }
    } else {
        m_program = scenario::CompileScenario(document.Value());
    }
    for (const std::string& err : m_program.errors) {
        PX_LOG_ERROR("VM compile ({}) : {}", path, err);
    }
    if (!m_program.errors.empty()) return false;
    m_scriptPath = scriptPath;
    m_pc = 0;
    m_pendingVoice.clear();
    return true;
}

bool VM::LoadScript(const std::string& scriptPath) {
    m_callStack.clear();
    m_debugResumeState.reset();
    if (!LoadProgram(scriptPath)) {
        m_state = VMState::Finished;
        return false;
    }
    m_textSpeed = m_config.defaultTextSpeed;
    Run();
    return true;
}

bool VM::LoadScenarioText(const std::string_view text, const std::string& scriptPath) {
    m_callStack.clear();
    m_debugResumeState.reset();
    auto document = scenario::ParseScenario(text, scriptPath);
    if (!document) {
        m_program = {};
        for (const auto& diagnostic : document.Diagnostics())
            m_program.errors.push_back(diag::Describe(diagnostic));
        m_state = VMState::Finished;
        return false;
    }
    m_program = scenario::CompileScenario(document.Value());
    if (!m_program.errors.empty()) { m_state = VMState::Finished; return false; }
    m_scriptPath = scriptPath;
    m_pc = 0;
    m_pendingVoice.clear();
    m_textSpeed = m_config.defaultTextSpeed;
    Run();
    return true;
}

bool VM::LoadCompiledProgram(Program program, const std::string& scriptPath) {
    m_callStack.clear();
    m_debugResumeState.reset();
    m_seekTargetPc.reset();
    m_seeking = false;
    m_safetyRejected = false;
    m_program = std::move(program);
    if (!m_program.errors.empty()) {
        m_state = VMState::Finished;
        return false;
    }
    m_scriptPath = scriptPath;
    m_pc = 0;
    m_pendingVoice.clear();
    m_textSpeed = m_config.defaultTextSpeed;
    Run();
    return true;
}

bool VM::RestartAtLabel(const std::string_view label) {
    const int target = m_program.LabelIndex(std::string(label));
    if (target < 0) return false;
    m_callStack.clear();
    m_debugResumeState.reset();
    m_seekTargetPc.reset();
    m_seeking = false;
    m_safetyRejected = false;
    m_pendingVoice.clear();
    m_choices.clear();
    m_pc = target;
    m_state = VMState::Idle;
    m_skipBacklogOnce = false;
    m_skipBreakOnce = false;
    m_stepping = false;
    m_stepBudget = 0;
    m_textSpeed = m_config.defaultTextSpeed;
    Run();
    return true;
}

ProgramSeekStatus VM::LoadCompiledProgramAt(Program program,
                                            const std::string& scriptPath,
                                            const int operationIndex) {
    if (!program.errors.empty()) return ProgramSeekStatus::InvalidProgram;
    if (operationIndex < 0 ||
        operationIndex >= static_cast<int>(program.code.size())) {
        return ProgramSeekStatus::InvalidOperation;
    }

    m_callStack.clear();
    m_debugResumeState.reset();
    m_program = std::move(program);
    m_scriptPath = scriptPath;
    m_pc = 0;
    m_pendingVoice.clear();
    m_choices.clear();
    m_textSpeed = m_config.defaultTextSpeed;
    m_state = VMState::Idle;
    m_skipBacklogOnce = false;
    m_skipBreakOnce = false;
    m_stepping = false;
    m_stepBudget = 0;
    m_safetyRejected = false;

    return ReplayCompiledProgramAt(operationIndex);
}

ProgramSeekStatus VM::ReplayCompiledProgramAt(
    const int operationIndex, const std::span<const int> branchPath,
    std::size_t* choicesConsumed) {
    if (operationIndex < 0 ||
        operationIndex >= static_cast<int>(m_program.code.size())) {
        return ProgramSeekStatus::InvalidOperation;
    }
    std::size_t consumedChoices = 0;
    if (choicesConsumed) *choicesConsumed = 0;
    if (m_state == VMState::WaitingVideo ||
        (m_state == VMState::WaitingExternal && !m_safetyRejected)) {
        return ProgramSeekStatus::UnsupportedBlockingState;
    }

    m_seekTargetPc = operationIndex;
    m_seeking = true;
    m_safetyRejected = false;

    std::set<int> retainedBreakpoints = std::move(m_breakpoints);
    m_breakpoints.clear();
    ProgramSeekStatus result = ProgramSeekStatus::Unreachable;
    const int budgetLimit =
        std::max(16, static_cast<int>(m_program.code.size()) * 4);

    Run();
    for (int budget = 0; budget < budgetLimit; ++budget) {
        if (m_state == VMState::Paused && m_pc == operationIndex) {
            m_seekTargetPc.reset();
            DebugStep();
            result = m_safetyRejected ? ProgramSeekStatus::UnsafeOperation
                                      : ProgramSeekStatus::Applied;
            break;
        }
        if (m_safetyRejected) {
            result = ProgramSeekStatus::UnsafeOperation;
            break;
        }
        if (m_state == VMState::WaitingClick) {
            m_dialogue.ShowAll();
            OnAdvance();
            continue;
        }
        if (m_state == VMState::WaitingTimer) {
            m_state = VMState::Idle;
            Run();
            continue;
        }
        if (m_state == VMState::WaitingChoice) {
            if (consumedChoices >= branchPath.size()) {
                result = ProgramSeekStatus::ChoicePathRequired;
                break;
            }
            const int choice = branchPath[consumedChoices];
            if (choice < 0 || choice >= static_cast<int>(m_choices.size())) {
                result = ProgramSeekStatus::ChoicePathRequired;
                break;
            }
            SelectChoice(choice);
            ++consumedChoices;
            if (choicesConsumed) *choicesConsumed = consumedChoices;
            continue;
        }
        if (m_state == VMState::WaitingVideo ||
            m_state == VMState::WaitingExternal) {
            result = ProgramSeekStatus::UnsupportedBlockingState;
            break;
        }
        if (m_state == VMState::Finished || m_state == VMState::Paused) break;
        Run();
    }

    m_breakpoints = std::move(retainedBreakpoints);
    m_seekTargetPc.reset();
    m_seeking = false;
    m_stepping = false;
    m_stepBudget = 0;
    return result;
}

ProgramPatchStatus VM::PatchCompiledProgram(Program program,
                                            const std::string& scriptPath) {
    if (!program.errors.empty()) return ProgramPatchStatus::InvalidProgram;
    if (scriptPath != m_scriptPath) return ProgramPatchStatus::ScriptMismatch;
    if (m_state == VMState::WaitingTimer ||
        m_state == VMState::WaitingVideo ||
        m_state == VMState::WaitingExternal) {
        return ProgramPatchStatus::UnsupportedState;
    }
    if (program.code.size() != m_program.code.size())
        return ProgramPatchStatus::StructuralChange;
    for (std::size_t index = 0; index < program.code.size(); ++index) {
        const Command& previous = m_program.code[index];
        const Command& next = program.code[index];
        if (previous.sourceId != next.sourceId || previous.type != next.type)
            return ProgramPatchStatus::StructuralChange;
    }

    const int activeIndex =
        m_state == VMState::WaitingClick && m_pc > 0 ? m_pc - 1 : m_pc;
    const std::string activeSource = CurrentSourceId();
    if (!activeSource.empty() &&
        (activeIndex < 0 ||
         activeIndex >= static_cast<int>(program.code.size()) ||
         program.code[static_cast<std::size_t>(activeIndex)].sourceId !=
             activeSource)) {
        return ProgramPatchStatus::MissingAnchor;
    }

    m_program = std::move(program);
    if (m_state == VMState::WaitingClick && activeIndex >= 0 &&
        activeIndex < static_cast<int>(m_program.code.size())) {
        const Command& active =
            m_program.code[static_cast<std::size_t>(activeIndex)];
        if (active.type == "say" ||
            (active.type == "text" &&
             (active.Has("value") || active.Has("text")))) {
            HandleSay(active, false);
        }
    } else if (m_state == VMState::WaitingChoice) {
        CollectChoices();
    }
    return ProgramPatchStatus::Applied;
}

void VM::SeekTo(const std::string& scriptPath, int pc) {
    m_callStack.clear();
    m_debugResumeState.reset();
    if (LoadProgram(scriptPath)) {
        m_pc = pc;
        m_state = VMState::Idle;
        // A save made mid-line points at the say itself; re-executing it
        // re-displays the text, but its backlog entry was restored from the
        // save, so the next push must be skipped.
        if (pc >= 0 && pc < static_cast<int>(m_program.code.size())) {
            const Command& c = m_program.code[pc];
            m_skipBacklogOnce =
                c.type == "say" || (c.type == "text" && (c.Has("value") || c.Has("text")));
        }
    }
}

VMRuntimeState VM::CaptureState() const {
    VMRuntimeState state;
    state.scriptPath=m_scriptPath;state.pc=m_pc;state.state=m_state;state.choices=m_choices;
    state.callStack.reserve(m_callStack.size());for(const auto& frame:m_callStack)state.callStack.push_back({frame.script,frame.pc});
    state.speaker=m_speaker;state.pendingVoice=m_pendingVoice;state.textColor=m_textColor;
    state.outlineColor=m_outlineColor;state.textSpeed=m_textSpeed;state.textEffect=m_textEffect;
    state.chapter=m_chapter;state.currentBgm=m_currentBgm;state.currentLineSeen=m_currentLineSeen;
    if(m_state==VMState::WaitingTimer){const std::uint64_t elapsed=m_nowMs>=m_timerStart?m_nowMs-m_timerStart:0;state.timerRemainingMs=elapsed>=m_timerMs?0:m_timerMs-elapsed;}
    return state;
}

bool VM::RestoreState(const VMRuntimeState& state,std::uint64_t nowMs) {
    if(state.scriptPath.empty()||!LoadProgram(state.scriptPath))return false;
    return RestoreCompiledState(state, std::move(m_program), nowMs);
}

bool VM::RestoreCompiledState(const VMRuntimeState& state, Program program,
                              std::uint64_t nowMs) {
    if (state.scriptPath.empty() || !program.errors.empty()) return false;
    m_program = std::move(program);
    if(state.pc<0||state.pc>static_cast<int>(m_program.code.size()))return false;
    m_scriptPath = state.scriptPath;
    m_pc=state.pc;m_state=state.state;m_choices=state.choices;m_callStack.clear();
    m_callStack.reserve(state.callStack.size());for(const auto& frame:state.callStack)m_callStack.push_back({frame.script,frame.pc});
    m_speaker=state.speaker;m_pendingVoice=state.pendingVoice;m_textColor=state.textColor;
    m_outlineColor=state.outlineColor;m_textSpeed=state.textSpeed;m_textEffect=state.textEffect;
    m_chapter=state.chapter;m_currentBgm=state.currentBgm;m_currentLineSeen=state.currentLineSeen;
    m_nowMs=nowMs;m_timerStart=nowMs;m_timerMs=state.timerRemainingMs;m_skipBacklogOnce=false;
    m_skipBreakOnce=false;m_stepping=false;m_stepBudget=0;m_debugResumeState.reset();
    m_seekTargetPc.reset();m_seeking=false;m_safetyRejected=false;return true;
}

bool VM::Blocking() const {
    return m_state == VMState::WaitingClick || m_state == VMState::WaitingChoice ||
           m_state == VMState::WaitingTimer || m_state == VMState::WaitingVideo ||
           m_state == VMState::WaitingExternal;
}

VMRunResult VM::Run() {
    return RunSlice(m_config.maxInstructionsPerTick);
}

VMRunResult VM::RunSlice(const std::size_t maxInstructions) {
    const auto& code = m_program.code;
    const std::size_t sliceBudget = std::max<std::size_t>(1, maxInstructions);
    std::size_t executed = 0;
    m_waitRequested = false;
    const auto finishSlice = [this, &executed](const VMRunStatus status,
                                               const bool observableYield) {
        if (observableYield) m_instructionsWithoutObservableYield = 0;
        m_lastRunResult = {status, executed};
        return m_lastRunResult;
    };
    while (m_pc >= 0 && m_pc < static_cast<int>(code.size())) {
        if (executed >= sliceBudget) {
            m_instructionsWithoutObservableYield += executed;
            if (m_instructionsWithoutObservableYield >=
                m_config.maxInstructionsWithoutYield) {
                const Command& stalled = code[static_cast<std::size_t>(m_pc)];
                diag::Diagnostic diagnostic{
                    .severity = diag::Severity::Fatal,
                    .code = "PXRUNTIME7311",
                    .category = "Runtime.VM",
                    .message = "Scenario exceeded the execution limit without yielding",
                    .details = "Add a dialogue, wait, choice, video, or asynchronous command to the loop."};
                ApplyDiagnosticSource(diagnostic, stalled);
                diag::Emit(std::move(diagnostic));
                m_state = VMState::Finished;
                return finishSlice(VMRunStatus::Faulted, true);
            }
            m_state = VMState::Running;
            return finishSlice(VMRunStatus::Yielded, false);
        }
        ++executed;
        const Command& cmd = code[m_pc];
        const std::string& t = cmd.type;

        if (m_seekTargetPc && m_pc == *m_seekTargetPc) {
            m_state = VMState::Paused;
            return finishSlice(VMRunStatus::AwaitingInput, true);
        }

        if (m_executionSafetyHook &&
            !m_executionSafetyHook(cmd, m_seeking)) {
            m_safetyRejected = true;
            m_state = VMState::WaitingExternal;
            return finishSlice(VMRunStatus::AwaitingInput, true);
        }

        // Debugger: pause after a single-step, or on a breakpoint line.
        if (m_stepping) {
            if (m_stepBudget <= 0) {
                m_stepping = false;
                m_state = VMState::Paused;
                return finishSlice(VMRunStatus::AwaitingInput, true);
            }
            --m_stepBudget;
        } else if (!m_breakpoints.empty() && m_breakpoints.count(cmd.line) != 0 &&
                   !m_skipBreakOnce) {
            m_state = VMState::Paused;
            return finishSlice(VMRunStatus::AwaitingInput, true);
        }
        m_skipBreakOnce = false;

        // "text" is the Node Editor's dialogue command. With inline text it is a
        // full say; bare it only sets the speaker for the following plain line.
        if (t == "say" || (t == "text" && (cmd.Has("value") || cmd.Has("text")))) {
            HandleSay(cmd, !m_seeking);
            ++m_pc;
            m_state = VMState::WaitingClick;
            return finishSlice(VMRunStatus::AwaitingInput, true);
        }
        if (t == "choice") {
            CollectChoices();
            m_state = VMState::WaitingChoice;
            return finishSlice(VMRunStatus::AwaitingInput, true);
        }
        if (t == "wait") {
            m_timerMs = static_cast<std::uint64_t>(ParseInt(cmd.Get("ms"), 800));
            m_timerStart = m_nowMs;
            ++m_pc;
            m_state = VMState::WaitingTimer;
            return finishSlice(VMRunStatus::AwaitingInput, true);
        }
        if (t == "camera") {
            Stage::TweenSpec spec;
            if (cmd.Has("x")) { spec.hasX = true; spec.x = ParseFloat(cmd.Get("x")); }
            if (cmd.Has("y")) { spec.hasY = true; spec.y = ParseFloat(cmd.Get("y")); }
            if (cmd.Has("zoom")) {
                spec.hasScale = true;
                spec.scale = ParseFloat(cmd.Get("zoom"), 1.0f);
            }
            spec.durationMs = ParseInt(cmd.Get("duration"), 600);
            spec.ease = cmd.Get("ease", "outCubic");
            if (!m_stage.Animate("$camera", spec))
                PX_LOG_WARN("VM: [camera] invalid camera target (line {})", cmd.line);
            ++m_pc;
            const std::string wait = cmd.Get("wait", "true");
            if (!wait.empty() && wait != "false" && wait != "0" && wait != "no" &&
                spec.durationMs > 0) {
                m_timerMs = static_cast<std::uint64_t>(spec.durationMs);
                m_timerStart = m_nowMs;
                m_state = VMState::WaitingTimer;
                return finishSlice(VMRunStatus::AwaitingInput, true);
            }
            continue;
        }
        if (t == "char_move" || t == "move") {
            const std::string target = cmd.Get("name", cmd.Get("id"));
            if (cmd.Has("slot") || cmd.Has("pos")) {
                m_stage.MoveCharacter(target,
                    ParseInt(cmd.Get("slot", cmd.Get("pos")), 2));
            }
            Stage::TweenSpec spec;
            if (cmd.Has("x")) { spec.hasX = true; spec.x = ParseFloat(cmd.Get("x")); }
            if (cmd.Has("y")) { spec.hasY = true; spec.y = ParseFloat(cmd.Get("y")); }
            if (cmd.Has("scale")) {
                spec.hasScale = true;
                spec.scale = ParseFloat(cmd.Get("scale"), 1.0f);
            }
            spec.durationMs =
                ParseInt(cmd.Get("duration", cmd.Get("time", cmd.Get("ms"))), 600);
            spec.ease = cmd.Get("ease", "outCubic");
            if ((spec.hasX || spec.hasY || spec.hasScale) &&
                !m_stage.Animate(target, spec)) {
                PX_LOG_WARN("VM: [move] target '{}' not found (line {})", target, cmd.line);
            }
            ++m_pc;
            const std::string wait = cmd.Get("wait", "false");
            if (!wait.empty() && wait != "false" && wait != "0" && wait != "no") {
                m_timerMs = static_cast<std::uint64_t>(std::max(0, spec.durationMs));
                m_timerStart = m_nowMs;
                m_state = VMState::WaitingTimer;
                return finishSlice(VMRunStatus::AwaitingInput, true);
            }
            continue;
        }
        if (t == "anim" || t == "tween") {
            Stage::TweenSpec spec;
            if (cmd.Has("x")) { spec.hasX = true; spec.x = ParseFloat(cmd.Get("x")); }
            if (cmd.Has("y")) { spec.hasY = true; spec.y = ParseFloat(cmd.Get("y")); }
            if (cmd.Has("scale") || cmd.Has("zoom")) {
                spec.hasScale = true;
                spec.scale = ParseFloat(cmd.Get("scale", cmd.Get("zoom")), 1.0f);
            }
            if (cmd.Has("alpha") || cmd.Has("opacity")) {
                spec.hasAlpha = true;
                spec.alpha = ParseFloat(cmd.Get("alpha", cmd.Get("opacity")), 255.0f);
            }
            spec.durationMs =
                ParseInt(cmd.Get("duration", cmd.Get("time", cmd.Get("ms"))), 600);
            spec.ease = cmd.Get("ease", "outCubic");
            const std::string target = cmd.Get("target", cmd.Get("name"));
            if (!m_stage.Animate(target, spec)) {
                PX_LOG_WARN("VM: [anim] target '{}' not found (line {})", target, cmd.line);
            }
            ++m_pc;
            const std::string w = cmd.Get("wait", "false");
            if (!w.empty() && w != "false" && w != "0" && w != "no") {
                m_timerMs = static_cast<std::uint64_t>(std::max(0, spec.durationMs));
                m_timerStart = m_nowMs;
                m_state = VMState::WaitingTimer;
                return finishSlice(VMRunStatus::AwaitingInput, true);
            }
            continue;
        }
        if (t == "video" || t == "movie") {
            const std::string file = cmd.Get("file", cmd.Get("value"));
            ++m_pc;
            if (m_videoHook && !file.empty()) {
                const std::string s = cmd.Get("skippable", "true");
                m_state = VMState::WaitingVideo;
                m_videoHook(Resolve(m_config.videoDir, file), s != "false" && s != "0");
                return finishSlice(VMRunStatus::AwaitingInput, true);
            }
            PX_LOG_WARN("VM: [video] ignored (no video host) at line {}", cmd.line);
            continue;
        }
        if (t == "branch") {
            const bool condition = EvaluateCondition(cmd);
            const std::string target = condition ? cmd.Get("trueTarget") : cmd.Get("falseTarget");
            if (!JumpToTarget(target)) ++m_pc;
            continue;
        }
        if (t == "label") {
            ++m_pc;
            continue;
        }
        if (t == "jump") {
            if (m_program.branch[m_pc] >= 0) {
                m_pc = m_program.branch[m_pc];
                continue;
            }
            if (!JumpToTarget(cmd.Get("target", cmd.Get("name")))) {
                ++m_pc;
            }
            continue;
        }
        if (t == "call") {
            m_callStack.push_back(CallFrame{ m_scriptPath, m_pc + 1 });
            if (m_program.branch[m_pc] >= 0) {
                m_pc = m_program.branch[m_pc];
            } else if (!JumpToTarget(cmd.Get("target", cmd.Get("name")))) {
                PX_LOG_WARN("VM: [call] target not found (line {})", cmd.line);
                m_callStack.pop_back();
                ++m_pc;
            }
            continue;
        }
        if (t == "return") {
            if (!m_callStack.empty()) {
                const CallFrame frame = m_callStack.back();
                m_callStack.pop_back();
                if (frame.script != m_scriptPath && !LoadProgram(frame.script)) {
                    m_state = VMState::Finished;
                    return finishSlice(VMRunStatus::Completed, true);
                }
                m_pc = frame.pc;
            } else {
                ++m_pc;
            }
            continue;
        }

        ExecuteSimple(cmd);
        ++m_pc;
        if (m_state == VMState::WaitingExternal || std::exchange(m_waitRequested, false))
            return finishSlice(VMRunStatus::AwaitingInput, true);
    }
    m_state = VMState::Finished;
    return finishSlice(VMRunStatus::Completed, true);
}

void VM::ExecuteSimple(const Command& cmd) {
    const std::string& t = cmd.type;

    if (t == "name" || t == "speaker" || t == "text") {
        const std::string speaker = cmd.Get("speaker", cmd.Get("name", cmd.Get("value")));
        if (!speaker.empty()) m_speaker = speaker;
        if (cmd.Has("color")) m_textColor = ParseColor(cmd.Get("color"), m_textColor);
        if (cmd.Has("outline")) m_outlineColor = ParseColor(cmd.Get("outline"), m_outlineColor);
        if (cmd.Has("speed")) m_textSpeed = ParseInt(cmd.Get("speed"), m_textSpeed);
        // Text headers describe the following dialogue block. An omitted
        // effect intentionally clears the preceding block's effect.
        m_textEffect = cmd.Get("effect");
        // A voice on the header belongs to the next spoken line.
        m_pendingVoice = cmd.Get("voice");
        return;
    }
    if (t == "bg") {
        const std::string file = Resolve(m_config.bgDir, cmd.Get("file", cmd.Get("value")));
        const int durationMs = ParseInt(cmd.Get("time", cmd.Get("ms")), 0);
        if (cmd.Has("rule")) {
            m_stage.SetBackgroundRule(file, Resolve(m_config.ruleDir, cmd.Get("rule")),
                                      durationMs > 0 ? durationMs : 600,
                                      ParseInt(cmd.Get("vague"), 64));
        } else {
            m_stage.SetBackground(file, TruthyFlag(cmd), durationMs);
        }
        // Framing applies to a new background at once and pans or zooms an
        // unchanged one over the same duration.
        if (const auto placement = ParsePlacement(cmd, m_stage.BackgroundPlacement()))
            m_stage.PlaceBackground(placement, ParseFromPlacement(cmd, placement),
                                    durationMs, cmd.Get("ease", "outCubic"));
        return;
    }
    if (t == "layer") {
        const std::string name = cmd.Get("name", cmd.Get("id"));
        const std::string file = cmd.Get("file", cmd.Get("value"));
        const int durationMs = ParseInt(cmd.Get("time", cmd.Get("ms")), 0);
        if (cmd.Has("clear") || file == "none") {
            m_stage.FadeOutLayer(name, durationMs);
            return;
        }
        const bool existed = m_stage.HasLayer(name);
        const float x = ParseFloat(cmd.Get("x"));
        const float y = ParseFloat(cmd.Get("y"));
        const float zoom = std::max(0.001f, ParseFloat(cmd.Get("zoom", cmd.Get("scale")), 1.0f));
        const int alpha = std::clamp(ParseInt(cmd.Get("alpha", cmd.Get("opacity")), 255), 0, 255);
        // A new layer with a duration fades in unless a start is authored.
        const int fromAlpha = std::clamp(
            ParseInt(cmd.Get("from_alpha"), !existed && durationMs > 0 ? 0 : alpha), 0, 255);
        const float fromX = cmd.Has("from_x") ? ParseFloat(cmd.Get("from_x")) : x;
        const float fromY = cmd.Has("from_y") ? ParseFloat(cmd.Get("from_y")) : y;
        const float fromZoom = cmd.Has("from_zoom")
            ? std::max(0.001f, ParseFloat(cmd.Get("from_zoom"), 1.0f)) : zoom;
        const bool animated = !m_fastForward && durationMs > 0 &&
            (fromX != x || fromY != y || fromZoom != zoom || fromAlpha != alpha);
        m_stage.SetLayerTransform(
            name, Resolve(m_config.bgDir, file), animated ? fromX : x,
            animated ? fromY : y, animated ? fromZoom : zoom,
            animated ? fromZoom : zoom, ParseFloat(cmd.Get("rotate")),
            static_cast<std::uint8_t>(animated ? fromAlpha : alpha),
            ParseInt(cmd.Get("z"), 0));
        const auto placement = ParsePlacement(cmd);
        m_stage.SetLayerAnchor(name, placement && placement->anchored,
                               placement ? placement->anchorX : 0.0f,
                               placement ? placement->anchorY : 0.0f);
        if (animated) {
            Stage::TweenSpec spec;
            spec.hasX = spec.hasY = spec.hasScale = spec.hasAlpha = true;
            spec.x = x;
            spec.y = y;
            spec.scale = zoom;
            spec.alpha = static_cast<float>(alpha);
            spec.durationMs = durationMs;
            spec.ease = cmd.Get("ease", "outCubic");
            (void)m_stage.Animate(name, spec);
        }
        return;
    }
    if (t == "layer_clear") {
        const std::string name = cmd.Get("name", cmd.Get("id"));
        const int durationMs = ParseInt(cmd.Get("time", cmd.Get("ms")), 0);
        if (name == "*" || name == "all") {
            for (const auto& layer : m_stage.LayerNames())
                m_stage.FadeOutLayer(layer, durationMs);
        } else {
            m_stage.FadeOutLayer(name, durationMs);
        }
        return;
    }
    if (t == "char") {
        const std::string name = cmd.Get("name", cmd.Get("id"));
        std::string file = cmd.Get("file");
        if (file.empty()) {
            const std::string expression = cmd.Get("diff", cmd.Get("expression", cmd.Get("exp")));
            if (const auto image = m_catalog.ResolveCharacterImage(name, expression)) {
                file = image->lastKnownPath;
            } else {
                const std::string fallbackExpression = expression.empty() ? "d" : expression;
                if (const auto* character = m_catalog.FindCharacter(name);
                    character && !character->expressions.empty()) {
                    diag::Diagnostic diagnostic{.severity=diag::Severity::Warning,
                        .code="PXVN6101",.category="VN.Character",
                        .message="Character expression was not found; using legacy filename fallback",
                        .details=name+" / "+fallbackExpression};
                    ApplyDiagnosticSource(diagnostic, cmd);
                    diag::Emit(std::move(diagnostic));
                }
                file = name + "_" + fallbackExpression + ".png";
            }
        }
        // A show without a position keeps a visible actor where it stands,
        // so an expression change does not snap it back to center. An
        // explicit position is a fresh placement with default offsets.
        const bool placed = cmd.Has("slot") || cmd.Has("pos");
        const auto current = placed ? std::nullopt : m_stage.FindCharacterPlacement(name);
        const Stage::CharacterPlacement keep = current.value_or(Stage::CharacterPlacement{});
        const int slot = placed ? ParseInt(cmd.Get("slot", cmd.Get("pos", "2")), 2) : keep.slot;
        const float ox = ParseFloat(cmd.Get("x"), keep.offsetX);
        const float oy = ParseFloat(cmd.Get("y"), keep.offsetY);
        const float scale = ParseFloat(cmd.Get("scale", cmd.Get("zoom")), keep.scale);
        const auto imagePath=Resolve(m_config.charDir, file);
        if(cmd.Get("transitionMode")=="auto")
            m_stage.SetCharacterAuto(name,imagePath,slot,ox,oy,scale);
        else
            m_stage.SetCharacter(name,imagePath,slot,TruthyFlag(cmd),ox,oy,scale);
        return;
    }
    if (t == "char_clear") {
        m_stage.ClearCharacter(cmd.Get("name", cmd.Get("id")), TruthyFlag(cmd));
        return;
    }
    if (t == "char_clear_all") {
        m_stage.ClearCharacters(TruthyFlag(cmd));
        return;
    }
    if (t == "bgm") {
        m_currentBgm = Resolve(m_config.bgmDir, cmd.Get("file", cmd.Get("value")));
        const int fade = ParseInt(cmd.Get("fade"), 0);
        if (cmd.Has("intro")) {
            // Intro plays once, then the body loops (saves store the body).
            m_audio.PlayBGMWithIntro(Resolve(m_config.bgmDir, cmd.Get("intro")), m_currentBgm,
                                     fade);
        } else {
            const std::string loop = cmd.Get("loop", "true");
            m_audio.PlayBGM(m_currentBgm, loop != "false" && loop != "0", fade);
        }
        return;
    }
    if (t == "stopbgm") {
        m_currentBgm.clear();
        m_audio.StopBGM(ParseInt(cmd.Get("fade"), 0));
        return;
    }
    if (t == "se") {
        if (!m_fastForward)
            m_audio.PlaySE(Resolve(m_config.seDir, cmd.Get("file", cmd.Get("value"))));
        return;
    }
    if (t == "stopse") {
        m_audio.StopSE(ParseInt(cmd.Get("fade"), 0));
        return;
    }
    if (t == "stopvoice") {
        m_audio.StopVoice();
        return;
    }
    if (t == "voice") {
        if (!m_fastForward)
            m_audio.PlayVoice(Resolve(m_config.voiceDir, cmd.Get("file", cmd.Get("value"))));
        return;
    }
    if (t == "var") {
        const std::string name = cmd.Get("name");
        VariableScope scope = VariableScope::Session;
        if (const auto* encodedScope = cmd.FindTyped("scope"); encodedScope) {
            if (const auto* value = encodedScope->TryGet<std::string>();
                value && *value == "profile")
                scope = VariableScope::Profile;
        }
        if (cmd.FindTyped("add")) {
            int delta = 0;
            if (const Variant* typed = cmd.FindTyped("add")) {
                if (const auto* integer = typed->TryGet<std::int64_t>()) {
                    delta = static_cast<int>(*integer);
                } else if (const auto* number = typed->TryGet<double>()) {
                    delta = static_cast<int>(*number);
                }
            }
            m_vars.Add(name, delta, scope);
        } else if (const Variant* value = cmd.FindTyped("value")) {
            m_vars.SetValue(name, value->Clone(), scope);
        }
        return;
    }
    if (t == "chapter") {
        m_chapter = cmd.Get("title", cmd.Get("value"));
        return;
    }
    if (t == "cg") {
        const std::string id = cmd.Get("id", cmd.Get("image"));
        const std::string image = cmd.Get("image", cmd.Get("file"));
        if (!image.empty()) {
            m_stage.SetBackground(Resolve(m_config.cgDir, image), TruthyFlag(cmd));
        }
        if (m_unlockHook && !id.empty()) m_unlockHook("cg", id);
        return;
    }
    if (t == "unlock") {
        if (m_unlockHook) m_unlockHook(cmd.Get("kind", "scene"), cmd.Get("id"));
        return;
    }
    if (t == "speed") {
        m_textSpeed = ParseInt(cmd.Get("value"), m_config.defaultTextSpeed);
        return;
    }
    if (t == "shake" || t == "quake") {
        // The message window belongs to the host UI; hosts without one
        // ignore a window shake instead of shaking the Stage.
        if (cmd.Get("target") == "window") {
            if (m_commandHook) (void)m_commandHook(cmd);
            return;
        }
        const int ms = ParseInt(cmd.Get("ms", cmd.Get("time")), 400);
        const int amp = ParseInt(cmd.Get("amp", cmd.Get("power")), 12);
        m_stage.Shake(ms, static_cast<float>(amp));
        return;
    }
    if (t == "actor_action") {
        ActorActionRequest request;
        request.preset = cmd.Get("preset");
        request.size = ParseFloat(cmd.Get("size"), 0.0f);
        request.timeMs = ParseInt(cmd.Get("time"), 0);
        request.loop = ParseInt(cmd.Get("loop"), 1);
        request.ease = cmd.Get("ease");
        const std::string name = cmd.Get("id", cmd.Get("name"));
        if (!m_stage.PlayActorAction(name, request)) {
            diag::Diagnostic diagnostic{.severity=diag::Severity::Warning,
                .code="PXVN6110",.category="VN.Character",
                .message="Character action needs a visible character and a known preset",
                .details=name+" / "+request.preset};
            ApplyDiagnosticSource(diagnostic, cmd);
            diag::Emit(std::move(diagnostic));
        }
        return;
    }

    if (m_commandHook && m_commandHook(cmd)) {
        return;
    }
    PX_LOG_WARN("VM: unhandled command '{}' (line {})", t, cmd.line);
}

void VM::HandleSay(const Command& cmd, const bool recordPlayback) {
    if (m_seenHook) {
        const std::string source = cmd.sourceId.empty()
                                       ? std::to_string(cmd.line)
                                       : cmd.sourceId;
        const std::string operation = cmd.operationId.empty()
                                          ? source
                                          : cmd.operationId;
        m_currentLineSeen = m_seenHook(
            m_program.documentId + ":" + source + ":" + operation);
    }
    const std::string character = cmd.Get("char", cmd.Get("character"));
    std::string speaker = cmd.Has("speaker") ? cmd.Get("speaker") : m_speaker;
    if (!cmd.Has("speaker") && !character.empty()) {
        speaker = m_catalog.CharacterDisplayName(character, m_characterLocale);
    }
    if (cmd.Has("speaker") || !character.empty()) m_speaker = speaker;
    if (cmd.Has("color")) m_textColor = ParseColor(cmd.Get("color"), m_textColor);

    const std::string text = FilterText(m_vars.Substitute(cmd.Get("value", cmd.Get("text"))),cmd.Get("textId"));
    std::string voice = cmd.Get("voice");
    if (voice.empty()) {
        voice = m_pendingVoice;  // from the preceding [text voice=...] header
    }
    m_pendingVoice.clear();
    if (voice.empty() && !m_voiceDirs.empty()) {
        // Auto-voice convention: <voiceDir><scriptStem>_<line>.{ogg,wav,mp3}
        auto it = m_voiceDirs.find(character.empty() ? speaker : character);
        if (it == m_voiceDirs.end()) it = m_voiceDirs.find(speaker);
        if (it != m_voiceDirs.end() && !it->second.empty()) {
            std::string stem = m_scriptPath;
            if (const std::size_t slash = stem.find_last_of("/\\"); slash != std::string::npos) {
                stem = stem.substr(slash + 1);
            }
            if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos) {
                stem = stem.substr(0, dot);
            }
            std::string dir = it->second;
            if (!dir.empty() && dir.back() != '/') dir += '/';
            const std::string base = dir + stem + "_" + std::to_string(cmd.line);
            for (const char* ext : { ".ogg", ".wav", ".mp3" }) {
                if (m_vfs.Exists(base + ext)) {
                    voice = base + ext;
                    break;
                }
            }
        }
    }
    const int speed = cmd.Has("speed") ? ParseInt(cmd.Get("speed"), m_textSpeed) : m_textSpeed;

    m_dialogue.SetText(speaker, text, speed, m_textColor, m_outlineColor, voice,
                       cmd.Get("effect", m_textEffect));
    if (recordPlayback && !m_skipBacklogOnce) {
        // Use the dialogue's cleaned text: inline tags like {w=300} are stripped.
        m_backlog.Push(speaker, m_dialogue.State().fullText, voice, false,
                       cmd.sourceId, cmd.operationId);
    }
    m_skipBacklogOnce = false;
    if (recordPlayback && !voice.empty()) {
        if (m_fastForward) m_audio.StopVoice();
        else m_audio.PlayVoice(Resolve(m_config.voiceDir, voice));
    }
}

Status VM::PresentScriptSay(const Command& command) {
    if (command.type != "say" || command.sourceId.empty() ||
        command.operationId.empty() || m_program.documentId.empty() ||
        (!command.Has("text") && !command.Has("value")) || m_seeking ||
        (m_state != VMState::Running && m_state != VMState::WaitingExternal &&
         m_state != VMState::WaitingClick)) {
        diag::Diagnostic diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXVM7602",
            .category = "VN.Runtime",
            .message = "Script say requires a loaded Story, stable identities and text"};
        ApplyDiagnosticSource(diagnostic, command);
        diag::Emit(diagnostic);
        return Status::Fail(std::move(diagnostic));
    }
    HandleSay(command);
    return Status::Ok();
}

void VM::CollectChoices() {
    m_choices.clear();
    const auto& code = m_program.code;
    int i = m_pc;
    while (i < static_cast<int>(code.size())) {
        if (code[i].type == "choice") {
            m_choices.push_back(
                Choice{ FilterText(m_vars.Substitute(code[i].Get("text", code[i].Get("value"))),code[i].Get("textId")),
                        code[i].Get("target"), code[i].sourceId,
                        code[i].operationId });
            ++i;
            continue;
        }
        // Scenario IR emits a stable statement label before every command.
        // Labels between linked Choice nodes are metadata, not a block break.
        if (!m_choices.empty() && code[i].type == "label" &&
            i + 1 < static_cast<int>(code.size()) && code[i + 1].type == "choice") {
            ++i;
            continue;
        }
        break;
    }
}

bool VM::EvaluateCondition(const Command& cmd) const {
    if (const Variant* encoded = cmd.FindTyped("expression")) {
        const auto expression = ExpressionFromValue(*encoded, m_scriptPath);
        if (!expression) {
            for (const auto& diagnostic : expression.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
        const auto result = m_vars.Evaluate(expression.Value());
        if (!result) {
            for (const auto& diagnostic : result.Diagnostics()) diag::Emit(diagnostic);
            return false;
        }
        if (const bool* boolean = result.Value().TryGet<bool>()) return *boolean;
        diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXVM7601",
                                    .category="VN.Runtime",
                                    .message="If expression did not evaluate to bool"};
        ApplyDiagnosticSource(diagnostic, cmd);
        diag::Emit(std::move(diagnostic));return false;
    }
    return false;
}

bool VM::JumpToTarget(const std::string& target) {
    if (target.empty()) {
        return false;
    }
    const int idx = m_program.LabelIndex(target);
    if (idx >= 0) {
        m_pc = idx;
        return true;
    }
    const std::size_t fragment = target.find('#');
    const std::string script = fragment == std::string::npos ? target : target.substr(0, fragment);
    const std::string entry = fragment == std::string::npos ? std::string{} : target.substr(fragment + 1);
    if (LoadProgram(script)) {
        m_pc = entry.empty() ? 0 : m_program.LabelIndex(entry);
        if (m_pc < 0) {
            PX_LOG_ERROR("VM: scenario entry '{}' not found in '{}'", entry, script);
            return false;
        }
        return true;
    }
    return false;
}

void VM::SelectChoice(int index) {
    if (m_state != VMState::WaitingChoice || index < 0 ||
        index >= static_cast<int>(m_choices.size())) {
        return;
    }
    const Choice chosen = m_choices[index];
    if (m_choiceSeenHook) {
        const std::string source = chosen.sourceId.empty()
                                       ? std::to_string(index)
                                       : chosen.sourceId;
        const std::string operation = chosen.operationId.empty()
                                          ? source
                                          : chosen.operationId;
        m_choiceSeenHook(m_program.documentId + ":" + source + ":" + operation);
    }
    m_backlog.Push("", chosen.text, "", /*isChoice=*/true, chosen.sourceId,
                   chosen.operationId);
    m_choices.clear();
    // Step past the whole consecutive choice block first; a choice without a
    // target then continues after the block instead of re-prompting the rest.
    const auto& code = m_program.code;
    while (m_pc < static_cast<int>(code.size())) {
        if (code[m_pc].type == "choice") { ++m_pc; continue; }
        if (code[m_pc].type == "label" && m_pc + 1 < static_cast<int>(code.size()) &&
            code[m_pc + 1].type == "choice") { ++m_pc; continue; }
        break;
    }
    JumpToTarget(chosen.target);
    Run();
}

void VM::Resume() {
    if (m_state == VMState::Idle) {
        Run();
    }
}

void VM::NotifyVideoDone() {
    if (m_state == VMState::WaitingVideo) {
        Run();
    }
}

void VM::NotifyExternalDone() {
    if (m_state == VMState::WaitingExternal) {
        m_state = VMState::Idle;
        Run();
    }
}

void VM::ApplyDiagnosticSource(diag::Diagnostic& diagnostic,
                               const Command& command) const {
    diagnostic.source.path = m_scriptPath;
    diagnostic.source.resourceId = command.sourceId;
    diagnostic.source.nodeId = command.sourceId;
    diagnostic.source.line = command.line;
    diagnostic.operationId = command.operationId;
    diagnostic.documentId = m_program.documentId;
    diagnostic.sourceId = command.sourceId;
    if (m_diagnosticSourceResolver) {
        m_diagnosticSourceResolver(diagnostic, command);
    }
}

bool VM::DebugPause() {
    if (m_state == VMState::Paused || m_state == VMState::Finished) {
        return false;
    }
    m_debugResumeState = m_state;
    m_state = VMState::Paused;
    return true;
}

void VM::DebugContinue() {
    if (m_state != VMState::Paused) {
        return;
    }
    if (m_debugResumeState) {
        const VMState resumeState = *m_debugResumeState;
        m_debugResumeState.reset();
        m_state = resumeState;
        if (resumeState == VMState::Idle || resumeState == VMState::Running) {
            Run();
        }
        return;
    }
    m_skipBreakOnce = true;
    m_stepping = false;
    m_stepBudget = 0;
    Run();
}

void VM::DebugStep() {
    if (m_state != VMState::Paused || m_debugResumeState) {
        return;
    }
    m_skipBreakOnce = true;
    m_stepping = true;
    m_stepBudget = 1;
    Run();
    if (m_state != VMState::Paused) {
        // The stepped command entered a waiting state (say/choice/wait);
        // don't pause again on the user's next advance.
        m_stepping = false;
        m_stepBudget = 0;
    }
}

void VM::SetFastForward(const bool enabled) {
    m_fastForward = enabled;
    m_stage.SetFastForward(enabled);
}

void VM::OnAdvance() {
    if (m_state == VMState::WaitingClick) {
        if (!m_dialogue.Finished()) {
            m_dialogue.ShowAll();
        } else {
            Run();
        }
    } else if (m_state == VMState::WaitingTimer) {
        Run();
    }
}

void VM::Update(std::uint64_t nowMs, float dt) {
    m_nowMs = nowMs;
    m_stage.Update(dt);
    if (m_state == VMState::Running) {
        Run();
    } else if (m_state == VMState::WaitingClick ||
               m_state == VMState::WaitingExternal) {
        m_dialogue.Update(nowMs);
    } else if (m_state == VMState::WaitingTimer) {
        if (nowMs - m_timerStart >= m_timerMs) {
            Run();
        }
    }
}

}
