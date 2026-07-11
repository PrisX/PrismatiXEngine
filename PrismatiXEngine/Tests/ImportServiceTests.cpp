#include "Editor/Assets/ImportService.h"

#include "Engine/Core/Uuid.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {

int failures = 0;
void Check(const bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("prismatix-import-tests-" + px::Uuid::Random().ToString());
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

bool WaitForReady(px::editor::ImportService& service) {
    const auto deadline = std::chrono::steady_clock::now() +
#ifdef PRISMATIX_STRESS_TESTS
        std::chrono::seconds(180);
#else
        std::chrono::seconds(10);
#endif
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = service.Progress().state;
        if (state == px::editor::ImportState::Ready) return true;
        if (state == px::editor::ImportState::Failed ||
            state == px::editor::ImportState::Cancelled) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void TestBulkImportAndIncrementalRegistry() {
    TempDirectory temp;
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
        inputs.push_back({file, file.filename()});
    }
    px::editor::ImportService service;
    auto plan = service.Prepare(project, project / "Content" / "Images", inputs, false, true);
    Check(static_cast<bool>(plan) && plan.Value().items.size() == count,
          "Unicode asset stress set should prepare without truncation");
    if (!plan) return;
    Check(static_cast<bool>(service.Start(plan.TakeValue())),
          "bulk import staging should start on the worker queue");
    Check(WaitForReady(service), "bulk import staging should finish without blocking the UI thread");
    px::resource::AssetRegistry registry;
    Check(static_cast<bool>(registry.Scan(project)), "empty project registry should initialize");
    Check(static_cast<bool>(service.Commit(registry)), "bulk import should commit atomically");
    Check(service.CommittedPaths().size() == count && registry.Entries().size() == count,
          "commit should incrementally register every imported asset");
}

void TestInterruptedCommitRecovery() {
    TempDirectory temp;
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
        journal << "staging\ncommitting\nrestore \"" << backup.generic_string() << "\" \""
                << target.generic_string() << "\"\nremove \"" << target.generic_string()
                << "\"\n";
    }
    px::editor::ImportService service;
    const auto prepared = service.Prepare(project, project / "Content" / "Images",
                                          {{newSource, newSource.filename()}}, false, true);
    std::ifstream restored(target);
    std::string content;
    restored >> content;
    if (!(prepared && content == "original" && !std::filesystem::exists(transaction))) {
        std::cerr << "recovery details: prepared=" << static_cast<bool>(prepared)
                  << " content=" << content
                  << " transaction=" << std::filesystem::exists(transaction) << '\n';
    }
    Check(static_cast<bool>(prepared) && content == "original" &&
              !std::filesystem::exists(transaction),
          "next import should rollback an interrupted commit from its journal");
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
    if (failures == 0) std::cout << "All PrismatiX import tests passed.\n";
    return failures == 0 ? 0 : 1;
}
