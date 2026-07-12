#include "Editor/Tools/UIDesigner/UIDesigner.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/ActionRegistry.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <new>
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

bool AppendCapturedNodes(const VariantObject& value,const Uuid& parent,std::vector<resource::NodeRecord>& output){
    const auto idIt=value.find("id"),nameIt=value.find("name"),typeIt=value.find("type"),propertiesIt=value.find("properties");
    if(idIt==value.end()||nameIt==value.end()||typeIt==value.end()||propertiesIt==value.end())return false;
    const auto* id=idIt->second.TryGet<Uuid>();const auto* name=nameIt->second.TryGet<std::string>();const auto* type=typeIt->second.TryGet<std::string>();const auto* properties=propertiesIt->second.AsObject();if(!id||!name||!type||!properties)return false;
    output.push_back({*id,parent,*name,*type,*properties});
    if(const auto childrenIt=value.find("children");childrenIt!=value.end())if(const auto* children=childrenIt->second.AsArray())for(const auto& child:*children){const auto* object=child.AsObject();if(!object||!AppendCapturedNodes(*object,*id,output))return false;}
    return true;
}

VariantObject CaptureExpandedNodes(const std::vector<resource::NodeRecord>& nodes, const Uuid& id) {
    const auto found=std::find_if(nodes.begin(),nodes.end(),[&](const auto& node){return node.id==id;});
    if(found==nodes.end())return {};
    VariantArray children;
    for(const auto& child:nodes)if(child.parent==id)children.emplace_back(CaptureExpandedNodes(nodes,child.id));
    return {{"id",found->id},{"name",found->name},{"type",found->type},
            {"properties",VariantObject(found->properties)},{"children",std::move(children)}};
}
}

UIDesigner::UIDesigner() { const Status status = ui::RegisterBuiltinUITypes(); if (!status) EmitStatus(status); }

UIDesigner& UIDesigner::operator=(UIDesigner&& other) noexcept {
    if(this==&other)return *this;
    // PropertyEditTransaction refers to m_document. Destroy in normal reverse-member order
    // before replacing the designer; default memberwise move-assignment would replace the
    // document first and leave an active transaction pointing at freed memory.
    this->~UIDesigner();
    ::new (static_cast<void*>(this)) UIDesigner(std::move(other));
    return *this;
}

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
    if (type == "TextureRect" && !image.empty()) {properties["path"] = Variant(std::move(image));properties["scaleMode"]=Variant(std::string("Fit"));properties["lockAspectRatio"]=Variant(true);}
    return {{"id", Variant(Uuid::Random())}, {"name", Variant(std::move(name))}, {"type", Variant(std::move(type))},
            {"properties", Variant(std::move(properties))}, {"children", Variant(VariantArray{})}};
}

void UIDesigner::RegenerateIds(VariantObject& subtree) {
    subtree["id"] = Variant(Uuid::Random());
    if (auto* children = subtree["children"].AsArray()) for (auto& value : *children) if (auto* child = value.AsObject()) RegenerateIds(*child);
}

void UIDesigner::AddNode(std::string type, Vec2 canvas, std::string image) {
    if (!m_document) return; const Uuid parent = ParentForNewNode();
    float w = type == "TextureRect" ? 320.0f : 180.0f, h = type == "TextureRect" ? 180.0f : 52.0f;
    if(type=="TextureRect"&&!image.empty()&&m_imageSizeResolver)if(const auto size=m_imageSizeResolver(image);size&&size->x>0&&size->y>0){w=size->x;h=size->y;const Vec2 canvasSize=CanvasSize();const float fit=std::min({1.0f,(canvasSize.x-40.0f)/w,(canvasSize.y-40.0f)/h});w*=fit;h*=fit;}
    Rect offsets{canvas.x - w * 0.5f, canvas.y - h * 0.5f, w, h};
    if (canvas == Vec2{}) offsets = {20,20,w,h};
    VariantObject subtree = NewSubtree(type, UniqueName(*m_document, type), {0,0,0,0}, offsets, std::move(image));
    const Uuid id = *subtree["id"].TryGet<Uuid>();
    auto command = std::make_unique<SubtreeEditCommand>("Add " + type, SubtreeOperation::Insert, id, parent,
                                                        m_document->Children(parent).size(), std::move(subtree));
    const Status status = m_document->History().Execute(std::move(command)); if (!status) return EmitStatus(status);
    m_selected = id; MarkEdited(true);
}

void UIDesigner::RenderAddControlPalette(Vec2 canvasPosition){
    ImGui::InputTextWithHint("##control-search","Search controls…",m_paletteFilter,sizeof(m_paletteFilter));
    const std::string filter=m_paletteFilter;const auto category=[](std::string_view type)->std::string_view{if(type.find("Container")!=std::string_view::npos)return "Layout";if(type=="Button"||type.find("Input")!=std::string_view::npos)return "Input";if(type=="Label"||type=="TextureRect"||type=="Panel")return "Display";return "Data";};
    for(const char* group:{"Display","Input","Layout","Data"}){bool header=false;for(const auto* type:TypeRegistry::Global().TypesDerivedFrom("Control")){if(type->name=="Control"||category(type->name)!=group)continue;if(!filter.empty()&&type->name.find(filter)==std::string::npos)continue;if(!header){ImGui::SeparatorText(group);header=true;}if(ImGui::Selectable(type->name.c_str())){AddNode(type->name,canvasPosition);ImGui::CloseCurrentPopup();}}}
}

void UIDesigner::RemoveSelected() {
    if(!m_document)return;std::unordered_set<Uuid,UuidHash> requested=m_selection;if(requested.empty()&&!m_selected.Empty())requested.insert(m_selected);requested.erase(RootId());
    if(requested.empty()){m_canvasHint="根節點不能刪除";return;}
    std::vector<Uuid> targets;for(const Uuid& id:requested){const auto* node=m_document->Find(id);bool covered=false;for(const auto* parent=node?m_document->Find(node->parent):nullptr;parent;parent=m_document->Find(parent->parent))if(requested.contains(parent->id)){covered=true;break;}if(node&&!covered)targets.push_back(id);}
    auto command=std::make_unique<CompositeEditCommand>(targets.size()>1?"Delete UI controls":"Delete UI control");Uuid fallback=RootId();
    for(const Uuid& id:targets){const auto* node=m_document->Find(id);if(!node)continue;fallback=node->parent;auto captured=m_document->CaptureSubtree(id);if(!captured){EmitStatus(Status::Fail(captured.Diagnostics()));return;}command->Add(std::make_unique<SubtreeEditCommand>("Delete "+node->name,SubtreeOperation::Remove,id,node->parent,m_document->ChildIndex(id),captured.TakeValue()));}
    if(command->Empty())return;const Status status=m_document->History().Execute(std::move(command));if(!status)return EmitStatus(status);
    m_selected=fallback;m_selection={fallback};m_canvasHint.clear();MarkEdited(true);
}

