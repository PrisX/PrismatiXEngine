#pragma once

#include "Engine/IO/Crypto.h"
#include "Engine/Core/Result.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VM.h"
#include "Engine/VN/Expression/Expression.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/UIRuntimeState.h"
#include "Engine/Script/ScriptState.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::progress {

struct ExecutionAnchor {
    std::string runtimeDocumentId;
    std::string sourceId;
    std::string operationId;
};

struct SaveSnapshot {
    std::string engineVersion = "0.2.0";
    std::string gameId;
    std::string packageFingerprint;
    std::string contentVersion;
    std::uint32_t saveVersion = 1;
    ExecutionAnchor anchor;
    // In-memory only. Rollback entries share the immutable compiled program;
    // disk saves reconstruct it from the verified package.
    std::shared_ptr<const vn::Program> runtimeProgram;
    std::string scriptPath;
    int pc = 0;
    std::string chapter;
    std::string bgmPath;
    vn::Stage::RuntimeState stage;
    audio::AudioEngine::RuntimeState audio;
    std::unordered_map<std::string, int> variables;
    std::unordered_map<std::string, vn::Value> typedVariables;
    vn::VMRuntimeState vm;
    vn::DialogueSnapshot dialogue;
    ui::RouteState routes;
    std::vector<animation::PlaybackState> timelines;
    std::vector<animation::AnimationClip> animationClips;
    script::PendingCommandsState scriptPending;
    script::PendingActionsState scriptActions;
    ui::UIRuntimeState ui;
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

// Save migrations are deliberately pure-data transforms. The Player reads
// migration documents from the already verified package VFS; migration code
// never receives Engine, filesystem, network, or native bindings.
struct SaveMigrationDescriptor {
    std::string id;
    std::string fromContentVersion;
    std::uint32_t fromSaveVersion = 0;
    std::string toContentVersion;
    std::uint32_t toSaveVersion = 0;
    std::string asset;
};

struct SaveMigrationTarget {
    std::string gameId;
    std::string packageFingerprint;
    std::string contentVersion;
    std::uint32_t saveVersion = 0;
};

using SaveMigrationReadText =
    std::function<std::optional<std::string>(std::string_view asset)>;

// Strict in-memory decoder used by the Player, save inspector, and fuzzers.
// The input is the authenticated JSON envelope (without the persistence-file
// magic/encryption wrapper).  Keeping this public avoids tools growing a
// second, weaker interpretation of SaveEnvelopeV2.
[[nodiscard]] std::optional<SaveSnapshot> ParseSaveEnvelopeV2(
    std::string_view json, std::string_view sourcePath = "<memory-save>");

// Finds one unambiguous ordered chain and applies it to an isolated copy.
// Failure never mutates the source snapshot or the live RuntimeSession.
[[nodiscard]] Result<SaveSnapshot> MigrateSaveSnapshot(
    const SaveSnapshot& source, const SaveMigrationTarget& target,
    const std::vector<SaveMigrationDescriptor>& migrations,
    const SaveMigrationReadText& readText);

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
