#include "Editor/Tools/UIDesigner/UIDesigner.h"
#include "Editor/Tools/UIDesigner/DesignerDiagnostics.h"
#include "Editor/Tools/UIDesigner/DesignerNodeFactory.h"
#include "Editor/Tools/UIDesigner/BehaviorGraphEditor.h"
#include "Editor/Tools/UIDesigner/AnimationStateMachineEditor.h"
#include "Editor/Tools/UIDesigner/PropertyEditorRegistry.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/ActionRegistry.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/Styles/StyleResolver.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
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

Variant DefaultValueFor(const VariantType type) {
    switch(type){
        case VariantType::Bool:return false;
        case VariantType::Integer:return std::int64_t{0};
        case VariantType::Number:return 0.0;
        case VariantType::String:return std::string{};
        case VariantType::Vec2:return Vec2{};
        case VariantType::Rect:return Rect{};
        case VariantType::Color:return Color{255,255,255,255};
        case VariantType::Uuid:return Uuid{};
        case VariantType::ResourceRef:return ResourceRefValue{};
        case VariantType::TokenRef:return TokenRefValue{};
        case VariantType::Array:return VariantArray{};
        case VariantType::Object:return VariantObject{};
        default:return Variant{};
    }
}

std::string ComponentInterfaceId(std::string value){
    std::string result;bool upper=false;for(const unsigned char character:value){if(std::isalnum(character)){char output=static_cast<char>(character);if(result.empty())output=static_cast<char>(std::tolower(character));else if(upper)output=static_cast<char>(std::toupper(character));result.push_back(output);upper=false;}else upper=!result.empty();}if(result.empty())result="item";return result;
}

bool RenderActionArgument(const ui::ActionArgumentDescriptor& descriptor,Variant& value){
    bool changed=false;const char* label=descriptor.displayName.empty()?descriptor.name.c_str():descriptor.displayName.c_str();
    const auto help=[&]{if(!descriptor.description.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",descriptor.description.c_str());};
    if(!descriptor.enumValues.empty()&&value.TryGet<std::string>()){
        auto* text=value.TryGet<std::string>();if(ImGui::BeginCombo(label,text->empty()?"(none)":text->c_str())){for(const auto& option:descriptor.enumValues)if(ImGui::Selectable(option.c_str(),option==*text)){*text=option;changed=true;}ImGui::EndCombo();}help();return changed;
    }
    if(auto* text=value.TryGet<std::string>()){if(descriptor.editorHint==ui::ActionEditorHint::Multiline)changed=ImGui::InputTextMultiline(label,text,ImVec2(-1,ImGui::GetTextLineHeight()*4));else changed=ImGui::InputText(label,text);}
    else if(auto* boolean=value.TryGet<bool>())changed=ImGui::Checkbox(label,boolean);
    else if(auto* integer=value.TryGet<std::int64_t>()){int edited=static_cast<int>(*integer);changed=ImGui::DragInt(label,&edited,1.0f,descriptor.minimum?static_cast<int>(*descriptor.minimum):0,descriptor.maximum?static_cast<int>(*descriptor.maximum):0);if(changed)*integer=edited;}
    else if(auto* number=value.TryGet<double>()){float edited=static_cast<float>(*number);changed=ImGui::DragFloat(label,&edited,.1f,descriptor.minimum?static_cast<float>(*descriptor.minimum):0,descriptor.maximum?static_cast<float>(*descriptor.maximum):0);if(changed)*number=edited;}
    else if(auto* vector=value.TryGet<Vec2>()){float edited[2]{vector->x,vector->y};changed=ImGui::DragFloat2(label,edited,1);if(changed)*vector={edited[0],edited[1]};}
    else if(auto* rectangle=value.TryGet<Rect>()){float edited[4]{rectangle->x,rectangle->y,rectangle->w,rectangle->h};changed=ImGui::DragFloat4(label,edited,1);if(changed)*rectangle={edited[0],edited[1],edited[2],edited[3]};}
    else if(auto* color=value.TryGet<Color>()){float edited[4]{color->r/255.f,color->g/255.f,color->b/255.f,color->a/255.f};changed=ImGui::ColorEdit4(label,edited);if(changed)*color={static_cast<std::uint8_t>(edited[0]*255),static_cast<std::uint8_t>(edited[1]*255),static_cast<std::uint8_t>(edited[2]*255),static_cast<std::uint8_t>(edited[3]*255)};}
    else if(auto* resource=value.TryGet<ResourceRefValue>()){ImGui::Text("%s：%s",label,resource->lastKnownPath.empty()?"拖入資源":resource->lastKnownPath.c_str());if(ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){resource->lastKnownPath=std::string(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);resource->id=Uuid::FromName(resource->lastKnownPath);changed=true;}ImGui::EndDragDropTarget();}}
    else if(auto* token=value.TryGet<TokenRefValue>())changed=ImGui::InputText(label,&token->name);
    else if(auto* uuid=value.TryGet<Uuid>()){std::string uuidText=uuid->Empty()?std::string{}:uuid->ToString();if(ImGui::InputText(label,&uuidText)){if(uuidText.empty()){*uuid={};changed=true;}else if(const auto parsed=Uuid::Parse(uuidText)){*uuid=*parsed;changed=true;}}}
    else ImGui::TextDisabled("%s：尚無對應編輯器",label);
    help();
    return changed;
}

VariantObject CaptureExpandedNodes(const std::vector<resource::NodeRecord>& nodes, const Uuid& id) {
    const auto found=std::find_if(nodes.begin(),nodes.end(),[&](const auto& node){return node.id==id;});
    if(found==nodes.end())return {};
    VariantArray children;
    for(const auto& child:nodes)if(child.parent==id)children.emplace_back(CaptureExpandedNodes(nodes,child.id));
    return {{"id",found->id},{"name",found->name},{"type",found->type},
            {"properties",VariantObject(found->properties)},{"children",std::move(children)}};
}
void RegisterPropertyEditors(){static const bool once=[](){auto& registry=PropertyEditorRegistry::Global();const auto key=[](VariantType type){return "type:"+std::to_string(static_cast<int>(type));};
    (void)registry.Register(key(VariantType::String),[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();if(!value)return false;const char* label=request.property.editor.displayName.empty()?request.property.name.c_str():request.property.editor.displayName.c_str();return ImGui::InputText(label,value);});
    (void)registry.Register("multiline",[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();return value&&ImGui::InputTextMultiline(request.property.name.c_str(),value,ImVec2(-1,ImGui::GetTextLineHeight()*4));});
    (void)registry.Register("enum",[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();if(!value)return false;bool changed=false;if(ImGui::BeginCombo(request.property.name.c_str(),value->empty()?"(none)":value->c_str())){for(const auto& choice:request.property.editor.enumChoices)if(ImGui::Selectable(choice.c_str(),choice==*value)){*value=choice;changed=true;}ImGui::EndCombo();}return changed;});
    (void)registry.Register("resource",[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();if(!value)return false;ImGui::Text("%s: %s",request.property.name.c_str(),value->empty()?"Drop a resource":value->c_str());bool changed=false;if(ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){*value=std::string(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);changed=true;}ImGui::EndDragDropTarget();}return changed;});
    (void)registry.Register(key(VariantType::Bool),[](PropertyEditRequest& request){auto* value=request.value.TryGet<bool>();return value&&ImGui::Checkbox(request.property.name.c_str(),value);});
    (void)registry.Register(key(VariantType::Integer),[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::int64_t>();if(!value)return false;int edited=static_cast<int>(*value);request.continuous=true;const bool changed=ImGui::DragInt(request.property.name.c_str(),&edited,static_cast<float>(request.property.editor.step),request.property.editor.hasRange?static_cast<int>(request.property.editor.minimum):0,request.property.editor.hasRange?static_cast<int>(request.property.editor.maximum):0);*value=edited;return changed;});
    (void)registry.Register(key(VariantType::Number),[](PropertyEditRequest& request){auto* value=request.value.TryGet<double>();if(!value)return false;float edited=static_cast<float>(*value);request.continuous=true;const bool changed=ImGui::DragFloat(request.property.name.c_str(),&edited,static_cast<float>(request.property.editor.step),request.property.editor.hasRange?static_cast<float>(request.property.editor.minimum):0,request.property.editor.hasRange?static_cast<float>(request.property.editor.maximum):0);*value=edited;return changed;});
    (void)registry.Register(key(VariantType::Vec2),[](PropertyEditRequest& request){auto* value=request.value.TryGet<Vec2>();if(!value)return false;float edited[2]{value->x,value->y};request.continuous=true;const bool changed=ImGui::DragFloat2(request.property.name.c_str(),edited,1);*value={edited[0],edited[1]};return changed;});
    (void)registry.Register(key(VariantType::Rect),[](PropertyEditRequest& request){auto* value=request.value.TryGet<Rect>();if(!value)return false;float edited[4]{value->x,value->y,value->w,value->h};request.continuous=true;const bool changed=ImGui::DragFloat4(request.property.name.c_str(),edited,request.property.name=="anchors"?.01f:1.0f);*value={edited[0],edited[1],edited[2],edited[3]};return changed;});
    (void)registry.Register(key(VariantType::Color),[](PropertyEditRequest& request){auto* value=request.value.TryGet<Color>();if(!value)return false;float edited[4]{value->r/255.f,value->g/255.f,value->b/255.f,value->a/255.f};request.continuous=true;const bool changed=ImGui::ColorEdit4(request.property.name.c_str(),edited);*value={static_cast<uint8_t>(edited[0]*255),static_cast<uint8_t>(edited[1]*255),static_cast<uint8_t>(edited[2]*255),static_cast<uint8_t>(edited[3]*255)};return changed;});return true;}();(void)once;}
}

UIDesigner::UIDesigner():m_session(std::make_unique<UIDesignerSession>()),m_behaviorEditor(std::make_unique<BehaviorGraphEditor>()),m_animationStateEditor(std::make_unique<AnimationStateMachineEditor>()) { const Status status = ui::RegisterBuiltinUITypes(); if (!status) EmitStatus(status); RegisterPropertyEditors(); }

void UIDesigner::SetAnimationParameterTester(std::function<Status(std::string_view,const Variant&)> tester){m_animationStateEditor->SetParameterTester(std::move(tester));}
UIDesigner::~UIDesigner()=default;
UIDesigner::UIDesigner(UIDesigner&&) noexcept=default;

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
    const Status status = m_session->Open(path);
    if (!status) return status;
    m_document = m_session->Document(); m_pathText = path.string(); m_selected = RootId();
    m_selection={m_selected}; RebuildLayout(); return Status::Ok();
}

Status UIDesigner::New(const std::filesystem::path& path, int width, int height) {
    const Status status = m_session->New(path,width,height);
    if (!status) return status;
    m_document = m_session->Document(); m_pathText = path.string(); m_selected = RootId();
    m_selection={m_selected}; RebuildLayout(); MarkEdited(true); return Status::Ok();
}

bool UIDesigner::Save() { if (!m_document) return false; Status status = m_document->Save();if(status&&m_identityRegistrar)status=m_identityRegistrar(m_document->Path()); if (!status) EmitStatus(status); return static_cast<bool>(status); }
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
    if(m_session&&m_document){if(structural)(void)m_session->DocumentView().Rebuild(*m_document);std::vector<Uuid> ordered;if(!m_selected.Empty()&&m_selection.contains(m_selected))ordered.push_back(m_selected);for(const auto& id:m_selection)if(id!=m_selected)ordered.push_back(id);m_session->Selection().Replace(std::move(ordered),m_selected);m_session->MarkDirty(structural?DesignerDirtyFlags::Structure|DesignerDirtyFlags::Layout:DesignerDirtyFlags::Paint);}
    RebuildLayout(); if (structural && m_onStructure) m_onStructure(); if (m_onEdit) m_onEdit();
}

void UIDesigner::RegenerateIds(VariantObject& subtree) {
    subtree["id"] = Variant(Uuid::Random());
    if (auto* children = subtree["children"].AsArray()) for (auto& value : *children) if (auto* child = value.AsObject()) RegenerateIds(*child);
}

void UIDesigner::AddNode(std::string type, Vec2 canvas, std::string image) {
    if (!m_document) return; const Uuid parent = ParentForNewNode();
    const auto* info=TypeRegistry::Global().Find(type);if(!info||!info->designer)return;
    float w=info->designer->defaultSize.x,h=info->designer->defaultSize.y;
    if(type=="TextureRect"&&!image.empty()&&m_imageSizeResolver)if(const auto size=m_imageSizeResolver(image);size&&size->x>0&&size->y>0){w=size->x;h=size->y;const Vec2 canvasSize=CanvasSize();const float fit=std::min({1.0f,(canvasSize.x-40.0f)/w,(canvasSize.y-40.0f)/h});w*=fit;h*=fit;}
    Rect offsets{canvas.x - w * 0.5f, canvas.y - h * 0.5f, w, h};
    if (canvas == Vec2{}) offsets = {20,20,w,h};
    auto created=DesignerNodeFactory::Create(type,{},image);if(!created){EmitStatus(Status::Fail(created.Diagnostics()));return;}VariantObject subtree=created.TakeValue();subtree["name"]=UniqueName(*m_document,info->designer->displayName);if(auto* properties=subtree["properties"].AsObject())(*properties)["offsets"]=offsets;
    const Uuid id = *subtree["id"].TryGet<Uuid>();
    auto command = std::make_unique<SubtreeEditCommand>("Add " + type, SubtreeOperation::Insert, id, parent,
                                                        m_document->Children(parent).size(), std::move(subtree));
    const Status status = m_document->History().Execute(std::move(command)); if (!status) return EmitStatus(status);
    m_selected = id; MarkEdited(true);
}

