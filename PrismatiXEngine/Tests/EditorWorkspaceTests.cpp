#include "Editor/Workspace/DocumentRegistry.h"
#include "Editor/Workspace/EditHistory.h"

#include "Engine/Core/Uuid.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

namespace {

int failures = 0;
void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("prismatix-workspace-tests-" + px::Uuid::Random().ToString());
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct FailingEditableDocument final : px::editor::IEditableDocument {
    px::Uuid id = px::Uuid::Random();
    std::vector<px::Uuid> targets{px::Uuid::Random(), px::Uuid::Random(), px::Uuid::Random()};
    std::unordered_map<px::Uuid, px::Variant, px::UuidHash> values;
    int failIndex = -1;

    FailingEditableDocument() {
        for (std::size_t index = 0; index < targets.size(); ++index)
            values[targets[index]] = std::string("before") + std::to_string(index);
    }

    [[nodiscard]] px::Uuid DocumentId() const override { return id; }
    [[nodiscard]] px::Result<px::Variant> ReadProperty(
        const px::Uuid& target, const std::string&) const override {
        const auto found = values.find(target);
        if (found != values.end()) return px::Result<px::Variant>::Success(found->second);
        return px::Result<px::Variant>::Failure(Failure("read target missing"));
    }
    px::Status WriteProperty(const px::Uuid& target, const std::string&,
                             const px::Variant& value) override {
        const auto found = std::find(targets.begin(), targets.end(), target);
        if (found == targets.end()) return px::Status::Fail(Failure("write target missing"));
        const int index = static_cast<int>(std::distance(targets.begin(), found));
        if (index == failIndex && value == px::Variant(std::string("updated")))
            return px::Status::Fail(Failure("injected write failure"));
        values[target] = value.Clone();
        return px::Status::Ok();
    }
    [[nodiscard]] px::Result<px::VariantObject> CaptureSubtree(
        const px::Uuid&) const override {
        return px::Result<px::VariantObject>::Failure(Failure("unsupported"));
    }
    px::Status InsertSubtree(const px::Uuid&, std::size_t,
                             const px::VariantObject&) override {
        return px::Status::Fail(Failure("unsupported"));
    }
    [[nodiscard]] px::Result<px::VariantObject> RemoveSubtree(
        const px::Uuid&) override {
        return px::Result<px::VariantObject>::Failure(Failure("unsupported"));
    }
    px::Status Reparent(const px::Uuid&, const px::Uuid&, std::size_t) override {
        return px::Status::Fail(Failure("unsupported"));
    }
    px::Status MoveChild(const px::Uuid&, const px::Uuid&, std::size_t) override {
        return px::Status::Fail(Failure("unsupported"));
    }

    [[nodiscard]] static px::diag::Diagnostic Failure(std::string message) {
        return {.severity = px::diag::Severity::Error,
                .code = "PXTEST0001",
                .category = "Test.EditHistory",
                .message = std::move(message)};
    }
};

void TestUIDocumentActivationAndViewportSession() {
    TempDirectory temp;
    const auto title = temp.path / "Title.pxscene";
    const auto hud = temp.path / "HUD.pxscene";
    std::ofstream(title) << "@pxscene 4 " << px::Uuid::Random().ToString() << " UIScene\n";
    std::ofstream(hud) << "@pxscene 4 " << px::Uuid::Random().ToString() << " UIScene\n";

    px::editor::DocumentManager documents;
    px::editor::DocumentSession titleSession;
    titleSession.id = {px::Uuid::Random(), title};
    titleSession.label = "Title";
    titleSession.type = px::editor::DocumentType::UIScene;
    px::editor::DocumentSession hudSession;
    hudSession.id = {px::Uuid::Random(), hud};
    hudSession.label = "HUD";
    hudSession.type = px::editor::DocumentType::UIScene;
    (void)documents.Open(std::move(titleSession));
    (void)documents.Open(std::move(hudSession));

    Check(documents.Activate(hud), "workspace should activate the requested UI document once");
    for (const auto& tab : documents.Documents()) (void)tab;
    Check(documents.Active() &&
              documents.Active()->id.canonicalPath == px::editor::DocumentManager::Canonical(hud),
          "enumerating tabs must not change the active document");

    documents.SetViewport(hud, {1.75f, 120.0f, 240.0f, false});
    documents.WorkspacePanels() = {.leftPanelVisible=false,
                                   .rightPanelVisible=true,
                                   .bottomPanelVisible=true,
                                   .leftPanelWidth=312.0f,
                                   .rightPanelWidth=376.0f,
                                   .bottomPanelHeight=280.0f,
                                   .bottomPanelTab=3};
    const auto sessionPath = temp.path / "EditorSession.pxres";
    Check(static_cast<bool>(documents.SaveSession(sessionPath)),
          "workspace document session should save in the current format");
    std::ifstream input(sessionPath);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    Check(text.starts_with("@pxresource 4 ") && text.find("scrollX") != std::string::npos &&
              text.find("panX") == std::string::npos,
          "workspace persistence should use scroll state without legacy pan state");

    px::editor::DocumentManager restored;
    Check(static_cast<bool>(restored.RestoreSession(sessionPath)) && restored.Active() &&
              restored.Active()->id.canonicalPath ==
                  px::editor::DocumentManager::Canonical(hud) &&
               restored.Active()->viewport.scrollX == 120.0f &&
               !restored.Active()->viewport.fitToViewport &&
               !restored.WorkspacePanels().leftPanelVisible &&
               restored.WorkspacePanels().bottomPanelVisible &&
               restored.WorkspacePanels().leftPanelWidth == 312.0f &&
               restored.WorkspacePanels().bottomPanelTab == 3,
           "active document and viewport should restore without tab-selection side effects");
    Check(text.find("workspacePanels") != std::string::npos &&
              text.find("leftPanelVisible") == std::string::npos,
          "panel layout should persist once at workspace scope, never inside documents");
}

void TestMultiPropertyTransactionAtomicity() {
    for (int failIndex = 0; failIndex < 3; ++failIndex) {
        FailingEditableDocument document;
        px::editor::EditHistory history(document);
        px::editor::MultiPropertyEditTransaction transaction(
            document, history, document.targets, "value", "Batch edit");
        document.failIndex = failIndex;
        const px::Status update = transaction.Update(std::string("updated"));
        bool restored = !update && transaction.Active() && history.Cursor() == 0;
        for (std::size_t index = 0; index < document.targets.size(); ++index)
            restored &= document.values[document.targets[index]] ==
                        px::Variant(std::string("before") + std::to_string(index));
        Check(restored,
              "first, middle, or last multi-property write failure must roll back atomically");

        document.failIndex = -1;
        Check(static_cast<bool>(transaction.Update(std::string("updated"))) &&
                  static_cast<bool>(transaction.Cancel()) && history.Cursor() == 0,
              "a failed active transaction should remain retryable and cancellable");
        bool cancelled = true;
        for (std::size_t index = 0; index < document.targets.size(); ++index)
            cancelled &= document.values[document.targets[index]] ==
                         px::Variant(std::string("before") + std::to_string(index));
        Check(cancelled, "multi-property cancel must restore every exact starting value");
    }

    FailingEditableDocument document;
    px::editor::EditHistory history(document);
    px::editor::MultiPropertyEditTransaction transaction(
        document, history, document.targets, "value", "Batch edit");
    Check(static_cast<bool>(transaction.Update(std::string("updated"))) &&
              static_cast<bool>(transaction.Commit()) && history.Cursor() == 1 &&
              static_cast<bool>(history.Undo()) && history.Cursor() == 0 &&
              static_cast<bool>(history.Redo()) && history.Cursor() == 1,
          "successful multi-property commit must create exactly one undo/redo entry");
}

}  // namespace

int main() {
    TestUIDocumentActivationAndViewportSession();
    TestMultiPropertyTransactionAtomicity();
    if (failures == 0) std::cout << "All Editor workspace tests passed.\n";
    return failures == 0 ? 0 : 1;
}
