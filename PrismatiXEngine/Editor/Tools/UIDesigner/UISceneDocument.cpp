#include "Editor/Tools/UIDesigner/UISceneDocument.h"

#include "Engine/IO/AtomicFile.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace px::editor {

UISceneDocument::UISceneDocument() : m_history(*this) {}

diag::Diagnostic UISceneDocument::Error(std::string code, std::string message, const Uuid& node) {
    diag::Diagnostic d{.severity = diag::Severity::Error, .code = std::move(code),
                       .category = "Editor.UIDocument", .message = std::move(message)};
    if (!node.Empty()) d.source.nodeId = node.ToString();
    diag::Emit(d); return d;
}

Status UISceneDocument::New(std::filesystem::path path, int width, int height) {
    m_data = {}; m_data.kind = resource::DocumentKind::Scene; m_data.formatVersion = 1;
    m_data.id = Uuid::Random(); m_data.type = "UIScene";
    m_data.properties["canvasSize"] = Vec2{static_cast<float>(width), static_cast<float>(height)};
    resource::NodeRecord root; root.id = Uuid::Random(); root.name = "Root"; root.type = "StackContainer";
    root.properties["anchors"] = Rect{0,0,1,1}; m_data.nodes.push_back(std::move(root));
    m_path = std::move(path); m_history.Clear(); return Status::Ok();
}

Status UISceneDocument::Load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Status::Fail(Error("PXEDUI3001", "Cannot open UI scene: " + path.string()));
    std::ostringstream text; text << stream.rdbuf();
    auto parsed = resource::ParseTypedDocument(text.str(), path.string());
    if (!parsed) { for (const auto& d : parsed.Diagnostics()) diag::Emit(d); return Status::Fail(parsed.Diagnostics()); }
    if (parsed.Value().kind != resource::DocumentKind::Scene || parsed.Value().type != "UIScene")
        return Status::Fail(Error("PXEDUI3002", "Document is not a typed UIScene: " + path.string()));
    m_data = std::move(parsed.Value()); m_path = path;
    const Status valid = ValidateTree(); if (!valid) return valid;
    m_history.Clear(); m_history.MarkSaved(); return Status::Ok();
}

Status UISceneDocument::Save() {
    const Status valid = ValidateTree(); if (!valid) return valid;
    if (m_path.empty()) return Status::Fail(Error("PXEDUI3003", "UI scene has no save path"));
    const Status saved = io::AtomicFile::WriteText(m_path, Serialize());
    if (saved) m_history.MarkSaved();
    else for (const auto& d : saved.Diagnostics()) diag::Emit(d);
    return saved;
}

std::string UISceneDocument::Serialize() const { return resource::WriteTypedDocument(m_data); }

resource::NodeRecord* UISceneDocument::Find(const Uuid& id) {
    const auto it = std::find_if(m_data.nodes.begin(), m_data.nodes.end(), [&](const auto& n) { return n.id == id; });
    return it == m_data.nodes.end() ? nullptr : &*it;
}
const resource::NodeRecord* UISceneDocument::Find(const Uuid& id) const {
    const auto it = std::find_if(m_data.nodes.begin(), m_data.nodes.end(), [&](const auto& n) { return n.id == id; });
    return it == m_data.nodes.end() ? nullptr : &*it;
}

std::vector<resource::NodeRecord*> UISceneDocument::Children(const Uuid& parent) {
    std::vector<resource::NodeRecord*> result; for (auto& n : m_data.nodes) if (n.parent == parent) result.push_back(&n); return result;
}
std::vector<const resource::NodeRecord*> UISceneDocument::Children(const Uuid& parent) const {
    std::vector<const resource::NodeRecord*> result; for (const auto& n : m_data.nodes) if (n.parent == parent) result.push_back(&n); return result;
}
std::size_t UISceneDocument::ChildIndex(const Uuid& id) const {
    const auto* node = Find(id); if (!node) return 0;
    std::size_t index = 0; for (const auto& candidate : m_data.nodes) if (candidate.parent == node->parent) {
        if (candidate.id == id) return index; ++index;
    }
    return index;
}
bool UISceneDocument::IsDescendant(const Uuid& possible, const Uuid& ancestor) const {
    const auto* node = Find(possible); std::size_t guard = 0;
    while (node && !node->parent.Empty() && guard++ <= m_data.nodes.size()) {
        if (node->parent == ancestor) return true; node = Find(node->parent);
    }
    return false;
}