void UIDesigner::RenderAddControlPalette(Vec2 canvasPosition){
    ImGui::InputTextWithHint("##control-search","Search controls…",m_paletteFilter,sizeof(m_paletteFilter));
    std::string category;for(const auto* type:DesignerNodeFactory::Palette(m_paletteFilter)){if(type->designer->category!=category){category=type->designer->category;ImGui::SeparatorText(category.c_str());}if(ImGui::Selectable((type->designer->displayName+"##palette-"+type->name).c_str())){AddNode(type->name,canvasPosition);ImGui::CloseCurrentPopup();}if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",type->designer->description.c_str());}
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
    resource::TypedDocument component;component.kind=resource::DocumentKind::Scene;component.id=Uuid::Random();component.type="UIComponent";component.properties["canvasSize"]=Vec2{std::max(1.0f,SelectedRect().w),std::max(1.0f,SelectedRect().h)};component.properties["uiSchemaVersion"]=std::int64_t{5};if(!AppendCapturedNodes(captured.Value(),{},component.nodes))return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3042",.category="Editor.UIDesigner",.message="Selected subtree could not be serialized as a component"});
    auto written=m_componentWriter(path,resource::WriteTypedDocument(component));if(!written)return Status::Fail(written.Diagnostics());
    VariantObject instanceProperties=selected->properties;instanceProperties["component"]=written.Value();instanceProperties["overrides"]=VariantObject{};instanceProperties["componentProperties"]=VariantObject{};instanceProperties["componentEvents"]=VariantObject{};
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
    std::string visibility="Visible";if(const auto found=record.properties.find("visibility");found!=record.properties.end())if(const auto* value=found->second.TryGet<std::string>())visibility=*value;
    const bool visible=visibility=="Visible";const bool collapsed=visibility=="Collapsed";
    const bool locked=record.properties.contains("editorLocked")&&record.properties["editorLocked"].TryGet<bool>()&&*record.properties["editorLocked"].TryGet<bool>();
    const auto policy=m_childPolicies.find(record.id);const bool container=policy!=m_childPolicies.end()&&policy->second!=ui::ChildLayoutPolicy::Free;
    const auto batchProperty=[&](const std::string& property,const Variant& value,const std::string& label){std::vector<Uuid> targets;if(m_selection.contains(record.id))targets.assign(m_selection.begin(),m_selection.end());else targets.push_back(record.id);auto command=std::make_unique<CompositeEditCommand>(label);for(const Uuid& target:targets){auto before=m_document->ReadProperty(target,property);if(before&&before.Value()!=value)command->Add(std::make_unique<PropertyChangeCommand>(label,target,property,before.Value(),value.Clone(),std::chrono::steady_clock::now(),false));}if(command->Empty())return;const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();};
    if(ImGui::SmallButton(visible?"◉":collapsed?"×":"○"))batchProperty("visibility",Variant(std::string(visible?"Hidden":"Visible")),"切換可見性");if(ImGui::IsItemHovered())ImGui::SetTooltip("眼睛：Visible / Hidden；右鍵選單可設為 Collapsed");ImGui::SameLine();
    if(ImGui::SmallButton(locked?"◆":"◇"))batchProperty("editorLocked",Variant(!locked),locked?"解除鎖定":"鎖定元件");if(ImGui::IsItemHovered())ImGui::SetTooltip(locked?"解除鎖定":"鎖定（編輯模式不可拖曳）");ImGui::SameLine();
    const std::string badges=std::string(locked?"  ◆":"")+(container?"  [Layout]":"");
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
        if(const ImGuiPayload* payload=ImGui::GetDragDropPayload();payload&&payload->IsDataType(kNodePayload)){const float rowHeight=ImGui::GetItemRectSize().y;const float fraction=rowHeight>0?(ImGui::GetMousePos().y-ImGui::GetItemRectMin().y)/rowHeight:.5f;if(fraction<.25f||fraction>.75f){const float y=fraction<.25f?ImGui::GetItemRectMin().y:ImGui::GetItemRectMax().y;ImGui::GetWindowDrawList()->AddLine({ImGui::GetItemRectMin().x,y},{ImGui::GetItemRectMax().x,y},IM_COL32(71,140,191,255),3.0f);}else ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),IM_COL32(71,140,191,255),3.0f,0,2.0f);}
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
        const auto setVisibility=[&](const char* value){batchProperty("visibility",Variant(std::string(value)),"設定可見性");};
        if(ImGui::BeginMenu("可見性")){if(ImGui::MenuItem("Visible",nullptr,visibility=="Visible"))setVisibility("Visible");if(ImGui::MenuItem("Hidden",nullptr,visibility=="Hidden"))setVisibility("Hidden");if(ImGui::MenuItem("Collapsed",nullptr,visibility=="Collapsed"))setVisibility("Collapsed");ImGui::EndMenu();}
        if(ImGui::MenuItem(locked?"解除鎖定":"鎖定"))batchProperty("editorLocked",Variant(!locked),"切換鎖定");
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

void UIDesigner::RenderInsert(){
    if(!m_document){ImGui::TextDisabled("Open a UI scene to insert Controls.");return;}
    ImGui::InputTextWithHint("##insert-filter","搜尋 Control…",m_paletteFilter,sizeof(m_paletteFilter));
    RenderAddControlPalette();
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
    if(m_selection.size()>1){ImGui::SameLine();ImGui::TextDisabled("· %zu 個元件",m_selection.size());}
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##inspector-search","搜尋屬性...",&m_session->inspectorSearch);
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
    bool categoryOpen = true;
    for (const auto* property : properties) {
        if(!m_session->inspectorSearch.empty()){
            const auto lower=[](std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char character){return static_cast<char>(std::tolower(character));});return value;};
            const std::string query=lower(m_session->inspectorSearch);const std::string searchable=lower(property->name+" "+property->editor.displayName+" "+property->category+" "+property->editor.description);if(searchable.find(query)==std::string::npos)continue;
        }
        if (property->category != lastCategory) {
            lastCategory = property->category;
            const char* category=lastCategory=="Layout"?"配置":lastCategory=="Appearance"?"外觀":lastCategory=="Interaction"?"互動":lastCategory=="Data"?"資料":lastCategory.c_str();
            ImGui::PushID(lastCategory.c_str());
            categoryOpen = ImGui::CollapsingHeader(category, ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopID();
        }
        if (!categoryOpen) continue;
        if(property->name=="offsets"&&SelectedParentPolicy()!=ui::ChildLayoutPolicy::Free){
            ImGui::TextDisabled("位置與尺寸  由 %s 控制",ui::ChildLayoutPolicyName(SelectedParentPolicy()));
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Container 子元件不寫入無效 offsets；請使用 minimum size、size flags 或拖曳排序。");
            continue;
        }
        const auto current = node->properties.find(property->name);
        Variant before = current == node->properties.end() ? Variant{} : current->second;
        Variant value = current == node->properties.end() ? property->defaultValue : current->second;
        std::vector<Uuid> propertyTargets;bool mixed=false;Variant effective=value.Clone();
        for(const Uuid& selectedId:m_selection){const auto* selectedNode=m_document->Find(selectedId);if(!selectedNode)continue;const auto* selectedProperty=TypeRegistry::Global().FindProperty(selectedNode->type,property->name);if(!selectedProperty||selectedProperty->type!=property->type||!HasFlag(selectedProperty->flags,PropertyFlags::Editable))continue;propertyTargets.push_back(selectedId);const auto found=selectedNode->properties.find(property->name);const Variant selectedValue=found==selectedNode->properties.end()?selectedProperty->defaultValue:found->second;if(selectedValue!=effective)mixed=true;}
        if(propertyTargets.empty())propertyTargets.push_back(m_selected);
        bool changed = false, continuous = false;
        ImGui::PushID(property->name.c_str());
        if(mixed){ImGui::TextDisabled("— 混合值 (%zu)",propertyTargets.size());if(ImGui::IsItemHovered())ImGui::SetTooltip("修改後會批次套用到所有相容的已選元件。");}
        const char* propertyLabel=property->editor.displayName.empty()?property->name.c_str():property->editor.displayName.c_str();
        PropertyEditRequest editRequest{.property=*property,.value=value.Clone()};
        if(const auto* editor=PropertyEditorRegistry::Global().Resolve(*property)){changed=(*editor)(editRequest);value=std::move(editRequest.value);continuous=editRequest.continuous;}
        else if(auto* reference=value.TryGet<ResourceRefValue>()){ImGui::Text("%s: %s",propertyLabel,reference->lastKnownPath.empty()?"Select resource":reference->lastKnownPath.c_str());if(ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){reference->id={};reference->lastKnownPath=std::string(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);changed=true;}ImGui::EndDragDropTarget();}}
        else if(auto* token=value.TryGet<TokenRefValue>()){std::vector<std::string> tokenNames;if(const auto modern=m_document->Data().properties.find("styleSystem");modern!=m_document->Data().properties.end())if(const auto* system=modern->second.AsObject())if(const auto tokens=system->find("tokens");tokens!=system->end())if(const auto* list=tokens->second.AsArray())for(const auto& item:*list)if(const auto* definition=item.AsObject())if(const auto tokenName=definition->find("name");tokenName!=definition->end())if(const auto* text=tokenName->second.TryGet<std::string>())tokenNames.push_back(*text);if(ImGui::BeginCombo((std::string("Token: ")+propertyLabel).c_str(),token->name.empty()?"Select token":token->name.c_str())){for(const auto& tokenName:tokenNames)if(ImGui::Selectable(tokenName.c_str(),tokenName==token->name)){token->name=tokenName;changed=true;}ImGui::EndCombo();}}
        else ImGui::TextDisabled("%s  (unsupported value)", property->name.c_str());
        if(HasFlag(property->flags,PropertyFlags::ResourcePath)&&!selectedAssetPath.empty()){ImGui::SameLine();if(ImGui::SmallButton("使用選取素材")){if(auto* path=value.TryGet<std::string>())*path=selectedAssetPath;else if(auto* reference=value.TryGet<ResourceRefValue>()){reference->id={};reference->lastKnownPath=selectedAssetPath;}changed=true;continuous=false;}}
        const bool deactivated=ImGui::IsItemDeactivatedAfterEdit();
        if(!property->editor.description.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",property->editor.description.c_str());
        ImGui::SameLine();
        if(ImGui::SmallButton("↶")){value=property->defaultValue.Clone();changed=true;continuous=false;}if(ImGui::IsItemHovered())ImGui::SetTooltip("重設為 TypeRegistry 預設值；目前來源：%s",current==node->properties.end()?"型別預設":"本地覆寫");
        const bool recordKey=m_timelineAutoKey&&propertyTargets.size()==1&&((changed&&!continuous)||(continuous&&deactivated));
        Variant keyValue=recordKey?value.Clone():Variant{};
        if(propertyTargets.size()==1){
            EditVariant(property->name.c_str(), property->name, std::move(before), std::move(value), changed, continuous);
            if (continuous && deactivated && m_propertyTransaction) { const Status status = m_propertyTransaction->Commit(); if (!status) EmitStatus(status); m_propertyTransaction.reset(); }
        }else if(continuous){
            if(changed){if(!m_multiPropertyTransaction||m_multiTransactionProperty!=property->name){if(m_multiPropertyTransaction)(void)m_multiPropertyTransaction->Cancel();m_multiPropertyTransaction=std::make_unique<MultiPropertyEditTransaction>(*m_document,m_document->History(),propertyTargets,property->name,"Batch change "+property->name);m_multiTransactionProperty=property->name;}const Status status=m_multiPropertyTransaction->Update(value);if(!status)EmitStatus(status);else MarkEdited();}
            if(deactivated&&m_multiPropertyTransaction){const Status status=m_multiPropertyTransaction->Commit();if(!status)EmitStatus(status);m_multiPropertyTransaction.reset();m_multiTransactionProperty.clear();MarkEdited();}
        }else if(changed){auto command=std::make_unique<CompositeEditCommand>("Batch change "+property->name);for(const Uuid& target:propertyTargets){auto original=m_document->ReadProperty(target,property->name);if(original&&original.Value()!=value)command->Add(std::make_unique<PropertyChangeCommand>("Change "+property->name,target,property->name,original.Value(),value.Clone(),std::chrono::steady_clock::now(),false));}if(!command->Empty()){const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}}
        if(recordKey)RecordAnimationKey(m_selected,property->name,keyValue);
        ImGui::PopID();
    }

    ImGui::SeparatorText("Binding");
    VariantObject bindings;if(const auto found=node->properties.find("bindings");found!=node->properties.end()&&found->second.AsObject())bindings=*found->second.AsObject();
    for(auto& [target,value]:bindings){auto* definition=value.AsObject();if(!definition)continue;std::string path=definition->contains("path")&&definition->at("path").TryGet<std::string>()?*definition->at("path").TryGet<std::string>():"";std::string formatter=definition->contains("formatter")&&definition->at("formatter").TryGet<std::string>()?*definition->at("formatter").TryGet<std::string>():"";bool changedBinding=false;
        const auto* targetProperty=TypeRegistry::Global().FindProperty(node->type,target);if(ImGui::BeginCombo((target+" source").c_str(),path.empty()?"Select ViewModel property":path.c_str())){for(const auto& source:m_previewFixture.ViewModel().EnumerateProperties()){bool compatible=targetProperty&&source.type==targetProperty->type;for(const auto* candidate:m_formatters.Descriptors())if(candidate->input==source.type&&targetProperty&&candidate->output==targetProperty->type)compatible=true;if(!compatible)continue;if(ImGui::Selectable((source.path+"##binding-"+target).c_str(),path==source.path)){path=source.path;formatter.clear();changedBinding=true;}}ImGui::EndCombo();}
        if(ImGui::BeginCombo(("Formatter##"+target).c_str(),formatter.empty()?"None":formatter.c_str())){if(ImGui::Selectable("None",formatter.empty())){formatter.clear();changedBinding=true;}const auto source=m_previewFixture.ViewModel().Describe(path);for(const auto* candidate:m_formatters.Descriptors())if(source&&targetProperty&&candidate->input==source->type&&candidate->output==targetProperty->type)if(ImGui::Selectable((candidate->name+"##formatter-"+target).c_str(),formatter==candidate->name)){formatter=candidate->name;changedBinding=true;}ImGui::EndCombo();}
        if(changedBinding){VariantObject changed=bindings;auto* changedDefinition=changed[target].AsObject();(*changedDefinition)["path"]=path;(*changedDefinition)["formatter"]=formatter;EditVariant("bindings","bindings",node->properties.contains("bindings")?node->properties["bindings"]:Variant{},Variant(std::move(changed)),true,false);}}
    if (ImGui::Button("Add typed binding")) ImGui::OpenPopup("AddBinding");
    if (ImGui::BeginPopup("AddBinding")) { for (const auto* property : properties) if (HasFlag(property->flags, PropertyFlags::Bindable) && ImGui::MenuItem(property->name.c_str())) {
        VariantObject changed=bindings;changed[property->name]=VariantObject{{"path",std::string{}},{"formatter",std::string{}}};EditVariant("bindings","bindings",node->properties.contains("bindings")?node->properties["bindings"]:Variant{},Variant(std::move(changed)),true,false); ImGui::CloseCurrentPopup();
    } ImGui::EndPopup(); }
    if (!selectedAssetPath.empty() && node->type == "TextureRect" && ImGui::Button("Use selected asset"))
        EditVariant("path", "path", node->properties.contains("path") ? node->properties["path"] : Variant{}, Variant(selectedAssetPath), true, false);
    ImGui::TextDisabled("條件式 UI 請繫結 ViewModel computed property；Binding 不執行表達式。");
    if(ImGui::CollapsingHeader("Appearance · Style Binding"))RenderTheme();
    if(ImGui::CollapsingHeader("Accessibility"))ImGui::TextWrapped("Accessibility 屬性與焦點導覽直接顯示在上方對應分類；此區保留語意與驗證摘要。");
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
    for(const auto& record:preview.nodes){const auto visibility=record.properties.find("visibility");const auto* value=visibility==record.properties.end()?nullptr:visibility->second.TryGet<std::string>();if(!value||*value!="Collapsed")continue;const Rect parent=record.parent.Empty()?Rect{0,0,canvas.x,canvas.y}:(m_layout.contains(record.parent)?m_layout.at(record.parent):Rect{0,0,canvas.x,canvas.y});Rect anchors{},offsets{0,0,120,40};Vec2 minimum{120,40};if(const auto found=record.properties.find("anchors");found!=record.properties.end()&&found->second.TryGet<Rect>())anchors=*found->second.TryGet<Rect>();if(const auto found=record.properties.find("offsets");found!=record.properties.end()&&found->second.TryGet<Rect>())offsets=*found->second.TryGet<Rect>();if(const auto found=record.properties.find("minimumSize");found!=record.properties.end()&&found->second.TryGet<Vec2>())minimum=*found->second.TryGet<Vec2>();Rect authored=ui::ControlLayoutMath::ResolveChildRect(parent,anchors,offsets,minimum);authored.w=std::max(authored.w,16.0f);authored.h=std::max(authored.h,16.0f);m_layout[record.id]=authored;}
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
    m_gestureDragged = false;
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
    if(m_gesture==Gesture::Pivot&&m_document)(void)m_document->WriteProperty(m_selected,"pivot",Variant(m_pivotStart));
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
    m_gestureDragged = false;
    m_resizeHandle = 0;
    m_guideX = m_guideY = std::numeric_limits<float>::quiet_NaN();
}

