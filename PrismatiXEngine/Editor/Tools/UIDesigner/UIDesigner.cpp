#include "Editor/Tools/UIDesigner/UIDesigner.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <utility>

namespace px::editor {
namespace {
constexpr const char* kNodePayload = "PX_UI_NODE_UUID";

void EmitStatus(const Status& status) { for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic); }

std::string UniqueName(const UISceneDocument& document, std::string base) {
    std::unordered_set<std::string> names; for (const auto& node : document.Data().nodes) names.insert(node.name);
    if (!names.contains(base)) return base;
    for (int i = 2; i < 10000; ++i) if (!names.contains(base + std::to_string(i))) return base + std::to_string(i);
    return base + "Copy";
}

const char* TypeGlyph(std::string_view type) {
    if (type.find("Container") != std::string_view::npos) return "◇";
    if (type == "Button") return "▣";
    if (type == "Label") return "T";
    if (type == "TextureRect") return "▧";
    return "○";
}
}

UIDesigner::UIDesigner() { const Status status = ui::RegisterBuiltinUITypes(); if (!status) EmitStatus(status); }

Status UIDesigner::Open(const std::filesystem::path& path) {
    auto document = std::make_unique<UISceneDocument>(); const Status status = document->Load(path);
    if (!status) return status;
    m_document = std::move(document); m_pathText = path.string(); m_selected = RootId();
    m_selection={m_selected}; RebuildLayout(); return Status::Ok();
}

Status UIDesigner::New(const std::filesystem::path& path, int width, int height) {
    auto document = std::make_unique<UISceneDocument>(); const Status status = document->New(path, width, height);
    if (!status) return status;
    m_document = std::move(document); m_pathText = path.string(); m_selected = RootId();
    m_selection={m_selected}; RebuildLayout(); MarkEdited(true); return Status::Ok();
}

bool UIDesigner::Save() { if (!m_document) return false; const Status status = m_document->Save(); if (!status) EmitStatus(status); return static_cast<bool>(status); }
Status UIDesigner::Undo() { if (!m_document) return Status::Ok(); auto status = m_document->History().Undo(); if (status) { RebuildLayout(); MarkEdited(); } return status; }
Status UIDesigner::Redo() { if (!m_document) return Status::Ok(); auto status = m_document->History().Redo(); if (status) { RebuildLayout(); MarkEdited(); } return status; }
void UIDesigner::RelocateDocument(const std::filesystem::path& oldPath,
                                  const std::filesystem::path& newPath) {
    if (!m_document) return;
    std::error_code ec;
    const auto current = std::filesystem::weakly_canonical(m_document->Path(), ec);
    ec.clear(); const auto oldCanonical = std::filesystem::weakly_canonical(oldPath, ec);
    if (!ec && current == oldCanonical) {
        m_document->RelocatePath(newPath); m_pathText = newPath.string();
    }
}

Uuid UIDesigner::RootId() const { if (!m_document) return {}; for (const auto& node : m_document->Data().nodes) if (node.parent.Empty()) return node.id; return {}; }
Uuid UIDesigner::ParentForNewNode() const {
    if (!m_document) return {};
    if (const auto* selected = m_document->Find(m_selected)) {
        if (selected->type.find("Container") != std::string::npos || selected->type == "Panel" || selected->type == "Control") return selected->id;
        return selected->parent;
    }
    return RootId();
}

void UIDesigner::MarkEdited(bool structural) {
    m_lastEdit = std::chrono::steady_clock::now();
    RebuildLayout(); if (structural && m_onStructure) m_onStructure(); if (m_onEdit) m_onEdit();
}

VariantObject UIDesigner::NewSubtree(std::string type, std::string name, Rect anchors, Rect offsets, std::string image) {
    VariantObject properties{{"anchors", Variant(anchors)}, {"offsets", Variant(offsets)}};
    if (type == "Label") properties["text"] = Variant(std::string("Text"));
    if (type == "Button") properties["text"] = Variant(std::string("Button"));
    if (type == "TextureRect" && !image.empty()) properties["path"] = Variant(std::move(image));
    return {{"id", Variant(Uuid::Random())}, {"name", Variant(std::move(name))}, {"type", Variant(std::move(type))},
            {"properties", Variant(std::move(properties))}, {"children", Variant(VariantArray{})}};
}

void UIDesigner::RegenerateIds(VariantObject& subtree) {
    subtree["id"] = Variant(Uuid::Random());
    if (auto* children = subtree["children"].AsArray()) for (auto& value : *children) if (auto* child = value.AsObject()) RegenerateIds(*child);
}

void UIDesigner::AddNode(std::string type, Vec2 canvas, std::string image) {
    if (!m_document) return; const Uuid parent = ParentForNewNode();
    const float w = type == "TextureRect" ? 320.0f : 180.0f, h = type == "TextureRect" ? 180.0f : 52.0f;
    Rect offsets{canvas.x - w * 0.5f, canvas.y - h * 0.5f, w, h};
    if (canvas == Vec2{}) offsets = {20,20,w,h};
    VariantObject subtree = NewSubtree(type, UniqueName(*m_document, type), {0,0,0,0}, offsets, std::move(image));
    const Uuid id = *subtree["id"].TryGet<Uuid>();
    auto command = std::make_unique<SubtreeEditCommand>("Add " + type, SubtreeOperation::Insert, id, parent,
                                                        m_document->Children(parent).size(), std::move(subtree));
    const Status status = m_document->History().Execute(std::move(command)); if (!status) return EmitStatus(status);
    m_selected = id; MarkEdited(true);
}

void UIDesigner::RemoveSelected() {
    if (!m_document || m_selected.Empty() || m_selected == RootId()) return;
    auto captured = m_document->CaptureSubtree(m_selected); if (!captured) return EmitStatus(Status::Fail(captured.Diagnostics()));
    const auto* node = m_document->Find(m_selected); const Uuid parent = node->parent; const std::size_t index = m_document->ChildIndex(m_selected);
    auto command = std::make_unique<SubtreeEditCommand>("Delete " + node->name, SubtreeOperation::Remove, m_selected, parent, index, std::move(captured.Value()));
    const Status status = m_document->History().Execute(std::move(command)); if (!status) return EmitStatus(status);
    m_selected = parent; m_selection={parent}; MarkEdited(true);
}

void UIDesigner::DuplicateSelected() {
    if (!m_document || m_selected.Empty()) return;
    const auto* node = m_document->Find(m_selected); if (!node) return;
    auto captured = m_document->CaptureSubtree(m_selected); if (!captured) return;
    RegenerateIds(captured.Value());
    if (auto* name = captured.Value()["name"].TryGet<std::string>()) *name = UniqueName(*m_document, *name + "Copy");
    const Uuid newId = *captured.Value()["id"].TryGet<Uuid>(); const Uuid parent = node->parent.Empty() ? node->id : node->parent;
    auto command = std::make_unique<SubtreeEditCommand>("Duplicate " + node->name, SubtreeOperation::Insert, newId, parent,
                                                        m_document->ChildIndex(m_selected) + 1, std::move(captured.Value()));
    const Status status = m_document->History().Execute(std::move(command)); if (!status) return EmitStatus(status);
    m_selected = newId; m_selection={newId}; MarkEdited(true);
}

bool UIDesigner::TreeMatches(const resource::NodeRecord& record) const {
    if (m_treeFilter[0] == 0) return true;
    const std::string filter=m_treeFilter;
    if(record.name.find(filter)!=std::string::npos||record.type.find(filter)!=std::string::npos)return true;
    for(const auto* child:std::as_const(*m_document).Children(record.id))if(TreeMatches(*child))return true;
    return false;
}