void UIDesigner::DuplicateSelected() {
    if (!m_document || m_selected.Empty()) return;
    if(m_selection.size()>1){std::vector<Uuid> targets;for(const Uuid& id:m_selection){if(id==RootId())continue;const auto* node=m_document->Find(id);bool covered=false;for(const auto* parent=node?m_document->Find(node->parent):nullptr;parent;parent=m_document->Find(parent->parent))if(m_selection.contains(parent->id)){covered=true;break;}if(node&&!covered)targets.push_back(id);}auto command=std::make_unique<CompositeEditCommand>("Duplicate UI controls");std::unordered_set<Uuid,UuidHash> created;for(Uuid id:targets){const auto* node=m_document->Find(id);auto captured=m_document->CaptureSubtree(id);if(!node||!captured)continue;RegenerateIds(captured.Value());if(auto* name=captured.Value()["name"].TryGet<std::string>())*name=UniqueName(*m_document,*name+"Copy");const Uuid newId=*captured.Value()["id"].TryGet<Uuid>();created.insert(newId);command->Add(std::make_unique<SubtreeEditCommand>("Duplicate "+node->name,SubtreeOperation::Insert,newId,node->parent,m_document->ChildIndex(id)+1,captured.TakeValue()));}if(command->Empty())return;const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else{m_selection=std::move(created);if(!m_selection.empty())m_selected=*m_selection.begin();MarkEdited(true);}return;}
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

void UIDesigner::CopySelected() {
    if (!m_document || m_selected.Empty()) return;
    auto captured=m_document->CaptureSubtree(m_selected);if(captured)m_clipboardSubtree=captured.TakeValue();
}

void UIDesigner::PasteClipboard(Vec2 canvasPosition) {
    if(!m_document||m_clipboardSubtree.empty())return;
    VariantObject subtree=m_clipboardSubtree;RegenerateIds(subtree);
    if(auto* name=subtree["name"].TryGet<std::string>())*name=UniqueName(*m_document,*name+"Copy");
    if(canvasPosition!=Vec2{})if(auto* properties=subtree["properties"].AsObject())if(auto found=properties->find("offsets");found!=properties->end())if(auto* rect=found->second.TryGet<Rect>()){rect->x=canvasPosition.x;rect->y=canvasPosition.y;}
    const Uuid id=*subtree["id"].TryGet<Uuid>();const Uuid parent=ParentForNewNode();
    auto command=std::make_unique<SubtreeEditCommand>("Paste UI Control",SubtreeOperation::Insert,id,parent,m_document->Children(parent).size(),std::move(subtree));
    const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else{m_selected=id;m_selection={id};MarkEdited(true);}
}

Result<resource::TypedDocument> UIDesigner::LoadReferencedUI(const ResourceRefValue& reference) const {
    std::filesystem::path candidate=reference.lastKnownPath;
    if(candidate.is_relative()&&m_document){auto parent=m_document->Path().parent_path();while(!parent.empty()){const auto resolved=parent/candidate;if(std::filesystem::exists(resolved)){candidate=resolved;break;}const auto next=parent.parent_path();if(next==parent)break;parent=next;}}
    std::ifstream input(candidate,std::ios::binary);
    if(!input)return Result<resource::TypedDocument>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3030",.category="Editor.UIDesigner",.message="Referenced UI resource not found: "+candidate.string()});
    std::ostringstream buffer;buffer<<input.rdbuf();return resource::ParseTypedDocument(buffer.str(),candidate.string());
}

void UIDesigner::ResetComponentOverride(const std::string& sourceNode,const std::string& property) {
    if(!m_document)return;const auto* node=m_document->Find(m_selected);if(!node||node->type!="ComponentInstance")return;
    const auto found=node->properties.find("overrides");if(found==node->properties.end()||!found->second.AsObject())return;
    VariantObject changed=*found->second.AsObject();
    if(sourceNode.empty())changed.clear();
    else if(auto source=changed.find(sourceNode);source!=changed.end()){
        if(property.empty())changed.erase(source);
        else if(auto* values=source->second.AsObject()){values->erase(property);if(values->empty())changed.erase(source);}
    }
    auto command=std::make_unique<PropertyChangeCommand>(property.empty()?"Reset component overrides":"Reset component override",m_selected,"overrides",found->second,Variant(std::move(changed)),std::chrono::steady_clock::now(),false);
    const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
}

void UIDesigner::DetachSelectedComponent() {
    if(!m_document)return;const auto* node=m_document->Find(m_selected);if(!node||node->type!="ComponentInstance")return;
    resource::TypedDocument temporary=m_document->Data();temporary.nodes={*node};temporary.nodes.front().parent={};
    const ui::UIDocumentLoader loader=[this](const ResourceRefValue& reference){return LoadReferencedUI(reference);};
    auto expanded=ui::ExpandUIComponents(temporary,loader);if(!expanded){EmitStatus(Status::Fail(expanded.Diagnostics()));return;}
    VariantObject materialized=CaptureExpandedNodes(expanded.Value().document.nodes,m_selected);if(materialized.empty())return;
    auto original=m_document->CaptureSubtree(m_selected);if(!original)return;
    const Uuid parent=node->parent;const std::size_t index=m_document->ChildIndex(m_selected);
    auto command=std::make_unique<CompositeEditCommand>("Detach UI Component");
    command->Add(std::make_unique<SubtreeEditCommand>("Remove component instance",SubtreeOperation::Remove,m_selected,parent,index,original.TakeValue()));
    command->Add(std::make_unique<SubtreeEditCommand>("Materialize component",SubtreeOperation::Insert,m_selected,parent,index,std::move(materialized)));
    const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited(true);
}

void UIDesigner::ChangeSelectedLayer(LayerAction action) {
    if(!m_document||m_selected.Empty()||m_selected==RootId())return;const auto* node=m_document->Find(m_selected);if(!node)return;
    const auto siblings=m_document->Children(node->parent);if(siblings.size()<2)return;const std::size_t oldIndex=m_document->ChildIndex(m_selected);std::size_t target=oldIndex;
    switch(action){case LayerAction::BringForward:target=std::min(oldIndex+1,siblings.size()-1);break;case LayerAction::SendBackward:target=oldIndex?oldIndex-1:0;break;case LayerAction::BringToFront:target=siblings.size()-1;break;case LayerAction::SendToBack:target=0;break;}
    if(target==oldIndex)return;auto command=std::make_unique<MoveChildEditCommand>("Change UI layer",node->parent,m_selected,oldIndex,target);const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited(true);
}

void UIDesigner::SetSelectedAsBackground(bool lock){
    if(!m_document)return;auto* node=m_document->Find(m_selected);if(!node||node->type!="TextureRect"){m_canvasHint="請先選取圖片元件";return;}
    const Uuid root=RootId();auto command=std::make_unique<CompositeEditCommand>(lock?"Set and lock background":"Set background");
    if(node->parent==root){const std::size_t old=m_document->ChildIndex(node->id);if(old)command->Add(std::make_unique<MoveChildEditCommand>("Send background to back",root,node->id,old,0));}
    else command->Add(std::make_unique<ReparentEditCommand>("Move background to root",node->id,node->parent,m_document->ChildIndex(node->id),root,0));
    const auto add=[&](const char* property,Variant value){const Variant before=node->properties.contains(property)?node->properties.at(property):Variant{};if(before!=value)command->Add(std::make_unique<PropertyChangeCommand>("Set background",node->id,property,before,std::move(value),std::chrono::steady_clock::now(),false));};
    add("anchors",Variant(Rect{0,0,1,1}));add("offsets",Variant(Rect{0,0,0,0}));add("scaleMode",Variant(std::string("Fill")));if(lock)add("editorLocked",Variant(true));
    if(command->Empty())return;const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else{m_canvasHint=lock?"已設為背景並鎖定":"已設為背景";MarkEdited(true);}
}

void UIDesigner::RestoreSelectedImageSize(){
    if(!m_document||!m_imageSizeResolver)return;auto* node=m_document->Find(m_selected);if(!node||node->type!="TextureRect")return;
    const auto path=node->properties.find("path");if(path==node->properties.end()||!path->second.TryGet<std::string>())return;const auto size=m_imageSizeResolver(*path->second.TryGet<std::string>());if(!size){m_canvasHint="無法讀取圖片原始尺寸";return;}
    Rect visual=SelectedRect();visual.w=size->x;visual.h=size->y;Rect anchors{};if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;
    const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(m_selected),anchors,visual);const Variant before=node->properties.contains("offsets")?node->properties.at("offsets"):Variant{};
    auto command=std::make_unique<PropertyChangeCommand>("Restore image size",m_selected,"offsets",before,Variant(offsets),std::chrono::steady_clock::now(),false);const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
}