void UIDesigner::RenderViewportToolbar() {
    if(m_viewport.interactivePreview&&ImGui::IsKeyPressed(ImGuiKey_Escape))m_viewport.interactivePreview=false;
    if(m_viewport.interactivePreview){
        ImGui::TextColored({.32f,1,.58f,1},"PREVIEW · 所有輸入交給 Runtime");ImGui::SameLine();
        if(ImGui::Button("返回 Edit  (Esc)"))m_viewport.interactivePreview=false;
        ImGui::SameLine();ImGui::Checkbox("像素精確",&m_viewport.pixelExactPreview);return;
    }
    ImGui::TextDisabled("EDIT");ImGui::SameLine();if(ImGui::Button("進入 Preview"))m_viewport.interactivePreview=true;ImGui::SameLine();
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
    ImGui::Checkbox("像素精確",&m_viewport.pixelExactPreview);
    ImGui::SameLine();
    ImGui::TextDisabled("%s · %s", SelectionSummary().c_str(),
                        ui::ChildLayoutPolicyName(SelectedParentPolicy()));
    if (!m_canvasHint.empty()) {
        ImGui::TextColored(ImVec4(.95f,.72f,.30f,1), "%s", m_canvasHint.c_str());
    }
}

bool UIDesigner::HandleCanvasInteraction(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
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
    bool pivotHandle=false;
    if(m_viewport.tool==DesignerTool::Select&&!m_selected.Empty()&&m_selected!=RootId()&&m_selection.size()==1&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=m_document->Find(m_selected);Vec2 pivot{.5f,.5f};if(node)if(const auto found=node->properties.find("pivot");found!=node->properties.end())if(const auto* value=found->second.TryGet<Vec2>())pivot=*value;const Rect selected=SelectedRect();const Vec2 point{selected.x+selected.w*pivot.x,selected.y+selected.h*pivot.y};const float hs=9.0f/scale;pivotHandle=std::abs(canvas.x-point.x)<=hs&&std::abs(canvas.y-point.y)<=hs;if(pivotHandle)handle=0;}
    if(handle==2||handle==6)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);else if(handle==4||handle==8)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);else if(handle==1||handle==5)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);else if(handle==3||handle==7)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        m_dragScale=std::max(scale,.01f);
        Uuid hit=(handle||anchorHandle||pivotHandle)?m_selected:HitTest(canvas);
        if(io.KeyAlt&&!handle){
            std::vector<Uuid> hits;for(auto iterator=m_document->Data().nodes.rbegin();iterator!=m_document->Data().nodes.rend();++iterator){if(const auto locked=iterator->properties.find("editorLocked");locked!=iterator->properties.end())if(const auto* value=locked->second.TryGet<bool>();value&&*value)continue;const auto found=m_layout.find(iterator->id);if(found==m_layout.end())continue;const Rect rect=found->second;if(canvas.x>=rect.x&&canvas.x<=rect.x+rect.w&&canvas.y>=rect.y&&canvas.y<=rect.y+rect.h)hits.push_back(iterator->id);}
            if(!hits.empty()){auto current=std::find(hits.begin(),hits.end(),m_selected);if(current==hits.end()||++current==hits.end())hit=hits.front();else hit=*current;}
        }
        if(!hit.Empty()){
            if(io.KeyCtrl)m_selection.insert(hit);else if(!m_selection.contains(hit))m_selection={hit};
            m_selected=hit;m_canvasHint.clear();const auto policy=SelectedParentPolicy();
            const auto* selectedNode=m_document->Find(hit);const bool locked=selectedNode&&selectedNode->properties.contains("editorLocked")&&selectedNode->properties.at("editorLocked").TryGet<bool>()&&*selectedNode->properties.at("editorLocked").TryGet<bool>();
            if(locked)m_canvasHint="此元件已在 Scene Tree 鎖定";
            else if(pivotHandle){m_pivotStart={.5f,.5f};if(selectedNode)if(const auto found=selectedNode->properties.find("pivot");found!=selectedNode->properties.end())if(const auto* value=found->second.TryGet<Vec2>())m_pivotStart=*value;m_dragStart=canvas;m_dragCurrent=canvas;m_gesture=Gesture::Pivot;m_gestureDragged=false;m_canvasHint="拖曳旋轉／縮放中心";}
            else if(anchorHandle){const auto* anchorNode=m_document->Find(hit);m_anchorsStart={};m_anchorOffsetsStart={};if(anchorNode){if(const auto found=anchorNode->properties.find("anchors");found!=anchorNode->properties.end())if(const auto* value=found->second.TryGet<Rect>())m_anchorsStart=*value;if(const auto found=anchorNode->properties.find("offsets");found!=anchorNode->properties.end())if(const auto* value=found->second.TryGet<Rect>())m_anchorOffsetsStart=*value;}m_rectStart=SelectedRect();m_dragStart=canvas;m_dragCurrent=canvas;m_anchorHandle=anchorHandle;m_gesture=Gesture::Anchors;m_gestureDragged=false;}
            else if(hit!=RootId()){if(policy==ui::ChildLayoutPolicy::Free){BeginFreeTransform(hit,canvas,handle);m_dragCurrent=canvas;}else{m_gesture=Gesture::Reorder;m_gestureDragged=false;m_dragStart=canvas;m_dragCurrent=canvas;m_reorderPreview=m_document->ChildIndex(hit);m_canvasHint=std::string("由 ")+ui::ChildLayoutPolicyName(policy)+" 控制；拖曳只會排序";}}}
        else{m_gesture=Gesture::Marquee;m_dragStart=canvas;m_dragCurrent=canvas;m_marqueeCurrent=canvas;m_marqueeAdditive=io.KeyCtrl;if(!m_marqueeAdditive){m_selection.clear();m_selected={};}}
    }
    m_guideX=m_guideY=std::numeric_limits<float>::quiet_NaN();
    if(m_gesture==Gesture::Anchors&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){
        m_gestureDragged=true;const ImVec2 drag=ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,0.0f);m_dragCurrent={m_dragStart.x+drag.x/m_dragScale,m_dragStart.y+drag.y/m_dragScale};
        const Rect parent=ParentRect(m_selected);Rect anchors=m_anchorsStart;float x=parent.w>0?(m_dragCurrent.x-parent.x)/parent.w:0,y=parent.h>0?(m_dragCurrent.y-parent.y)/parent.h:0;
        const auto snap=[](float value){for(float target:{0.0f,.5f,1.0f})if(std::abs(value-target)<.035f)return target;return std::clamp(value,0.0f,1.0f);};if(m_viewport.smartGuides){x=snap(x);y=snap(y);}else{x=std::clamp(x,0.0f,1.0f);y=std::clamp(y,0.0f,1.0f);}
        if(m_anchorHandle==1||m_anchorHandle==4)anchors.x=std::min(x,anchors.w);else anchors.w=std::max(x,anchors.x);if(m_anchorHandle==1||m_anchorHandle==2)anchors.y=std::min(y,anchors.h);else anchors.h=std::max(y,anchors.y);
        const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(parent,anchors,m_rectStart);Status status=m_document->WriteProperty(m_selected,"anchors",Variant(anchors));if(status)status=m_document->WriteProperty(m_selected,"offsets",Variant(offsets));if(!status){EmitStatus(status);CancelCanvasGesture();}else MarkEdited();return true;
    }
    if(m_gesture==Gesture::Pivot&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){
        m_gestureDragged=true;const ImVec2 drag=ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,0.0f);m_dragCurrent={m_dragStart.x+drag.x/m_dragScale,m_dragStart.y+drag.y/m_dragScale};const Rect selected=SelectedRect();Vec2 pivot{selected.w>0?(m_dragCurrent.x-selected.x)/selected.w:.5f,selected.h>0?(m_dragCurrent.y-selected.y)/selected.h:.5f};const auto snap=[](float value){for(float target:{0.0f,.5f,1.0f})if(std::abs(value-target)<.035f)return target;return std::clamp(value,0.0f,1.0f);};pivot.x=snap(pivot.x);pivot.y=snap(pivot.y);const Status status=m_document->WriteProperty(m_selected,"pivot",Variant(pivot));if(!status){EmitStatus(status);CancelCanvasGesture();}else{m_canvasHint="Pivot  "+std::to_string(static_cast<int>(std::round(pivot.x*100)))+"%, "+std::to_string(static_cast<int>(std::round(pivot.y*100)))+"%";MarkEdited();}return true;
    }
    if(m_gesture==Gesture::Move&&m_groupMove&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){
        m_gestureDragged=true;const ImVec2 drag=ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,0.0f);m_dragCurrent={m_dragStart.x+drag.x/m_dragScale,m_dragStart.y+drag.y/m_dragScale};
        Vec2 delta{m_dragCurrent.x-m_dragStart.x,m_dragCurrent.y-m_dragStart.y};
        if(m_viewport.gridSnap&&!io.KeyAlt){delta.x=std::round(delta.x/m_gridSize)*m_gridSize;delta.y=std::round(delta.y/m_gridSize)*m_gridSize;}
        for(const auto& [id,before]:m_groupOffsetsStart){Rect value=before;value.x+=delta.x;value.y+=delta.y;const Status status=m_document->WriteProperty(id,"offsets",Variant(value));if(!status){EmitStatus(status);CancelCanvasGesture();return true;}}
        MarkEdited();return true;
    }
    if((m_gesture==Gesture::Move||m_gesture==Gesture::Resize)&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)&&m_propertyTransaction){
        m_gestureDragged=true;const ImVec2 drag=ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,0.0f);m_dragCurrent={m_dragStart.x+drag.x/m_dragScale,m_dragStart.y+drag.y/m_dragScale};
        Vec2 delta{m_dragCurrent.x-m_dragStart.x,m_dragCurrent.y-m_dragStart.y};Rect target=m_rectStart;
        if(m_viewport.gridSnap&&m_gesture==Gesture::Move&&!io.KeyAlt){delta.x=std::round(delta.x/m_gridSize)*m_gridSize;delta.y=std::round(delta.y/m_gridSize)*m_gridSize;}
        if(m_gesture==Gesture::Move){target.x+=delta.x;target.y+=delta.y;}else{float left=target.x,top=target.y,right=target.x+target.w,bottom=target.y+target.h;
            if(m_resizeHandle==1||m_resizeHandle==7||m_resizeHandle==8)left+=delta.x;if(m_resizeHandle==3||m_resizeHandle==4||m_resizeHandle==5)right+=delta.x;
            if(m_resizeHandle>=1&&m_resizeHandle<=3)top+=delta.y;if(m_resizeHandle>=5&&m_resizeHandle<=7)bottom+=delta.y;
            if(right-left<8){if(m_resizeHandle==1||m_resizeHandle==7||m_resizeHandle==8)left=right-8;else right=left+8;}
            if(bottom-top<8){if(m_resizeHandle>=1&&m_resizeHandle<=3)top=bottom-8;else bottom=top+8;}target={left,top,right-left,bottom-top};}
        if(m_viewport.gridSnap&&m_gesture==Gesture::Resize&&!io.KeyAlt){const auto snap=[&](float value){return std::round(value/m_gridSize)*m_gridSize;};const float right=snap(target.x+target.w),bottom=snap(target.y+target.h);target.x=snap(target.x);target.y=snap(target.y);target.w=std::max(8.0f,right-target.x);target.h=std::max(8.0f,bottom-target.y);}
        if(m_viewport.smartGuides&&m_gesture==Gesture::Move&&!io.KeyAlt){std::vector<float> xs{0,CanvasSize().x*.5f,CanvasSize().x},ys{0,CanvasSize().y*.5f,CanvasSize().y};
            const auto* selected=m_document->Find(m_selected);for(const auto& [id,r]:m_layout){if(id==m_selected)continue;const auto* other=m_document->Find(id);if(selected&&other&&selected->parent==other->parent){xs.insert(xs.end(),{r.x,r.x+r.w*.5f,r.x+r.w});ys.insert(ys.end(),{r.y,r.y+r.h*.5f,r.y+r.h});}}
            const float threshold=6.0f/scale;float bestX=threshold,bestY=threshold,adjustX=0,adjustY=0;for(float line:xs)for(float edge:{target.x,target.x+target.w*.5f,target.x+target.w})if(std::abs(line-edge)<bestX){bestX=std::abs(line-edge);adjustX=line-edge;m_guideX=line;}for(float line:ys)for(float edge:{target.y,target.y+target.h*.5f,target.y+target.h})if(std::abs(line-edge)<bestY){bestY=std::abs(line-edge);adjustY=line-edge;m_guideY=line;}target.x+=adjustX;target.y+=adjustY;}
        if(m_viewport.smartGuides&&m_gesture==Gesture::Resize&&!io.KeyAlt){std::vector<float> xs{0,CanvasSize().x*.5f,CanvasSize().x},ys{0,CanvasSize().y*.5f,CanvasSize().y};const auto* selected=m_document->Find(m_selected);for(const auto& [id,r]:m_layout){if(id==m_selected)continue;const auto* other=m_document->Find(id);if(selected&&other&&selected->parent==other->parent){xs.insert(xs.end(),{r.x,r.x+r.w*.5f,r.x+r.w});ys.insert(ys.end(),{r.y,r.y+r.h*.5f,r.y+r.h});}}const float threshold=6.0f/scale;float left=target.x,right=target.x+target.w,top=target.y,bottom=target.y+target.h,bestX=threshold,bestY=threshold;const bool moveLeft=m_resizeHandle==1||m_resizeHandle==7||m_resizeHandle==8,moveRight=m_resizeHandle==3||m_resizeHandle==4||m_resizeHandle==5,moveTop=m_resizeHandle>=1&&m_resizeHandle<=3,moveBottom=m_resizeHandle>=5&&m_resizeHandle<=7;for(float line:xs){const float edge=moveLeft?left:right;if((moveLeft||moveRight)&&std::abs(line-edge)<bestX){bestX=std::abs(line-edge);if(moveLeft)left=line;else right=line;m_guideX=line;}}for(float line:ys){const float edge=moveTop?top:bottom;if((moveTop||moveBottom)&&std::abs(line-edge)<bestY){bestY=std::abs(line-edge);if(moveTop)top=line;else bottom=line;m_guideY=line;}}target={left,top,std::max(8.0f,right-left),std::max(8.0f,bottom-top)};}
        if(m_gesture==Gesture::Resize&&(m_resizeHandle==1||m_resizeHandle==3||m_resizeHandle==5||m_resizeHandle==7)){const auto* selected=m_document->Find(m_selected);bool locked=selected&&selected->type=="TextureRect";if(selected)if(const auto found=selected->properties.find("lockAspectRatio");found!=selected->properties.end())if(const auto* value=found->second.TryGet<bool>())locked=*value;if(io.KeyShift)locked=!locked;if(locked){float ratio=m_rectStart.h>0?m_rectStart.w/m_rectStart.h:1.0f;if(selected&&m_imageSizeResolver)if(const auto path=selected->properties.find("path");path!=selected->properties.end()&&path->second.TryGet<std::string>())if(const auto size=m_imageSizeResolver(*path->second.TryGet<std::string>());size&&size->y>0)ratio=size->x/size->y;float left=target.x,right=target.x+target.w,top=target.y,bottom=target.y+target.h;if(std::abs(delta.x)>=std::abs(delta.y)*ratio){const float height=target.w/ratio;if(m_resizeHandle==1||m_resizeHandle==3)top=bottom-height;else bottom=top+height;}else{const float width=target.h*ratio;if(m_resizeHandle==1||m_resizeHandle==7)left=right-width;else right=left+width;}target={left,top,std::max(8.0f,right-left),std::max(8.0f,bottom-top)};}}
        if(m_gesture==Gesture::Resize)m_canvasHint=std::to_string(static_cast<int>(std::round(target.w)))+" × "+std::to_string(static_cast<int>(std::round(target.h)));
        Rect anchors{};if(const auto* node=m_document->Find(m_selected))if(const auto it=node->properties.find("anchors");it!=node->properties.end())if(const auto* value=it->second.TryGet<Rect>())anchors=*value;
        const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(m_selected),anchors,target);const Status status=m_propertyTransaction->Update(Variant(offsets));if(!status)EmitStatus(status);MarkEdited();return true;
    }
    if(m_gesture==Gesture::Reorder&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){m_gestureDragged=true;const ImVec2 drag=ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,0.0f);m_dragCurrent={m_dragStart.x+drag.x/m_dragScale,m_dragStart.y+drag.y/m_dragScale};m_reorderPreview=InsertionIndex(m_selected,m_dragCurrent);return true;}
    if(m_gesture==Gesture::Marquee&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){const ImVec2 drag=ImGui::GetMouseDragDelta(ImGuiMouseButton_Left,0.0f);m_dragCurrent={m_dragStart.x+drag.x/m_dragScale,m_dragStart.y+drag.y/m_dragScale};m_marqueeCurrent=m_dragCurrent;return true;}
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        if(m_gesture==Gesture::Marquee){const float left=std::min(m_dragStart.x,m_marqueeCurrent.x),right=std::max(m_dragStart.x,m_marqueeCurrent.x),top=std::min(m_dragStart.y,m_marqueeCurrent.y),bottom=std::max(m_dragStart.y,m_marqueeCurrent.y);for(const auto& node:m_document->Data().nodes){if(node.id==RootId())continue;if(const auto locked=node.properties.find("editorLocked");locked!=node.properties.end())if(const auto* value=locked->second.TryGet<bool>();value&&*value)continue;const auto found=m_layout.find(node.id);if(found==m_layout.end())continue;const Rect r=found->second;if(r.x<=right&&r.x+r.w>=left&&r.y<=bottom&&r.y+r.h>=top){m_selection.insert(node.id);m_selected=node.id;}}m_gesture=Gesture::None;return true;}
        if(m_gesture==Gesture::Anchors){if(m_gestureDragged){auto anchors=m_document->ReadProperty(m_selected,"anchors"),offsets=m_document->ReadProperty(m_selected,"offsets");auto command=std::make_unique<CompositeEditCommand>("調整錨點");if(anchors)command->Add(std::make_unique<PropertyChangeCommand>("錨點",m_selected,"anchors",Variant(m_anchorsStart),anchors.Value()));if(offsets)command->Add(std::make_unique<PropertyChangeCommand>("保持位置",m_selected,"offsets",Variant(m_anchorOffsetsStart),offsets.Value()));const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);}m_anchorHandle=0;m_gesture=Gesture::None;m_gestureDragged=false;return true;}
        if(m_gesture==Gesture::Pivot){if(m_gestureDragged){auto pivot=m_document->ReadProperty(m_selected,"pivot");if(pivot){auto command=std::make_unique<PropertyChangeCommand>("調整 Pivot",m_selected,"pivot",Variant(m_pivotStart),pivot.Value(),std::chrono::steady_clock::now(),false);const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);}}m_gesture=Gesture::None;m_gestureDragged=false;return true;}
        if(m_gesture==Gesture::Move&&m_groupMove){if(m_gestureDragged){auto command=std::make_unique<CompositeEditCommand>("移動多個元件");for(const auto& [id,before]:m_groupOffsetsStart){auto after=m_document->ReadProperty(id,"offsets");if(after)command->Add(std::make_unique<PropertyChangeCommand>("移動元件",id,"offsets",Variant(before),after.Value()));}const Status status=m_document->History().CommitApplied(std::move(command));if(!status)EmitStatus(status);}m_groupOffsetsStart.clear();m_groupMove=false;m_gesture=Gesture::None;m_gestureDragged=false;return true;}
        if(m_gesture==Gesture::Move||m_gesture==Gesture::Resize){if(m_propertyTransaction){const Status status=m_propertyTransaction->Commit();if(!status)EmitStatus(status);m_propertyTransaction.reset();}m_gesture=Gesture::None;m_gestureDragged=false;return true;}
        if(m_gesture==Gesture::Reorder){if(m_gestureDragged)CommitManagedDrag(m_dragCurrent,false);m_gesture=Gesture::None;m_gestureDragged=false;return true;}}
    return false;
}