void UIDesigner::RenderTreeNode(resource::NodeRecord& record) {
    if(!TreeMatches(record))return;
    const auto children = m_document->Children(record.id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf; if (record.id == m_selected) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(record.id.ToString().c_str());
    const bool visible=!record.properties.contains("visible")||!record.properties["visible"].TryGet<bool>()||*record.properties["visible"].TryGet<bool>();
    const bool locked=record.properties.contains("editorLocked")&&record.properties["editorLocked"].TryGet<bool>()&&*record.properties["editorLocked"].TryGet<bool>();
    const auto policy=m_childPolicies.find(record.id);const bool container=policy!=m_childPolicies.end()&&policy->second!=ui::ChildLayoutPolicy::Free;
    const std::string badges=std::string(visible?"":"  ◌")+(locked?"  ◆":"")+(container?"  [Layout]":"");
    const bool open = ImGui::TreeNodeEx("node", flags, "%s  %s%s", TypeGlyph(record.type), record.name.c_str(),badges.c_str());
    if (ImGui::IsItemClicked()) {
        if (ImGui::GetIO().KeyCtrl) {
            if (m_selection.contains(record.id)) m_selection.erase(record.id);
            else m_selection.insert(record.id);
            if (!m_selection.empty()) m_selected = record.id;
        } else { m_selected = record.id; m_selection={record.id}; }
    }
    if (ImGui::BeginDragDropSource()) { const std::string id = record.id.ToString(); ImGui::SetDragDropPayload(kNodePayload, id.c_str(), id.size()+1); ImGui::Text("Move %s", record.name.c_str()); ImGui::EndDragDropSource(); }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload)) {
            if (auto id = Uuid::Parse(static_cast<const char*>(payload->Data)); id && *id != record.id) {
                if (const auto* moved = m_document->Find(*id)) {
                    const float rowHeight=ImGui::GetItemRectSize().y;
                    const float fraction=rowHeight>0?(ImGui::GetMousePos().y-ImGui::GetItemRectMin().y)/rowHeight:.5f;
                    const bool siblingDrop=fraction<.25f||fraction>.75f;
                    const Uuid targetParent=siblingDrop?record.parent:record.id;
                    std::size_t targetIndex=siblingDrop?m_document->ChildIndex(record.id)+(fraction>.75f?1:0):m_document->Children(record.id).size();
                    std::unique_ptr<EditCommand> command;
                    if(moved->parent==targetParent)command=std::make_unique<MoveChildEditCommand>("重新排列 "+moved->name,targetParent,*id,m_document->ChildIndex(*id),targetIndex);
                    else command=std::make_unique<ReparentEditCommand>("Reparent " + moved->name, *id, moved->parent,m_document->ChildIndex(*id),targetParent,targetIndex);
                    const Status status = m_document->History().Execute(std::move(command)); if (!status) EmitStatus(status); else MarkEdited(true);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if(ImGui::BeginPopupContextItem("TreeNodeMenu")){
        if(ImGui::MenuItem(visible?"隱藏":"顯示")){const Variant before=record.properties.contains("visible")?record.properties["visible"]:Variant(true);auto command=std::make_unique<PropertyChangeCommand>("切換可見性",record.id,"visible",before,Variant(!visible));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
        if(ImGui::MenuItem(locked?"解除鎖定":"鎖定")){const Variant before=record.properties.contains("editorLocked")?record.properties["editorLocked"]:Variant(false);auto command=std::make_unique<PropertyChangeCommand>("切換鎖定",record.id,"editorLocked",before,Variant(!locked));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
        ImGui::Separator();
        if(ImGui::MenuItem("重新命名","F2")){m_selected=record.id;m_selection={record.id};std::snprintf(m_treeRename,sizeof(m_treeRename),"%s",record.name.c_str());m_treeRenameOpen=true;}
        if(ImGui::MenuItem("複製","Ctrl+D")){m_selected=record.id;m_selection={record.id};DuplicateSelected();}
        if(record.id!=RootId()&&ImGui::MenuItem("刪除","Delete")){m_selected=record.id;m_selection={record.id};RemoveSelected();}
        ImGui::EndPopup();
    }
    if (open) { for (auto* child : children) RenderTreeNode(*child); ImGui::TreePop(); }
    ImGui::PopID();
}

void UIDesigner::RenderHierarchy() {
    if (!m_document) { ImGui::TextDisabled("Open a typed .pxscene UI document."); return; }
    ImGui::InputTextWithHint("##tree-search","搜尋節點或型別…",m_treeFilter,sizeof(m_treeFilter));
    if (ImGui::Button("＋ Add")) ImGui::OpenPopup("AddUIControl"); ImGui::SameLine();
    if (ImGui::Button("Duplicate")) DuplicateSelected(); ImGui::SameLine();
    if (ImGui::Button("Delete")) RemoveSelected(); ImGui::SameLine();
    if (ImGui::Button(Dirty() ? "Save *" : "Save")) Save();
    if (ImGui::BeginPopup("AddUIControl")) {
        for (const auto* type : TypeRegistry::Global().TypesDerivedFrom("Control")) if (type->name != "Control")
            if (ImGui::MenuItem(type->name.c_str())) AddNode(type->name);
        ImGui::EndPopup();
    }
    if(!ImGui::GetIO().WantTextInput&&ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::IsKeyPressed(ImGuiKey_F2)){
        if(const auto* selected=m_document->Find(m_selected)){std::snprintf(m_treeRename,sizeof(m_treeRename),"%s",selected->name.c_str());m_treeRenameOpen=true;}
    }
    if(m_treeRenameOpen)ImGui::OpenPopup("重新命名節點");
    if(ImGui::BeginPopupModal("重新命名節點",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
        const bool submit=ImGui::InputText("名稱",m_treeRename,sizeof(m_treeRename),ImGuiInputTextFlags_EnterReturnsTrue);
        if((submit||ImGui::Button("重新命名"))&&m_treeRename[0]){if(const auto* selected=m_document->Find(m_selected))EditVariant("Name","$name",Variant(selected->name),Variant(std::string(m_treeRename)),true,false);m_treeRenameOpen=false;ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("取消")){m_treeRenameOpen=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::Separator();
    if (auto* root = m_document->Find(RootId())) RenderTreeNode(*root);
}

void UIDesigner::EditVariant(const char*, const std::string& property, Variant before, Variant value,
                             bool changed, bool continuous) {
    if (!m_document || !changed || before == value) return;
    if (continuous) {
        if (!m_propertyTransaction || m_transactionProperty != property || m_transactionTarget != m_selected) {
            if (m_propertyTransaction) m_propertyTransaction->Commit();
            m_propertyTransaction = std::make_unique<PropertyEditTransaction>(*m_document, m_document->History(), m_selected, property, "Change " + property);
            m_transactionProperty = property; m_transactionTarget = m_selected;
        }
        const Status status = m_propertyTransaction->Update(std::move(value)); if (!status) EmitStatus(status);
    } else {
        auto command = std::make_unique<PropertyChangeCommand>("Change " + property, m_selected, property, std::move(before), std::move(value));
        const Status status = m_document->History().Execute(std::move(command)); if (!status) EmitStatus(status);
    }
    MarkEdited();
}

void UIDesigner::RenderInspector(const std::string& selectedAssetPath) {
    if (!m_document) { ImGui::TextDisabled("No UI document."); return; }
    auto* node = m_document->Find(m_selected); if (!node) { ImGui::TextDisabled("Select a Control."); return; }
    ImGui::TextDisabled("%s", node->type.c_str());
    std::string name = node->name; if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue))
        EditVariant("Name", "$name", Variant(node->name), Variant(name), true, false);

    std::vector<const PropertyInfo*> properties; std::string type = node->type;
    while (const auto* info = TypeRegistry::Global().Find(type)) {
        for (const auto& property : info->properties) if (HasFlag(property.flags, PropertyFlags::Editable)) properties.push_back(&property);
        type = info->base; if (type.empty()) break;
    }
    std::string lastCategory;
    for (const auto* property : properties) {
        if (property->category != lastCategory) {
            lastCategory = property->category;
            const char* category=lastCategory=="Layout"?"配置":lastCategory=="Appearance"?"外觀":lastCategory=="Interaction"?"互動":lastCategory=="Data"?"資料":lastCategory.c_str();
            ImGui::SeparatorText(category);
        }
        if(property->name=="offsets"&&SelectedParentPolicy()!=ui::ChildLayoutPolicy::Free){
            ImGui::TextDisabled("位置與尺寸  由 %s 控制",ui::ChildLayoutPolicyName(SelectedParentPolicy()));
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Container 子元件不寫入無效 offsets；請使用 minimum size、size flags 或拖曳排序。");
            continue;
        }
        const auto current = node->properties.find(property->name);
        Variant before = current == node->properties.end() ? Variant{} : current->second;
        Variant value = current == node->properties.end() ? property->defaultValue : current->second;
        bool changed = false, continuous = false;
        ImGui::PushID(property->name.c_str());
        if (auto* text = value.TryGet<std::string>()) changed = ImGui::InputText(property->name.c_str(), text);
        else if (auto* boolean = value.TryGet<bool>()) changed = ImGui::Checkbox(property->name.c_str(), boolean);
        else if (auto* integer = value.TryGet<std::int64_t>()) { int v = static_cast<int>(*integer); changed = ImGui::DragInt(property->name.c_str(), &v); *integer = v; continuous = true; }
        else if (auto* number = value.TryGet<double>()) { float v = static_cast<float>(*number); changed = ImGui::DragFloat(property->name.c_str(), &v, 0.1f); *number = v; continuous = true; }
        else if (auto* vec = value.TryGet<Vec2>()) { float v[2]{vec->x,vec->y}; changed = ImGui::DragFloat2(property->name.c_str(), v, 1); *vec={v[0],v[1]}; continuous=true; }
        else if (auto* rect = value.TryGet<Rect>()) { float v[4]{rect->x,rect->y,rect->w,rect->h}; changed=ImGui::DragFloat4(property->name.c_str(),v,1); *rect={v[0],v[1],v[2],v[3]}; continuous=true; }
        else if (auto* color = value.TryGet<Color>()) { float v[4]{color->r/255.f,color->g/255.f,color->b/255.f,color->a/255.f}; changed=ImGui::ColorEdit4(property->name.c_str(),v); *color={static_cast<uint8_t>(v[0]*255),static_cast<uint8_t>(v[1]*255),static_cast<uint8_t>(v[2]*255),static_cast<uint8_t>(v[3]*255)}; continuous=true; }
        else ImGui::TextDisabled("%s  (unsupported value)", property->name.c_str());
        ImGui::SameLine();
        if(ImGui::SmallButton("↶")){value=property->defaultValue.Clone();changed=true;continuous=false;}
        EditVariant(property->name.c_str(), property->name, std::move(before), std::move(value), changed, continuous);
        if (continuous && ImGui::IsItemDeactivatedAfterEdit() && m_propertyTransaction) { const Status status = m_propertyTransaction->Commit(); if (!status) EmitStatus(status); m_propertyTransaction.reset(); }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Binding");
    std::vector<std::string> bindingNames; for (const auto& [key, _] : node->properties) if (key.starts_with("bind.")) bindingNames.push_back(key);
    for (const auto& key : bindingNames) {
        std::string path = *node->properties[key].TryGet<std::string>();
        if (ImGui::InputText(key.c_str(), &path, ImGuiInputTextFlags_EnterReturnsTrue)) EditVariant(key.c_str(), key, node->properties[key], Variant(path), true, false);
        const std::string target=key.substr(5),formatterKey="formatter."+target;
        std::string formatter=node->properties.contains(formatterKey)&&node->properties[formatterKey].TryGet<std::string>()?*node->properties[formatterKey].TryGet<std::string>():"";
        if(ImGui::InputText(("formatter##"+target).c_str(),&formatter,ImGuiInputTextFlags_EnterReturnsTrue))
            EditVariant("formatter",formatterKey,node->properties.contains(formatterKey)?node->properties[formatterKey]:Variant{},Variant(formatter),true,false);
    }
    if (ImGui::Button("Add typed binding")) ImGui::OpenPopup("AddBinding");
    if (ImGui::BeginPopup("AddBinding")) { for (const auto* property : properties) if (HasFlag(property->flags, PropertyFlags::Bindable) && ImGui::MenuItem(property->name.c_str())) {
        EditVariant("binding", "bind." + property->name, Variant{}, Variant(std::string("viewModel.property")), true, false); ImGui::CloseCurrentPopup();
    } ImGui::EndPopup(); }
    if (!selectedAssetPath.empty() && node->type == "TextureRect" && ImGui::Button("Use selected asset"))
        EditVariant("path", "path", node->properties.contains("path") ? node->properties["path"] : Variant{}, Variant(selectedAssetPath), true, false);
    ImGui::TextDisabled("條件式 UI 請繫結 ViewModel computed property；Binding 不執行表達式。");
    ImGui::SeparatorText("事件");
    type=node->type;
    while(const auto* info=TypeRegistry::Global().Find(type)){
        for(const auto& signal:info->signals){const std::string key="event."+signal.name;std::string command=node->properties.contains(key)&&node->properties[key].TryGet<std::string>()?*node->properties[key].TryGet<std::string>():"";
            if(ImGui::InputText((signal.name+"##event").c_str(),&command,ImGuiInputTextFlags_EnterReturnsTrue))EditVariant(signal.name.c_str(),key,node->properties.contains(key)?node->properties[key]:Variant{},Variant(command),true,false);}
        type=info->base;if(type.empty())break;
    }
}

void UIDesigner::RebuildLayout() {
    m_layout.clear(); m_childPolicies.clear(); if (!m_document) return;
    resource::TypedDocument preview = m_document->Data();
    for (auto& node : preview.nodes) std::erase_if(node.properties, [](const auto& item) { return item.first.starts_with("bind.") || item.first.starts_with("formatter."); });
    auto scene = ui::InstantiateUIScene(preview, nullptr, m_formatters); if (!scene) return;
    Vec2 canvas{1280,720}; if (const auto it = preview.properties.find("canvasSize"); it != preview.properties.end()) if (const auto* size=it->second.TryGet<Vec2>()) canvas=*size;
    (void)scene.Value().root->Measure(canvas); scene.Value().root->Arrange({0,0,canvas.x,canvas.y});
    for (const auto& record : preview.nodes) {
        if (auto* control = dynamic_cast<ui::Control*>(scene.Value().root->Find(record.id))) {
            m_layout[record.id] = control->LayoutRect();
            m_childPolicies[record.id] = control->ChildPolicy();
        }
    }
}

Rect UIDesigner::SelectedRect() const { const auto it=m_layout.find(m_selected); return it==m_layout.end()?Rect{}:it->second; }

Vec2 UIDesigner::CanvasSize() const {
    if (m_document) {
        if (const auto it = m_document->Data().properties.find("canvasSize");
            it != m_document->Data().properties.end()) {
            if (const auto* value = it->second.TryGet<Vec2>()) return *value;
        }
    }
    return {1280, 720};
}

std::string UIDesigner::SelectionSummary() const {
    if (!m_document) return "未開啟 UI 文件";
    const auto* node = m_document->Find(m_selected);
    if (!node) return "未選取元件";
    if (m_selection.size() > 1) return std::to_string(m_selection.size()) + " 個元件";
    return node->name + "  ·  " + node->type;
}

ui::ChildLayoutPolicy UIDesigner::SelectedParentPolicy() const {
    if (!m_document) return ui::ChildLayoutPolicy::Free;
    const auto* node = m_document->Find(m_selected);
    if (!node || node->parent.Empty()) return ui::ChildLayoutPolicy::Free;
    const auto it = m_childPolicies.find(node->parent);
    return it == m_childPolicies.end() ? ui::ChildLayoutPolicy::Free : it->second;
}

Rect UIDesigner::ParentRect(const Uuid& nodeId) const {
    if (!m_document) return {};
    const auto* node = m_document->Find(nodeId);
    if (!node || node->parent.Empty()) return {0, 0, CanvasSize().x, CanvasSize().y};
    const auto it = m_layout.find(node->parent);
    return it == m_layout.end() ? Rect{0, 0, CanvasSize().x, CanvasSize().y} : it->second;
}

Uuid UIDesigner::HitTest(Vec2 canvas) const {
    if (!m_document) return {};
    for (auto it = m_document->Data().nodes.rbegin(); it != m_document->Data().nodes.rend(); ++it) {
        const auto rect = m_layout.find(it->id);
        if (rect == m_layout.end() || !rect->second.Contains(canvas.x, canvas.y)) continue;
        const auto visibility = it->properties.find("visibility");
        if (visibility != it->properties.end()) {
            if (const auto* value = visibility->second.TryGet<std::string>();
                value && *value != "Visible") continue;
        }
        return it->id;
    }
    return {};
}

Uuid UIDesigner::NearestFreeAncestor(const Uuid& nodeId) const {
    if (!m_document) return {};
    const auto* node = m_document->Find(nodeId);
    Uuid candidate = node ? node->parent : Uuid{};
    while (!candidate.Empty()) {
        const auto policy = m_childPolicies.find(candidate);
        if (policy == m_childPolicies.end() || policy->second == ui::ChildLayoutPolicy::Free)
            return candidate;
        const auto* parent = m_document->Find(candidate);
        candidate = parent ? parent->parent : Uuid{};
    }
    return {};
}

std::size_t UIDesigner::InsertionIndex(const Uuid& nodeId, Vec2 canvas) const {
    if (!m_document) return 0;
    const auto* node = m_document->Find(nodeId);
    if (!node) return 0;
    const auto policy = SelectedParentPolicy();
    std::vector<const resource::NodeRecord*> siblings;
    for (const auto* sibling : std::as_const(*m_document).Children(node->parent))
        if (sibling->id != nodeId) siblings.push_back(sibling);
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        const auto rect = m_layout.find(siblings[i]->id);
        if (rect == m_layout.end()) continue;
        const Rect value = rect->second;
        if (policy == ui::ChildLayoutPolicy::LinearX) {
            if (canvas.x < value.x + value.w * 0.5f) return i;
        } else if (policy == ui::ChildLayoutPolicy::Grid ||
                   policy == ui::ChildLayoutPolicy::Flow) {
            if (canvas.y < value.y + value.h * 0.5f ||
                (std::abs(canvas.y - (value.y + value.h * 0.5f)) < value.h * 0.4f &&
                 canvas.x < value.x + value.w * 0.5f)) return i;
        } else {
            if (canvas.y < value.y + value.h * 0.5f) return i;
        }
    }
    return siblings.size();
}

void UIDesigner::BeginFreeTransform(const Uuid& nodeId, Vec2 canvas, int handle) {
    if (!m_document) return;
    m_selected = nodeId;
    if (!m_selection.contains(nodeId)) m_selection={nodeId};
    m_dragStart = canvas;
    m_rectStart = SelectedRect();
    m_resizeHandle = handle;
    m_gesture = handle == 0 ? Gesture::Move : Gesture::Resize;
    m_groupMove = false;
    m_groupOffsetsStart.clear();
    if (handle == 0 && m_selection.size() > 1) {
        const auto* primary=m_document->Find(nodeId);
        for(const Uuid& id:m_selection){
            const auto* candidate=m_document->Find(id);
            if(!primary||!candidate||candidate->parent!=primary->parent)continue;
            const auto policy=m_childPolicies.find(candidate->parent);
            if(policy!=m_childPolicies.end()&&policy->second!=ui::ChildLayoutPolicy::Free)continue;
            if(const auto property=candidate->properties.find("offsets");property!=candidate->properties.end())
                if(const auto* value=property->second.TryGet<Rect>())m_groupOffsetsStart[id]=*value;
        }
        m_groupMove=m_groupOffsetsStart.size()>1;
        if(m_groupMove)return;
    }
    if (auto* node = m_document->Find(nodeId)) {
        if (const auto it = node->properties.find("offsets"); it != node->properties.end())
            if (const auto* value = it->second.TryGet<Rect>()) m_offsetsStart = *value;
        m_propertyTransaction = std::make_unique<PropertyEditTransaction>(
            *m_document, m_document->History(), nodeId, "offsets",
            handle == 0 ? "移動元件" : "調整元件大小");
    }
}

void UIDesigner::CommitManagedDrag(Vec2 canvas, bool detach) {
    if (!m_document) return;
    auto* node = m_document->Find(m_selected);
    if (!node) return;
    const Uuid oldParent = node->parent;
    const std::size_t oldIndex = m_document->ChildIndex(m_selected);
    const auto policy = SelectedParentPolicy();
    if (detach) {
        const Uuid freeParent = NearestFreeAncestor(m_selected);
        if (freeParent.Empty() || freeParent == oldParent) {
            m_canvasHint = "找不到可自由定位的父節點";
            return;
        }
        const Rect visual = SelectedRect();
        const Rect parent = m_layout.contains(freeParent)
                                ? m_layout.at(freeParent)
                                : Rect{0, 0, CanvasSize().x, CanvasSize().y};
        const Rect anchors{0, 0, 0, 0};
        const Rect offsets = ui::ControlLayoutMath::OffsetsForRect(parent, anchors, visual);
        const Variant oldAnchors = node->properties.contains("anchors")
                                       ? node->properties.at("anchors") : Variant{};
        const Variant oldOffsets = node->properties.contains("offsets")
                                       ? node->properties.at("offsets") : Variant{};
        auto command = std::make_unique<CompositeEditCommand>("抽離並移動元件");
        command->Add(std::make_unique<ReparentEditCommand>(
            "抽離元件", m_selected, oldParent, oldIndex, freeParent,
            m_document->Children(freeParent).size()));
        command->Add(std::make_unique<PropertyChangeCommand>(
            "重設錨點", m_selected, "anchors", oldAnchors, Variant(anchors),
            std::chrono::steady_clock::now(), false));
        command->Add(std::make_unique<PropertyChangeCommand>(
            "保持畫面位置", m_selected, "offsets", oldOffsets, Variant(offsets),
            std::chrono::steady_clock::now(), false));
        const Status status = m_document->History().Execute(std::move(command));
        if (!status) EmitStatus(status); else { m_canvasHint = "已抽離為自由佈局"; MarkEdited(true); }
        return;
    }
    if (policy == ui::ChildLayoutPolicy::SingleSlot ||
        policy == ui::ChildLayoutPolicy::RuntimeManaged) {
        m_canvasHint = std::string("位置由 ") + ui::ChildLayoutPolicyName(policy) +
                       "控制；按住 Ctrl 拖曳可抽離";
        return;
    }
    const std::size_t target = InsertionIndex(m_selected, canvas);
    if (target == oldIndex) return;
    auto command = std::make_unique<MoveChildEditCommand>(
        "重新排列元件", oldParent, m_selected, oldIndex, target);
    const Status status = m_document->History().Execute(std::move(command));
    if (!status) EmitStatus(status); else MarkEdited(true);
}

void UIDesigner::CancelCanvasGesture() {
    if(m_gesture==Gesture::Anchors&&m_document){(void)m_document->WriteProperty(m_selected,"anchors",Variant(m_anchorsStart));(void)m_document->WriteProperty(m_selected,"offsets",Variant(m_anchorOffsetsStart));m_anchorHandle=0;RebuildLayout();}
    if (m_groupMove && m_document) {
        for (const auto& [id, value] : m_groupOffsetsStart)
            (void)m_document->WriteProperty(id, "offsets", Variant(value));
        m_groupOffsetsStart.clear(); m_groupMove=false; RebuildLayout();
    }
    if (m_propertyTransaction) {
        const Status status = m_propertyTransaction->Cancel();
        if (!status) EmitStatus(status);
        m_propertyTransaction.reset();
    }
    m_gesture = Gesture::None;
    m_resizeHandle = 0;
    m_guideX = m_guideY = std::numeric_limits<float>::quiet_NaN();
}

void UIDesigner::RenderViewportToolbar() {
    const auto toolButton = [&](const char* label, DesignerTool tool) {
        const bool active = m_viewport.tool == tool;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) m_viewport.tool = tool;
        if (active) ImGui::PopStyleColor();
    };
    toolButton("選取 Q", DesignerTool::Select); ImGui::SameLine();
    toolButton("移動 W", DesignerTool::Move); ImGui::SameLine();
    toolButton("縮放 R", DesignerTool::Resize); ImGui::SameLine();
    toolButton("錨點 A", DesignerTool::Anchors); ImGui::SameLine();
    if (ImGui::Button("錨點預設")) ImGui::OpenPopup("DesignerAnchorPresets");
    if (ImGui::BeginPopup("DesignerAnchorPresets")) {
        const auto apply = [&](const char* label, Rect anchors) {
            if (!ImGui::MenuItem(label) || !m_document || m_selected == RootId()) return;
            auto* node = m_document->Find(m_selected); if (!node) return;
            const Rect visual = SelectedRect(); const Rect parent = ParentRect(m_selected);
            const Rect offsets = ui::ControlLayoutMath::OffsetsForRect(parent, anchors, visual);
            const Variant beforeAnchors = node->properties.contains("anchors") ? node->properties.at("anchors") : Variant{};
            const Variant beforeOffsets = node->properties.contains("offsets") ? node->properties.at("offsets") : Variant{};
            auto command = std::make_unique<CompositeEditCommand>("套用錨點預設");
            command->Add(std::make_unique<PropertyChangeCommand>("錨點",m_selected,"anchors",beforeAnchors,Variant(anchors),std::chrono::steady_clock::now(),false));
            command->Add(std::make_unique<PropertyChangeCommand>("保持位置",m_selected,"offsets",beforeOffsets,Variant(offsets),std::chrono::steady_clock::now(),false));
            const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
        };
        apply("左上", {0,0,0,0}); apply("置中", {.5f,.5f,.5f,.5f});
        apply("水平延展", {0,.5f,1,.5f}); apply("垂直延展", {.5f,0,.5f,1});
        apply("全區域", {0,0,1,1}); ImGui::EndPopup();
    }
    ImGui::SameLine(); ImGui::Checkbox("參考線", &m_viewport.smartGuides);
    ImGui::SameLine(); ImGui::Checkbox("格線 G", &m_viewport.gridVisible);
    ImGui::SameLine(); ImGui::Checkbox("吸附", &m_viewport.gridSnap);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110);
    int zoomPercent = static_cast<int>(std::round(m_viewport.zoom * 100.0f));
    if (ImGui::SliderInt("##DesignerZoom", &zoomPercent, 25, 400, "%d%%"))
        m_viewport.zoom = std::clamp(zoomPercent / 100.0f, .25f, 4.0f);
    ImGui::SameLine();
    if (ImGui::Button(m_viewport.interactivePreview ? "停止預覽" : "互動預覽"))
        m_viewport.interactivePreview = !m_viewport.interactivePreview;
    ImGui::SameLine();
    ImGui::TextDisabled("%s · %s", SelectionSummary().c_str(),
                        ui::ChildLayoutPolicyName(SelectedParentPolicy()));
    if (!m_canvasHint.empty()) {
        ImGui::TextColored(ImVec4(.95f,.72f,.30f,1), "%s", m_canvasHint.c_str());
    }
}

bool UIDesigner::CanvasInput(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
                             const std::string&) {
    if (!m_document || scale <= 0.0f) return false;
    ImGuiIO& io = ImGui::GetIO(); const ImVec2 mouse = io.MousePos;
    const Vec2 canvas{(mouse.x-p0.x)/scale,(mouse.y-p0.y)/scale};
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) m_viewport.tool=DesignerTool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_viewport.tool=DesignerTool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_viewport.tool=DesignerTool::Resize;
        if (ImGui::IsKeyPressed(ImGuiKey_A)) m_viewport.tool=DesignerTool::Anchors;
        if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            if (io.KeyShift) m_viewport.gridSnap=!m_viewport.gridSnap;
            else m_viewport.gridVisible=!m_viewport.gridVisible;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0)) {
            m_viewport.zoom=1.0f;m_viewport.pan={};return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !m_selected.Empty()) {
            const Rect selected=SelectedRect();
            const ImVec2 screenCenter{p0.x+(selected.x+selected.w*.5f)*scale,
                                      p0.y+(selected.y+selected.h*.5f)*scale};
            const ImVec2 viewportCenter{(viewport.Min.x+viewport.Max.x)*.5f,
                                        (viewport.Min.y+viewport.Max.y)*.5f};
            m_viewport.pan.x+=viewportCenter.x-screenCenter.x;
            m_viewport.pan.y+=viewportCenter.y-screenCenter.y;
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)||ImGui::IsKeyPressed(ImGuiKey_Backspace)) RemoveSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) CancelCanvasGesture();
        if (io.MouseWheel != 0.0f) {
            const float oldScale=scale; const float factor=std::pow(1.12f,io.MouseWheel);
            const float oldZoom=m_viewport.zoom; m_viewport.zoom=std::clamp(oldZoom*factor,.25f,4.0f);
            const float ratio=m_viewport.zoom/oldZoom;
            const ImVec2 center((viewport.Min.x+viewport.Max.x)*.5f,(viewport.Min.y+viewport.Max.y)*.5f);
            const Vec2 size=CanvasSize(); const float newScale=oldScale*ratio;
            m_viewport.pan.x=mouse.x-center.x+size.x*newScale*.5f-canvas.x*newScale;
            m_viewport.pan.y=mouse.y-center.y+size.y*newScale*.5f-canvas.y*newScale;
            return true;
        }
    }
    const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool panHeld=ImGui::IsMouseDown(ImGuiMouseButton_Middle)||(spaceHeld&&ImGui::IsMouseDown(ImGuiMouseButton_Left));
    if ((hovered||m_gesture==Gesture::Pan)&&panHeld) {
        m_gesture=Gesture::Pan; m_viewport.pan.x+=io.MouseDelta.x; m_viewport.pan.y+=io.MouseDelta.y; return true;
    }
    if(m_gesture==Gesture::Pan&&!panHeld)m_gesture=Gesture::None;
    m_hovered=hovered?HitTest(canvas):Uuid{};
    if(m_viewport.interactivePreview)return false;

    int handle=0;
    if(!m_selected.Empty()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const Rect r=SelectedRect();const float hs=7.0f/scale;
        const Vec2 points[8]{{r.x,r.y},{r.x+r.w*.5f,r.y},{r.x+r.w,r.y},{r.x+r.w,r.y+r.h*.5f},{r.x+r.w,r.y+r.h},{r.x+r.w*.5f,r.y+r.h},{r.x,r.y+r.h},{r.x,r.y+r.h*.5f}};
        for(int i=0;i<8;++i)if(std::abs(canvas.x-points[i].x)<=hs&&std::abs(canvas.y-points[i].y)<=hs){handle=i+1;break;}}
    int anchorHandle=0;
    if(m_viewport.tool==DesignerTool::Anchors&&!m_selected.Empty()&&m_selected!=RootId()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){
        const auto* node=m_document->Find(m_selected);Rect anchors{};if(node)if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;
        const Rect parent=ParentRect(m_selected);const float hs=8.0f/scale;const Vec2 points[4]{{parent.x+parent.w*anchors.x,parent.y+parent.h*anchors.y},{parent.x+parent.w*anchors.w,parent.y+parent.h*anchors.y},{parent.x+parent.w*anchors.w,parent.y+parent.h*anchors.h},{parent.x+parent.w*anchors.x,parent.y+parent.h*anchors.h}};
        for(int index=0;index<4;++index)if(std::abs(canvas.x-points[index].x)<=hs&&std::abs(canvas.y-points[index].y)<=hs){anchorHandle=index+1;break;}
    }
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&!spaceHeld){
        Uuid hit=(handle||anchorHandle)?m_selected:HitTest(canvas);
        if(io.KeyAlt&&!handle){
            std::vector<Uuid> hits;for(auto iterator=m_document->Data().nodes.rbegin();iterator!=m_document->Data().nodes.rend();++iterator){const auto found=m_layout.find(iterator->id);if(found==m_layout.end())continue;const Rect rect=found->second;if(canvas.x>=rect.x&&canvas.x<=rect.x+rect.w&&canvas.y>=rect.y&&canvas.y<=rect.y+rect.h)hits.push_back(iterator->id);}
            if(!hits.empty()){auto current=std::find(hits.begin(),hits.end(),m_selected);if(current==hits.end()||++current==hits.end())hit=hits.front();else hit=*current;}
        }
        if(!hit.Empty()){
            if(io.KeyCtrl)m_selection.insert(hit);else if(!m_selection.contains(hit))m_selection={hit};
            m_selected=hit;m_canvasHint.clear();const auto policy=SelectedParentPolicy();
            const auto* selectedNode=m_document->Find(hit);const bool locked=selectedNode&&selectedNode->properties.contains("editorLocked")&&selectedNode->properties.at("editorLocked").TryGet<bool>()&&*selectedNode->properties.at("editorLocked").TryGet<bool>();
            if(locked)m_canvasHint="此元件已在 Scene Tree 鎖定";
            else if(anchorHandle){const auto* anchorNode=m_document->Find(hit);m_anchorsStart={};m_anchorOffsetsStart={};if(anchorNode){if(const auto found=anchorNode->properties.find("anchors");found!=anchorNode->properties.end())if(const auto* value=found->second.TryGet<Rect>())m_anchorsStart=*value;if(const auto found=anchorNode->properties.find("offsets");found!=anchorNode->properties.end())if(const auto* value=found->second.TryGet<Rect>())m_anchorOffsetsStart=*value;}m_rectStart=SelectedRect();m_anchorHandle=anchorHandle;m_gesture=Gesture::Anchors;}
            else if(hit!=RootId()){if(policy==ui::ChildLayoutPolicy::Free)BeginFreeTransform(hit,canvas,handle);else{m_gesture=Gesture::Reorder;m_dragStart=canvas;m_reorderPreview=m_document->ChildIndex(hit);m_canvasHint=std::string("由 ")+ui::ChildLayoutPolicyName(policy)+" 控制；拖曳排序，Ctrl 拖曳抽離";}}}
    }
    m_guideX=m_guideY=std::numeric_limits<float>::quiet_NaN();
    if(m_gesture==Gesture::Anchors&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const Rect parent=ParentRect(m_selected);Rect anchors=m_anchorsStart;float x=parent.w>0?(canvas.x-parent.x)/parent.w:0,y=parent.h>0?(canvas.y-parent.y)/parent.h:0;
        const auto snap=[](float value){for(float target:{0.0f,.5f,1.0f})if(std::abs(value-target)<.035f)return target;return std::clamp(value,0.0f,1.0f);};if(m_viewport.smartGuides){x=snap(x);y=snap(y);}else{x=std::clamp(x,0.0f,1.0f);y=std::clamp(y,0.0f,1.0f);}
        if(m_anchorHandle==1||m_anchorHandle==4)anchors.x=std::min(x,anchors.w);else anchors.w=std::max(x,anchors.x);if(m_anchorHandle==1||m_anchorHandle==2)anchors.y=std::min(y,anchors.h);else anchors.h=std::max(y,anchors.y);
        const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(parent,anchors,m_rectStart);Status status=m_document->WriteProperty(m_selected,"anchors",Variant(anchors));if(status)status=m_document->WriteProperty(m_selected,"offsets",Variant(offsets));if(!status){EmitStatus(status);CancelCanvasGesture();}else MarkEdited();return true;
    }
    if(m_gesture==Gesture::Move&&m_groupMove&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        Vec2 delta{canvas.x-m_dragStart.x,canvas.y-m_dragStart.y};
        if(m_viewport.gridSnap&&!io.KeyAlt){delta.x=std::round(delta.x/m_gridSize)*m_gridSize;delta.y=std::round(delta.y/m_gridSize)*m_gridSize;}
        for(const auto& [id,before]:m_groupOffsetsStart){Rect value=before;value.x+=delta.x;value.y+=delta.y;const Status status=m_document->WriteProperty(id,"offsets",Variant(value));if(!status){EmitStatus(status);CancelCanvasGesture();return true;}}
        MarkEdited();return true;
    }
    if((m_gesture==Gesture::Move||m_gesture==Gesture::Resize)&&ImGui::IsMouseDown(ImGuiMouseButton_Left)&&m_propertyTransaction){
        Vec2 delta{canvas.x-m_dragStart.x,canvas.y-m_dragStart.y};Rect target=m_rectStart;
        if(m_gesture==Gesture::Move){target.x+=delta.x;target.y+=delta.y;}else{float left=target.x,top=target.y,right=target.x+target.w,bottom=target.y+target.h;
            if(m_resizeHandle==1||m_resizeHandle==7||m_resizeHandle==8)left+=delta.x;if(m_resizeHandle==3||m_resizeHandle==4||m_resizeHandle==5)right+=delta.x;
            if(m_resizeHandle>=1&&m_resizeHandle<=3)top+=delta.y;if(m_resizeHandle>=5&&m_resizeHandle<=7)bottom+=delta.y;
            if(right-left<8){if(m_resizeHandle==1||m_resizeHandle==7||m_resizeHandle==8)left=right-8;else right=left+8;}
            if(bottom-top<8){if(m_resizeHandle>=1&&m_resizeHandle<=3)top=bottom-8;else bottom=top+8;}target={left,top,right-left,bottom-top};}
        if(m_viewport.gridSnap&&!io.KeyAlt){const auto snap=[&](float value){return std::round(value/m_gridSize)*m_gridSize;};if(m_gesture==Gesture::Move){target.x=snap(target.x);target.y=snap(target.y);}else{const float right=snap(target.x+target.w),bottom=snap(target.y+target.h);target.x=snap(target.x);target.y=snap(target.y);target.w=std::max(8.0f,right-target.x);target.h=std::max(8.0f,bottom-target.y);}}
        if(m_viewport.smartGuides&&m_gesture==Gesture::Move&&!io.KeyAlt){std::vector<float> xs{0,CanvasSize().x*.5f,CanvasSize().x},ys{0,CanvasSize().y*.5f,CanvasSize().y};
            const auto* selected=m_document->Find(m_selected);for(const auto& [id,r]:m_layout){if(id==m_selected)continue;const auto* other=m_document->Find(id);if(selected&&other&&selected->parent==other->parent){xs.insert(xs.end(),{r.x,r.x+r.w*.5f,r.x+r.w});ys.insert(ys.end(),{r.y,r.y+r.h*.5f,r.y+r.h});}}
            const float threshold=6.0f/scale;float bestX=threshold,bestY=threshold,adjustX=0,adjustY=0;for(float line:xs)for(float edge:{target.x,target.x+target.w*.5f,target.x+target.w})if(std::abs(line-edge)<bestX){bestX=std::abs(line-edge);adjustX=line-edge;m_guideX=line;}for(float line:ys)for(float edge:{target.y,target.y+target.h*.5f,target.y+target.h})if(std::abs(line-edge)<bestY){bestY=std::abs(line-edge);adjustY=line-edge;m_guideY=line;}target.x+=adjustX;target.y+=adjustY;}
        Rect anchors{};if(const auto* node=m_document->Find(m_selected))if(const auto it=node->properties.find("anchors");it!=node->properties.end())if(const auto* value=it->second.TryGet<Rect>())anchors=*value;
        const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(m_selected),anchors,target);const Status status=m_propertyTransaction->Update(Variant(offsets));if(!status)EmitStatus(status);MarkEdited();return true;
    }
    if(m_gesture==Gesture::Reorder&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){m_reorderPreview=InsertionIndex(m_selected,canvas);return true;}
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        if(m_gesture==Gesture::Anchors){auto anchors=m_document->ReadProperty(m_selected,"anchors"),offsets=m_document->ReadProperty(m_selected,"offsets");auto command=std::make_unique<CompositeEditCommand>("調整錨點");if(anchors)command->Add(std::make_unique<PropertyChangeCommand>("錨點",m_selected,"anchors",Variant(m_anchorsStart),anchors.Value()));if(offsets)command->Add(std::make_unique<PropertyChangeCommand>("保持位置",m_selected,"offsets",Variant(m_anchorOffsetsStart),offsets.Value()));const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);m_anchorHandle=0;m_gesture=Gesture::None;MarkEdited();return true;}
        if(m_gesture==Gesture::Move&&m_groupMove){auto command=std::make_unique<CompositeEditCommand>("移動多個元件");for(const auto& [id,before]:m_groupOffsetsStart){auto after=m_document->ReadProperty(id,"offsets");if(after)command->Add(std::make_unique<PropertyChangeCommand>("移動元件",id,"offsets",Variant(before),after.Value()));}const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);m_groupOffsetsStart.clear();m_groupMove=false;m_gesture=Gesture::None;MarkEdited();return true;}
        if(m_gesture==Gesture::Move||m_gesture==Gesture::Resize){if(m_propertyTransaction){const Status status=m_propertyTransaction->Commit();if(!status)EmitStatus(status);m_propertyTransaction.reset();}m_gesture=Gesture::None;return true;}
        if(m_gesture==Gesture::Reorder){CommitManagedDrag(canvas,io.KeyCtrl);m_gesture=Gesture::None;return true;}}
    return false;
}

