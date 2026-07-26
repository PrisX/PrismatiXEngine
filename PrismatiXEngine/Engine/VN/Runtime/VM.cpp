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

void VM::SeekTo(const std::string& scriptPath, int pc) {
    m_callStack.clear();
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
    if(state.pc<0||state.pc>static_cast<int>(m_program.code.size()))return false;
    m_pc=state.pc;m_state=state.state;m_choices=state.choices;m_callStack.clear();
    m_callStack.reserve(state.callStack.size());for(const auto& frame:state.callStack)m_callStack.push_back({frame.script,frame.pc});
    m_speaker=state.speaker;m_pendingVoice=state.pendingVoice;m_textColor=state.textColor;
    m_outlineColor=state.outlineColor;m_textSpeed=state.textSpeed;m_textEffect=state.textEffect;
    m_chapter=state.chapter;m_currentBgm=state.currentBgm;m_currentLineSeen=state.currentLineSeen;
    m_nowMs=nowMs;m_timerStart=nowMs;m_timerMs=state.timerRemainingMs;m_skipBacklogOnce=false;
    m_skipBreakOnce=false;m_stepping=false;m_stepBudget=0;return true;
}

bool VM::Blocking() const {
    return m_state == VMState::WaitingClick || m_state == VMState::WaitingChoice ||
           m_state == VMState::WaitingTimer || m_state == VMState::WaitingVideo ||
           m_state == VMState::WaitingExternal;
}

void VM::Run() {
    const auto& code = m_program.code;
    while (m_pc >= 0 && m_pc < static_cast<int>(code.size())) {
        const Command& cmd = code[m_pc];
        const std::string& t = cmd.type;

        // Debugger: pause after a single-step, or on a breakpoint line.
        if (m_stepping) {
            if (m_stepBudget <= 0) {
                m_stepping = false;
                m_state = VMState::Paused;
                return;
            }
            --m_stepBudget;
        } else if (!m_breakpoints.empty() && m_breakpoints.count(cmd.line) != 0 &&
                   !m_skipBreakOnce) {
            m_state = VMState::Paused;
            return;
        }
        m_skipBreakOnce = false;

        // "text" is the Node Editor's dialogue command. With inline text it is a
        // full say; bare it only sets the speaker for the following plain line.
        if (t == "say" || (t == "text" && (cmd.Has("value") || cmd.Has("text")))) {
            HandleSay(cmd);
            ++m_pc;
            m_state = VMState::WaitingClick;
            return;
        }
        if (t == "choice") {
            CollectChoices();
            m_state = VMState::WaitingChoice;
            return;
        }
        if (t == "wait") {
            m_timerMs = static_cast<std::uint64_t>(ParseInt(cmd.Get("ms"), 800));
            m_timerStart = m_nowMs;
            ++m_pc;
            m_state = VMState::WaitingTimer;
            return;
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
                return;
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
                return;
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
                    return;
                }
                m_pc = frame.pc;
            } else {
                ++m_pc;
            }
            continue;
        }

        ExecuteSimple(cmd);
        ++m_pc;
        if (m_state == VMState::WaitingExternal) return;
    }
    m_state = VMState::Finished;
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
        if (cmd.Has("rule")) {
            m_stage.SetBackgroundRule(file, Resolve(m_config.ruleDir, cmd.Get("rule")),
                                      ParseInt(cmd.Get("time", cmd.Get("ms")), 600),
                                      ParseInt(cmd.Get("vague"), 64));
        } else {
            m_stage.SetBackground(file, TruthyFlag(cmd));
        }
        return;
    }
    if (t == "layer") {
        const std::string name = cmd.Get("name", cmd.Get("id"));
        const std::string file = cmd.Get("file", cmd.Get("value"));
        if (cmd.Has("clear") || file == "none") {
            m_stage.ClearLayer(name);
            return;
        }
        const int alpha = ParseInt(cmd.Get("alpha", cmd.Get("opacity")), 255);
        m_stage.SetLayer(name, Resolve(m_config.bgDir, file), ParseFloat(cmd.Get("x")),
                         ParseFloat(cmd.Get("y")), ParseFloat(cmd.Get("scale"), 1.0f),
                         static_cast<std::uint8_t>(std::clamp(alpha, 0, 255)),
                         ParseInt(cmd.Get("z"), 0));
        return;
    }
    if (t == "layer_clear") {
        m_stage.ClearLayer(cmd.Get("name", cmd.Get("id")));
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
                    diagnostic.source.path=m_scriptPath;diagnostic.source.line=cmd.line;
                    diag::Emit(std::move(diagnostic));
                }
                file = name + "_" + fallbackExpression + ".png";
            }
        }
        const int slot = ParseInt(cmd.Get("slot", cmd.Get("pos", "2")), 2);
        const float ox = ParseFloat(cmd.Get("x"), 0.0f);
        const float oy = ParseFloat(cmd.Get("y"), 0.0f);
        const float scale = ParseFloat(cmd.Get("scale", cmd.Get("zoom")), 1.0f);
        m_stage.SetCharacter(name, Resolve(m_config.charDir, file), slot, TruthyFlag(cmd), ox, oy,
                             scale);
        return;
    }
    if (t == "char_clear") {
        m_stage.ClearCharacter(cmd.Get("name", cmd.Get("id")), TruthyFlag(cmd));
        return;
    }
    if (t == "char_move" || t == "move") {
        const int slot = ParseInt(cmd.Get("slot", cmd.Get("pos", "2")), 2);
        m_stage.MoveCharacter(cmd.Get("name", cmd.Get("id")), slot);
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
            m_audio.PlayBGM(m_currentBgm, true, fade);
        }
        return;
    }
    if (t == "stopbgm") {
        m_currentBgm.clear();
        m_audio.StopBGM(ParseInt(cmd.Get("fade"), 0));
        return;
    }
    if (t == "se") {
        m_audio.PlaySE(Resolve(m_config.seDir, cmd.Get("file", cmd.Get("value"))));
        return;
    }
    if (t == "voice") {
        m_audio.PlayVoice(Resolve(m_config.voiceDir, cmd.Get("file", cmd.Get("value"))));
        return;
    }
    if (t == "var") {
        const std::string name = cmd.Get("name");
        const bool typedPersistent = cmd.FindTyped("persistent") &&
            cmd.FindTyped("persistent")->TryGet<bool>() &&
            *cmd.FindTyped("persistent")->TryGet<bool>();
        const bool persistent = typedPersistent;
        if (cmd.FindTyped("add")) {
            int delta = 0;
            if (const Variant* typed = cmd.FindTyped("add")) {
                if (const auto* integer = typed->TryGet<std::int64_t>()) {
                    delta = static_cast<int>(*integer);
                } else if (const auto* number = typed->TryGet<double>()) {
                    delta = static_cast<int>(*number);
                }
            }
            m_vars.Add(name, delta, persistent);
        } else if (const Variant* value = cmd.FindTyped("value")) {
            m_vars.SetValue(name, value->Clone(), persistent ? VariableScope::Persistent
                                                             : VariableScope::SaveLocal);
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
        const int ms = ParseInt(cmd.Get("ms", cmd.Get("time")), 400);
        const int amp = ParseInt(cmd.Get("amp", cmd.Get("power")), 12);
        m_stage.Shake(ms, static_cast<float>(amp));
        return;
    }

    if (m_commandHook && m_commandHook(cmd)) {
        return;
    }
    PX_LOG_WARN("VM: unhandled command '{}' (line {})", t, cmd.line);
}