void UIDesigner::RenderCanvasOverlay(ImVec2 p0, float scale) {
    if (!m_document) return; ImDrawList* draw=ImGui::GetWindowDrawList();const Vec2 canvas=CanvasSize();const ImVec2 max{p0.x+canvas.x*scale,p0.y+canvas.y*scale};draw->PushClipRect(p0,max,true);
    if(m_viewport.gridVisible){float step=static_cast<float>(m_gridSize);while(step*scale<12)step*=2;int line=0;for(float x=0;x<=canvas.x;x+=step,++line)draw->AddLine({p0.x+x*scale,p0.y},{p0.x+x*scale,max.y},line%4==0?IM_COL32(92,110,135,75):IM_COL32(72,84,102,40));line=0;for(float y=0;y<=canvas.y;y+=step,++line)draw->AddLine({p0.x,p0.y+y*scale},{max.x,p0.y+y*scale},line%4==0?IM_COL32(92,110,135,75):IM_COL32(72,84,102,40));}
    if(!std::isnan(m_guideX))draw->AddLine({p0.x+m_guideX*scale,p0.y},{p0.x+m_guideX*scale,max.y},IM_COL32(255,92,180,230),1.5f);
    if(!std::isnan(m_guideY))draw->AddLine({p0.x,p0.y+m_guideY*scale},{max.x,p0.y+m_guideY*scale},IM_COL32(255,92,180,230),1.5f);
    for(const auto& node:m_document->Data().nodes){const auto it=m_layout.find(node.id);if(it==m_layout.end())continue;const bool selected=m_selection.contains(node.id),primary=node.id==m_selected,hover=node.id==m_hovered;if(!selected&&!hover&&!m_viewport.showAllOutlines)continue;std::string visibility="Visible";if(const auto found=node.properties.find("visibility");found!=node.properties.end()&&found->second.TryGet<std::string>())visibility=*found->second.TryGet<std::string>();const Rect r=it->second;const ImU32 color=visibility=="Collapsed"?IM_COL32(190,100,205,220):visibility=="Hidden"?IM_COL32(135,150,175,210):selected?IM_COL32(71,140,191,255):IM_COL32(150,170,195,150);draw->AddRect({p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},color,3.0f,ImDrawFlags_None,selected?2.0f:1.0f);if(primary){const std::string label=node.name+(visibility=="Visible"?"":"  ["+visibility+"]");draw->AddText({p0.x+r.x*scale+5,p0.y+r.y*scale-20},IM_COL32(220,232,245,255),label.c_str());}}
    if(!m_selected.Empty()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const Rect r=SelectedRect();const ImVec2 points[8]{{p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h*.5f)*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h*.5f)*scale}};for(const auto& point:points)draw->AddRectFilled({point.x-4,point.y-4},{point.x+4,point.y+4},IM_COL32(225,235,248,255),1);}
    if(m_viewport.tool==DesignerTool::Select&&!m_selected.Empty()&&m_selected!=RootId()&&m_selection.size()==1&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=m_document->Find(m_selected);Vec2 pivot{.5f,.5f};if(node)if(const auto found=node->properties.find("pivot");found!=node->properties.end())if(const auto* value=found->second.TryGet<Vec2>())pivot=*value;const Rect selected=SelectedRect();const ImVec2 point{p0.x+(selected.x+selected.w*pivot.x)*scale,p0.y+(selected.y+selected.h*pivot.y)*scale};draw->AddCircleFilled(point,5.5f,IM_COL32(255,185,72,255));draw->AddCircle(point,8.0f,IM_COL32(28,32,40,240),0,2.0f);draw->AddLine({point.x-11,point.y},{point.x+11,point.y},IM_COL32(255,185,72,230),1.5f);draw->AddLine({point.x,point.y-11},{point.x,point.y+11},IM_COL32(255,185,72,230),1.5f);}
    if(m_viewport.tool==DesignerTool::Anchors&&!m_selected.Empty()&&m_selected!=RootId()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=m_document->Find(m_selected);Rect anchors{};if(node)if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;const Rect parent=ParentRect(m_selected),selected=SelectedRect();const ImVec2 points[4]{{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale},{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale}};const ImVec2 corners[4]{{p0.x+selected.x*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+(selected.y+selected.h)*scale},{p0.x+selected.x*scale,p0.y+(selected.y+selected.h)*scale}};for(int index=0;index<4;++index){draw->AddLine(points[index],corners[index],IM_COL32(255,120,190,150));draw->AddQuadFilled({points[index].x,points[index].y-6},{points[index].x+6,points[index].y},{points[index].x,points[index].y+6},{points[index].x-6,points[index].y},IM_COL32(255,120,190,255));}}
    if(m_gesture==Gesture::Reorder){const auto* node=m_document->Find(m_selected);if(node){std::vector<const resource::NodeRecord*> siblings;for(const auto* sibling:std::as_const(*m_document).Children(node->parent))if(sibling->id!=m_selected)siblings.push_back(sibling);Rect marker{};const auto policy=SelectedParentPolicy();if(!siblings.empty()){if(m_reorderPreview<siblings.size()&&m_layout.contains(siblings[m_reorderPreview]->id))marker=m_layout.at(siblings[m_reorderPreview]->id);else if(m_layout.contains(siblings.back()->id)){marker=m_layout.at(siblings.back()->id);if(policy==ui::ChildLayoutPolicy::LinearX)marker.x+=marker.w;else marker.y+=marker.h;}}if(policy==ui::ChildLayoutPolicy::LinearX)draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+marker.x*scale,p0.y+(marker.y+marker.h)*scale},IM_COL32(71,140,191,255),3);else draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+(marker.x+marker.w)*scale,p0.y+marker.y*scale},IM_COL32(71,140,191,255),3);}}
    if(m_gesture==Gesture::Marquee){const ImVec2 a{p0.x+m_dragStart.x*scale,p0.y+m_dragStart.y*scale},b{p0.x+m_marqueeCurrent.x*scale,p0.y+m_marqueeCurrent.y*scale};draw->AddRectFilled({std::min(a.x,b.x),std::min(a.y,b.y)},{std::max(a.x,b.x),std::max(a.y,b.y)},IM_COL32(71,140,191,35));draw->AddRect({std::min(a.x,b.x),std::min(a.y,b.y)},{std::max(a.x,b.x),std::max(a.y,b.y)},IM_COL32(71,140,191,220));}
    if(m_gesture==Gesture::Resize&&!m_canvasHint.empty()){const ImVec2 mouse=ImGui::GetMousePos(),textSize=ImGui::CalcTextSize(m_canvasHint.c_str());const ImVec2 a{mouse.x+14,mouse.y+14},b{a.x+textSize.x+12,a.y+textSize.y+8};draw->AddRectFilled(a,b,IM_COL32(20,24,32,235),4);draw->AddText({a.x+6,a.y+4},IM_COL32(235,240,248,255),m_canvasHint.c_str());}
    draw->PopClipRect();
}

