#include <filesystem>
#include <fstream>
#include <iostream>

#include "Editor/Assets/ImportService.h"
#include "Engine/Core/Uuid.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

int failures = 0;
void Check(const bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestBulkImportAndIncrementalRegistry() {
    px::test::TempDirectory temp{"asset-import-bulk"};
    const auto project = temp.path / "Project";
    const auto sources = temp.path / std::filesystem::path(std::u8string(u8"來源素材"));
    std::filesystem::create_directories(project / "Content" / "Images");
    std::filesystem::create_directories(sources);
    std::vector<px::editor::ImportSource> inputs;
#ifdef PRISMATIX_STRESS_TESTS
    constexpr int count = 50000;
#else
    constexpr int count = 1000;
#endif
    for (int index = 0; index < count; ++index) {
        const auto file = sources / ("asset_" + std::to_string(index) + ".txt");
        std::ofstream(file, std::ios::binary) << "asset-" << index;
        inputs.push_back({ file, file.filename() });
    }
    px::editor::ImportService service;
    auto plan = service.Prepare(project, project / "Content" / "Images", inputs, false, true);
    Check(static_cast<bool>(plan) && plan.Value().items.size() == count, "Unicode asset stress set should prepare without truncation");
    if (!plan) return;
    Check(static_cast<bool>(service.Start(plan.TakeValue())), "bulk import staging should start on the worker queue");
    const auto staged = service.WaitForStaging();
    Check(staged.state == px::editor::ImportState::Ready, "bulk import staging should reach Ready at a deterministic synchronization boundary");
    px::resource::AssetRegistry registry;
    Check(static_cast<bool>(registry.Scan(project)), "empty project registry should initialize");
    Check(static_cast<bool>(service.Commit(registry)), "bulk import should commit atomically");
    Check(service.CommittedPaths().size() == count && registry.Entries().size() == count, "commit should incrementally register every imported asset");
}

void TestInterruptedCommitRecovery() {
    px::test::TempDirectory temp{"asset-import-recovery"};
    const auto project = temp.path / "Project";
    const auto target = project / "Content" / "Images" / "hero.txt";
    const auto transaction = project / ".prismatix" / "import-staging" / "interrupted";
    const auto backup = transaction / "backup" / "0";
    const auto newSource = temp.path / "new.txt";
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::create_directories(backup.parent_path());
    std::ofstream(target) << "partial-new";
    std::ofstream(backup) << "original";
    std::ofstream(newSource) << "next-import";
    {
        std::ofstream journal(transaction / "transaction.journal");
        journal << "staging\ncommitting\nrestore \"" << backup.generic_string() << "\" \"" << target.generic_string() << "\"\nremove \"" << target.generic_string() << "\"\n";
    }
    px::editor::ImportService service;
    const auto prepared = service.Prepare(project, project / "Content" / "Images", { { newSource, newSource.filename() } }, false, true);
    std::ifstream restored(target);
    std::string content;
    restored >> content;
    if (!(prepared && content == "original" && !std::filesystem::exists(transaction))) {
        std::cerr << "recovery details: prepared=" << static_cast<bool>(prepared) << " content=" << content << " transaction=" << std::filesystem::exists(transaction) << '\n';
    }
    Check(static_cast<bool>(prepared) && content == "original" && !std::filesystem::exists(transaction), "next import should rollback an interrupted commit from its journal");
}

}  // namespace

int main() {
    try {
        TestBulkImportAndIncrementalRegistry();
        TestInterruptedCommitRecovery();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures == 0) std::cout << "Asset import integration scenarios passed.\n";
    return failures == 0 ? 0 : 1;
}
