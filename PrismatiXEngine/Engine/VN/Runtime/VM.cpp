#include "Engine/VN/Runtime/VM.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/IO/VFS.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/PDS/Parser.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/Support/Logger.h"

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
    ParsedScript parsed = ParsePDS(*text);
    m_program = Compile(parsed);
    for (const std::string& err : m_program.errors) {
        PX_LOG_WARN("VM compile ({}) : {}", path, err);
    }
    m_scriptPath = scriptPath;
    m_pc = 0;
    m_callStack.clear();
    return true;
}

bool VM::LoadScript(const std::string& scriptPath) {
    if (!LoadProgram(scriptPath)) {
        m_state = VMState::Finished;
        return false;
    }
    m_textSpeed = m_config.defaultTextSpeed;
    Run();
    return true;
}

void VM::SeekTo(const std::string& scriptPath, int pc) {
    if (LoadProgram(scriptPath)) {
        m_pc = pc;
        m_state = VMState::Idle;
    }
}

bool VM::Blocking() const {
    return m_state == VMState::WaitingClick || m_state == VMState::WaitingChoice ||
           m_state == VMState::WaitingTimer;
}

void VM::Run() {
    const auto& code = m_program.code;
    while (m_pc >= 0 && m_pc < static_cast<int>(code.size())) {
        const Command& cmd = code[m_pc];
        const std::string& t = cmd.type;

        if (t == "say") {
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
        if (t == "if") {
            const bool ok = EvaluateCondition(cmd);
            m_pc = ok ? m_pc + 1 : (m_program.branch[m_pc] >= 0 ? m_program.branch[m_pc] : m_pc + 1);
            continue;
        }
        if (t == "else") {
            m_pc = m_program.branch[m_pc] >= 0 ? m_program.branch[m_pc] : m_pc + 1;
            continue;
        }
        if (t == "endif" || t == "label") {
            ++m_pc;
            continue;
        }
        if (t == "jump") {
            if (m_program.branch[m_pc] >= 0) {
                m_pc = m_program.branch[m_pc];
                continue;
            }
            JumpToTarget(cmd.Get("target", cmd.Get("name")));
            continue;
        }
        if (t == "call") {
            m_callStack.push_back(m_pc + 1);
            if (m_program.branch[m_pc] >= 0) {
                m_pc = m_program.branch[m_pc];
            } else {
                ++m_pc;
            }
            continue;
        }
        if (t == "return") {
            if (!m_callStack.empty()) {
                m_pc = m_callStack.back();
                m_callStack.pop_back();
            } else {
                ++m_pc;
            }
            continue;
        }

        ExecuteSimple(cmd);
        ++m_pc;
    }
    m_state = VMState::Finished;
}

void VM::ExecuteSimple(const Command& cmd) {
    const std::string& t = cmd.type;

    if (t == "name" || t == "speaker") {
        m_speaker = cmd.Get("speaker", cmd.Get("name", cmd.Get("value")));
        if (cmd.Has("color")) m_textColor = ParseColor(cmd.Get("color"), m_textColor);
        return;
    }
    if (t == "bg") {
        m_stage.SetBackground(Resolve(m_config.bgDir, cmd.Get("file", cmd.Get("value"))),
                              TruthyFlag(cmd));
        return;
    }
    if (t == "char") {
        const std::string name = cmd.Get("name", cmd.Get("id"));
        std::string file = cmd.Get("file");
        if (file.empty()) {
            const std::string diff = cmd.Get("diff", cmd.Get("expression", cmd.Get("exp", "d")));
            file = name + "_" + diff + ".png";
        }
        const int slot = ParseInt(cmd.Get("slot", cmd.Get("pos", "2")), 2);
        m_stage.SetCharacter(name, Resolve(m_config.charDir, file), slot, TruthyFlag(cmd));
        return;
    }
    if (t == "char_clear") {
        m_stage.ClearCharacter(cmd.Get("name", cmd.Get("id")), TruthyFlag(cmd));
        return;
    }
    if (t == "bgm") {
        m_currentBgm = Resolve(m_config.bgmDir, cmd.Get("file", cmd.Get("value")));
        m_audio.PlayBGM(m_currentBgm, true, ParseInt(cmd.Get("fade"), 0));
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
        const bool persistent = TruthyFlag(cmd) && cmd.Has("persistent");
        if (cmd.Has("add")) {
            m_vars.Add(name, ParseInt(cmd.Get("add")), persistent);
        } else {
            m_vars.Set(name, ParseInt(cmd.Get("value")), persistent);
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

    if (m_commandHook && m_commandHook(cmd)) {
        return;
    }
    PX_LOG_WARN("VM: unhandled command '{}' (line {})", t, cmd.line);
}

void VM::HandleSay(const Command& cmd) {
    std::string speaker = cmd.Has("speaker") ? cmd.Get("speaker") : m_speaker;
    if (cmd.Has("speaker")) m_speaker = speaker;
    if (cmd.Has("color")) m_textColor = ParseColor(cmd.Get("color"), m_textColor);

    const std::string text = m_vars.Substitute(cmd.Get("value", cmd.Get("text")));
    const std::string voice = cmd.Get("voice");
    const int speed = cmd.Has("speed") ? ParseInt(cmd.Get("speed"), m_textSpeed) : m_textSpeed;

    m_dialogue.SetText(speaker, text, speed, m_textColor, m_outlineColor, voice, cmd.Get("effect"));
    m_backlog.Push(speaker, text, voice);
    if (!voice.empty()) {
        m_audio.PlayVoice(Resolve(m_config.voiceDir, voice));
    }
}

void VM::CollectChoices() {
    m_choices.clear();
    const auto& code = m_program.code;
    int i = m_pc;
    while (i < static_cast<int>(code.size()) && code[i].type == "choice") {
        m_choices.push_back(Choice{ m_vars.Substitute(code[i].Get("text", code[i].Get("value"))),
                                    code[i].Get("target") });
        ++i;
    }
}

bool VM::EvaluateCondition(const Command& cmd) const {
    const std::string name = cmd.Get("var", cmd.Get("flag"));
    const std::string op = cmd.Get("op");
    const int rhs = ParseInt(cmd.Get("value"), 0);
    return m_vars.Evaluate(name, op, rhs);
}

void VM::JumpToTarget(const std::string& target) {
    if (target.empty()) {
        ++m_pc;
        return;
    }
    const int idx = m_program.LabelIndex(target);
    if (idx >= 0) {
        m_pc = idx;
        return;
    }
    if (LoadProgram(target)) {
        m_pc = 0;
    } else {
        ++m_pc;
    }
}

void VM::SelectChoice(int index) {
    if (m_state != VMState::WaitingChoice || index < 0 ||
        index >= static_cast<int>(m_choices.size())) {
        return;
    }
    const Choice chosen = m_choices[index];
    m_backlog.Push("", chosen.text, "", /*isChoice=*/true);
    m_choices.clear();
    JumpToTarget(chosen.target);
    Run();
}

void VM::Resume() {
    if (m_state == VMState::Idle) {
        Run();
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