Result<Variant> UISceneDocument::ReadProperty(const Uuid& target, const std::string& property) const {
    if(target==m_data.id){if(const auto it=m_data.properties.find(property);it!=m_data.properties.end())return Result<Variant>::Success(it->second);return Result<Variant>::Success(Variant{});}
    const auto* node = Find(target);
    if (!node) return Result<Variant>::Failure(Error("PXEDUI3004", "UI edit target does not exist", target));
    if (property == "$name") return Result<Variant>::Success(Variant(node->name));
    if (property == "$type") return Result<Variant>::Success(Variant(node->type));
    if (property == "$parent") return Result<Variant>::Success(Variant(node->parent));
    if (const auto it = node->properties.find(property); it != node->properties.end()) return Result<Variant>::Success(it->second);
    return Result<Variant>::Success(Variant{});
}

Status UISceneDocument::WriteProperty(const Uuid& target, const std::string& property, const Variant& value) {
    if(target==m_data.id){if(value.Type()==VariantType::Null)m_data.properties.erase(property);else m_data.properties[property]=value;return Status::Ok();}
    auto* node = Find(target); if (!node) return Status::Fail(Error("PXEDUI3004", "UI edit target does not exist", target));
    if (property == "$name") {
        const auto* text = value.TryGet<std::string>(); if (!text || text->empty()) return Status::Fail(Error("PXEDUI3005", "Node name cannot be empty", target));
        node->name = *text; return Status::Ok();
    }
    if (property == "$type") {
        const auto* text = value.TryGet<std::string>(); if (!text || text->empty()) return Status::Fail(Error("PXEDUI3006", "Node type cannot be empty", target));
        node->type = *text; return Status::Ok();
    }
    if (property == "$parent") return Status::Fail(Error("PXEDUI3007", "Parent changes must use ReparentEditCommand", target));
    if (value.Type() == VariantType::Null) node->properties.erase(property); else node->properties[property] = value;
    return Status::Ok();
}

Result<VariantObject> UISceneDocument::Capture(const resource::NodeRecord& record) const {
    VariantObject result{{"id", Variant(record.id)}, {"name", Variant(record.name)}, {"type", Variant(record.type)},
                         {"properties", Variant(VariantObject(record.properties))}};
    VariantArray children;
    for (const auto* child : Children(record.id)) {
        auto captured = Capture(*child); if (!captured) return captured;
        children.emplace_back(std::move(captured.Value()));
    }
    result["children"] = Variant(std::move(children)); return Result<VariantObject>::Success(std::move(result));
}

Result<VariantObject> UISceneDocument::CaptureSubtree(const Uuid& target) const {
    const auto* node = Find(target); return node ? Capture(*node) : Result<VariantObject>::Failure(Error("PXEDUI3004", "UI edit target does not exist", target));
}

Status UISceneDocument::DecodeAndInsert(const Uuid& parent, std::size_t index, const VariantObject& data) {
    const auto idIt = data.find("id"), nameIt = data.find("name"), typeIt = data.find("type"), propsIt = data.find("properties");
    if (idIt == data.end() || nameIt == data.end() || typeIt == data.end() || propsIt == data.end())
        return Status::Fail(Error("PXEDUI3008", "Captured UI subtree is malformed"));
    const auto* id = idIt->second.TryGet<Uuid>(); const auto* name = nameIt->second.TryGet<std::string>(); const auto* type = typeIt->second.TryGet<std::string>();
    const auto* properties = propsIt->second.AsObject();
    if (!id || !name || !type || !properties || Find(*id)) return Status::Fail(Error("PXEDUI3009", "Captured UI subtree identity/type is invalid"));
    resource::NodeRecord record{*id, parent, *name, *type, *properties};
    auto siblings = Children(parent); index = std::min(index, siblings.size());
    auto insertion = m_data.nodes.end();
    if (index < siblings.size()) insertion = std::find_if(m_data.nodes.begin(), m_data.nodes.end(), [&](const auto& n) { return n.id == siblings[index]->id; });
    m_data.nodes.insert(insertion, std::move(record));
    if (const auto childIt = data.find("children"); childIt != data.end()) if (const auto* children = childIt->second.AsArray()) {
        std::size_t childIndex = 0; for (const auto& childValue : *children) {
            const auto* child = childValue.AsObject(); if (!child) return Status::Fail(Error("PXEDUI3010", "Captured child subtree is malformed", *id));
            const Status status = DecodeAndInsert(*id, childIndex++, *child); if (!status) return status;
        }
    }
    return Status::Ok();
}