void UIDesigner::DrawOverlay(ImVec2 p0, float scale) {
    if (!m_document) return; ImDrawList* draw=ImGui::GetWindowDrawList();const Vec2 canvas=CanvasSize();const ImVec2 max{p0.x+canvas.x*scale,p0.y+canvas.y*scale};draw->PushClipRect(p0,max,true);
    if(m_viewport.gridVisible){float step=m_gridSize;while(step*scale<12)step*=2;int line=0;for(float x=0;x<=canvas.x;x+=step,++line)draw->AddLine({p0.x+x*scale,p0.y},{p0.x+x*scale,max.y},line%4==0?IM_COL32(92,110,135,75):IM_COL32(72,84,102,40));line=0;for(float y=0;y<=canvas.y;y+=step,++line)draw->AddLine({p0.x,p0.y+y*scale},{max.x,p0.y+y*scale},line%4==0?IM_COL32(92,110,135,75):IM_COL32(72,84,102,40));}
    if(!std::isnan(m_guideX))draw->AddLine({p0.x+m_guideX*scale,p0.y},{p0.x+m_guideX*scale,max.y},IM_COL32(255,92,180,230),1.5f);
    if(!std::isnan(m_guideY))draw->AddLine({p0.x,p0.y+m_guideY*scale},{max.x,p0.y+m_guideY*scale},IM_COL32(255,92,180,230),1.5f);
    for(const auto& node:m_document->Data().nodes){const auto it=m_layout.find(node.id);if(it==m_layout.end())continue;const bool selected=m_selection.contains(node.id),primary=node.id==m_selected,hover=node.id==m_hovered;if(!selected&&!hover&&!m_viewport.showAllOutlines)continue;const Rect r=it->second;const ImU32 color=selected?IM_COL32(71,140,191,255):IM_COL32(150,170,195,150);draw->AddRect({p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},color,3.0f,ImDrawFlags_None,selected?2.0f:1.0f);if(primary)draw->AddText({p0.x+r.x*scale+5,p0.y+r.y*scale-20},IM_COL32(220,232,245,255),node.name.c_str());}
    if(!m_selected.Empty()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const Rect r=SelectedRect();const ImVec2 points[8]{{p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h*.5f)*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h*.5f)*scale}};for(const auto& point:points)draw->AddRectFilled({point.x-4,point.y-4},{point.x+4,point.y+4},IM_COL32(225,235,248,255),1);}
    if(m_viewport.tool==DesignerTool::Anchors&&!m_selected.Empty()&&m_selected!=RootId()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=m_document->Find(m_selected);Rect anchors{};if(node)if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;const Rect parent=ParentRect(m_selected),selected=SelectedRect();const ImVec2 points[4]{{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale},{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale}};const ImVec2 corners[4]{{p0.x+selected.x*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+(selected.y+selected.h)*scale},{p0.x+selected.x*scale,p0.y+(selected.y+selected.h)*scale}};for(int index=0;index<4;++index){draw->AddLine(points[index],corners[index],IM_COL32(255,120,190,150));draw->AddQuadFilled({points[index].x,points[index].y-6},{points[index].x+6,points[index].y},{points[index].x,points[index].y+6},{points[index].x-6,points[index].y},IM_COL32(255,120,190,255));}}
    if(m_gesture==Gesture::Reorder){const auto* node=m_document->Find(m_selected);if(node){std::vector<const resource::NodeRecord*> siblings;for(const auto* sibling:std::as_const(*m_document).Children(node->parent))if(sibling->id!=m_selected)siblings.push_back(sibling);Rect marker{};const auto policy=SelectedParentPolicy();if(!siblings.empty()){if(m_reorderPreview<siblings.size()&&m_layout.contains(siblings[m_reorderPreview]->id))marker=m_layout.at(siblings[m_reorderPreview]->id);else if(m_layout.contains(siblings.back()->id)){marker=m_layout.at(siblings.back()->id);if(policy==ui::ChildLayoutPolicy::LinearX)marker.x+=marker.w;else marker.y+=marker.h;}}if(policy==ui::ChildLayoutPolicy::LinearX)draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+marker.x*scale,p0.y+(marker.y+marker.h)*scale},IM_COL32(71,140,191,255),3);else draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+(marker.x+marker.w)*scale,p0.y+marker.y*scale},IM_COL32(71,140,191,255),3);}}
    draw->PopClipRect();
}