void UIDesigner::AlignSelection(AlignAction action){
    if(!m_document||m_selected.Empty())return;const auto* primary=m_document->Find(m_selected);if(!primary||m_selected==RootId())return;
    std::vector<Uuid> ids;for(const Uuid& id:m_selection){const auto* node=m_document->Find(id);if(!node||node->parent!=primary->parent||id==RootId())continue;const auto policy=m_childPolicies.find(node->parent);if(policy!=m_childPolicies.end()&&policy->second!=ui::ChildLayoutPolicy::Free)continue;ids.push_back(id);}
    if(ids.empty()){m_canvasHint="此佈局不允許自由對齊";return;}if((action==AlignAction::DistributeH||action==AlignAction::DistributeV)&&ids.size()<3){m_canvasHint="平均分布至少需要三個元件";return;}
    std::unordered_map<Uuid,Rect,UuidHash> targets;for(const Uuid& id:ids)targets[id]=m_layout.at(id);
    const Rect reference=ids.size()==1?ParentRect(m_selected):m_layout.at(m_selected);
    if(action==AlignAction::DistributeH){std::sort(ids.begin(),ids.end(),[&](Uuid a,Uuid b){return targets[a].x<targets[b].x;});const float left=targets[ids.front()].x,right=targets[ids.back()].x+targets[ids.back()].w;float widths=0;for(Uuid id:ids)widths+=targets[id].w;const float gap=(right-left-widths)/(ids.size()-1);float x=left;for(Uuid id:ids){targets[id].x=x;x+=targets[id].w+gap;}}
    else if(action==AlignAction::DistributeV){std::sort(ids.begin(),ids.end(),[&](Uuid a,Uuid b){return targets[a].y<targets[b].y;});const float top=targets[ids.front()].y,bottom=targets[ids.back()].y+targets[ids.back()].h;float heights=0;for(Uuid id:ids)heights+=targets[id].h;const float gap=(bottom-top-heights)/(ids.size()-1);float y=top;for(Uuid id:ids){targets[id].y=y;y+=targets[id].h+gap;}}
    else for(Uuid id:ids){Rect& r=targets[id];if(action==AlignAction::Left)r.x=reference.x;else if(action==AlignAction::HCenter)r.x=reference.x+(reference.w-r.w)*.5f;else if(action==AlignAction::Right)r.x=reference.x+reference.w-r.w;else if(action==AlignAction::Top)r.y=reference.y;else if(action==AlignAction::VCenter)r.y=reference.y+(reference.h-r.h)*.5f;else if(action==AlignAction::Bottom)r.y=reference.y+reference.h-r.h;}
    auto command=std::make_unique<CompositeEditCommand>("Align UI controls");for(Uuid id:ids){const auto* node=m_document->Find(id);Rect anchors{};if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(id),anchors,targets[id]);const Variant before=node->properties.contains("offsets")?node->properties.at("offsets"):Variant{};if(before!=Variant(offsets))command->Add(std::make_unique<PropertyChangeCommand>("Align control",id,"offsets",before,Variant(offsets),std::chrono::steady_clock::now(),false));}
    if(command->Empty())return;const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
}