Status UISceneDocument::InsertSubtree(const Uuid& parent, std::size_t index, const VariantObject& subtree) {
    if (!parent.Empty() && !Find(parent)) return Status::Fail(Error("PXEDUI3011", "Insert parent does not exist", parent));
    return DecodeAndInsert(parent, index, subtree);
}

Result<VariantObject> UISceneDocument::RemoveSubtree(const Uuid& target) {
    auto captured = CaptureSubtree(target); if (!captured) return captured;
    std::unordered_set<Uuid, UuidHash> remove{target};
    bool changed = true; while (changed) { changed = false; for (const auto& n : m_data.nodes)
        if (remove.contains(n.parent) && remove.insert(n.id).second) changed = true; }
    std::erase_if(m_data.nodes, [&](const auto& n) { return remove.contains(n.id); });
    return captured;
}

Status UISceneDocument::Reparent(const Uuid& target, const Uuid& parent, std::size_t index) {
    auto* node = Find(target); if (!node) return Status::Fail(Error("PXEDUI3004", "UI edit target does not exist", target));
    if (!parent.Empty() && !Find(parent)) return Status::Fail(Error("PXEDUI3011", "Reparent target does not exist", parent));
    if (target == parent || IsDescendant(parent, target)) return Status::Fail(Error("PXEDUI3012", "Reparent would create a scene tree cycle", target));
    if (node->parent.Empty()) return Status::Fail(Error("PXEDUI3013", "The UI scene root cannot be reparented", target));
    auto captured = CaptureSubtree(target); if (!captured) return Status::Fail(captured.Diagnostics());
    auto removed = RemoveSubtree(target); if (!removed) return Status::Fail(removed.Diagnostics());
    return InsertSubtree(parent, index, captured.Value());
}

Status UISceneDocument::MoveChild(const Uuid& parent, const Uuid& target, std::size_t index) {
    const auto* node = Find(target);
    if (!node || node->parent != parent) {
        return Status::Fail(Error("PXEDUI3018", "MoveChild target is not owned by the parent", target));
    }
    const std::size_t oldIndex = ChildIndex(target);
    auto siblings = Children(parent);
    if (siblings.size() <= 1 || oldIndex == index) return Status::Ok();
    index = std::min(index, siblings.size() - 1);
    auto captured = CaptureSubtree(target);
    if (!captured) return Status::Fail(captured.Diagnostics());
    auto removed = RemoveSubtree(target);
    if (!removed) return Status::Fail(removed.Diagnostics());
    return InsertSubtree(parent, index, captured.Value());
}

Status UISceneDocument::ValidateTree() const {
    std::unordered_set<Uuid, UuidHash> ids; std::size_t roots = 0;
    for (const auto& node : m_data.nodes) {
        if (node.id.Empty() || !ids.insert(node.id).second) return Status::Fail(Error("PXEDUI3014", "UI scene contains empty or duplicate node UUID", node.id));
        if (node.parent.Empty()) ++roots;
    }
    if (roots != 1) return Status::Fail(Error("PXEDUI3015", "UI scene must contain exactly one root"));
    for (const auto& node : m_data.nodes) {
        if (!node.parent.Empty() && !Find(node.parent)) return Status::Fail(Error("PXEDUI3016", "UI scene node has a missing parent", node.id));
        if (IsDescendant(node.parent, node.id)) return Status::Fail(Error("PXEDUI3017", "UI scene tree contains a cycle", node.id));
    }
    return Status::Ok();
}

}  // namespace px::editor