void VM::HandleSay(const Command& cmd) {
    if (m_seenHook) {
        m_currentLineSeen = m_seenHook(m_scriptPath + ":" + std::to_string(cmd.line));
    }
    const std::string character = cmd.Get("char", cmd.Get("character"));
    std::string speaker = cmd.Has("speaker") ? cmd.Get("speaker") : m_speaker;
    if (!cmd.Has("speaker") && !character.empty()) {
        speaker = m_catalog.CharacterDisplayName(character);
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
    if (!m_skipBacklogOnce) {
        // Use the dialogue's cleaned text: inline tags like {w=300} are stripped.
        m_backlog.Push(speaker, m_dialogue.State().fullText, voice);
    }
    m_skipBacklogOnce = false;
    if (!voice.empty()) {
        m_audio.PlayVoice(Resolve(m_config.voiceDir, voice));
    }
}

void VM::CollectChoices() {
    m_choices.clear();
    const auto& code = m_program.code;
    int i = m_pc;
    while (i < static_cast<int>(code.size())) {
        if (code[i].type == "choice") {
            m_choices.push_back(
                Choice{ FilterText(m_vars.Substitute(code[i].Get("text", code[i].Get("value"))),code[i].Get("textId")),
                        code[i].Get("target") });
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
        diagnostic.source.path=m_scriptPath;diag::Emit(std::move(diagnostic));return false;
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
    m_backlog.Push("", chosen.text, "", /*isChoice=*/true);
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

void VM::DebugContinue() {
    if (m_state != VMState::Paused) {
        return;
    }
    m_skipBreakOnce = true;
    m_stepping = false;
    m_stepBudget = 0;
    Run();
}

void VM::DebugStep() {
    if (m_state != VMState::Paused) {
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
    if (m_state == VMState::WaitingClick) {
        m_dialogue.Update(nowMs);
    } else if (m_state == VMState::WaitingTimer) {
        if (nowMs - m_timerStart >= m_timerMs) {
            Run();
        }
    }
}

}