Status UIDesigner::CreateComponentFromSelected(const std::filesystem::path& path){
    if(!m_document||m_selected.Empty()||m_selected==RootId()||!m_componentWriter)return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3041",.category="Editor.UIDesigner",.message="Select a non-root Control before creating a component"});
    const auto* selected=m_document->Find(m_selected);if(!selected)return Status::Ok();auto captured=m_document->CaptureSubtree(m_selected);if(!captured)return Status::Fail(captured.Diagnostics());
    resource::TypedDocument component;component.kind=resource::DocumentKind::Scene;component.id=Uuid::Random();component.type="UIComponent";component.properties["canvasSize"]=Vec2{std::max(1.0f,SelectedRect().w),std::max(1.0f,SelectedRect().h)};if(!AppendCapturedNodes(captured.Value(),{},component.nodes))return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3042",.category="Editor.UIDesigner",.message="Selected subtree could not be serialized as a component"});
    auto written=m_componentWriter(path,resource::WriteTypedDocument(component));if(!written)return Status::Fail(written.Diagnostics());
    VariantObject instanceProperties=selected->properties;instanceProperties["component"]=written.Value();instanceProperties["overrides"]=VariantObject{};
    VariantObject instance{{"id",m_selected},{"name",selected->name},{"type",std::string("ComponentInstance")},{"properties",std::move(instanceProperties)},{"children",VariantArray{}}};
    const Uuid parent=selected->parent;const std::size_t index=m_document->ChildIndex(m_selected);auto command=std::make_unique<CompositeEditCommand>("Create UI Component");command->Add(std::make_unique<SubtreeEditCommand>("Remove source subtree",SubtreeOperation::Remove,m_selected,parent,index,captured.Value()));command->Add(std::make_unique<SubtreeEditCommand>("Insert component instance",SubtreeOperation::Insert,m_selected,parent,index,std::move(instance)));const Status status=m_document->History().Execute(std::move(command));if(status)MarkEdited(true);return status;
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
        if(!m_selection.contains(record.id)){m_selected=record.id;m_selection={record.id};}else m_selected=record.id;
        if(ImGui::MenuItem(visible?"隱藏":"顯示")){const Variant before=record.properties.contains("visible")?record.properties["visible"]:Variant(true);auto command=std::make_unique<PropertyChangeCommand>("切換可見性",record.id,"visible",before,Variant(!visible));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
        if(ImGui::MenuItem(locked?"解除鎖定":"鎖定")){const Variant before=record.properties.contains("editorLocked")?record.properties["editorLocked"]:Variant(false);auto command=std::make_unique<PropertyChangeCommand>("切換鎖定",record.id,"editorLocked",before,Variant(!locked));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
        ImGui::Separator();
        if(ImGui::MenuItem("重新命名","F2")){m_selected=record.id;m_selection={record.id};std::snprintf(m_treeRename,sizeof(m_treeRename),"%s",record.name.c_str());m_treeRenameOpen=true;}
        if(ImGui::MenuItem("複製","Ctrl+D"))DuplicateSelected();
        if(record.id!=RootId()&&ImGui::MenuItem("建立 Component…")){m_selected=record.id;m_selection={record.id};m_createComponentOpen=true;}
        if(record.type=="ComponentInstance"){
            if(const auto component=record.properties.find("component");component!=record.properties.end())if(const auto* reference=component->second.TryGet<ResourceRefValue>())if(m_openResource&&ImGui::MenuItem("Edit Main")){m_selected=record.id;m_selection={record.id};m_openResource(*reference);}
            if(ImGui::MenuItem("Reset All Overrides")){m_selected=record.id;m_selection={record.id};ResetComponentOverride();}
            if(ImGui::MenuItem("Detach")){m_selected=record.id;m_selection={record.id};DetachSelectedComponent();}
        }
        if(record.id!=RootId()&&ImGui::BeginMenu("圖層順序")){m_selected=record.id;m_selection={record.id};if(ImGui::MenuItem("置頂","Ctrl+Shift+]"))ChangeSelectedLayer(LayerAction::BringToFront);if(ImGui::MenuItem("上移一層","Ctrl+]"))ChangeSelectedLayer(LayerAction::BringForward);if(ImGui::MenuItem("下移一層","Ctrl+["))ChangeSelectedLayer(LayerAction::SendBackward);if(ImGui::MenuItem("置底","Ctrl+Shift+["))ChangeSelectedLayer(LayerAction::SendToBack);ImGui::EndMenu();}
        if(record.id!=RootId()&&ImGui::MenuItem("刪除","Delete"))RemoveSelected();
        ImGui::EndPopup();
    }
    if (open) { for (auto it=children.rbegin();it!=children.rend();++it) RenderTreeNode(**it); ImGui::TreePop(); }
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
        RenderAddControlPalette();
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
    if(m_createComponentOpen)ImGui::OpenPopup("建立 UI Component");
    if(ImGui::BeginPopupModal("建立 UI Component",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::InputText("Path",m_componentPath,sizeof(m_componentPath));if(ImGui::Button("建立",ImVec2(100,0))){const Status status=CreateComponentFromSelected(m_componentPath);if(!status)EmitStatus(status);else{m_createComponentOpen=false;ImGui::CloseCurrentPopup();}}ImGui::SameLine();if(ImGui::Button("取消",ImVec2(100,0))){m_createComponentOpen=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
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
    if(node->type=="TextureRect"){
        if(ImGui::Button("設為背景"))SetSelectedAsBackground(false);ImGui::SameLine();if(ImGui::Button("原始尺寸"))RestoreSelectedImageSize();
    }

    if(node->type=="ComponentInstance"){
        ImGui::SeparatorText("Component");
        const auto component=node->properties.find("component");
        const auto* reference=component==node->properties.end()?nullptr:component->second.TryGet<ResourceRefValue>();
        ImGui::TextWrapped("%s",reference?reference->lastKnownPath.c_str():"Missing component resource");
        if(reference&&m_openResource&&ImGui::Button("Edit Main"))m_openResource(*reference);
        if(reference&&m_openResource)ImGui::SameLine();
        if(ImGui::Button("Detach")){DetachSelectedComponent();return;}
        VariantObject overrides;
        if(const auto found=node->properties.find("overrides");found!=node->properties.end()&&found->second.AsObject())overrides=*found->second.AsObject();
        ImGui::SameLine();ImGui::BeginDisabled(overrides.empty());
        if(ImGui::Button("Reset All"))ResetComponentOverride();
        ImGui::EndDisabled();
        std::size_t overrideCount=0;for(const auto& [source,value]:overrides)if(const auto* fields=value.AsObject())overrideCount+=fields->size();
        ImGui::TextDisabled("%zu property override(s); internal structure is locked",overrideCount);
        for(const auto& [source,value]:overrides)if(const auto* fields=value.AsObject()){
            if(ImGui::TreeNode(source.c_str(),"Source %s",source.substr(0,8).c_str())){
                for(const auto& [property,unused]:*fields){(void)unused;ImGui::PushID((source+property).c_str());ImGui::TextUnformatted(property.c_str());ImGui::SameLine();if(ImGui::SmallButton("Reset")){ResetComponentOverride(source,property);ImGui::PopID();ImGui::TreePop();return;}ImGui::PopID();}
                ImGui::TreePop();
            }
        }
    }

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
        const char* propertyLabel=property->editor.displayName.empty()?property->name.c_str():property->editor.displayName.c_str();
        if (auto* text = value.TryGet<std::string>()) {
            if(!property->editor.enumChoices.empty()){
                if(ImGui::BeginCombo(propertyLabel,text->empty()?"(none)":text->c_str())){for(const auto& choice:property->editor.enumChoices)if(ImGui::Selectable(choice.c_str(),choice==*text)){*text=choice;changed=true;}ImGui::EndCombo();}
            }else if(property->editor.multiline)changed=ImGui::InputTextMultiline(propertyLabel,text,ImVec2(-1,ImGui::GetTextLineHeight()*4));
            else changed = ImGui::InputText(propertyLabel, text);
            if(!property->editor.resourceFilter.empty()&&ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){*text=std::string(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);changed=true;}ImGui::EndDragDropTarget();}
        }
        else if (auto* boolean = value.TryGet<bool>()) changed = ImGui::Checkbox(property->name.c_str(), boolean);
        else if (auto* integer = value.TryGet<std::int64_t>()) { int v = static_cast<int>(*integer); changed = ImGui::DragInt(propertyLabel, &v,static_cast<float>(property->editor.step),property->editor.hasRange?static_cast<int>(property->editor.minimum):0,property->editor.hasRange?static_cast<int>(property->editor.maximum):0); *integer = v; continuous = true; }
        else if (auto* number = value.TryGet<double>()) { float v = static_cast<float>(*number); changed = ImGui::DragFloat(propertyLabel, &v, static_cast<float>(property->editor.step),property->editor.hasRange?static_cast<float>(property->editor.minimum):0,property->editor.hasRange?static_cast<float>(property->editor.maximum):0); *number = v; continuous = true; }
        else if (auto* vec = value.TryGet<Vec2>()) { float v[2]{vec->x,vec->y}; changed = ImGui::DragFloat2(property->name.c_str(), v, 1); *vec={v[0],v[1]}; continuous=true; }
        else if (auto* rect = value.TryGet<Rect>()) { float position[2]{rect->x,rect->y},size[2]{rect->w,rect->h};ImGui::TextUnformatted(propertyLabel);changed=ImGui::DragFloat2("X / Y",position,property->name=="anchors"?.01f:1.0f);changed|=ImGui::DragFloat2("W / H",size,property->name=="anchors"?.01f:1.0f);*rect={position[0],position[1],size[0],size[1]};continuous=true; }
        else if (auto* color = value.TryGet<Color>()) { float v[4]{color->r/255.f,color->g/255.f,color->b/255.f,color->a/255.f}; changed=ImGui::ColorEdit4(property->name.c_str(),v); *color={static_cast<uint8_t>(v[0]*255),static_cast<uint8_t>(v[1]*255),static_cast<uint8_t>(v[2]*255),static_cast<uint8_t>(v[3]*255)}; continuous=true; }
        else if(auto* reference=value.TryGet<ResourceRefValue>()){std::string path=reference->lastKnownPath;if(ImGui::InputText(propertyLabel,&path,ImGuiInputTextFlags_EnterReturnsTrue)){reference->lastKnownPath=std::move(path);changed=true;}if(ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){reference->id={};reference->lastKnownPath=std::string(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);changed=true;}ImGui::EndDragDropTarget();}}
        else if(auto* token=value.TryGet<TokenRefValue>()){std::string tokenName=token->name;if(ImGui::InputText((std::string("Token: ")+propertyLabel).c_str(),&tokenName,ImGuiInputTextFlags_EnterReturnsTrue)){token->name=std::move(tokenName);changed=true;}}
        else ImGui::TextDisabled("%s  (unsupported value)", property->name.c_str());
        const bool deactivated=ImGui::IsItemDeactivatedAfterEdit();
        if(!property->editor.description.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",property->editor.description.c_str());
        ImGui::SameLine();
        if(ImGui::SmallButton("↶")){value=property->defaultValue.Clone();changed=true;continuous=false;}
        EditVariant(property->name.c_str(), property->name, std::move(before), std::move(value), changed, continuous);
        if (continuous && deactivated && m_propertyTransaction) { const Status status = m_propertyTransaction->Commit(); if (!status) EmitStatus(status); m_propertyTransaction.reset(); }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Binding");
    VariantObject bindings;if(const auto found=node->properties.find("bindings");found!=node->properties.end()&&found->second.AsObject())bindings=*found->second.AsObject();
    for(auto& [target,value]:bindings){auto* definition=value.AsObject();if(!definition)continue;std::string path=definition->contains("path")&&definition->at("path").TryGet<std::string>()?*definition->at("path").TryGet<std::string>():"";std::string formatter=definition->contains("formatter")&&definition->at("formatter").TryGet<std::string>()?*definition->at("formatter").TryGet<std::string>():"";bool changedBinding=ImGui::InputText((target+" path").c_str(),&path,ImGuiInputTextFlags_EnterReturnsTrue);changedBinding|=ImGui::InputText(("formatter##"+target).c_str(),&formatter,ImGuiInputTextFlags_EnterReturnsTrue);if(changedBinding){VariantObject changed=bindings;auto* changedDefinition=changed[target].AsObject();(*changedDefinition)["path"]=path;(*changedDefinition)["formatter"]=formatter;EditVariant("bindings","bindings",node->properties.contains("bindings")?node->properties["bindings"]:Variant{},Variant(std::move(changed)),true,false);}}
    if (ImGui::Button("Add typed binding")) ImGui::OpenPopup("AddBinding");
    if (ImGui::BeginPopup("AddBinding")) { for (const auto* property : properties) if (HasFlag(property->flags, PropertyFlags::Bindable) && ImGui::MenuItem(property->name.c_str())) {
        VariantObject changed=bindings;changed[property->name]=VariantObject{{"path",std::string("viewModel.property")},{"formatter",std::string{}}};EditVariant("bindings","bindings",node->properties.contains("bindings")?node->properties["bindings"]:Variant{},Variant(std::move(changed)),true,false); ImGui::CloseCurrentPopup();
    } ImGui::EndPopup(); }
    if (!selectedAssetPath.empty() && node->type == "TextureRect" && ImGui::Button("Use selected asset"))
        EditVariant("path", "path", node->properties.contains("path") ? node->properties["path"] : Variant{}, Variant(selectedAssetPath), true, false);
    ImGui::TextDisabled("條件式 UI 請繫結 ViewModel computed property；Binding 不執行表達式。");
    ImGui::SeparatorText("事件");
    type=node->type;
    VariantObject events;if(const auto found=node->properties.find("events");found!=node->properties.end()&&found->second.AsObject())events=*found->second.AsObject();
    while(const auto* info=TypeRegistry::Global().Find(type)){
        for(const auto& signal:info->signals){std::string command;if(const auto found=events.find(signal.name);found!=events.end()&&found->second.AsObject()){const auto action=found->second.AsObject()->find("action");if(action!=found->second.AsObject()->end()&&action->second.TryGet<std::string>())command=*action->second.TryGet<std::string>();}
            if(ImGui::BeginCombo((signal.name+"##event").c_str(),command.empty()?"Select action":command.c_str())){for(const auto& action:ui::ActionRegistry::Builtins().Descriptors())if(ImGui::Selectable((action.category+" / "+action.label+"##"+action.id).c_str(),command==action.id)){VariantObject changed=events;changed[signal.name]=VariantObject{{"action",action.id},{"arguments",VariantObject{}}};EditVariant("events","events",node->properties.contains("events")?node->properties["events"]:Variant{},Variant(std::move(changed)),true,false);}ImGui::EndCombo();}}
        type=info->base;if(type.empty())break;
    }
}

void UIDesigner::RebuildLayout() {
    m_layout.clear(); m_childPolicies.clear(); if (!m_document) return;
    resource::TypedDocument preview = m_document->Data();
    for (auto& node : preview.nodes) node.properties.erase("bindings");
    const ui::UIDocumentLoader loader=[this](const ResourceRefValue& reference){return LoadReferencedUI(reference);};
    auto scene = ui::InstantiateUIScene(preview, nullptr, m_formatters, loader); if (!scene) return;
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
        if(const auto locked=it->properties.find("editorLocked");locked!=it->properties.end())
            if(const auto* value=locked->second.TryGet<bool>();value&&*value)continue;
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
    toolButton("選取 V", DesignerTool::Select); ImGui::SameLine();
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
    ImGui::SameLine();if(ImGui::Button("對齊"))ImGui::OpenPopup("DesignerAlignmentMenu");
    if(ImGui::BeginPopup("DesignerAlignmentMenu")){
        if(ImGui::MenuItem("靠左"))AlignSelection(AlignAction::Left);if(ImGui::MenuItem("水平置中"))AlignSelection(AlignAction::HCenter);if(ImGui::MenuItem("靠右"))AlignSelection(AlignAction::Right);
        if(ImGui::MenuItem("靠上"))AlignSelection(AlignAction::Top);if(ImGui::MenuItem("垂直置中"))AlignSelection(AlignAction::VCenter);if(ImGui::MenuItem("靠下"))AlignSelection(AlignAction::Bottom);
        ImGui::Separator();if(ImGui::MenuItem("水平平均分布"))AlignSelection(AlignAction::DistributeH);if(ImGui::MenuItem("垂直平均分布"))AlignSelection(AlignAction::DistributeV);ImGui::EndPopup();}
    ImGui::SameLine(); ImGui::Checkbox("智慧吸附", &m_viewport.smartGuides);
    ImGui::SameLine(); ImGui::Checkbox("格線 G", &m_viewport.gridVisible);
    ImGui::SameLine(); ImGui::Checkbox("格線吸附", &m_viewport.gridSnap);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) { m_viewport.fitToViewport=true; m_viewport.applyStoredScroll=true; }
    ImGui::SameLine();
    if (ImGui::Button("100%")) { m_viewport.fitToViewport=false; m_viewport.zoom=1.0f; m_viewport.applyStoredScroll=true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130);
    int zoomPercent = static_cast<int>(std::round(m_viewport.zoom * 100.0f));
    if (ImGui::SliderInt("##DesignerZoom", &zoomPercent, 25, 400, "%d%%")) {
        m_viewport.fitToViewport=false;
        m_viewport.zoom = std::clamp(zoomPercent / 100.0f, .25f, 4.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button(m_viewport.interactivePreview ? "停止預覽" : "互動預覽"))
        m_viewport.interactivePreview = !m_viewport.interactivePreview;
    ImGui::SameLine();ImGui::Checkbox("像素精確",&m_viewport.pixelExactPreview);
    ImGui::SameLine();
    ImGui::TextDisabled("%s · %s", SelectionSummary().c_str(),
                        ui::ChildLayoutPolicyName(SelectedParentPolicy()));
    if (!m_canvasHint.empty()) {
        ImGui::TextColored(ImVec4(.95f,.72f,.30f,1), "%s", m_canvasHint.c_str());
    }
}

bool UIDesigner::CanvasInput(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
                             const std::string& selectedAssetPath) {
    if (!m_document || scale <= 0.0f) return false;
    ImGuiIO& io = ImGui::GetIO(); const ImVec2 mouse = io.MousePos;
    const Vec2 canvas{(mouse.x-p0.x)/scale,(mouse.y-p0.y)/scale};
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_V)||ImGui::IsKeyPressed(ImGuiKey_Q)) m_viewport.tool=DesignerTool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_A)) m_viewport.tool=DesignerTool::Anchors;
        if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            if (io.KeyShift) m_viewport.gridSnap=!m_viewport.gridSnap;
            else m_viewport.gridVisible=!m_viewport.gridVisible;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0)) {
            m_viewport.zoom=1.0f;m_viewport.fitToViewport=false;m_viewport.applyStoredScroll=true;return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !m_selected.Empty()) {
            const Rect selected=SelectedRect();
            m_viewport.scrollX=std::max(0.0f,(selected.x+selected.w*.5f)*scale-viewport.GetWidth()*.5f);
            m_viewport.scrollY=std::max(0.0f,(selected.y+selected.h*.5f)*scale-viewport.GetHeight()*.5f);
            m_viewport.applyStoredScroll=true;
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)||ImGui::IsKeyPressed(ImGuiKey_Backspace)) RemoveSelected();
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_C))CopySelected();
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_V))PasteClipboard(canvas);
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_D))DuplicateSelected();
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_A)){m_selection.clear();for(const auto& node:m_document->Data().nodes)if(node.id!=RootId())m_selection.insert(node.id);if(!m_selection.empty())m_selected=*m_selection.begin();return true;}
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_RightBracket))ChangeSelectedLayer(io.KeyShift?LayerAction::BringToFront:LayerAction::BringForward);
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_LeftBracket))ChangeSelectedLayer(io.KeyShift?LayerAction::SendToBack:LayerAction::SendBackward);
        const bool left=ImGui::IsKeyPressed(ImGuiKey_LeftArrow),right=ImGui::IsKeyPressed(ImGuiKey_RightArrow),up=ImGui::IsKeyPressed(ImGuiKey_UpArrow),down=ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if((left||right||up||down)&&!m_selection.empty()){
            const float step=io.KeyShift?10.0f:1.0f;const Vec2 delta{(right?step:0)-(left?step:0),(down?step:0)-(up?step:0)};
            auto command=std::make_unique<CompositeEditCommand>("Nudge UI controls");
            for(const Uuid& id:m_selection){const auto* selected=m_document->Find(id);if(!selected||id==RootId())continue;const auto policy=m_childPolicies.find(selected->parent);if(policy!=m_childPolicies.end()&&policy->second!=ui::ChildLayoutPolicy::Free)continue;const auto found=selected->properties.find("offsets");if(found==selected->properties.end())continue;if(const auto* before=found->second.TryGet<Rect>()){Rect after=*before;after.x+=delta.x;after.y+=delta.y;command->Add(std::make_unique<PropertyChangeCommand>("Nudge control",id,"offsets",Variant(*before),Variant(after),std::chrono::steady_clock::now(),false));}}
            if(!command->Empty()){const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();return true;}
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) CancelCanvasGesture();
    }
    const bool spaceHeld=ImGui::IsKeyDown(ImGuiKey_Space);
    m_hovered=hovered?HitTest(canvas):Uuid{};
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        m_contextCanvas=canvas;m_contextTarget=m_hovered;if(!m_contextTarget.Empty()){if(!m_selection.contains(m_contextTarget))m_selection={m_contextTarget};m_selected=m_contextTarget;}ImGui::OpenPopup("UIDesignerCanvasContext");
    }
    if(ImGui::BeginPopup("UIDesignerCanvasContext")){
        if(ImGui::BeginMenu("新增控制項")){RenderAddControlPalette(m_contextCanvas);ImGui::EndMenu();}
        if(!selectedAssetPath.empty()&&ImGui::MenuItem("Use selected image as TextureRect"))AddNode("TextureRect",m_contextCanvas,selectedAssetPath);
        if(!selectedAssetPath.empty()&&ImGui::MenuItem("將選取圖片設為背景")){AddNode("TextureRect",m_contextCanvas,selectedAssetPath);SetSelectedAsBackground(false);}
        ImGui::Separator();if(ImGui::MenuItem("複製","Ctrl+C",false,!m_selected.Empty()))CopySelected();if(ImGui::MenuItem("貼上","Ctrl+V",false,!m_clipboardSubtree.empty()))PasteClipboard(m_contextCanvas);if(ImGui::MenuItem("建立副本","Ctrl+D",false,!m_selected.Empty()))DuplicateSelected();if(ImGui::MenuItem("建立 Component…",nullptr,false,!m_selected.Empty()&&m_selected!=RootId()))m_createComponentOpen=true;
        if(const auto* context=m_document->Find(m_contextTarget);context&&context->type=="TextureRect"){if(ImGui::MenuItem("設為背景"))SetSelectedAsBackground(false);if(ImGui::MenuItem("設為背景並鎖定"))SetSelectedAsBackground(true);if(ImGui::MenuItem("恢復圖片原始尺寸"))RestoreSelectedImageSize();}
        if(!m_contextTarget.Empty()&&m_contextTarget!=RootId()&&ImGui::BeginMenu("對齊")){if(ImGui::MenuItem("靠左"))AlignSelection(AlignAction::Left);if(ImGui::MenuItem("水平置中"))AlignSelection(AlignAction::HCenter);if(ImGui::MenuItem("靠右"))AlignSelection(AlignAction::Right);if(ImGui::MenuItem("靠上"))AlignSelection(AlignAction::Top);if(ImGui::MenuItem("垂直置中"))AlignSelection(AlignAction::VCenter);if(ImGui::MenuItem("靠下"))AlignSelection(AlignAction::Bottom);ImGui::EndMenu();}
        if(ImGui::BeginMenu("圖層順序",!m_selected.Empty())){if(ImGui::MenuItem("置頂"))ChangeSelectedLayer(LayerAction::BringToFront);if(ImGui::MenuItem("上移一層"))ChangeSelectedLayer(LayerAction::BringForward);if(ImGui::MenuItem("下移一層"))ChangeSelectedLayer(LayerAction::SendBackward);if(ImGui::MenuItem("置底"))ChangeSelectedLayer(LayerAction::SendToBack);ImGui::EndMenu();}
        ImGui::Separator();const bool canDelete=!m_contextTarget.Empty()&&m_contextTarget!=RootId();if(ImGui::MenuItem("刪除","Delete",false,canDelete))RemoveSelected();if(!m_contextTarget.Empty()&&!canDelete&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("根節點不能刪除");ImGui::EndPopup();
    }
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
    if(hovered&&(ImGui::IsMouseClicked(ImGuiMouseButton_Middle)||(spaceHeld&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)))){m_gesture=Gesture::Pan;m_panMouseStart=mouse;m_panScrollX=ImGui::GetScrollX();m_panScrollY=ImGui::GetScrollY();ImGui::SetWindowFocus();return true;}
    if(m_gesture==Gesture::Pan){const bool held=ImGui::IsMouseDown(ImGuiMouseButton_Middle)||ImGui::IsMouseDown(ImGuiMouseButton_Left);if(held){ImGui::SetScrollX(ImGui::GetCurrentWindow(),std::max(0.0f,m_panScrollX-(mouse.x-m_panMouseStart.x)));ImGui::SetScrollY(ImGui::GetCurrentWindow(),std::max(0.0f,m_panScrollY-(mouse.y-m_panMouseStart.y)));ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);return true;}m_gesture=Gesture::None;}
    if(hovered&&(spaceHeld||ImGui::IsMouseDown(ImGuiMouseButton_Middle)))ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);else if(handle==2||handle==6)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);else if(handle==4||handle==8)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);else if(handle==1||handle==5)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);else if(handle==3||handle==7)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&!spaceHeld){
        Uuid hit=(handle||anchorHandle)?m_selected:HitTest(canvas);
        if(io.KeyAlt&&!handle){
            std::vector<Uuid> hits;for(auto iterator=m_document->Data().nodes.rbegin();iterator!=m_document->Data().nodes.rend();++iterator){if(const auto locked=iterator->properties.find("editorLocked");locked!=iterator->properties.end())if(const auto* value=locked->second.TryGet<bool>();value&&*value)continue;const auto found=m_layout.find(iterator->id);if(found==m_layout.end())continue;const Rect rect=found->second;if(canvas.x>=rect.x&&canvas.x<=rect.x+rect.w&&canvas.y>=rect.y&&canvas.y<=rect.y+rect.h)hits.push_back(iterator->id);}
            if(!hits.empty()){auto current=std::find(hits.begin(),hits.end(),m_selected);if(current==hits.end()||++current==hits.end())hit=hits.front();else hit=*current;}
        }
        if(!hit.Empty()){
            if(io.KeyCtrl)m_selection.insert(hit);else if(!m_selection.contains(hit))m_selection={hit};
            m_selected=hit;m_canvasHint.clear();const auto policy=SelectedParentPolicy();
            const auto* selectedNode=m_document->Find(hit);const bool locked=selectedNode&&selectedNode->properties.contains("editorLocked")&&selectedNode->properties.at("editorLocked").TryGet<bool>()&&*selectedNode->properties.at("editorLocked").TryGet<bool>();
            if(locked)m_canvasHint="此元件已在 Scene Tree 鎖定";
            else if(anchorHandle){const auto* anchorNode=m_document->Find(hit);m_anchorsStart={};m_anchorOffsetsStart={};if(anchorNode){if(const auto found=anchorNode->properties.find("anchors");found!=anchorNode->properties.end())if(const auto* value=found->second.TryGet<Rect>())m_anchorsStart=*value;if(const auto found=anchorNode->properties.find("offsets");found!=anchorNode->properties.end())if(const auto* value=found->second.TryGet<Rect>())m_anchorOffsetsStart=*value;}m_rectStart=SelectedRect();m_anchorHandle=anchorHandle;m_gesture=Gesture::Anchors;}
            else if(hit!=RootId()){if(policy==ui::ChildLayoutPolicy::Free)BeginFreeTransform(hit,canvas,handle);else{m_gesture=Gesture::Reorder;m_dragStart=canvas;m_reorderPreview=m_document->ChildIndex(hit);m_canvasHint=std::string("由 ")+ui::ChildLayoutPolicyName(policy)+" 控制；拖曳排序，Ctrl 拖曳抽離";}}}
        else{m_gesture=Gesture::Marquee;m_dragStart=canvas;m_marqueeCurrent=canvas;m_marqueeAdditive=io.KeyCtrl;if(!m_marqueeAdditive){m_selection.clear();m_selected={};}}
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
        if(m_viewport.smartGuides&&m_gesture==Gesture::Resize&&!io.KeyAlt){std::vector<float> xs{0,CanvasSize().x*.5f,CanvasSize().x},ys{0,CanvasSize().y*.5f,CanvasSize().y};const auto* selected=m_document->Find(m_selected);for(const auto& [id,r]:m_layout){if(id==m_selected)continue;const auto* other=m_document->Find(id);if(selected&&other&&selected->parent==other->parent){xs.insert(xs.end(),{r.x,r.x+r.w*.5f,r.x+r.w});ys.insert(ys.end(),{r.y,r.y+r.h*.5f,r.y+r.h});}}const float threshold=6.0f/scale;float left=target.x,right=target.x+target.w,top=target.y,bottom=target.y+target.h,bestX=threshold,bestY=threshold;const bool moveLeft=m_resizeHandle==1||m_resizeHandle==7||m_resizeHandle==8,moveRight=m_resizeHandle==3||m_resizeHandle==4||m_resizeHandle==5,moveTop=m_resizeHandle>=1&&m_resizeHandle<=3,moveBottom=m_resizeHandle>=5&&m_resizeHandle<=7;for(float line:xs){const float edge=moveLeft?left:right;if((moveLeft||moveRight)&&std::abs(line-edge)<bestX){bestX=std::abs(line-edge);if(moveLeft)left=line;else right=line;m_guideX=line;}}for(float line:ys){const float edge=moveTop?top:bottom;if((moveTop||moveBottom)&&std::abs(line-edge)<bestY){bestY=std::abs(line-edge);if(moveTop)top=line;else bottom=line;m_guideY=line;}}target={left,top,std::max(8.0f,right-left),std::max(8.0f,bottom-top)};}
        if(m_gesture==Gesture::Resize&&(m_resizeHandle==1||m_resizeHandle==3||m_resizeHandle==5||m_resizeHandle==7)){const auto* selected=m_document->Find(m_selected);bool locked=selected&&selected->type=="TextureRect";if(selected)if(const auto found=selected->properties.find("lockAspectRatio");found!=selected->properties.end())if(const auto* value=found->second.TryGet<bool>())locked=*value;if(io.KeyShift)locked=!locked;if(locked){float ratio=m_rectStart.h>0?m_rectStart.w/m_rectStart.h:1.0f;if(selected&&m_imageSizeResolver)if(const auto path=selected->properties.find("path");path!=selected->properties.end()&&path->second.TryGet<std::string>())if(const auto size=m_imageSizeResolver(*path->second.TryGet<std::string>());size&&size->y>0)ratio=size->x/size->y;float left=target.x,right=target.x+target.w,top=target.y,bottom=target.y+target.h;if(std::abs(delta.x)>=std::abs(delta.y)*ratio){const float height=target.w/ratio;if(m_resizeHandle==1||m_resizeHandle==3)top=bottom-height;else bottom=top+height;}else{const float width=target.h*ratio;if(m_resizeHandle==1||m_resizeHandle==7)left=right-width;else right=left+width;}target={left,top,std::max(8.0f,right-left),std::max(8.0f,bottom-top)};}}
        if(m_gesture==Gesture::Resize)m_canvasHint=std::to_string(static_cast<int>(std::round(target.w)))+" × "+std::to_string(static_cast<int>(std::round(target.h)));
        Rect anchors{};if(const auto* node=m_document->Find(m_selected))if(const auto it=node->properties.find("anchors");it!=node->properties.end())if(const auto* value=it->second.TryGet<Rect>())anchors=*value;
        const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(m_selected),anchors,target);const Status status=m_propertyTransaction->Update(Variant(offsets));if(!status)EmitStatus(status);MarkEdited();return true;
    }
    if(m_gesture==Gesture::Reorder&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){m_reorderPreview=InsertionIndex(m_selected,canvas);return true;}
    if(m_gesture==Gesture::Marquee&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){m_marqueeCurrent=canvas;return true;}
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        if(m_gesture==Gesture::Marquee){const float left=std::min(m_dragStart.x,m_marqueeCurrent.x),right=std::max(m_dragStart.x,m_marqueeCurrent.x),top=std::min(m_dragStart.y,m_marqueeCurrent.y),bottom=std::max(m_dragStart.y,m_marqueeCurrent.y);for(const auto& node:m_document->Data().nodes){if(node.id==RootId())continue;if(const auto locked=node.properties.find("editorLocked");locked!=node.properties.end())if(const auto* value=locked->second.TryGet<bool>();value&&*value)continue;const auto found=m_layout.find(node.id);if(found==m_layout.end())continue;const Rect r=found->second;if(r.x<=right&&r.x+r.w>=left&&r.y<=bottom&&r.y+r.h>=top){m_selection.insert(node.id);m_selected=node.id;}}m_gesture=Gesture::None;return true;}
        if(m_gesture==Gesture::Anchors){auto anchors=m_document->ReadProperty(m_selected,"anchors"),offsets=m_document->ReadProperty(m_selected,"offsets");auto command=std::make_unique<CompositeEditCommand>("調整錨點");if(anchors)command->Add(std::make_unique<PropertyChangeCommand>("錨點",m_selected,"anchors",Variant(m_anchorsStart),anchors.Value()));if(offsets)command->Add(std::make_unique<PropertyChangeCommand>("保持位置",m_selected,"offsets",Variant(m_anchorOffsetsStart),offsets.Value()));const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);m_anchorHandle=0;m_gesture=Gesture::None;MarkEdited();return true;}
        if(m_gesture==Gesture::Move&&m_groupMove){auto command=std::make_unique<CompositeEditCommand>("移動多個元件");for(const auto& [id,before]:m_groupOffsetsStart){auto after=m_document->ReadProperty(id,"offsets");if(after)command->Add(std::make_unique<PropertyChangeCommand>("移動元件",id,"offsets",Variant(before),after.Value()));}const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);m_groupOffsetsStart.clear();m_groupMove=false;m_gesture=Gesture::None;MarkEdited();return true;}
        if(m_gesture==Gesture::Move||m_gesture==Gesture::Resize){if(m_propertyTransaction){const Status status=m_propertyTransaction->Commit();if(!status)EmitStatus(status);m_propertyTransaction.reset();}m_gesture=Gesture::None;return true;}
        if(m_gesture==Gesture::Reorder){CommitManagedDrag(canvas,io.KeyCtrl);m_gesture=Gesture::None;return true;}}
    return false;
}

