#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Stage.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::progress {

struct SaveSnapshot {
    std::string scriptPath;
    int pc = 0;
    std::string chapter;
    std::string bgPath;
    std::string bgmPath;
    std::vector<vn::Stage::SavedActor> actors;
    std::vector<vn::Stage::SavedLayer> layers;
    std::unordered_map<std::string, int> variables;
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