void UIDesigner::AddImageAt(float x,float y,const std::string& image){AddNode("TextureRect",{x,y},image);}

void UIDesigner::RenderAnimation() {
    if (!m_document) { ImGui::TextDisabled("Open a UI scene to edit animation."); return; }
    auto durationValue=m_document->ReadProperty(m_document->DocumentId(),"animation.duration");
    Variant durationBefore=durationValue&&durationValue.Value().Type()!=VariantType::Null?durationValue.Value():Variant(1.0);
    float duration=durationBefore.TryGet<double>()?static_cast<float>(*durationBefore.TryGet<double>()):1.0f;
    ImGui::TextDisabled("AnimationPlayer  •  typed tracks  •  %.2fs", duration);
    if(ImGui::DragFloat("Duration",&duration,.05f,0,120,"%.2fs")){
        auto command=std::make_unique<PropertyChangeCommand>("Change animation duration",m_document->DocumentId(),"animation.duration",durationBefore,Variant(static_cast<double>(duration)));
        const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
    }
    auto loopValue=m_document->ReadProperty(m_document->DocumentId(),"animation.loop");Variant loopBefore=loopValue&&loopValue.Value().Type()!=VariantType::Null?loopValue.Value():Variant(false);bool loop=loopBefore.TryGet<bool>()?*loopBefore.TryGet<bool>():false;
    if(ImGui::Checkbox("Loop",&loop)){auto command=std::make_unique<PropertyChangeCommand>("Toggle animation loop",m_document->DocumentId(),"animation.loop",loopBefore,Variant(loop));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
    auto tracksValue=m_document->ReadProperty(m_document->DocumentId(),"animation.tracks");Variant tracksBefore=tracksValue&&tracksValue.Value().Type()!=VariantType::Null?tracksValue.Value():Variant(VariantArray{});
    if (ImGui::Button("Add property track") && !m_selected.Empty()) {
        Variant tracks=tracksBefore.Clone();auto* array=tracks.AsArray();Variant value=Rect{};if(auto current=m_document->ReadProperty(m_selected,"offsets");current&&current.Value().Type()!=VariantType::Null)value=current.Value();
        VariantArray keys{Variant(VariantObject{{"time",Variant(0.0)},{"value",value},{"ease",Variant(std::string("Linear"))}}),
                          Variant(VariantObject{{"time",Variant(static_cast<double>(std::max(.25f,duration)))},{"value",value},{"ease",Variant(std::string("EaseInOut"))}})};
        array->push_back(Variant(VariantObject{{"node",Variant(m_selected)},{"property",Variant(std::string("offsets"))},{"keys",Variant(std::move(keys))}}));
        auto command=std::make_unique<PropertyChangeCommand>("Add animation track",m_document->DocumentId(),"animation.tracks",tracksBefore,std::move(tracks));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
    }
    const auto refreshed=m_document->ReadProperty(m_document->DocumentId(),"animation.tracks");const VariantArray* tracks=refreshed?refreshed.Value().AsArray():nullptr;
    if (tracks) {
        for (std::size_t i = 0; i < tracks->size(); ++i) {
            const auto* track = (*tracks)[i].AsObject();
            if (!track) {
                ImGui::TextColored({1.0f, .45f, .35f, 1.0f}, "Track %zu is not an object", i);
                continue;
            }
            const auto nodeIt = track->find("node");
            const auto propertyIt = track->find("property");
            const auto keysIt = track->find("keys");
            const auto* node = nodeIt == track->end() ? nullptr : nodeIt->second.TryGet<Uuid>();
            const auto* property = propertyIt == track->end()
                                       ? nullptr
                                       : propertyIt->second.TryGet<std::string>();
            const auto* keys = keysIt == track->end() ? nullptr : keysIt->second.AsArray();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode("track", "%s • %s",
                                node ? node->ToString().substr(0, 8).c_str() : "invalid",
                                property ? property->c_str() : "invalid")) {
                if (!node || !property || !keys) {
                    ImGui::TextColored({1.0f, .45f, .35f, 1.0f},
                                       "Malformed typed track; see Problems for details.");
                } else {
                    ImGui::Text("%zu typed keys", keys->size());
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void UIDesigner::RenderTheme(){
    if(!m_document){ImGui::TextDisabled("Open a UI scene to edit its embedded theme.");return;}
    auto result=m_document->ReadProperty(m_document->DocumentId(),"theme.styles");Variant before=result&&result.Value().Type()!=VariantType::Null?result.Value():Variant(VariantObject{});Variant after=before.Clone();auto* styles=after.AsObject();
    if(!styles){ImGui::TextColored({1.0f,.45f,.35f,1.0f},"theme.styles must be a typed object; see Problems for details.");return;}
    if(!styles->contains("Default"))(*styles)["Default"]=Variant(VariantObject{{"background",Variant(Color{28,31,40,255})},{"border",Variant(Color{65,72,91,255})},{"text",Variant(Color{236,239,244,255})},{"cornerRadius",Variant(6.0)},{"font",Variant(std::string("Content/Fonts/NotoSansTC-Bold.ttf"))},{"fontSize",Variant(std::int64_t{24})}});
    auto* style=(*styles)["Default"].AsObject();if(!style){ImGui::TextColored({1.0f,.45f,.35f,1.0f},"Default theme style must be a typed object; see Problems for details.");return;}bool changed=false;
    auto colorField=[&](const char* label,const char* key,Color fallback){Color color=fallback;if(const auto it=style->find(key);it!=style->end())if(const auto* typed=it->second.TryGet<Color>())color=*typed;float value[4]{color.r/255.f,color.g/255.f,color.b/255.f,color.a/255.f};if(ImGui::ColorEdit4(label,value)){(*style)[key]=Color{static_cast<uint8_t>(value[0]*255),static_cast<uint8_t>(value[1]*255),static_cast<uint8_t>(value[2]*255),static_cast<uint8_t>(value[3]*255)};changed=true;}};
    ImGui::SeparatorText("Default style");colorField("Background","background",Color{28,31,40,255});colorField("Border","border",Color{65,72,91,255});colorField("Text","text",Color{236,239,244,255});
    double radius=6.0;if(const auto it=style->find("cornerRadius");it!=style->end())if(const auto* typed=it->second.TryGet<double>())radius=*typed;float radiusValue=static_cast<float>(radius);if(ImGui::DragFloat("Corner radius",&radiusValue,.25f,0,64)){(*style)["cornerRadius"]=static_cast<double>(radiusValue);changed=true;}
    std::string font="Content/Fonts/NotoSansTC-Bold.ttf";if(const auto it=style->find("font");it!=style->end())if(const auto* typed=it->second.TryGet<std::string>())font=*typed;if(ImGui::InputText("Font",&font,ImGuiInputTextFlags_EnterReturnsTrue)){(*style)["font"]=font;changed=true;}
    if(changed){auto command=std::make_unique<PropertyChangeCommand>("Edit UI theme",m_document->DocumentId(),"theme.styles",before,std::move(after));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
    ImGui::TextDisabled("Theme edits are embedded typed data and previewed after reload.");
}

}  // namespace px::editor
