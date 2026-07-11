#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VM.h"
#include "Engine/VN/Expression/Expression.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/Lua/LuaState.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::progress {

struct SaveSnapshot {
    std::string scriptPath;
    int pc = 0;
    std::string chapter;
    std::string bgmPath;
    vn::Stage::RuntimeState stage;
    audio::AudioEngine::RuntimeState audio;
    std::unordered_map<std::string, int> variables;
    std::unordered_map<std::string, vn::Value> typedVariables;
    std::set<std::string> persistentVariables;
    vn::VMRuntimeState vm;
    vn::DialogueSnapshot dialogue;
    ui::RouteState routes;
    std::vector<animation::PlaybackState> timelines;
    std::vector<animation::AnimationClip> animationClips;
    lua::PendingCommandsState luaPending;
    std::vector<vn::BacklogEntry> backlog;
    bool nvlMode = false;
    std::vector<vn::BacklogEntry> nvlLines;
    std::uint64_t timestamp = 0;
    std::uint64_t playtimeMs = 0;
    std::vector<std::uint8_t> thumbnailPng;
};

struct SlotInfo {
    bool exists = false;
    std::string chapter;
    std::uint64_t timestamp = 0;
    std::vector<std::uint8_t> thumbnailPng;
};

class SaveSystem {
public:
    void Configure(const std::string& directory, const crypto::Key* key);

    bool Save(int slot, const SaveSnapshot& snapshot);
    [[nodiscard]] std::optional<SaveSnapshot> Load(int slot) const;
    [[nodiscard]] SlotInfo Peek(int slot) const;
    [[nodiscard]] std::vector<SlotInfo> List(int count) const;
    bool Delete(int slot);

    [[nodiscard]] std::string SlotPath(int slot) const;

private:
    std::string m_dir = "Save";
    bool m_encrypt = false;
    crypto::Key m_key{};
};

}