void UIDesigner::AddImageAt(float x,float y,const std::string& image){AddNode("TextureRect",{x,y},image);}

void UIDesigner::RecordAnimationKey(const Uuid& node,const std::string& property,const Variant& value){
    if(!m_document||node.Empty()||property.empty())return;
    const auto found=m_document->Data().properties.find("animations");
    if(found==m_document->Data().properties.end())return;
    const Variant before=found->second.Clone();auto parsed=ui::ParseUIAnimationLibrary(before,m_pathText);
    if(!parsed){EmitStatus(Status::Fail(parsed.Diagnostics()));return;}
    auto library=parsed.TakeValue();
    auto clip=std::find_if(library.clips.begin(),library.clips.end(),[&](const ui::AnimationClip& candidate){return candidate.id==m_selectedClip;});
    if(clip==library.clips.end())clip=library.clips.begin();if(clip==library.clips.end())return;m_selectedClip=clip->id;
    auto track=std::find_if(clip->tracks.begin(),clip->tracks.end(),[&](const ui::AnimationTrack& candidate){return candidate.node==node&&candidate.property==property;});
    if(track==clip->tracks.end()){clip->tracks.push_back({.node=node,.property=property});track=std::prev(clip->tracks.end());m_timelineTrack=static_cast<int>(clip->tracks.size())-1;}
    const float time=std::max(0.0f,m_timelineTime);auto key=std::find_if(track->keys.begin(),track->keys.end(),[&](const ui::AnimationKey& candidate){return std::abs(candidate.time-time)<.0005f;});
    if(key==track->keys.end())track->keys.push_back({time,value.Clone(),ui::Ease::Linear,ui::KeyInterpolation::Linear});else key->value=value.Clone();
    std::stable_sort(track->keys.begin(),track->keys.end(),[](const ui::AnimationKey& left,const ui::AnimationKey& right){return left.time<right.time;});
    const Variant after=ui::WriteUIAnimationLibrary(library);if(after==before)return;
    auto command=std::make_unique<PropertyChangeCommand>("Auto-key "+property,m_document->DocumentId(),"animations",before,after);
    const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();
}

