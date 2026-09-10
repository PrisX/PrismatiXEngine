#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Tests/TestSupport/TestHarness.h"

#include <cstdint>
#include <string>

namespace {

px::progress::SaveSnapshot MakeSaveSnapshot() {
    px::progress::SaveSnapshot snapshot;
    snapshot.gameId = "backlog-identity-test";
    snapshot.packageFingerprint = std::string(64, 'a');
    snapshot.contentVersion = "v1";
    snapshot.saveVersion = 1;
    snapshot.anchor = {"story", "line", "operation"};
    snapshot.backlog = {
        {"Alice", "First", "first.ogg", false, "source-41", "op-41", 41},
        {"Bob", "Second", "", false, "source-42", "op-42", 42}};
    snapshot.nvlLines = {snapshot.backlog.back()};
    return snapshot;
}

}  // namespace

int main() {
    px::test::Suite suite("BacklogStableIdentity");

    suite.Run("BoundedSequenceSurvivesEviction", [&] {
        px::vn::Backlog backlog;
        for (int line = 1; line <= 501; ++line) {
            backlog.Push("Alice", "Line " + std::to_string(line));
        }

        suite.Expect(backlog.Size() == 500,
                     "the production backlog remains bounded to 500 entries");
        suite.Expect(backlog.Entries().front().sequence == 2 &&
                         backlog.Entries().back().sequence == 501 &&
                         backlog.LastSequence() == 501,
                     "eviction changes physical indices without changing append identities");
        suite.Expect(backlog.Find(1) == nullptr && backlog.Find(501) != nullptr,
                     "stable lookup distinguishes an evicted line from the newest line");
    });

    suite.Run("PlayerConsumersObserveLine501AndBeyond", [&] {
        px::vn::Backlog backlog;
        std::uint64_t cursor = 0;
        std::size_t nvlAppends = 0;
        std::size_t speechRequests = 0;
        std::uint64_t rollbackTarget = 0;

        const auto consumePlayerEvents = [&] {
            for (const auto& entry : backlog.EntriesAfter(cursor)) {
                ++nvlAppends;
                ++speechRequests;
                rollbackTarget = entry.sequence;
            }
            cursor = backlog.LastSequence();
        };

        for (int line = 1; line <= 500; ++line) {
            backlog.Push("Narrator", "Line " + std::to_string(line));
            consumePlayerEvents();
        }
        backlog.Push("Narrator", "Line 501");
        consumePlayerEvents();
        backlog.Push("Narrator", "Line 502");
        consumePlayerEvents();

        suite.Expect(nvlAppends == 502 && speechRequests == 502,
                     "NVL and self-voice consumers receive appends after capacity is full");
        suite.Expect(rollbackTarget == 502 && backlog.Find(rollbackTarget) != nullptr,
                     "rollback captures the stable identity of post-eviction dialogue");
        suite.Expect(backlog.EntriesAfter(cursor).empty(),
                     "an unchanged backlog does not replay peripheral work");
    });

    suite.Run("RestorePreservesSequenceAndContinuesMonotonically", [&] {
        px::vn::Backlog backlog;
        backlog.Push("", "Restored 1", "", false, "source-700", "op-700", 700);
        backlog.Push("", "Restored 2", "", false, "source-701", "op-701", 701);
        const std::uint64_t appended = backlog.Push("", "New line");

        suite.Expect(appended == 702 && backlog.Find(701) != nullptr,
                     "restored identities are retained and the next append advances them");
        backlog.Clear();
        suite.Expect(backlog.Push("", "After clear") == 703,
                     "clearing physical history does not rewind the runtime append clock");
    });

    suite.Run("SaveCodecRoundTripsAndAcceptsLegacyEntries", [&] {
        px::test::TempDirectory temp("backlog-stable-identity");
        px::progress::SaveSystem saves;
        saves.Configure(temp.path.string(), nullptr);
        const auto snapshot = MakeSaveSnapshot();

        suite.Require(saves.Save(0, snapshot),
                      "a save containing stable backlog identities can be written");
        const auto loaded = saves.Load(0);
        suite.Expect(loaded && loaded->backlog.size() == 2 &&
                         loaded->backlog[0].sequence == 41 &&
                         loaded->backlog[1].sequence == 42 &&
                         loaded->nvlLines.size() == 1 &&
                         loaded->nvlLines[0].sequence == 42,
                     "save/load preserves backlog and NVL entry identities");

        auto legacy = px::progress::LoadJson(saves.SlotPath(0), nullptr);
        suite.Require(legacy.has_value(),
                      "the current save envelope is available for the compatibility fixture");
        for (auto& entry : (*legacy)["state"]["backlog"]) entry.erase("sequence");
        for (auto& entry : (*legacy)["state"]["nvlLines"]) entry.erase("sequence");
        legacy->erase("integrityHash");
        (*legacy)["integrityHash"] = px::crypto::Sha256Hex(legacy->dump());
        suite.Require(px::progress::SaveJson(saves.SlotPath(1), *legacy, nullptr),
                      "a pre-sequence save fixture can be written");

        const auto legacyLoaded = saves.Load(1);
        suite.Expect(legacyLoaded && legacyLoaded->backlog.size() == 2 &&
                         legacyLoaded->backlog[0].sequence == 0 &&
                         legacyLoaded->backlog[1].sequence == 0,
                     "older saves without backlog identity fields remain readable");
        if (legacyLoaded) {
            px::vn::Backlog restored;
            for (const auto& entry : legacyLoaded->backlog) {
                restored.Push(entry.speaker, entry.text, entry.voice,
                              entry.isChoice, entry.sourceId,
                              entry.operationId, entry.sequence);
            }
            suite.Expect(restored.Entries()[0].sequence == 1 &&
                             restored.Entries()[1].sequence == 2,
                         "legacy entries receive deterministic identities during restore");
        }
    });

    return suite.Finish();
}
