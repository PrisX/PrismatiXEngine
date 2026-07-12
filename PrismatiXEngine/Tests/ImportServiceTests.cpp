#include "Editor/Assets/ImportService.h"
#include "Editor/Workspace/DocumentRegistry.h"

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

void TestUIDocumentActivationAndViewportSession() {
    TempDirectory temp;
    const auto title=temp.path/"Title.pxscene",hud=temp.path/"HUD.pxscene";
    std::ofstream(title)<<"@pxscene 4 "<<px::Uuid::Random().ToString()<<" UIScene\n";
    std::ofstream(hud)<<"@pxscene 4 "<<px::Uuid::Random().ToString()<<" UIScene\n";
    px::editor::DocumentManager documents;
    px::editor::DocumentSession titleSession;titleSession.id={px::Uuid::Random(),title};titleSession.label="Title";titleSession.type=px::editor::DocumentType::UIScene;
    px::editor::DocumentSession hudSession;hudSession.id={px::Uuid::Random(),hud};hudSession.label="HUD";hudSession.type=px::editor::DocumentType::UIScene;
    (void)documents.Open(std::move(titleSession));(void)documents.Open(std::move(hudSession));
    Check(documents.Activate(hud),"opening HUD from Title should activate the requested document exactly once");
    for(const auto& tab:documents.Documents())(void)tab;
    Check(documents.Active()&&documents.Active()->id.canonicalPath==px::editor::DocumentManager::Canonical(hud),
          "enumerating scene tabs must not pull active document back to Title");
    documents.SetViewport(hud,{1.75f,120.0f,240.0f,false});
    const auto sessionPath=temp.path/"EditorSession.pxres";
    Check(static_cast<bool>(documents.SaveSession(sessionPath)),"v4 editor document session should save");
    std::ifstream sessionInput(sessionPath);const std::string sessionText((std::istreambuf_iterator<char>(sessionInput)),{});
    Check(sessionText.starts_with("@pxresource 4 ")&&sessionText.find("scrollX")!=std::string::npos&&sessionText.find("panX")==std::string::npos,
          "designer session v4 should persist scroll state and remove legacy pan state");
    px::editor::DocumentManager restored;
    const bool restoredOk=static_cast<bool>(restored.RestoreSession(sessionPath));
    const bool sessionMatches=restoredOk&&restored.Active()&&
              restored.Active()->id.canonicalPath==px::editor::DocumentManager::Canonical(hud)&&
              restored.Active()->viewport.scrollX==120.0f&&!restored.Active()->viewport.fitToViewport;
    if(!sessionMatches&&restored.Active())std::cerr<<"restored active="<<restored.Active()->id.canonicalPath<<" scroll="<<restored.Active()->viewport.scrollX<<" fit="<<restored.Active()->viewport.fitToViewport<<'\n';
    Check(sessionMatches,
          "active HUD document and viewport scroll should restore without a tab-selection side effect");
}

}  // namespace

int main() {
    try {
    TestBulkImportAndIncrementalRegistry();
    TestInterruptedCommitRecovery();
    TestUIDocumentActivationAndViewportSession();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures == 0) std::cout << "All PrismatiX import tests passed.\n";
    return failures == 0 ? 0 : 1;
}
