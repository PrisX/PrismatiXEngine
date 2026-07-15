#include "Editor/Workspace/EditHistory.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace {

class FailureInjectedDocument final : public px::editor::IEditableDocument {
public:
    FailureInjectedDocument() {
        for (std::size_t index = 0; index < targets.size(); ++index) {
            targets[index] = px::Uuid::FromName("PrismatiX.Transaction.Target." +
                                                std::to_string(index));
            values[targets[index]] = px::Variant(std::int64_t{0});
        }
    }

    [[nodiscard]] px::Uuid DocumentId() const override {
        return px::Uuid::FromName("PrismatiX.Transaction.Document");
    }

    [[nodiscard]] px::Result<px::Variant> ReadProperty(
        const px::Uuid& target, const std::string&) const override {
        const auto found = values.find(target);
        if (found != values.end()) return px::Result<px::Variant>::Success(found->second);
        return px::Result<px::Variant>::Failure(Error("read target missing"));
    }

    px::Status WriteProperty(const px::Uuid& target, const std::string&,
                             const px::Variant& value) override {
        const auto found = std::find(targets.begin(), targets.end(), target);
        if (found == targets.end()) return px::Status::Fail(Error("write target missing"));
        const int index = static_cast<int>(std::distance(targets.begin(), found));
        if (index == failIndex && value == failValue)
            return px::Status::Fail(Error("injected write failure"));
        values[target] = value.Clone();
        return px::Status::Ok();
    }

    [[nodiscard]] px::Result<px::VariantObject> CaptureSubtree(
        const px::Uuid&) const override {
        return px::Result<px::VariantObject>::Failure(Error("unsupported"));
    }
    px::Status InsertSubtree(const px::Uuid&, std::size_t,
                             const px::VariantObject&) override {
        return px::Status::Fail(Error("unsupported"));
    }
    [[nodiscard]] px::Result<px::VariantObject> RemoveSubtree(
        const px::Uuid&) override {
        return px::Result<px::VariantObject>::Failure(Error("unsupported"));
    }
    px::Status Reparent(const px::Uuid&, const px::Uuid&, std::size_t) override {
        return px::Status::Fail(Error("unsupported"));
    }
    px::Status MoveChild(const px::Uuid&, const px::Uuid&, std::size_t) override {
        return px::Status::Fail(Error("unsupported"));
    }

    [[nodiscard]] static px::diag::Diagnostic Error(std::string message) {
        return {.severity = px::diag::Severity::Error,
                .code = "PXTEST-TX-0001",
                .category = "Test.PropertyTransaction",
                .message = std::move(message)};
    }

    std::array<px::Uuid, 3> targets;
    std::unordered_map<px::Uuid, px::Variant, px::UuidHash> values;
    int failIndex = -1;
    px::Variant failValue = std::int64_t{20};
};

bool AllEqual(const FailureInjectedDocument& document, std::int64_t value) {
    return std::all_of(document.targets.begin(), document.targets.end(),
                       [&](const px::Uuid& target) {
                           return document.values.at(target) == px::Variant(value);
                       });
}

}  // namespace

int main() {
    px::test::Suite suite("PropertyTransaction");

    suite.Run("SingleCommitCancelUndoRedo_PreservesExactValues", [&] {
        FailureInjectedDocument document;
        px::editor::EditHistory history(document);
        {
            px::editor::PropertyEditTransaction transaction(
                document, history, document.targets[0], "value", "Edit value");
            suite.Expect(transaction.Active() &&
                             static_cast<bool>(transaction.Update(std::int64_t{10})) &&
                             static_cast<bool>(transaction.Commit()) && history.Cursor() == 1,
                         "single-property Begin/Update/Commit creates one history entry");
        }
        suite.Expect(document.values.at(document.targets[0]) == px::Variant(std::int64_t{10}) &&
                         static_cast<bool>(history.Undo()) &&
                         document.values.at(document.targets[0]) == px::Variant(std::int64_t{0}) &&
                         static_cast<bool>(history.Redo()) &&
                         document.values.at(document.targets[0]) == px::Variant(std::int64_t{10}),
                     "single-property undo/redo restores exact values");
        const auto cursor = history.Cursor();
        {
            px::editor::PropertyEditTransaction transaction(
                document, history, document.targets[0], "value", "Cancel value");
            suite.Expect(static_cast<bool>(transaction.Update(std::int64_t{30})) &&
                             static_cast<bool>(transaction.Cancel()) &&
                             document.values.at(document.targets[0]) ==
                                 px::Variant(std::int64_t{10}) &&
                             history.Cursor() == cursor,
                         "single-property cancel restores before value without history");
        }
    });

    suite.Run("MultiFailureAtEveryPosition_RollsBackAndRemainsRetryable", [&] {
        for (int failIndex = 0; failIndex < 3; ++failIndex) {
            FailureInjectedDocument document;
            px::editor::EditHistory history(document);
            px::editor::MultiPropertyEditTransaction transaction(
                document, history,
                std::vector<px::Uuid>(document.targets.begin(), document.targets.end()),
                "value", "Batch edit");
            document.failIndex = failIndex;
            const auto failed = transaction.Update(std::int64_t{20});
            suite.Expect(!failed && transaction.Active() && AllEqual(document, 0) &&
                             history.Cursor() == 0,
                         "first, middle, and last write failures roll back atomically");
            document.failIndex = -1;
            suite.Expect(static_cast<bool>(transaction.Update(std::int64_t{10})) &&
                             static_cast<bool>(transaction.Cancel()) &&
                             AllEqual(document, 0) && history.Cursor() == 0,
                         "failed transaction remains retryable and cancellable");
        }
    });

    suite.Run("SecondUpdateMiddleFailure_RollsBackToPreviousSuccessfulCurrentValue", [&] {
        FailureInjectedDocument document;
        px::editor::EditHistory history(document);
        px::editor::MultiPropertyEditTransaction transaction(
            document, history,
            std::vector<px::Uuid>(document.targets.begin(), document.targets.end()),
            "value", "Batch edit");
        suite.Expect(static_cast<bool>(transaction.Update(std::int64_t{10})) &&
                         AllEqual(document, 10),
                     "first update establishes current value 10 for A/B/C");
        document.failIndex = 1;
        const auto second = transaction.Update(std::int64_t{20});
        suite.Expect(!second && AllEqual(document, 10) && transaction.Active() &&
                         history.Cursor() == 0,
                     "A success then B failure restores A/B/C to previous current value 10");
        document.failIndex = -1;
        suite.Expect(static_cast<bool>(transaction.Update(std::int64_t{20})) &&
                         static_cast<bool>(transaction.Commit()) && history.Cursor() == 1 &&
                         AllEqual(document, 20) && static_cast<bool>(history.Undo()) &&
                         AllEqual(document, 0) && static_cast<bool>(history.Redo()) &&
                         AllEqual(document, 20),
                     "retry commit and undo/redo remain one atomic multi-target edit");
    });

    return suite.Finish();
}