void UIDesigner::RenderAnimation(){
    if(!m_document){ImGui::TextDisabled("開啟 UI Scene 以編輯 Clip。");return;}
    const auto found=m_document->Data().properties.find("animations");
    if(found==m_document->Data().properties.end()){
        ImGui::TextWrapped("此 Scene 尚未建立 Animation Library。建立後會同時產生 Default Clip、Entry 與 Default State。");
        if(ImGui::Button("建立 Animation Library")){
            ui::UIAnimationLibrary library;ui::AnimationClip clip;clip.id=Uuid::FromName(m_document->DocumentId().ToString()+"/animations/Default");clip.name="Default";clip.duration=.3f;
            const Uuid clipId=clip.id,state=Uuid::FromName(m_document->DocumentId().ToString()+"/animations/DefaultState");library.clips.push_back(std::move(clip));library.machine.entry=state;library.machine.states.push_back({state,"Default",clipId,{80,80}});
            auto command=std::make_unique<PropertyChangeCommand>("Create Animation Library",m_document->DocumentId(),"animations",Variant{},ui::WriteUIAnimationLibrary(library));
            const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else{m_selectedClip=clipId;MarkEdited();}
        }
        return;
    }
    const Variant before=found->second.Clone();auto parsed=ui::ParseUIAnimationLibrary(before,m_pathText);if(!parsed){EmitStatus(Status::Fail(parsed.Diagnostics()));ImGui::TextColored({1,.4f,.35f,1},"Animation Library 無法解析。");return;}auto library=parsed.TakeValue();
    if(library.clips.empty()){ImGui::TextColored({1,.4f,.35f,1},"Animation Library 沒有 Clip。");return;}
    auto selected=std::find_if(library.clips.begin(),library.clips.end(),[&](const ui::AnimationClip& clip){return clip.id==m_selectedClip;});if(selected==library.clips.end()){selected=library.clips.begin();m_selectedClip=selected->id;m_timelineTrack=m_timelineKey=-1;}
    bool changed=false,previewChanged=false;
    if(ImGui::BeginCombo("Clip",selected->name.c_str())){for(auto& clip:library.clips)if(ImGui::Selectable((clip.name+"##clip-"+clip.id.ToString()).c_str(),clip.id==selected->id)){m_selectedClip=clip.id;selected=std::find_if(library.clips.begin(),library.clips.end(),[&](const ui::AnimationClip& item){return item.id==m_selectedClip;});m_timelineTrack=m_timelineKey=-1;m_timelineTime=0;previewChanged=true;}ImGui::EndCombo();}
    ImGui::SameLine();if(ImGui::Button("＋ Clip")){ui::AnimationClip clip;clip.id=Uuid::Random();clip.name="Clip "+std::to_string(library.clips.size()+1);clip.duration=.3f;m_selectedClip=clip.id;library.clips.push_back(std::move(clip));selected=std::prev(library.clips.end());changed=true;}
    ImGui::SetNextItemWidth(180);changed|=ImGui::InputText("Name",&selected->name);float duration=selected->duration;if(ImGui::DragFloat("Duration",&duration,.01f,0,120,"%.3fs")){selected->duration=std::max(0.0f,duration);m_timelineTime=std::min(m_timelineTime,selected->duration);changed=previewChanged=true;}changed|=ImGui::Checkbox("Loop",&selected->loop);
    if(ImGui::Button(m_timelinePlaying?"Pause":"Play")){m_timelinePlaying=!m_timelinePlaying;previewChanged=true;}ImGui::SameLine();if(ImGui::Button("Stop")){m_timelinePlaying=false;m_timelineTime=0;previewChanged=true;}ImGui::SameLine();ImGui::Checkbox("Auto-key",&m_timelineAutoKey);
    if(m_timelinePlaying){m_timelineTime+=ImGui::GetIO().DeltaTime;if(m_timelineTime>selected->duration){if(selected->loop&&selected->duration>0)m_timelineTime=std::fmod(m_timelineTime,selected->duration);else{m_timelineTime=selected->duration;m_timelinePlaying=false;}previewChanged=true;}}
    ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##clip-scrub",&m_timelineTime,0,std::max(.001f,selected->duration),"%.3fs"))previewChanged=true;
    const auto animatable=[](const PropertyInfo& property){return property.set&&HasFlag(property.flags,PropertyFlags::Editable)&&(property.type==VariantType::Bool||property.type==VariantType::Integer||property.type==VariantType::Number||property.type==VariantType::String||property.type==VariantType::Vec2||property.type==VariantType::Rect||property.type==VariantType::Color);};
    const auto propertiesFor=[&](const resource::NodeRecord& record){std::vector<const PropertyInfo*> result;std::unordered_set<std::string> seen;std::string type=record.type;while(const auto* info=TypeRegistry::Global().Find(type)){for(const auto& property:info->properties)if(animatable(property)&&seen.insert(property.name).second)result.push_back(&property);type=info->base;if(type.empty())break;}return result;};
    if(const auto* control=m_document->Find(m_selected)){const auto properties=propertiesFor(*control);if(!properties.empty()&&!TypeRegistry::Global().FindProperty(control->type,m_timelineProperty))m_timelineProperty=properties.front()->name;if(ImGui::BeginCombo("Track Property",m_timelineProperty.c_str())){for(const auto* property:properties)if(ImGui::Selectable(property->name.c_str(),property->name==m_timelineProperty))m_timelineProperty=property->name;ImGui::EndCombo();}ImGui::SameLine();if(ImGui::Button("＋ Track")&&!m_timelineProperty.empty()){auto current=m_document->ReadProperty(m_selected,m_timelineProperty);const auto* descriptor=TypeRegistry::Global().FindProperty(control->type,m_timelineProperty);ui::AnimationTrack track{.node=m_selected,.property=m_timelineProperty};track.keys.push_back({m_timelineTime,current&&current.Value().Type()!=VariantType::Null?current.Value():(descriptor?descriptor->defaultValue.Clone():Variant{}),ui::Ease::Linear,ui::KeyInterpolation::Linear});selected->tracks.push_back(std::move(track));m_timelineTrack=static_cast<int>(selected->tracks.size())-1;m_timelineKey=0;changed=previewChanged=true;}}
    ImGui::SeparatorText("Typed Tracks");
    for(int index=0;index<static_cast<int>(selected->tracks.size());++index){const auto& track=selected->tracks[static_cast<std::size_t>(index)];const auto* target=m_document->Find(track.node);const std::string label=(target?target->name:track.node.ToString().substr(0,8))+" · "+track.property+"  ("+std::to_string(track.keys.size())+")";if(ImGui::Selectable((label+"##track-"+std::to_string(index)).c_str(),m_timelineTrack==index)){m_timelineTrack=index;m_timelineKey=-1;}}
    if(m_timelineTrack>=static_cast<int>(selected->tracks.size()))m_timelineTrack=selected->tracks.empty()?-1:static_cast<int>(selected->tracks.size()-1);
    if(m_timelineTrack>=0){auto& track=selected->tracks[static_cast<std::size_t>(m_timelineTrack)];if(ImGui::Button("＋ Key")){auto current=m_document->ReadProperty(track.node,track.property);track.keys.push_back({m_timelineTime,current?current.Value():Variant{},ui::Ease::Linear,ui::KeyInterpolation::Linear});m_timelineKey=static_cast<int>(track.keys.size())-1;changed=previewChanged=true;}ImGui::SameLine();if(ImGui::Button("Delete Track")){selected->tracks.erase(selected->tracks.begin()+m_timelineTrack);m_timelineTrack=m_timelineKey=-1;changed=previewChanged=true;}else{for(int index=0;index<static_cast<int>(track.keys.size());++index){auto& key=track.keys[static_cast<std::size_t>(index)];ImGui::PushID(index);if(ImGui::Selectable((std::to_string(key.time)+"s##animation-key").c_str(),m_timelineKey==index)){m_timelineKey=index;m_timelineTime=key.time;previewChanged=true;}ImGui::PopID();}if(m_timelineKey>=static_cast<int>(track.keys.size()))m_timelineKey=track.keys.empty()?-1:static_cast<int>(track.keys.size()-1);if(m_timelineKey>=0){auto& key=track.keys[static_cast<std::size_t>(m_timelineKey)];if(ImGui::DragFloat("Key Time",&key.time,.01f,0,selected->duration,"%.3fs")){key.time=std::clamp(key.time,0.0f,selected->duration);m_timelineTime=key.time;changed=previewChanged=true;}ui::ActionArgumentDescriptor valueEditor{.name="value",.displayName="Value",.type=key.value.Type()};changed|=RenderActionArgument(valueEditor,key.value);int ease=static_cast<int>(key.ease);if(ImGui::Combo("Ease",&ease,"Linear\0Ease In\0Ease Out\0Ease In Out\0Step\0")){key.ease=static_cast<ui::Ease>(ease);changed=true;}bool discrete=key.interpolation==ui::KeyInterpolation::Discrete;if(ImGui::Checkbox("Discrete",&discrete)){key.interpolation=discrete?ui::KeyInterpolation::Discrete:ui::KeyInterpolation::Linear;changed=true;}if(ImGui::Button("Delete Key")){track.keys.erase(track.keys.begin()+m_timelineKey);m_timelineKey=-1;changed=previewChanged=true;}}}}
    if(changed){for(auto& track:selected->tracks)std::stable_sort(track.keys.begin(),track.keys.end(),[](const ui::AnimationKey& left,const ui::AnimationKey& right){return left.time<right.time;});const Status valid=library.Validate(m_pathText);if(!valid)EmitStatus(valid);else{auto command=std::make_unique<PropertyChangeCommand>("Edit Animation Clip",m_document->DocumentId(),"animations",before,ui::WriteUIAnimationLibrary(library));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}}
    if((previewChanged||m_timelinePlaying)&&m_animationPreview){const Status status=m_animationPreview(m_selectedClip,m_timelineTime,m_timelinePlaying);if(!status)EmitStatus(status);}
}

