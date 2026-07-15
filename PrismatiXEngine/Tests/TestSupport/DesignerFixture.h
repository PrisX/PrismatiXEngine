#pragma once

#include "Editor/Tools/UIDesigner/DesignerCommandService.h"
#include "Editor/Tools/UIDesigner/UIDesignerSession.h"
#include "Tests/TestSupport/TestHarness.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace px::test {

class DesignerFixture {
public:
    explicit DesignerFixture(Suite& suite) : m_suite(suite) {
        const auto instance = ++s_counter;
        path = std::filesystem::temp_directory_path() /
               ("prismatix-designer-fixture-" + ProcessSuffix() + '-' +
                std::to_string(instance) + ".pxscene");
        m_suite.Expect(static_cast<bool>(session.New(path, 800, 600)),
                       "fixture creates a current UI document");
        root = session.DocumentView().Root();
        parent = Uuid::FromName("PrismatiX.Test.Parent." + std::to_string(instance));
        child = Uuid::FromName("PrismatiX.Test.Child." + std::to_string(instance));
        sibling = Uuid::FromName("PrismatiX.Test.Sibling." + std::to_string(instance));

        auto& nodes = session.Document()->Data().nodes;
        nodes.push_back({parent, root, "Parent", "Panel",
                         {{"visibility", std::string("Visible")}}});
        nodes.push_back({child, parent, "Child", "Button",
                         {{"visibility", std::string("Visible")}}});
        nodes.push_back({sibling, root, "Sibling", "Panel",
                         {{"visibility", std::string("Visible")}}});
        m_suite.Expect(static_cast<bool>(session.DocumentView().Rebuild(*session.Document())),
                       "fixture tree is indexed");
        ResetLayout();
    }

    ~DesignerFixture() {
        session.Close();
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    void ResetLayout() {
        session.DocumentView().SetLayoutRect(root, {0, 0, 800, 600});
        session.DocumentView().SetLayoutRect(parent, {20, 20, 160, 100});
        session.DocumentView().SetLayoutRect(child, {40, 40, 80, 40});
        session.DocumentView().SetLayoutRect(sibling, {30, 30, 160, 100});
    }

    [[nodiscard]] bool IndexMatchesDocument() const {
        const auto* document = session.Document();
        if (!document || session.DocumentView().NodeCount() != document->Data().nodes.size())
            return false;
        std::size_t roots = 0;
        for (const auto& node : document->Data().nodes) {
            if (!session.DocumentView().Contains(node.id) ||
                session.DocumentView().Parent(node.id) != node.parent)
                return false;
            if (node.parent.Empty()) {
                ++roots;
                if (session.DocumentView().Root() != node.id) return false;
            } else {
                const auto index = session.DocumentView().ChildIndex(node.id);
                const auto children = session.DocumentView().Children(node.parent);
                if (!index || *index >= children.size() || children[*index] != node.id)
                    return false;
            }
        }
        return roots == 1;
    }

    std::filesystem::path path;
    editor::UIDesignerSession session;
    Uuid root;
    Uuid parent;
    Uuid child;
    Uuid sibling;

private:
    Suite& m_suite;
    inline static std::atomic_uint64_t s_counter = 0;
};

}  // namespace px::test