void UIDesigner::DrawOverlay(ImVec2 p0, float scale) {
    if (!m_document) return; ImDrawList* draw=ImGui::GetWindowDrawList();const Vec2 canvas=CanvasSize();const ImVec2 max{p0.x+canvas.x*scale,p0.y+canvas.y*scale};draw->PushClipRect(p0,max,true);
    if(m_viewport.gridVisible){float step=static_cast<float>(m_gridSize);while(step*scale<12)step*=2;int line=0;for(float x=0;x<=canvas.x;x+=step,++line)draw->AddLine({p0.x+x*scale,p0.y},{p0.x+x*scale,max.y},line%4==0?IM_COL32(92,110,135,75):IM_COL32(72,84,102,40));line=0;for(float y=0;y<=canvas.y;y+=step,++line)draw->AddLine({p0.x,p0.y+y*scale},{max.x,p0.y+y*scale},line%4==0?IM_COL32(92,110,135,75):IM_COL32(72,84,102,40));}
    if(!std::isnan(m_guideX))draw->AddLine({p0.x+m_guideX*scale,p0.y},{p0.x+m_guideX*scale,max.y},IM_COL32(255,92,180,230),1.5f);
    if(!std::isnan(m_guideY))draw->AddLine({p0.x,p0.y+m_guideY*scale},{max.x,p0.y+m_guideY*scale},IM_COL32(255,92,180,230),1.5f);
    for(const auto& node:m_document->Data().nodes){const auto it=m_layout.find(node.id);if(it==m_layout.end())continue;const bool selected=m_selection.contains(node.id),primary=node.id==m_selected,hover=node.id==m_hovered;if(!selected&&!hover&&!m_viewport.showAllOutlines)continue;const Rect r=it->second;const ImU32 color=selected?IM_COL32(71,140,191,255):IM_COL32(150,170,195,150);draw->AddRect({p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},color,3.0f,ImDrawFlags_None,selected?2.0f:1.0f);if(primary)draw->AddText({p0.x+r.x*scale+5,p0.y+r.y*scale-20},IM_COL32(220,232,245,255),node.name.c_str());}
    if(!m_selected.Empty()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const Rect r=SelectedRect();const ImVec2 points[8]{{p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h*.5f)*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h*.5f)*scale}};for(const auto& point:points)draw->AddRectFilled({point.x-4,point.y-4},{point.x+4,point.y+4},IM_COL32(225,235,248,255),1);}
    if(m_viewport.tool==DesignerTool::Anchors&&!m_selected.Empty()&&m_selected!=RootId()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=m_document->Find(m_selected);Rect anchors{};if(node)if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;const Rect parent=ParentRect(m_selected),selected=SelectedRect();const ImVec2 points[4]{{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale},{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale}};const ImVec2 corners[4]{{p0.x+selected.x*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+(selected.y+selected.h)*scale},{p0.x+selected.x*scale,p0.y+(selected.y+selected.h)*scale}};for(int index=0;index<4;++index){draw->AddLine(points[index],corners[index],IM_COL32(255,120,190,150));draw->AddQuadFilled({points[index].x,points[index].y-6},{points[index].x+6,points[index].y},{points[index].x,points[index].y+6},{points[index].x-6,points[index].y},IM_COL32(255,120,190,255));}}
    if(m_gesture==Gesture::Reorder){const auto* node=m_document->Find(m_selected);if(node){std::vector<const resource::NodeRecord*> siblings;for(const auto* sibling:std::as_const(*m_document).Children(node->parent))if(sibling->id!=m_selected)siblings.push_back(sibling);Rect marker{};const auto policy=SelectedParentPolicy();if(!siblings.empty()){if(m_reorderPreview<siblings.size()&&m_layout.contains(siblings[m_reorderPreview]->id))marker=m_layout.at(siblings[m_reorderPreview]->id);else if(m_layout.contains(siblings.back()->id)){marker=m_layout.at(siblings.back()->id);if(policy==ui::ChildLayoutPolicy::LinearX)marker.x+=marker.w;else marker.y+=marker.h;}}if(policy==ui::ChildLayoutPolicy::LinearX)draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+marker.x*scale,p0.y+(marker.y+marker.h)*scale},IM_COL32(71,140,191,255),3);else draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+(marker.x+marker.w)*scale,p0.y+marker.y*scale},IM_COL32(71,140,191,255),3);}}
    if(m_gesture==Gesture::Marquee){const ImVec2 a{p0.x+m_dragStart.x*scale,p0.y+m_dragStart.y*scale},b{p0.x+m_marqueeCurrent.x*scale,p0.y+m_marqueeCurrent.y*scale};draw->AddRectFilled({std::min(a.x,b.x),std::min(a.y,b.y)},{std::max(a.x,b.x),std::max(a.y,b.y)},IM_COL32(71,140,191,35));draw->AddRect({std::min(a.x,b.x),std::min(a.y,b.y)},{std::max(a.x,b.x),std::max(a.y,b.y)},IM_COL32(71,140,191,220));}
    if(m_gesture==Gesture::Resize&&!m_canvasHint.empty()){const ImVec2 mouse=ImGui::GetMousePos(),textSize=ImGui::CalcTextSize(m_canvasHint.c_str());const ImVec2 a{mouse.x+14,mouse.y+14},b{a.x+textSize.x+12,a.y+textSize.y+8};draw->AddRectFilled(a,b,IM_COL32(20,24,32,235),4);draw->AddText({a.x+6,a.y+4},IM_COL32(235,240,248,255),m_canvasHint.c_str());}
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
