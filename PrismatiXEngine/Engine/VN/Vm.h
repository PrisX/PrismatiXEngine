#pragma once

#include "Engine/Common/Types.h"
#include "Engine/VN/Compiler.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace px::io {
class Vfs;
}
namespace px::audio {
class AudioEngine;
}

namespace px::vn {

class Stage;
class Dialogue;
class VariableStore;
class Backlog;

enum class VmState {
    Idle,
    Running,
    WaitingClick,
    WaitingChoice,
    WaitingTimer,
    Finished,
};

struct Choice {
    std::string text;
    std::string target;
};

struct VmConfig {
    std::string bgDir = "Data/Image/Background/";
    std::string charDir = "Data/Image/Character/";
    std::string cgDir = "Data/Image/CG/";
    std::string bgmDir = "Data/Audio/Music/";
    std::string seDir = "Data/Audio/SFX/";
    std::string voiceDir = "Data/Audio/Voice/";
    std::string scriptDir = "Data/Script/";
    int defaultTextSpeed = 28;
};

class Vm {
public:
    Vm(io::Vfs& vfs, audio::AudioEngine& audio, Stage& stage, Dialogue& dialogue,
       VariableStore& vars, Backlog& backlog);

    void SetConfig(const VmConfig& config) { m_config = config; }
    void SetCommandHook(std::function<bool(const Command&)> hook) { m_commandHook = std::move(hook); }
    void SetUnlockHook(std::function<void(const std::string& kind, const std::string& id)> hook) {
        m_unlockHook = std::move(hook);
    }

    bool LoadScript(const std::string& scriptPath);
    void Update(std::uint64_t nowMs, float dt);
    void OnAdvance();
    void SelectChoice(int index);
    void Resume();

    [[nodiscard]] VmState State() const { return m_state; }
    [[nodiscard]] bool Blocking() const;
    [[nodiscard]] const std::vector<Choice>& Choices() const { return m_choices; }
    [[nodiscard]] const std::string& Chapter() const { return m_chapter; }
    [[nodiscard]] const std::string& CurrentBgm() const { return m_currentBgm; }
    [[nodiscard]] const std::string& CurrentScript() const { return m_scriptPath; }
    [[nodiscard]] const Program& CurrentProgram() const { return m_program; }
    [[nodiscard]] int ProgramCounter() const { return m_pc; }

    void SeekTo(const std::string& scriptPath, int pc);

private:
    bool LoadProgram(const std::string& scriptPath);
    void Run();
    void ExecuteSimple(const Command& cmd);
    void HandleSay(const Command& cmd);
    void CollectChoices();
    bool EvaluateCondition(const Command& cmd) const;
    void JumpToTarget(const std::string& target);

    [[nodiscard]] std::string Resolve(const std::string& dir, const std::string& file) const;

    io::Vfs& m_vfs;
    audio::AudioEngine& m_audio;
    Stage& m_stage;
    Dialogue& m_dialogue;
    VariableStore& m_vars;
    Backlog& m_backlog;

    VmConfig m_config;
    std::function<bool(const Command&)> m_commandHook;
    std::function<void(const std::string&, const std::string&)> m_unlockHook;

    Program m_program;
    std::string m_scriptPath;
    int m_pc = 0;
    VmState m_state = VmState::Idle;

    std::vector<Choice> m_choices;
    std::vector<int> m_callStack;

    std::string m_speaker;
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
