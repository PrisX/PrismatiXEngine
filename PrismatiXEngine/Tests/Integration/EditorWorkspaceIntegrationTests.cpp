#include <filesystem>
#include <fstream>
#include <iostream>

#include "Editor/Workspace/DocumentRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

int failures = 0;
void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestUIDocumentActivationAndViewportSession() {
    px::test::TempDirectory temp{"editor-workspace"};
    const auto title = temp.path / "Title.pxscene";
    const auto hud = temp.path / "HUD.pxscene";
    std::ofstream(title) << "@pxscene 4 " << px::Uuid::Random().ToString() << " UIScene\n";
    std::ofstream(hud) << "@pxscene 4 " << px::Uuid::Random().ToString() << " UIScene\n";

    px::editor::DocumentManager documents;
    px::editor::DocumentSession titleSession;
    titleSession.id = { px::Uuid::Random(), title };
    titleSession.label = "Title";
    titleSession.type = px::editor::DocumentType::UIScene;
    px::editor::DocumentSession hudSession;
    hudSession.id = { px::Uuid::Random(), hud };
    hudSession.label = "HUD";
    hudSession.type = px::editor::DocumentType::UIScene;
    (void)documents.Open(std::move(titleSession));
    (void)documents.Open(std::move(hudSession));

    Check(documents.Activate(hud), "workspace should activate the requested UI document once");
    for (const auto& tab : documents.Documents()) (void)tab;
    Check(documents.Active() && documents.Active()->id.canonicalPath == px::editor::DocumentManager::Canonical(hud), "enumerating tabs must not change the active document");

    documents.SetViewport(hud, { 1.75f, 120.0f, 240.0f, false });
    documents.WorkspacePanels() = { .leftPanelVisible = false, .rightPanelVisible = true, .bottomPanelVisible = true, .leftPanelWidth = 312.0f, .rightPanelWidth = 376.0f, .bottomPanelHeight = 280.0f, .bottomPanelTab = 3 };
    const auto sessionPath = temp.path / "EditorSession.pxres";
    Check(static_cast<bool>(documents.SaveSession(sessionPath)), "workspace document session should save in the current format");
    std::ifstream input(sessionPath);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    Check(text.starts_with("@pxresource 4 ") && text.find("scrollX") != std::string::npos && text.find("panX") == std::string::npos, "workspace persistence should use scroll state without legacy pan state");

    px::editor::DocumentManager restored;
    Check(
        static_cast<bool>(restored.RestoreSession(sessionPath)) && restored.Active() && restored.Active()->id.canonicalPath == px::editor::DocumentManager::Canonical(hud) && restored.Active()->viewport.scrollX == 120.0f &&
            !restored.Active()->viewport.fitToViewport && !restored.WorkspacePanels().leftPanelVisible && restored.WorkspacePanels().bottomPanelVisible && restored.WorkspacePanels().leftPanelWidth == 312.0f &&
            restored.WorkspacePanels().bottomPanelTab == 3,
        "active document and viewport should restore without tab-selection side effects"
    );
    Check(text.find("workspacePanels") != std::string::npos && text.find("leftPanelVisible") == std::string::npos, "panel layout should persist once at workspace scope, never inside documents");
}

}  // namespace

int main() {
    TestUIDocumentActivationAndViewportSession();
    if (failures == 0) std::cout << "Editor workspace persistence integration passed.\n";
    return failures == 0 ? 0 : 1;
}