void UIDesigner::RenderTheme(){
    if(!m_document){ImGui::TextDisabled("Open a UI scene to edit styles.");return;}
    ui::StyleThemeData theme;std::string source="Embedded styleSystem";bool foundTheme=false;
    if(const auto found=m_document->Data().properties.find("styleSystem");found!=m_document->Data().properties.end()){auto parsed=ui::ParseStyleTheme(found->second);if(parsed){theme=parsed.TakeValue();foundTheme=true;}else EmitStatus(Status::Fail(parsed.Diagnostics()));}
    if(!foundTheme)if(const auto found=m_document->Data().properties.find("theme");found!=m_document->Data().properties.end())if(const auto* reference=found->second.TryGet<ResourceRefValue>()){auto document=LoadReferencedUI(*reference);if(document){if(const auto style=document.Value().properties.find("styleSystem");style!=document.Value().properties.end()){auto parsed=ui::ParseStyleTheme(style->second);if(parsed){theme=parsed.TakeValue();foundTheme=true;source=reference->lastKnownPath;}else EmitStatus(Status::Fail(parsed.Diagnostics()));}}}
    ImGui::TextDisabled("Style System 3 · %s",source.c_str());if(!foundTheme)ImGui::TextColored({1,.65f,.3f,1},"No styleSystem is available; local overrides still work.");
    auto* node=m_document->Find(m_selected);if(!node){ImGui::TextDisabled("Select a Control.");return;}
    const Variant before=node->properties.contains("styleBinding")?node->properties.at("styleBinding"):Variant{};ui::ControlStyleBinding binding;if(before.Type()!=VariantType::Null){auto parsed=ui::ParseStyleBinding(before);if(parsed)binding=parsed.TakeValue();else{EmitStatus(Status::Fail(parsed.Diagnostics()));return;}}bool changed=false;
    ImGui::SeparatorText((node->name+" · "+node->type).c_str());const char* base="(none)";if(binding.baseStyle)if(const auto* style=theme.FindStyle(*binding.baseStyle))base=style->displayName.c_str();if(ImGui::BeginCombo("Base Style",base)){if(ImGui::Selectable("(none)",!binding.baseStyle)){binding.baseStyle.reset();changed=true;}for(const auto& style:theme.styles)if(ui::IsStyleCompatibleWith(style.compatibleTypes,node->type))if(ImGui::Selectable((style.displayName+"##base-"+style.id.ToString()).c_str(),binding.baseStyle&&*binding.baseStyle==style.id)){binding.baseStyle=style.id;changed=true;}ImGui::EndCombo();}
    if(ImGui::TreeNode("Applied Styles")){for(const auto& style:theme.styles)if(ui::IsStyleCompatibleWith(style.compatibleTypes,node->type)){bool selected=std::find(binding.appliedStyles.begin(),binding.appliedStyles.end(),style.id)!=binding.appliedStyles.end();if(ImGui::Checkbox((style.displayName+"##applied-"+style.id.ToString()).c_str(),&selected)){if(selected)binding.appliedStyles.push_back(style.id);else std::erase(binding.appliedStyles,style.id);changed=true;}}ImGui::TreePop();}
    for(const auto& axis:theme.variantAxes)if(ui::IsStyleCompatibleWith(axis.compatibleTypes,node->type)){const auto selected=binding.variants.contains(axis.id)?binding.variants.at(axis.id):axis.defaultValue;const auto* value=axis.FindValue(selected);if(ImGui::BeginCombo(axis.displayName.c_str(),value?value->displayName.c_str():"Missing variant")){for(const auto& candidate:axis.values)if(ImGui::Selectable((candidate.displayName+"##variant-"+candidate.id.ToString()).c_str(),candidate.id==selected)){binding.variants[axis.id]=candidate.id;changed=true;}ImGui::EndCombo();}}
    ui::StylePropertyRegistry registry;const auto editStyleValue=[&](const ui::StylePropertyDescriptor& descriptor,ui::StyleValue& styleValue){bool edited=false;const char* kind=styleValue.IsTokenReference()?"Token":"Literal";if(ImGui::BeginCombo((descriptor.displayName+" source").c_str(),kind)){if(ImGui::Selectable("Literal",styleValue.IsLiteral())){styleValue=ui::StyleValue::Literal(DefaultValueFor(descriptor.valueType));edited=true;}if(ImGui::Selectable("Token",styleValue.IsTokenReference())&&!theme.tokens.empty()){const auto& token=theme.tokens.front();styleValue=ui::StyleValue::Token(token.id,token.displayName);edited=true;}ImGui::EndCombo();}if(styleValue.IsTokenReference()){const auto* current=theme.FindToken(styleValue.TokenReference());if(ImGui::BeginCombo(descriptor.displayName.c_str(),current?current->displayName.c_str():"Missing token")){for(const auto& token:theme.tokens)if(ui::IsStyleValueTypeCompatible(descriptor.valueType,token.type)&&ImGui::Selectable((token.displayName+"##token-"+token.id.ToString()).c_str(),token.id==styleValue.TokenReference())){styleValue=ui::StyleValue::Token(token.id,token.displayName);edited=true;}ImGui::EndCombo();}}else{Variant literal=styleValue.IsLiteral()?styleValue.LiteralValue().Clone():DefaultValueFor(descriptor.valueType);ui::ActionArgumentDescriptor argument{.name=descriptor.id,.displayName=descriptor.displayName,.type=descriptor.valueType};if(RenderActionArgument(argument,literal)){styleValue=ui::StyleValue::Literal(std::move(literal));edited=true;}}return edited;};
    ImGui::SeparatorText("Local Overrides");std::optional<ui::StylePropertyId> removeOverride;for(auto& [id,value]:binding.localOverrides){ImGui::PushID(id.c_str());if(const auto* descriptor=registry.Find(id))changed|=editStyleValue(*descriptor,value);else ImGui::TextColored({1,.45f,.35f,1},"Missing property: %s",id.c_str());ImGui::SameLine();if(ImGui::SmallButton("Remove"))removeOverride=id;ImGui::PopID();}if(removeOverride){binding.localOverrides.erase(*removeOverride);changed=true;}if(ImGui::Button("＋ Override"))ImGui::OpenPopup("AddStyleOverride");if(ImGui::BeginPopup("AddStyleOverride")){for(const auto* descriptor:registry.Descriptors())if(registry.Supports(descriptor->id,node->type)&&!binding.localOverrides.contains(descriptor->id))if(ImGui::MenuItem((descriptor->category+" / "+descriptor->displayName).c_str())){binding.localOverrides[descriptor->id]=ui::StyleValue::Literal(DefaultValueFor(descriptor->valueType));changed=true;ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    ui::StyleResolveRequest request{.controlType=node->type,.binding=binding};const auto resolved=ui::StyleResolver{}.Resolve(theme,request,registry);if(ImGui::TreeNode("Resolved Source Trace")){if(resolved&&ImGui::BeginTable("##selected-style-trace",3,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)){ImGui::TableSetupColumn("Property");ImGui::TableSetupColumn("Source");ImGui::TableSetupColumn("Token chain");ImGui::TableHeadersRow();for(const auto& [id,value]:resolved.Value().properties){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(id.c_str());ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(value.source.label.c_str());ImGui::TableSetColumnIndex(2);ImGui::Text("%zu",value.tokenChain.size());}ImGui::EndTable();}else if(!resolved)for(const auto& diagnostic:resolved.Diagnostics())ImGui::TextColored({1,.45f,.35f,1},"%s %s",diagnostic.code.c_str(),diagnostic.message.c_str());ImGui::TreePop();}
    if(changed){const Variant after=ui::WriteStyleBinding(binding);auto command=std::make_unique<PropertyChangeCommand>("Edit Control style binding",m_selected,"styleBinding",before,after);const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}
    ImGui::TextDisabled("Theme definitions are edited in the external .pxtheme document editor.");
}

void UIDesigner::RenderComponents(){
    if(!m_document){ImGui::TextDisabled("Open a UI scene to browse components.");return;}
    const auto commitDocumentArray=[&](const char* property,Variant before,Variant after,const char* label){auto command=std::make_unique<PropertyChangeCommand>(label,m_document->DocumentId(),property,std::move(before),std::move(after));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();};
    if(m_document->Data().type=="UIComponent"){
        ImGui::TextDisabled("Component public API · UI schema 5");auto* selected=m_document->Find(m_selected);if(!selected){ImGui::TextDisabled("Select a source Control.");return;}
        const auto editDefinitions=[&](const char* field,const char* title,const auto& candidates,const auto& addDefinition){Variant before=m_document->Data().properties.contains(field)?m_document->Data().properties.at(field).Clone():Variant(VariantArray{});Variant after=before.Clone();if(!after.AsArray())after=Variant(VariantArray{});auto* values=after.AsArray();bool changed=false;ImGui::SeparatorText(title);for(std::size_t index=0;index<values->size();++index){const auto* object=(*values)[index].AsObject();const auto id=object?object->find("id"):VariantObject::const_iterator{};const auto display=object?object->find("displayName"):VariantObject::const_iterator{};const std::string idText=object&&id!=object->end()&&id->second.TryGet<std::string>()?*id->second.TryGet<std::string>():"invalid";const std::string displayText=object&&display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():idText;ImGui::PushID((std::string(field)+std::to_string(index)).c_str());ImGui::Text("%s",displayText.c_str());ImGui::SameLine();ImGui::TextDisabled("%s",idText.c_str());ImGui::SameLine();if(ImGui::SmallButton("Remove")){values->erase(values->begin()+static_cast<std::ptrdiff_t>(index));changed=true;ImGui::PopID();break;}ImGui::PopID();}if(ImGui::BeginCombo((std::string("Expose##")+field).c_str(),"＋ Add")){for(const auto& candidate:candidates)if(ImGui::Selectable(candidate.first.c_str())){values->push_back(Variant(addDefinition(candidate)));changed=true;ImGui::CloseCurrentPopup();}ImGui::EndCombo();}if(changed)commitDocumentArray(field,std::move(before),std::move(after),(std::string("Edit ")+title).c_str());};
        std::vector<std::pair<std::string,const PropertyInfo*>> properties;std::unordered_set<std::string> seen;std::string type=selected->type;while(const auto* info=TypeRegistry::Global().Find(type)){for(const auto& property:info->properties)if(HasFlag(property.flags,PropertyFlags::Editable)&&seen.insert(property.name).second)properties.push_back({property.editor.displayName.empty()?property.name:property.editor.displayName,&property});type=info->base;if(type.empty())break;}
        editDefinitions("component.exposedProperties","Exposed Properties",properties,[&](const auto& candidate){const auto* property=candidate.second;const std::string id=ComponentInterfaceId(selected->name+" "+property->name);return VariantObject{{"id",id},{"displayName",candidate.first},{"node",selected->id},{"property",property->name},{"type",std::string(ToString(property->type))}};});
        std::vector<std::pair<std::string,const SignalInfo*>> signals;for(const auto* signal:TypeRegistry::Global().SignalsForType(selected->type))signals.push_back({signal->displayName.empty()?signal->name:signal->displayName,signal});
        editDefinitions("component.exposedSignals","Exposed Signals",signals,[&](const auto& candidate){const auto* signal=candidate.second;const std::string id=ComponentInterfaceId(selected->name+" "+signal->name);return VariantObject{{"id",id},{"displayName",candidate.first},{"node",selected->id},{"signal",signal->name}};});
        std::vector<std::pair<std::string,int>> slotCandidate{{"Use selected Control",0}};editDefinitions("component.slots","Content Slots",slotCandidate,[&](const auto&){const std::string id=ComponentInterfaceId(selected->name+" content");return VariantObject{{"id",id},{"displayName",selected->name+" Content"},{"node",selected->id}};});
        ImGui::TextWrapped("Exposed IDs are stable English schema names. Rename the displayName in the typed definition when a localized label is needed.");return;
    }
    if(ImGui::Button("Create component from selection"))m_createComponentOpen=true;
    ImGui::Separator();bool any=false;
    for(const auto& node:m_document->Data().nodes){if(node.type!="ComponentInstance")continue;any=true;
        std::string source="Missing source";if(const auto found=node.properties.find("component");found!=node.properties.end()){
            if(const auto* reference=found->second.TryGet<ResourceRefValue>())source=reference->lastKnownPath;}
        if(ImGui::Selectable((node.name+"##component-"+node.id.ToString()).c_str(),node.id==m_selected)){
            m_selected=node.id;m_selection={node.id};}
        ImGui::SameLine();ImGui::TextDisabled("%s",source.c_str());}
    if(!any)ImGui::TextDisabled("No component instances in this scene.");
    auto* instance=m_document->Find(m_selected);if(!instance||instance->type!="ComponentInstance")return;const auto referenceIt=instance->properties.find("component");const auto* reference=referenceIt==instance->properties.end()?nullptr:referenceIt->second.TryGet<ResourceRefValue>();if(!reference)return;auto source=LoadReferencedUI(*reference);if(!source){EmitStatus(Status::Fail(source.Diagnostics()));return;}
    const auto definitions=[&](const char* field){const auto found=source.Value().properties.find(field);return found==source.Value().properties.end()?static_cast<const VariantArray*>(nullptr):found->second.AsArray();};
    if(const auto* exposed=definitions("component.exposedProperties")){ImGui::SeparatorText("Public Properties");Variant before=instance->properties.contains("componentProperties")?instance->properties.at("componentProperties").Clone():Variant(VariantObject{});Variant after=before.Clone();if(!after.AsObject())after=Variant(VariantObject{});auto* values=after.AsObject();bool changed=false;for(const auto& item:*exposed){const auto* object=item.AsObject();if(!object)continue;const auto id=object->find("id"),node=object->find("node"),property=object->find("property"),display=object->find("displayName");if(id==object->end()||node==object->end()||property==object->end()||!id->second.TryGet<std::string>()||!node->second.TryGet<Uuid>()||!property->second.TryGet<std::string>())continue;const auto sourceNode=std::find_if(source.Value().nodes.begin(),source.Value().nodes.end(),[&](const auto& candidate){return candidate.id==*node->second.TryGet<Uuid>();});if(sourceNode==source.Value().nodes.end())continue;const auto* descriptor=TypeRegistry::Global().FindProperty(sourceNode->type,*property->second.TryGet<std::string>());if(!descriptor)continue;const std::string& publicId=*id->second.TryGet<std::string>();if(!values->contains(publicId)){const auto current=sourceNode->properties.find(descriptor->name);(*values)[publicId]=current==sourceNode->properties.end()?descriptor->defaultValue.Clone():current->second.Clone();}ui::ActionArgumentDescriptor editor{.name=publicId,.displayName=display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():publicId,.type=descriptor->type};ImGui::PushID(publicId.c_str());changed|=RenderActionArgument(editor,(*values)[publicId]);ImGui::PopID();}if(changed){auto command=std::make_unique<PropertyChangeCommand>("Edit exposed Component property",instance->id,"componentProperties",std::move(before),std::move(after));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}}
    if(const auto* exposed=definitions("component.exposedSignals")){ImGui::SeparatorText("Public Signals");Variant before=instance->properties.contains("componentEvents")?instance->properties.at("componentEvents").Clone():Variant(VariantObject{});Variant after=before.Clone();if(!after.AsObject())after=Variant(VariantObject{});auto* bindings=after.AsObject();bool changed=false;for(const auto& item:*exposed){const auto* object=item.AsObject();if(!object)continue;const auto id=object->find("id"),display=object->find("displayName");if(id==object->end()||!id->second.TryGet<std::string>())continue;const std::string publicId=*id->second.TryGet<std::string>(),label=display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():publicId;ImGui::PushID(publicId.c_str());ImGui::TextUnformatted(label.c_str());auto bindingIt=bindings->find(publicId);auto* binding=bindingIt==bindings->end()?nullptr:bindingIt->second.AsObject();std::string action;if(binding)if(const auto found=binding->find("action");found!=binding->end()&&found->second.TryGet<std::string>())action=*found->second.TryGet<std::string>();if(ImGui::BeginCombo("Action",action.empty()?"(none)":action.c_str())){if(ImGui::Selectable("(none)",action.empty())){bindings->erase(publicId);changed=true;}for(const auto& descriptor:ui::ActionCatalog::Global().Descriptors())if(descriptor.available&&ImGui::Selectable((descriptor.category+" / "+descriptor.displayName+"##"+descriptor.id).c_str(),descriptor.id==action)){VariantObject arguments;for(const auto& argument:descriptor.arguments)arguments[argument.name]=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);(*bindings)[publicId]=VariantObject{{"kind",std::string("action")},{"action",descriptor.id},{"arguments",std::move(arguments)},{"reentry",std::string(ui::ActionReentryPolicyName(descriptor.reentryPolicy))}};binding=(*bindings)[publicId].AsObject();action=descriptor.id;changed=true;}ImGui::EndCombo();}if(binding&&!action.empty())if(const auto* descriptor=ui::ActionCatalog::Global().Find(action)){auto& arguments=(*binding)["arguments"];if(!arguments.AsObject())arguments=VariantObject{};for(const auto& argument:descriptor->arguments){auto& value=(*arguments.AsObject())[argument.name];if(value.Type()==VariantType::Null)value=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);changed|=RenderActionArgument(argument,value);}}ImGui::PopID();}if(changed){auto command=std::make_unique<PropertyChangeCommand>("Bind exposed Component signal",instance->id,"componentEvents",std::move(before),std::move(after));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited();}}
    if(const auto* slots=definitions("component.slots")){ImGui::SeparatorText("Slot Content");for(auto* child:m_document->Children(instance->id)){std::string current;if(const auto found=child->properties.find("componentSlot");found!=child->properties.end()&&found->second.TryGet<std::string>())current=*found->second.TryGet<std::string>();ImGui::PushID(child->id.ToString().c_str());if(ImGui::BeginCombo(child->name.c_str(),current.empty()?"(root)":current.c_str())){if(ImGui::Selectable("(root)",current.empty())){auto command=std::make_unique<PropertyChangeCommand>("Clear Component slot",child->id,"componentSlot",child->properties.contains("componentSlot")?child->properties.at("componentSlot"):Variant{},Variant{});const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited(true);}for(const auto& item:*slots)if(const auto* object=item.AsObject()){const auto id=object->find("id"),display=object->find("displayName");if(id!=object->end()&&id->second.TryGet<std::string>()){const std::string slot=*id->second.TryGet<std::string>(),label=display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():slot;if(ImGui::Selectable((label+"##"+slot).c_str(),slot==current)){auto command=std::make_unique<PropertyChangeCommand>("Assign Component slot",child->id,"componentSlot",child->properties.contains("componentSlot")?child->properties.at("componentSlot"):Variant{},Variant(slot));const Status status=m_document->History().Execute(std::move(command));if(!status)EmitStatus(status);else MarkEdited(true);}}}ImGui::EndCombo();}ImGui::PopID();}}
}

void UIDesigner::RenderInteractionNavigator(){
    if(!m_document){ImGui::TextDisabled("開啟 UI Scene 以編輯 Trigger。");return;}
    auto* selected=m_document->Find(m_selected);
    ImGui::SeparatorText("Selected Control");
    if(!selected){ImGui::TextDisabled("請先選取 Control。");}
    else{
        ImGui::Text("%s · %s",selected->name.c_str(),selected->type.c_str());
        const auto triggersIt=selected->properties.find("triggers");const auto* triggers=triggersIt==selected->properties.end()?nullptr:triggersIt->second.AsObject();
        for(const auto* signal:TypeRegistry::Global().SignalsForType(selected->type)){
            std::string state="未綁定";if(triggers)if(const auto binding=triggers->find(signal->name);binding!=triggers->end())if(const auto* object=binding->second.AsObject())if(const auto kind=object->find("kind");kind!=object->end()&&kind->second.TryGet<std::string>())state=*kind->second.TryGet<std::string>()=="flow"?"Flow":"Direct Action";
            const std::string label=(signal->displayName.empty()?signal->name:signal->displayName)+"  ["+state+"]##signal-"+signal->name;
            if(ImGui::Selectable(label.c_str(),m_selectedSignal==signal->name)){m_selectedSignal=signal->name;m_behaviorEditor->ClearSelection();}
        }
    }
    ImGui::SeparatorText("Scene Triggers");bool any=false;
    for(const auto& node:m_document->Data().nodes){const auto found=node.properties.find("triggers");const auto* triggers=found==node.properties.end()?nullptr:found->second.AsObject();if(!triggers)continue;for(const auto& [signal,value]:*triggers){const auto* binding=value.AsObject();const auto kind=binding?binding->find("kind"):VariantObject::const_iterator{};const std::string state=binding&&kind!=binding->end()&&kind->second.TryGet<std::string>()&&*kind->second.TryGet<std::string>()=="flow"?"Flow":"Direct Action";if(ImGui::Selectable((node.name+" · "+signal+"  ["+state+"]##scene-trigger-"+node.id.ToString()+signal).c_str(),node.id==m_selected&&signal==m_selectedSignal)){m_selected=node.id;m_selection={node.id};m_selectedSignal=signal;m_behaviorEditor->ClearSelection();}any=true;}}
    if(!any)ImGui::TextDisabled("Scene 尚未建立 Trigger binding。");
}

