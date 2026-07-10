#pragma once

#include "Engine/Core/Object.h"
#include "Engine/Core/Result.h"

#include <memory>
#include <string>
#include <vector>

namespace px::scene {

class Node : public Object {
public:
    explicit Node(std::string name = "Node") : m_name(std::move(name)) {}
    ~Node() override = default;

    [[nodiscard]] std::string_view TypeName() const override { return "Node"; }
    [[nodiscard]] const std::string& Name() const { return m_name; }
    void SetName(std::string name) { m_name = std::move(name); }
    [[nodiscard]] Node* Parent() const { return m_parent; }
    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& Children() const { return m_children; }
    [[nodiscard]] std::size_t ChildCount() const { return m_children.size(); }
    [[nodiscard]] Node* Child(std::size_t index) const {
        return index < m_children.size() ? m_children[index].get() : nullptr;
    }

    Status AddChild(std::unique_ptr<Node> child, std::size_t index = static_cast<std::size_t>(-1));
    [[nodiscard]] std::unique_ptr<Node> RemoveChild(const Uuid& id);
    Status MoveChild(const Uuid& id, std::size_t index);
    [[nodiscard]] Node* Find(const Uuid& id);
    [[nodiscard]] const Node* Find(const Uuid& id) const;
    [[nodiscard]] Node* FindPath(std::string_view path);

protected:
    virtual void OnEnterTree() {}
    virtual void OnExitTree() {}

private:
    bool IsAncestorOf(const Node* node) const;
    void EnterTreeRecursive();
    void ExitTreeRecursive();

    std::string m_name;
    Node* m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
};

}  // namespace px::scene
