#include "Engine/Scene/Node.h"

#include <algorithm>

namespace px::scene {

namespace {
diag::Diagnostic TreeError(std::string message) {
    return diag::Diagnostic{ diag::Severity::Error, "PXSCENE-E1001", "scene-tree",
                             std::move(message), {}, {}, {}, {} };
}
}

Status Node::AddChild(std::unique_ptr<Node> child, std::size_t index) {
    if (!child) return Status::Fail(TreeError("Cannot add a null child."));
    if (child.get() == this || child->IsAncestorOf(this)) {
        return Status::Fail(TreeError("Adding this child would create a scene-tree cycle."));
    }
    if (child->m_parent) {
        return Status::Fail(TreeError("The child already belongs to another parent."));
    }
    child->m_parent = this;
    if (index > m_children.size()) index = m_children.size();
    auto it = m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(index),
                                std::move(child));
    (*it)->EnterTreeRecursive();
    return Status::Ok();
}

std::unique_ptr<Node> Node::RemoveChild(const Uuid& id) {
    auto it = std::find_if(m_children.begin(), m_children.end(),
                           [&](const auto& child) { return child->Id() == id; });
    if (it == m_children.end()) return nullptr;
    (*it)->ExitTreeRecursive();
    (*it)->m_parent = nullptr;
    std::unique_ptr<Node> result = std::move(*it);
    m_children.erase(it);
    return result;
}

Status Node::MoveChild(const Uuid& id, std::size_t index) {
    auto it = std::find_if(m_children.begin(), m_children.end(),
                           [&](const auto& child) { return child->Id() == id; });
    if (it == m_children.end()) return Status::Fail(TreeError("Child to move was not found."));
    index = std::min(index, m_children.size() - 1);
    std::unique_ptr<Node> child = std::move(*it);
    const std::size_t old = static_cast<std::size_t>(it - m_children.begin());
    m_children.erase(it);
    if (index > old) --index;
    m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
    return Status::Ok();
}

Node* Node::Find(const Uuid& id) {
    if (Id() == id) return this;
    for (auto& child : m_children)
        if (Node* found = child->Find(id)) return found;
    return nullptr;
}
const Node* Node::Find(const Uuid& id) const {
    if (Id() == id) return this;
    for (const auto& child : m_children)
        if (const Node* found = child->Find(id)) return found;
    return nullptr;
}

Node* Node::FindPath(std::string_view path) {
    Node* current = this;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash - start);
        if (part == "..") {
            current = current ? current->Parent() : nullptr;
        } else if (!part.empty() && part != ".") {
            Node* next = nullptr;
            if (current) {
                for (const auto& child : current->Children()) {
                    if (child->Name() == part) {
                        next = child.get();
                        break;
                    }
                }
            }
            current = next;
        }
        if (!current || slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return current;
}

bool Node::IsAncestorOf(const Node* node) const {
    for (const Node* current = node; current; current = current->m_parent)
        if (current == this) return true;
    return false;
}

void Node::EnterTreeRecursive() {
    OnEnterTree();
    for (auto& child : m_children) child->EnterTreeRecursive();
}
void Node::ExitTreeRecursive() {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) (*it)->ExitTreeRecursive();
    OnExitTree();
}

}  // namespace px::scene