void UIDesigner::RenderEvents(){
    if(!m_document||m_selected.Empty()){ImGui::TextDisabled("選取 Control 與 Signal 以設定 Trigger。");return;}
    auto* node=m_document->Find(m_selected);if(!node)return;const auto signals=TypeRegistry::Global().SignalsForType(node->type);if(signals.empty()){ImGui::TextDisabled("此 Control 沒有可綁定的 Signal。");return;}
    const auto signalIt=std::find_if(signals.begin(),signals.end(),[&](const SignalInfo* signal){return signal->name==m_selectedSignal;});const SignalInfo* signal=signalIt==signals.end()?signals.front():*signalIt;m_selectedSignal=signal->name;
    ImGui::TextDisabled("WHEN");ImGui::SameLine();ImGui::Text("%s · %s",node->name.c_str(),signal->displayName.empty()?signal->name.c_str():signal->displayName.c_str());if(!signal->description.empty())ImGui::TextWrapped("%s",signal->description.c_str());
    Variant before=node->properties.contains("triggers")?node->properties.at("triggers").Clone():Variant(VariantObject{});Variant after=before.Clone();if(!after.AsObject())after=Variant(VariantObject{});auto* triggers=after.AsObject();auto bindingIt=triggers->find(signal->name);auto* binding=bindingIt==triggers->end()?nullptr:bindingIt->second.AsObject();std::string kind,action,reentry="IgnoreWhileRunning";Uuid entry;if(binding){if(const auto found=binding->find("kind");found!=binding->end()&&found->second.TryGet<std::string>())kind=*found->second.TryGet<std::string>();if(const auto found=binding->find("action");found!=binding->end()&&found->second.TryGet<std::string>())action=*found->second.TryGet<std::string>();if(const auto found=binding->find("entry");found!=binding->end()&&found->second.TryGet<Uuid>())entry=*found->second.TryGet<Uuid>();if(const auto found=binding->find("reentry");found!=binding->end()&&found->second.TryGet<std::string>())reentry=*found->second.TryGet<std::string>();}
    const auto firstAction=std::find_if(ui::ActionCatalog::Global().Descriptors().begin(),ui::ActionCatalog::Global().Descriptors().end(),[](const ui::ActionDescriptor& descriptor){return descriptor.available;});
    const auto makeDirect=[&](const ui::ActionDescriptor& descriptor){VariantObject arguments;for(const auto& argument:descriptor.arguments)arguments[argument.name]=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);(*triggers)[signal->name]=VariantObject{{"kind",std::string("action")},{"action",descriptor.id},{"arguments",std::move(arguments)},{"reentry",std::string(ui::ActionReentryPolicyName(descriptor.reentryPolicy))}};};
    const auto createFlow=[&](const bool includeAction){
        const Variant graphBefore=m_document->Data().properties.contains("interactionGraph")?m_document->Data().properties.at("interactionGraph").Clone():Variant{};ui::BehaviorGraph graph;if(graphBefore.Type()!=VariantType::Null){auto parsed=ui::ParseBehaviorGraph(graphBefore,m_pathText);if(!parsed){EmitStatus(Status::Fail(parsed.Diagnostics()));return false;}graph=parsed.TakeValue();}
        const Uuid entryId=Uuid::Random();graph.nodes.push_back({entryId,ui::BehaviorNodeKind::SignalEntry,{40,80},{{"control",node->id},{"signal",signal->name}}});
        if(includeAction&&!action.empty()){VariantObject arguments;if(binding)if(const auto found=binding->find("arguments");found!=binding->end()&&found->second.AsObject())arguments=*found->second.AsObject();const Uuid actionId=Uuid::Random();graph.nodes.push_back({actionId,ui::BehaviorNodeKind::Action,{360,80},{{"action",action},{"arguments",arguments},{"wait",true}}});graph.links.push_back({Uuid::Random(),entryId,"out",actionId,"in"});}
        (*triggers)[signal->name]=VariantObject{{"kind",std::string("flow")},{"entry",entryId},{"reentry",reentry}};
        auto command=std::make_unique<CompositeEditCommand>(includeAction?"Convert Direct Action to Flow":"Create visual Flow");command->Add(std::make_unique<PropertyChangeCommand>("Create Flow entry",m_document->DocumentId(),"interactionGraph",graphBefore,ui::WriteBehaviorGraph(graph),std::chrono::steady_clock::now(),false));command->Add(std::make_unique<PropertyChangeCommand>("Bind Trigger to Flow",node->id,"triggers",before,after,std::chrono::steady_clock::now(),false));const Status status=m_document->History().Execute(std::move(command));if(!status){EmitStatus(status);return false;}MarkEdited();m_behaviorEditor->FocusNode(entryId);m_viewport.authorMode=1;if(m_requestAuthorMode)m_requestAuthorMode(1);return true;
    };
    if(!binding){ImGui::Spacing();ImGui::TextWrapped("選擇這個 Trigger 要執行單一步驟，或進入可加入 Delay、Branch 與非同步 Action 的視覺 Flow。");ImGui::BeginDisabled(firstAction==ui::ActionCatalog::Global().Descriptors().end());if(ImGui::Button("執行單一 Action")&&firstAction!=ui::ActionCatalog::Global().Descriptors().end()){makeDirect(*firstAction);EditVariant("Create Direct Trigger","triggers",before,after,true,false);return;}ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("建立視覺 Flow")){(void)createFlow(false);return;}return;}
    if(kind=="flow"){
        ImGui::SeparatorText("DO  Visual Flow");ImGui::Text("Entry  %s",entry.Empty()?"Invalid":entry.ToString().c_str());if(ImGui::Button("在 Flow Graph 中開啟")){m_behaviorEditor->FocusNode(entry);if(m_requestAuthorMode)m_requestAuthorMode(1);}ImGui::SameLine();if(ImGui::Button("移除 Trigger")){triggers->erase(signal->name);EditVariant("Remove Flow Trigger","triggers",before,after,true,false);}return;
    }
    ImGui::SeparatorText("DO  Direct Action");ImGui::InputTextWithHint("##action-filter","搜尋 Action…",m_actionFilter,sizeof(m_actionFilter));const char* preview=action.empty()?"選擇 Action":action.c_str();bool changed=false;
    if(ImGui::BeginCombo("Action",preview)){const std::string filter=m_actionFilter;for(const auto& descriptor:ui::ActionCatalog::Global().Descriptors()){if(!filter.empty()&&descriptor.id.find(filter)==std::string::npos&&descriptor.displayName.find(filter)==std::string::npos&&descriptor.category.find(filter)==std::string::npos)continue;ImGui::BeginDisabled(!descriptor.available);if(ImGui::Selectable((descriptor.category+" / "+descriptor.displayName+"##trigger-action-"+descriptor.id).c_str(),descriptor.id==action)){makeDirect(descriptor);binding=(*triggers)[signal->name].AsObject();action=descriptor.id;changed=true;}ImGui::EndDisabled();}ImGui::EndCombo();}
    if(binding&&!action.empty())if(const auto* descriptor=ui::ActionCatalog::Global().Find(action)){auto& arguments=(*binding)["arguments"];if(!arguments.AsObject())arguments=VariantObject{};for(const auto& argument:descriptor->arguments){auto& value=(*arguments.AsObject())[argument.name];if(value.Type()==VariantType::Null)value=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);changed|=RenderActionArgument(argument,value);}}
    if(binding){if(ImGui::BeginCombo("Reentry",reentry.c_str())){for(const char* option:{"Allow","IgnoreWhileRunning","Restart"})if(ImGui::Selectable(option,reentry==option)){(*binding)["reentry"]=std::string(option);reentry=option;changed=true;}ImGui::EndCombo();}}
    if(changed)EditVariant("Edit Direct Trigger","triggers",before,after,true,false);
    if(ImGui::Button("一鍵轉換成 Flow")){(void)createFlow(true);return;}ImGui::SameLine();if(ImGui::Button("移除 Trigger")){triggers->erase(signal->name);EditVariant("Remove Direct Trigger","triggers",before,after,true,false);}
}

void UIDesigner::RenderInteractionInspector(){if(!m_document)return;if(!m_behaviorEditor->SelectedNode().Empty()){ImGui::TextDisabled("Flow Step Inspector");RenderBehaviorInspector();return;}ImGui::TextDisabled("Trigger Inspector");RenderEvents();}

void UIDesigner::RenderBehaviorGraph(){if(!m_document){ImGui::TextDisabled("Open a UI scene to edit behavior.");return;}if(m_behaviorDebugProvider)m_behaviorEditor->SetDebugState(m_behaviorDebugProvider());if(m_behaviorEditor->Render(*m_document,m_selected))MarkEdited();}
void UIDesigner::RenderBehaviorInspector(){if(!m_document)return;if(m_behaviorDebugProvider)m_behaviorEditor->SetDebugState(m_behaviorDebugProvider());if(m_behaviorEditor->RenderInspector(*m_document,m_selected))MarkEdited();}
void UIDesigner::RenderAnimationNavigator(){if(!m_document)return;if(m_animationDebugProvider)m_animationStateEditor->SetDebugState(m_animationDebugProvider());if(m_animationStateEditor->RenderNavigator(*m_document))MarkEdited();}
void UIDesigner::RenderAnimationStateMachine(){if(!m_document)return;if(m_animationDebugProvider)m_animationStateEditor->SetDebugState(m_animationDebugProvider());if(m_animationStateEditor->Render(*m_document))MarkEdited();}
void UIDesigner::RenderAnimationInspector(){if(!m_document)return;if(m_animationDebugProvider)m_animationStateEditor->SetDebugState(m_animationDebugProvider());if(m_animationStateEditor->RenderInspector(*m_document))MarkEdited();}

void UIDesigner::RenderProblems(){
    if(!m_document){ImGui::TextDisabled("No UI document is open.");return;}
    DesignerDiagnostics diagnostics;DesignerDiagnostics::ValidationContext context;ComponentService components;components.SetLoader([this](const ResourceRefValue& reference){return LoadReferencedUI(reference);});context.components=&components;
    context.validateAction=[](std::string_view action,const VariantObject& arguments,const diag::Source& source){std::vector<diag::Diagnostic> result;const auto* descriptor=ui::ActionCatalog::Global().Find(action);if(!descriptor){result.push_back({.severity=diag::Severity::Error,.code="PXEDUIP5022",.category="Editor.UIDesigner",.message="Action is missing or its extension is disabled",.details=std::string(action),.source=source});return result;}if(!descriptor->available){result.push_back({.severity=diag::Severity::Warning,.code="PXEDUIP5023",.category="Editor.UIDesigner",.message="Action is currently unavailable",.details=descriptor->unavailableReason,.source=source});return result;}ui::ActionInvocation invocation{.action=std::string(action),.arguments=arguments};const auto checked=ui::ActionCatalog::Global().ValidateAndNormalize(invocation);for(auto item:checked.Diagnostics()){item.source=source;result.push_back(std::move(item));}return result;};
    diagnostics.Refresh(*m_document,context);
    ImGui::Text("%zu errors, %zu warnings",diagnostics.ErrorCount(),diagnostics.WarningCount());
    ImGui::Separator();if(diagnostics.Items().empty()){ImGui::TextDisabled("No designer problems.");return;}
    for(const auto& item:diagnostics.Items()){
        const ImVec4 color=item.severity>=diag::Severity::Error?ImVec4(1,.4f,.35f,1):ImVec4(1,.75f,.3f,1);
        if(ImGui::Selectable((item.code+"  "+item.message+"##problem-"+item.code+item.source.nodeId).c_str())){
            if(const auto id=Uuid::Parse(item.source.nodeId);id&&m_document->Find(*id)){m_selected=*id;m_selection={*id};}}
        if(ImGui::IsItemHovered()&&!item.details.empty())ImGui::SetTooltip("%s",item.details.c_str());
        ImGui::SameLine();ImGui::TextColored(color,"%s",item.source.property.c_str());
    }
}

}  // namespace px::editor
