#include "Editor/Tools/UIDesigner/UIDesigner.h"
#include "Editor/Tools/UIDesigner/DesignerDiagnostics.h"
#include "Editor/Tools/UIDesigner/DesignerNodeFactory.h"
#include "Editor/Tools/UIDesigner/DesignerUiState.h"
#include "Editor/Tools/UIDesigner/BehaviorGraphEditor.h"
#include "Editor/Tools/UIDesigner/AnimationStateMachineEditor.h"
#include "Editor/Tools/UIDesigner/PropertyEditorRegistry.h"
#include "Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.h"
#include "Editor/Tools/UIDesigner/Canvas/CanvasTransform.h"
#include "Editor/Tools/UIDesigner/Canvas/HitTestService.h"
#include "Editor/Tools/UIDesigner/Canvas/SnapEngine.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/Styles/StyleResolver.h"
#include "Editor/Theme/EditorTheme.h"
#include "Editor/Theme/EditorWidgets.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
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

DesignerDirtyFlags DirtyForProperty(std::string_view property) {
    if (property == "offsets" || property == "anchors" || property == "minimumSize" ||
        property == "maximumSize" || property == "stretchRatio" ||
        property == "visibility" || property == "padding" || property == "spacing")
        return DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint;
    if (property == "bindings" || property == "triggers" || property == "interactionGraph")
        return DesignerDirtyFlags::Binding | DesignerDirtyFlags::Paint;
    if (property == "animations")
        return DesignerDirtyFlags::Animation | DesignerDirtyFlags::Paint;
    if (property == "styleBinding" || property == "styleSystem")
        return DesignerDirtyFlags::Theme | DesignerDirtyFlags::Paint;
    if (property == "overrides" || property == "componentProperties" ||
        property == "componentEvents" || property == "componentSlot")
        return DesignerDirtyFlags::Structure | DesignerDirtyFlags::Paint;
    if (property.starts_with("component."))
        return DesignerDirtyFlags::Structure | DesignerDirtyFlags::Paint;
    return DesignerDirtyFlags::Paint;
}

DesignerDirtyFlags DirtyForProperty(const PropertyInfo& property) {
    DesignerDirtyFlags dirty = DesignerDirtyFlags::None;
    if (HasImpact(property.impact, PropertyImpact::Structure)) dirty |= DesignerDirtyFlags::Structure;
    if (HasImpact(property.impact, PropertyImpact::Layout)) dirty |= DesignerDirtyFlags::Layout;
    if (HasImpact(property.impact, PropertyImpact::Paint)) dirty |= DesignerDirtyFlags::Paint;
    if (HasImpact(property.impact, PropertyImpact::Binding)) dirty |= DesignerDirtyFlags::Binding;
    if (HasImpact(property.impact, PropertyImpact::Theme)) dirty |= DesignerDirtyFlags::Theme;
    if (HasImpact(property.impact, PropertyImpact::PreviewState)) dirty |= DesignerDirtyFlags::PreviewState;
    if (HasImpact(property.impact, PropertyImpact::Animation)) dirty |= DesignerDirtyFlags::Animation;
    return Any(dirty) ? dirty : DesignerDirtyFlags::Paint;
}

void EmitStatus(const Status& status) { for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic); }

std::string UniqueName(const UISceneDocument& document, std::string base) {
    std::unordered_set<std::string> names; for (const auto& node : document.Data().nodes) names.insert(node.name);
    if (!names.contains(base)) return base;
    for (int i = 2; i < 10000; ++i) if (!names.contains(base + std::to_string(i))) return base + std::to_string(i);
    return base + "Copy";
}

const char* TypeGlyph(std::string_view type) {
    // Keep hierarchy type markers inside the guaranteed ASCII glyph range. The
    // editor can run without the optional icon font, in which case geometric
    // Unicode markers are otherwise rendered as question marks.
    if (type.find("Container") != std::string_view::npos) return "[C]";
    if (type == "Button") return "[B]";
    if (type == "Label") return "[T]";
    if (type == "TextureRect") return "[I]";
    return "[N]";
}

enum class HierarchyStateIcon { Visible, Hidden, Collapsed, Locked, Unlocked };

bool HierarchyStateButton(const char* id, HierarchyStateIcon icon) {
    constexpr float buttonSize = 20.0f;
    const bool pressed = ImGui::Button(id, {buttonSize, buttonSize});
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 center{(minimum.x + maximum.x) * 0.5f,
                        (minimum.y + maximum.y) * 0.5f};
    const auto& colors = EditorTheme().colors;
    const ImVec4 iconColor =
        icon == HierarchyStateIcon::Hidden
            ? colors.textDisabled
            : (icon == HierarchyStateIcon::Collapsed ||
                       icon == HierarchyStateIcon::Locked
                   ? colors.warning
                   : colors.textPrimary);
    const ImU32 packed = ImGui::ColorConvertFloat4ToU32(iconColor);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (icon == HierarchyStateIcon::Visible ||
        icon == HierarchyStateIcon::Hidden ||
        icon == HierarchyStateIcon::Collapsed) {
        const ImVec2 left{center.x - 6.0f, center.y};
        const ImVec2 right{center.x + 6.0f, center.y};
        draw->AddBezierCubic(left, {center.x - 3.5f, center.y - 4.0f},
                             {center.x + 3.5f, center.y - 4.0f}, right,
                             packed, 1.4f);
        draw->AddBezierCubic(right, {center.x + 3.5f, center.y + 4.0f},
                             {center.x - 3.5f, center.y + 4.0f}, left,
                             packed, 1.4f);
        if (icon == HierarchyStateIcon::Visible)
            draw->AddCircleFilled(center, 2.1f, packed);
        else if (icon == HierarchyStateIcon::Hidden)
            draw->AddLine({center.x - 6.0f, center.y + 5.0f},
                          {center.x + 6.0f, center.y - 5.0f}, packed, 1.7f);
        else {
            draw->AddLine({center.x - 5.0f, center.y - 5.0f},
                          {center.x + 5.0f, center.y + 5.0f}, packed, 1.7f);
            draw->AddLine({center.x + 5.0f, center.y - 5.0f},
                          {center.x - 5.0f, center.y + 5.0f}, packed, 1.7f);
        }
    } else {
        const float bodyTop = center.y - 0.5f;
        draw->AddRect({center.x - 4.5f, bodyTop},
                      {center.x + 4.5f, center.y + 6.0f}, packed, 1.0f, 0,
                      1.4f);
        const float shackleRight =
            icon == HierarchyStateIcon::Locked ? bodyTop : bodyTop - 2.5f;
        draw->AddBezierCubic({center.x - 3.2f, bodyTop},
                             {center.x - 3.2f, center.y - 6.0f},
                             {center.x + 3.2f, center.y - 6.0f},
                             {center.x + 3.2f, shackleRight}, packed, 1.4f);
        draw->AddCircleFilled({center.x, center.y + 2.0f}, 1.0f, packed);
    }
    return pressed;
}

void RenderCanvasHintBar(std::string_view hint) {
    const auto& theme = EditorTheme();
    const bool hasHint = !hint.empty();
    const ImVec4 transparent{0, 0, 0, 0};
    const float fixedHeight =
        ImGui::GetTextLineHeight() + theme.metrics.space6 * 2.0f + 2.0f;
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        hasHint ? WithAlpha(theme.colors.warning, 0.10f) : transparent);
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        hasHint ? WithAlpha(theme.colors.warning, 0.55f) : transparent);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme.metrics.radius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        {theme.metrics.space8, theme.metrics.space6});
    ImGui::BeginChild("##canvas-hint", {0, fixedHeight},
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    if (hasHint) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.colors.warning);
        ImGui::TextUnformatted(hint.data(), hint.data() + hint.size());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() &&
            ImGui::CalcTextSize(hint.data(), hint.data() + hint.size()).x >
                ImGui::GetContentRegionAvail().x)
            ImGui::SetTooltip("%.*s", static_cast<int>(hint.size()), hint.data());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
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

bool StylePropertyChanged(const ui::StylePropertyMap& before,
                          const ui::StylePropertyMap& after,
                          std::string_view property) {
    const auto left = before.find(std::string(property));
    const auto right = after.find(std::string(property));
    if (left == before.end() || right == after.end())
        return left != before.end() || right != after.end();
    return !(left->second == right->second);
}

bool StyleBindingAffectsLayout(const ui::ControlStyleBinding& before,
                               const ui::ControlStyleBinding& after) {
    if (before.baseStyle != after.baseStyle || before.variants != after.variants ||
        before.appliedStyles != after.appliedStyles)
        return true;
    constexpr std::array<std::string_view, 4> styleProperties{
        "padding", "spacing", "typography.font", "typography.size"};
    for (const auto property : styleProperties) {
        if (!IsLayoutAffectingStyleProperty(property)) continue;
        if (StylePropertyChanged(before.componentOverrides, after.componentOverrides,
                                 property) ||
            StylePropertyChanged(before.localOverrides, after.localOverrides, property))
            return true;
    }
    return false;
}

std::string ComponentInterfaceId(std::string value){
    std::string result;bool upper=false;for(const unsigned char character:value){if(std::isalnum(character)){char output=static_cast<char>(character);if(result.empty())output=static_cast<char>(std::tolower(character));else if(upper)output=static_cast<char>(std::toupper(character));result.push_back(output);upper=false;}else upper=!result.empty();}if(result.empty())result="item";return result;
}

bool RenderActionArgument(const ui::ActionArgumentDescriptor& descriptor,Variant& value){
    bool changed=false;const char* label=descriptor.displayName.empty()?descriptor.name.c_str():descriptor.displayName.c_str();
    ImGui::PushID(descriptor.name.c_str());
    const auto help=[&]{if(!descriptor.description.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",descriptor.description.c_str());};
    if(!descriptor.enumValues.empty()&&value.TryGet<std::string>()){
        auto* text=value.TryGet<std::string>();if(ImGui::BeginCombo(label,text->empty()?"(none)":text->c_str())){for(std::size_t index=0;index<descriptor.enumValues.size();++index){const auto& option=descriptor.enumValues[index];if(ImGui::Selectable((option+"##enum-"+std::to_string(index)).c_str(),option==*text)){*text=option;changed=true;}}ImGui::EndCombo();}help();ImGui::PopID();return changed;
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
    ImGui::PopID();
    return changed;
}

void RegisterPropertyEditors(){static const bool once=[](){auto& registry=PropertyEditorRegistry::Global();const auto key=[](VariantType type){return "type:"+std::to_string(static_cast<int>(type));};
    (void)registry.Register(key(VariantType::String),[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();if(!value)return false;const char* label=request.property.editor.displayName.empty()?request.property.name.c_str():request.property.editor.displayName.c_str();return ImGui::InputText(label,value);});
    (void)registry.Register("multiline",[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();return value&&ImGui::InputTextMultiline(request.property.name.c_str(),value,ImVec2(-1,ImGui::GetTextLineHeight()*4));});
    (void)registry.Register("enum",[](PropertyEditRequest& request){auto* value=request.value.TryGet<std::string>();if(!value)return false;bool changed=false;if(ImGui::BeginCombo(request.property.name.c_str(),value->empty()?"(none)":value->c_str())){for(std::size_t index=0;index<request.property.editor.enumChoices.size();++index){const auto& choice=request.property.editor.enumChoices[index];if(ImGui::Selectable((choice+"##enum-"+std::to_string(index)).c_str(),choice==*value)){*value=choice;changed=true;}}ImGui::EndCombo();}return changed;});
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
void UIDesigner::SetImageSizeResolver(ImageSizeResolver resolver){
    m_imageSizeResolver=std::move(resolver);
    if(m_session)m_session->Interaction().SetImageSizeResolver(m_imageSizeResolver);
}
UIDesigner::~UIDesigner()=default;
UIDesigner::UIDesigner(UIDesigner&&) noexcept=default;

bool UIDesigner::Dirty() const { return m_session&&m_session->Commands().HistoryDirty(); }
bool UIDesigner::CanUndo() const { return m_session&&m_session->Commands().CanUndo(); }
bool UIDesigner::CanRedo() const { return m_session&&m_session->Commands().CanRedo(); }
std::string UIDesigner::NextUndoLabel() const { return m_session?m_session->Commands().NextUndoLabel():std::string{}; }
std::string UIDesigner::NextRedoLabel() const { return m_session?m_session->Commands().NextRedoLabel():std::string{}; }
std::size_t UIDesigner::HistoryCursor() const { return m_session?m_session->Commands().HistoryCursor():0; }

UIDesigner& UIDesigner::operator=(UIDesigner&& other) noexcept {
    if(this==&other)return *this;
    // Property transactions refer to the Session document. Destroy them before moving Session.
    // before replacing the designer; default memberwise move-assignment would replace the
    // document first and leave an active transaction pointing at freed memory.
    this->~UIDesigner();
    ::new (static_cast<void*>(this)) UIDesigner(std::move(other));
    return *this;
}

Status UIDesigner::Open(const std::filesystem::path& path) {
    const Status status = m_session->Open(path);
    if (!status) return status;
    RebuildLayout();
    (void)m_session->ConsumeDocumentChanges();
    return Status::Ok();
}

Status UIDesigner::New(const std::filesystem::path& path, int width, int height) {
    const Status status = m_session->New(path,width,height);
    if (!status) return status;
    RebuildLayout();
    (void)m_session->ConsumeDocumentChanges();
    return Status::Ok();
}

bool UIDesigner::Save() { if (!Document()) return false; Status status = Document()->Save();if(status&&m_identityRegistrar)status=m_identityRegistrar(Document()->Path()); if (!status) EmitStatus(status); return static_cast<bool>(status); }
Status UIDesigner::Undo() { if (!Document()) return Status::Ok(); auto status = m_session->Commands().Undo(); if (status) MarkEdited(); return status; }
Status UIDesigner::Redo() { if (!Document()) return Status::Ok(); auto status = m_session->Commands().Redo(); if (status) MarkEdited(); return status; }
void UIDesigner::RelocateDocument(const std::filesystem::path& oldPath,
                                  const std::filesystem::path& newPath) {
    if (!Document()) return;
    std::error_code ec;
    const auto current = std::filesystem::weakly_canonical(Document()->Path(), ec);
    ec.clear(); const auto oldCanonical = std::filesystem::weakly_canonical(oldPath, ec);
    if (!ec && current == oldCanonical) {
        Document()->RelocatePath(newPath);
    }
}

Uuid UIDesigner::RootId() const { if (!Document()) return {}; for (const auto& node : Document()->Data().nodes) if (node.parent.Empty()) return node.id; return {}; }
Uuid UIDesigner::ParentForNewNode() const {
    if (!Document()) return {};
    if (const auto* selected = View().Find(*Document(), Selected())) {
        if (selected->type.find("Container") != std::string::npos || selected->type == "Panel" || selected->type == "Control") return selected->id;
        return selected->parent;
    }
    return RootId();
}

void UIDesigner::MarkEdited(bool structural) {
    m_session->lastEdit = std::chrono::steady_clock::now();
    if (!m_session || !Document()) return;
    DocumentChangeSet changes = m_session->ConsumeDocumentChanges();
    if (structural) changes.Merge(DocumentChangeSet::Structure());
    const DesignerUpdate update = PlanDesignerUpdate(changes);
    if (HasDesignerUpdate(update, DesignerUpdate::RebuildLayoutScene))
        RebuildLayout();
    else if (HasDesignerUpdate(update, DesignerUpdate::Relayout))
        Relayout(changes);
}

void UIDesigner::RegenerateIds(VariantObject& subtree) {
    subtree["id"] = Variant(Uuid::Random());
    if (auto* children = subtree["children"].AsArray()) for (auto& value : *children) if (auto* child = value.AsObject()) RegenerateIds(*child);
}

void UIDesigner::AddNode(std::string type, Vec2 canvas, std::string image) {
    if (!Document()) return; const Uuid parent = ParentForNewNode();
    const auto* info=TypeRegistry::Global().Find(type);if(!info||!info->designer)return;
    float w=info->designer->defaultSize.x,h=info->designer->defaultSize.y;
    if(type=="TextureRect"&&!image.empty()&&m_imageSizeResolver)if(const auto size=m_imageSizeResolver(image);size&&size->x>0&&size->y>0){w=size->x;h=size->y;const Vec2 canvasSize=CanvasSize();const float fit=std::min({1.0f,(canvasSize.x-40.0f)/w,(canvasSize.y-40.0f)/h});w*=fit;h*=fit;}
    Rect offsets{canvas.x - w * 0.5f, canvas.y - h * 0.5f, w, h};
    if (canvas == Vec2{}) offsets = {20,20,w,h};
    auto created=DesignerNodeFactory::Create(type,{},image);if(!created){EmitStatus(Status::Fail(created.Diagnostics()));return;}VariantObject subtree=created.TakeValue();subtree["name"]=UniqueName(*Document(),info->designer->displayName);if(auto* properties=subtree["properties"].AsObject())(*properties)["offsets"]=offsets;
    const Uuid id = *subtree["id"].TryGet<Uuid>();
    auto command = std::make_unique<SubtreeEditCommand>("Add " + type, SubtreeOperation::Insert, id, parent,
                                                        View().Children(parent).size(), std::move(subtree));
    const Status status = m_session->Commands().Execute(
        std::move(command), DocumentChangeSet::Structure(parent));
    if (!status) return EmitStatus(status);
    MakePrimary(id); MarkEdited(true);
}

void UIDesigner::RenderAddControlPalette(Vec2 canvasPosition){
    ImGui::InputTextWithHint("##control-search","Search controls…",m_session->panels.paletteFilter,sizeof(m_session->panels.paletteFilter));
    std::string category;for(const auto* type:DesignerNodeFactory::Palette(m_session->panels.paletteFilter)){if(type->designer->category!=category){category=type->designer->category;ImGui::SeparatorText(category.c_str());}if(ImGui::Selectable((type->designer->displayName+"##palette-"+type->name).c_str())){AddNode(type->name,canvasPosition);ImGui::CloseCurrentPopup();}if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",type->designer->description.c_str());}
}

void UIDesigner::RemoveSelected() {
    if (!Document()) return;
    const std::size_t before = m_session->Commands().HistoryCursor();
    const Status status = m_session->Commands().DeleteSelection();
    if (!status) return EmitStatus(status);
    if (m_session->Commands().HistoryCursor() == before) {
        m_session->canvas.hint = "根節點不能刪除";
        return;
    }
    m_session->canvas.hint.clear();
    MarkEdited();
}

void UIDesigner::DuplicateSelected() {
    if (!Document() || Selected().Empty()) return;
    const std::size_t before = m_session->Commands().HistoryCursor();
    const Status status = m_session->Commands().DuplicateSelection();
    if (!status) return EmitStatus(status);
    if (m_session->Commands().HistoryCursor() != before) MarkEdited();
}

void UIDesigner::CopySelected() {
    if (!Document() || Selected().Empty()) return;
    auto captured=Document()->CaptureSubtree(Selected());if(captured)m_session->clipboardSubtree=captured.TakeValue();
}

void UIDesigner::PasteClipboard(Vec2 canvasPosition) {
    if(!Document()||m_session->clipboardSubtree.empty())return;
    VariantObject subtree=m_session->clipboardSubtree;RegenerateIds(subtree);
    if(auto* name=subtree["name"].TryGet<std::string>())*name=UniqueName(*Document(),*name+"Copy");
    if(canvasPosition!=Vec2{})if(auto* properties=subtree["properties"].AsObject())if(auto found=properties->find("offsets");found!=properties->end())if(auto* rect=found->second.TryGet<Rect>()){rect->x=canvasPosition.x;rect->y=canvasPosition.y;}
    const Uuid id=*subtree["id"].TryGet<Uuid>();const Uuid parent=ParentForNewNode();
    auto command=std::make_unique<SubtreeEditCommand>("Paste UI Control",SubtreeOperation::Insert,id,parent,View().Children(parent).size(),std::move(subtree));
    const Status status=m_session->Commands().Execute(std::move(command),DocumentChangeSet::Structure(parent));if(!status)EmitStatus(status);else{MakePrimary(id);SelectOnly(id);MarkEdited(true);}
}

Result<resource::TypedDocument> UIDesigner::LoadReferencedUI(const ResourceRefValue& reference) const {
    std::filesystem::path candidate=reference.lastKnownPath;
    if(candidate.is_relative()&&Document()){auto parent=Document()->Path().parent_path();while(!parent.empty()){const auto resolved=parent/candidate;if(std::filesystem::exists(resolved)){candidate=resolved;break;}const auto next=parent.parent_path();if(next==parent)break;parent=next;}}
    std::ifstream input(candidate,std::ios::binary);
    if(!input)return Result<resource::TypedDocument>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3030",.category="Editor.UIDesigner",.message="Referenced UI resource not found: "+candidate.string()});
    std::ostringstream buffer;buffer<<input.rdbuf();return resource::ParseTypedDocument(buffer.str(),candidate.string());
}

void UIDesigner::ResetComponentOverride(const std::string& sourceNode,const std::string& property) {
    if(!Document())return;ComponentService components;components.SetLoader([this](const ResourceRefValue& reference){return LoadReferencedUI(reference);});
    Status status=Status::Ok();if(sourceNode.empty())status=components.ResetAllOverrides(*Document(),m_session->Commands(),Selected());else if(const auto source=Uuid::Parse(sourceNode))status=components.ResetPropertyOverride(*Document(),m_session->Commands(),Selected(),*source,property);
    if(!status)EmitStatus(status);else MarkEdited(true);
}

void UIDesigner::DetachSelectedComponent() {
    if(!Document()||Selected().Empty())return;
    m_session->panels.detachConfirmOpen=true;
}

void UIDesigner::PerformDetachSelectedComponent() {
    if(!Document())return;ComponentService components;components.SetLoader([this](const ResourceRefValue& reference){return LoadReferencedUI(reference);});const Status status=components.Detach(*Document(),m_session->Commands(),Selected());if(!status)EmitStatus(status);else MarkEdited(true);
}

void UIDesigner::RenderDialogs(){
    if(m_session->panels.detachConfirmOpen)ImGui::OpenPopup("Detach Component Instance");
    if(ImGui::BeginPopupModal("Detach Component Instance",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::TextUnformatted("Detach will replace this Component Instance with local Controls.");
        ImGui::TextDisabled("The source Component is not changed. You can Undo this operation.");
        if(ImGui::Button("Detach",ImVec2(120,0))){m_session->panels.detachConfirmOpen=false;PerformDetachSelectedComponent();ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(120,0))){m_session->panels.detachConfirmOpen=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
}

void UIDesigner::ChangeSelectedLayer(LayerAction action) {
    if(!Document()||Selected().Empty()||Selected()==RootId())return;const auto* node=View().Find(*Document(),Selected());if(!node)return;
    const auto siblings=View().Children(node->parent);if(siblings.size()<2)return;const std::size_t oldIndex=View().ChildIndex(Selected()).value_or(0);std::size_t target=oldIndex;
    switch(action){case LayerAction::BringForward:target=std::min(oldIndex+1,siblings.size()-1);break;case LayerAction::SendBackward:target=oldIndex?oldIndex-1:0;break;case LayerAction::BringToFront:target=siblings.size()-1;break;case LayerAction::SendToBack:target=0;break;}
    if(target==oldIndex)return;const Status status=m_session->Commands().Reorder(Selected(),target);if(!status)EmitStatus(status);else MarkEdited(true);
}

void UIDesigner::SetSelectedAsBackground(bool lock){
    if(!Document())return;auto* node=View().Find(*Document(),Selected());if(!node||node->type!="TextureRect"){m_session->canvas.hint="請先選取圖片元件";return;}
    const Uuid root=RootId();auto command=std::make_unique<CompositeEditCommand>(lock?"Set and lock background":"Set background");
    if(node->parent==root){const std::size_t old=View().ChildIndex(node->id).value_or(0);if(old)command->Add(std::make_unique<MoveChildEditCommand>("Send background to back",root,node->id,old,0));}
    else command->Add(std::make_unique<ReparentEditCommand>("Move background to root",node->id,node->parent,View().ChildIndex(node->id).value_or(0),root,0));
    const auto add=[&](const char* property,Variant value){const Variant before=node->properties.contains(property)?node->properties.at(property):Variant{};if(before!=value)command->Add(std::make_unique<PropertyChangeCommand>("Set background",node->id,property,before,std::move(value),std::chrono::steady_clock::now(),false));};
    add("anchors",Variant(Rect{0,0,1,1}));add("offsets",Variant(Rect{0,0,0,0}));add("scaleMode",Variant(std::string("Fill")));if(lock)add("editorLocked",Variant(true));
    if(command->Empty())return;const Status status=m_session->Commands().Execute(std::move(command),DocumentChangeSet::Structure(root));if(!status)EmitStatus(status);else{m_session->canvas.hint=lock?"已設為背景並鎖定":"已設為背景";MarkEdited(true);}
}

void UIDesigner::RestoreSelectedImageSize(){
    if(!Document()||!m_imageSizeResolver)return;auto* node=View().Find(*Document(),Selected());if(!node||node->type!="TextureRect")return;
    const auto path=node->properties.find("path");if(path==node->properties.end()||!path->second.TryGet<std::string>())return;const auto size=m_imageSizeResolver(*path->second.TryGet<std::string>());if(!size){m_session->canvas.hint="無法讀取圖片原始尺寸";return;}
    Rect visual=SelectedRect();visual.w=size->x;visual.h=size->y;Rect anchors{};if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;
    const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(Selected()),anchors,visual);const Variant before=node->properties.contains("offsets")?node->properties.at("offsets"):Variant{};
    const Status status=m_session->Commands().SetProperty(Selected(),"offsets",Variant(offsets),"Restore image size",DesignerDirtyFlags::Layout|DesignerDirtyFlags::Paint);if(!status)EmitStatus(status);else MarkEdited();
}

void UIDesigner::AlignSelection(AlignAction action){
    if(!Document()||Selected().Empty())return;const auto* primary=View().Find(*Document(),Selected());if(!primary||Selected()==RootId())return;
    std::vector<Uuid> ids;for(const Uuid& id:Selection().OrderedItems()){const auto* node=View().Find(*Document(),id);if(!node||node->parent!=primary->parent||id==RootId())continue;const auto policy=View().ChildPolicy(node->parent);if(policy&&*policy!=ui::ChildLayoutPolicy::Free)continue;ids.push_back(id);}
    if(ids.empty()){m_session->canvas.hint="此佈局不允許自由對齊";return;}if((action==AlignAction::DistributeH||action==AlignAction::DistributeV)&&ids.size()<3){m_session->canvas.hint="平均分布至少需要三個元件";return;}
    std::unordered_map<Uuid,Rect,UuidHash> targets;for(const Uuid& id:ids)targets[id]=*View().LayoutRect(id);
    const Rect reference=ids.size()==1?ParentRect(Selected()):*View().LayoutRect(Selected());
    if(action==AlignAction::DistributeH){std::sort(ids.begin(),ids.end(),[&](Uuid a,Uuid b){return targets[a].x<targets[b].x;});const float left=targets[ids.front()].x,right=targets[ids.back()].x+targets[ids.back()].w;float widths=0;for(Uuid id:ids)widths+=targets[id].w;const float gap=(right-left-widths)/(ids.size()-1);float x=left;for(Uuid id:ids){targets[id].x=x;x+=targets[id].w+gap;}}
    else if(action==AlignAction::DistributeV){std::sort(ids.begin(),ids.end(),[&](Uuid a,Uuid b){return targets[a].y<targets[b].y;});const float top=targets[ids.front()].y,bottom=targets[ids.back()].y+targets[ids.back()].h;float heights=0;for(Uuid id:ids)heights+=targets[id].h;const float gap=(bottom-top-heights)/(ids.size()-1);float y=top;for(Uuid id:ids){targets[id].y=y;y+=targets[id].h+gap;}}
    else for(Uuid id:ids){Rect& r=targets[id];if(action==AlignAction::Left)r.x=reference.x;else if(action==AlignAction::HCenter)r.x=reference.x+(reference.w-r.w)*.5f;else if(action==AlignAction::Right)r.x=reference.x+reference.w-r.w;else if(action==AlignAction::Top)r.y=reference.y;else if(action==AlignAction::VCenter)r.y=reference.y+(reference.h-r.h)*.5f;else if(action==AlignAction::Bottom)r.y=reference.y+reference.h-r.h;}
    auto command=std::make_unique<CompositeEditCommand>("Align UI controls");DocumentChangeSet changes;for(Uuid id:ids){const auto* node=View().Find(*Document(),id);Rect anchors{};if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;const Rect offsets=ui::ControlLayoutMath::OffsetsForRect(ParentRect(id),anchors,targets[id]);const Variant before=node->properties.contains("offsets")?node->properties.at("offsets"):Variant{};if(before!=Variant(offsets)){command->Add(std::make_unique<PropertyChangeCommand>("Align control",id,"offsets",before,Variant(offsets),std::chrono::steady_clock::now(),false));changes.Merge(DocumentChangeSet::Property(id,"offsets",DesignerDirtyFlags::Layout|DesignerDirtyFlags::Paint));}}
    if(command->Empty())return;const Status status=m_session->Commands().Execute(std::move(command),std::move(changes));if(!status)EmitStatus(status);else MarkEdited();
}

Status UIDesigner::CreateComponentFromSelected(const std::filesystem::path& path){
    if(!Document()||Selected().Empty()||Selected()==RootId()||!m_componentWriter)return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUI3041",.category="Editor.UIDesigner",.message="Select a non-root Control before creating a component"});
    ComponentService components;components.SetWriter(m_componentWriter);components.SetLoader([this](const ResourceRefValue& reference){return LoadReferencedUI(reference);});
    const Status status=components.CreateFromSelection(*Document(),m_session->Commands(),Selected(),SelectedRect(),path);if(status)MarkEdited(true);return status;
}

bool UIDesigner::TreeMatches(const resource::NodeRecord& record) const {
    if (m_session->panels.treeFilter[0] == 0) return true;
    const std::string filter=m_session->panels.treeFilter;
    if(record.name.find(filter)!=std::string::npos||record.type.find(filter)!=std::string::npos)return true;
    for(const Uuid& childId:View().Children(record.id))if(const auto* child=View().Find(*Document(),childId);child&&TreeMatches(*child))return true;
    return false;
}

void UIDesigner::RenderTreeNode(resource::NodeRecord& record) {
    if(!TreeMatches(record))return;
    const auto children = View().Children(record.id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf; if (record.id == Selected()) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID(record.id.ToString().c_str());
    std::string visibility="Visible";if(const auto found=record.properties.find("visibility");found!=record.properties.end())if(const auto* value=found->second.TryGet<std::string>())visibility=*value;
    const bool visible=visibility=="Visible";const bool collapsed=visibility=="Collapsed";
    const bool locked=record.properties.contains("editorLocked")&&record.properties["editorLocked"].TryGet<bool>()&&*record.properties["editorLocked"].TryGet<bool>();
    const auto policy=View().ChildPolicy(record.id);const bool container=policy&&*policy!=ui::ChildLayoutPolicy::Free;
    const auto batchProperty=[&](const std::string& property,const Variant& value,const std::string& label){std::vector<Uuid> targets;if(Selection().Contains(record.id))targets.assign(Selection().OrderedItems().begin(),Selection().OrderedItems().end());else targets.push_back(record.id);const DesignerDirtyFlags dirty=property=="visibility"?DesignerDirtyFlags::Layout|DesignerDirtyFlags::Paint:DesignerDirtyFlags::Paint;const Status status=m_session->Commands().SetProperties(targets,property,value,label,dirty);if(!status)EmitStatus(status);else MarkEdited();};
    const auto visibilityIcon = visible
                                    ? HierarchyStateIcon::Visible
                                    : (collapsed ? HierarchyStateIcon::Collapsed
                                                 : HierarchyStateIcon::Hidden);
    if(HierarchyStateButton("##visibility",visibilityIcon))batchProperty("visibility",Variant(std::string(visible?"Hidden":"Visible")),"切換可見性");if(ImGui::IsItemHovered())ImGui::SetTooltip("眼睛：Visible / Hidden；右鍵選單可設為 Collapsed");ImGui::SameLine();
    if(HierarchyStateButton("##lock",locked?HierarchyStateIcon::Locked:HierarchyStateIcon::Unlocked))batchProperty("editorLocked",Variant(!locked),locked?"解除鎖定":"鎖定元件");if(ImGui::IsItemHovered())ImGui::SetTooltip(locked?"解除鎖定":"鎖定（編輯模式不可拖曳）");ImGui::SameLine();
    const std::string badges=std::string(locked?"  [Locked]":"")+(container?"  [Layout]":"");
    if(m_session->panels.treeFilter[0])ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    else ImGui::SetNextItemOpen(m_session->expandedTreeNodes.contains(record.id),ImGuiCond_Appearing);
    const bool open = ImGui::TreeNodeEx("node", flags, "%s  %s%s", TypeGlyph(record.type), record.name.c_str(),badges.c_str());
    if(ImGui::IsItemToggledOpen()){if(open)m_session->expandedTreeNodes.insert(record.id);else m_session->expandedTreeNodes.erase(record.id);}
    if (ImGui::IsItemClicked()) {
        if (ImGui::GetIO().KeyCtrl) Selection().Toggle(record.id);
        else SelectOnly(record.id);
    }
    if (ImGui::BeginDragDropSource()) { const std::string id = record.id.ToString(); ImGui::SetDragDropPayload(kNodePayload, id.c_str(), id.size()+1); ImGui::Text("Move %s", record.name.c_str()); ImGui::EndDragDropSource(); }
    if (ImGui::BeginDragDropTarget()) {
        if(const ImGuiPayload* payload=ImGui::GetDragDropPayload();payload&&payload->IsDataType(kNodePayload)){const ImU32 dropColor=ImGui::ColorConvertFloat4ToU32(EditorTheme().colors.selectionBorder);const float rowHeight=ImGui::GetItemRectSize().y;const float fraction=rowHeight>0?(ImGui::GetMousePos().y-ImGui::GetItemRectMin().y)/rowHeight:.5f;if(fraction<.25f||fraction>.75f){const float y=fraction<.25f?ImGui::GetItemRectMin().y:ImGui::GetItemRectMax().y;ImGui::GetWindowDrawList()->AddLine({ImGui::GetItemRectMin().x,y},{ImGui::GetItemRectMax().x,y},dropColor,3.0f);}else ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),dropColor,3.0f,0,2.0f);}
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload)) {
            if (auto id = Uuid::Parse(static_cast<const char*>(payload->Data)); id && *id != record.id) {
                if (const auto* moved = View().Find(*Document(),*id)) {
                    const float rowHeight=ImGui::GetItemRectSize().y;
                    const float fraction=rowHeight>0?(ImGui::GetMousePos().y-ImGui::GetItemRectMin().y)/rowHeight:.5f;
                    const bool siblingDrop=fraction<.25f||fraction>.75f;
                    const Uuid targetParent=siblingDrop?record.parent:record.id;
                    const auto targetPolicy=View().ChildPolicy(targetParent).value_or(ui::ChildLayoutPolicy::Free);
                    const bool reversedLayers=targetPolicy==ui::ChildLayoutPolicy::Free;
                    Status status=Status::Ok();
                    if(siblingDrop&&moved->parent==targetParent){
                        // Free-layout children are displayed in reverse paint order:
                        // dropping above a row therefore means moving after it in
                        // canonical data/paint order. Managed layouts stay forward.
                        const bool moveAfter=reversedLayers?(fraction<.25f):(fraction>.75f);
                        status=moveAfter?m_session->Commands().MoveAfter(*id,record.id):m_session->Commands().MoveBefore(*id,record.id);
                    }else{
                        const std::size_t recordIndex=View().ChildIndex(record.id).value_or(0);
                        const std::size_t targetIndex=siblingDrop
                            ? recordIndex+((reversedLayers?(fraction<.25f):(fraction>.75f))?1:0)
                            : View().Children(record.id).size();
                        status=moved->parent==targetParent?m_session->Commands().Reorder(*id,targetIndex):m_session->Commands().Reparent(*id,targetParent,targetIndex);
                    }
                    if(!status)EmitStatus(status);else MarkEdited();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if(ImGui::BeginPopupContextItem("TreeNodeMenu")){
        if(!Selection().Contains(record.id)){MakePrimary(record.id);SelectOnly(record.id);}else MakePrimary(record.id);
        const auto setVisibility=[&](const char* value){batchProperty("visibility",Variant(std::string(value)),"設定可見性");};
        if(ImGui::BeginMenu("可見性")){if(ImGui::MenuItem("Visible",nullptr,visibility=="Visible"))setVisibility("Visible");if(ImGui::MenuItem("Hidden",nullptr,visibility=="Hidden"))setVisibility("Hidden");if(ImGui::MenuItem("Collapsed",nullptr,visibility=="Collapsed"))setVisibility("Collapsed");ImGui::EndMenu();}
        if(ImGui::MenuItem(locked?"解除鎖定":"鎖定"))batchProperty("editorLocked",Variant(!locked),"切換鎖定");
        ImGui::Separator();
        if(ImGui::MenuItem("重新命名","F2")){MakePrimary(record.id);SelectOnly(record.id);std::snprintf(m_session->panels.treeRename,sizeof(m_session->panels.treeRename),"%s",record.name.c_str());m_session->panels.treeRenameOpen=true;}
        if(ImGui::MenuItem("複製","Ctrl+D"))DuplicateSelected();
        if(record.id!=RootId()&&ImGui::MenuItem("建立 Component…")){MakePrimary(record.id);SelectOnly(record.id);m_session->panels.createComponentOpen=true;}
        if(record.type=="ComponentInstance"){
            if(const auto component=record.properties.find("component");component!=record.properties.end())if(const auto* reference=component->second.TryGet<ResourceRefValue>())if(m_openResource&&ImGui::MenuItem("Edit Main")){MakePrimary(record.id);SelectOnly(record.id);m_openResource(*reference);}
            if(ImGui::MenuItem("Reset All Overrides")){MakePrimary(record.id);SelectOnly(record.id);ResetComponentOverride();}
            if(ImGui::MenuItem("Detach")){MakePrimary(record.id);SelectOnly(record.id);DetachSelectedComponent();}
        }
        if(record.id!=RootId()&&ImGui::BeginMenu("圖層順序")){MakePrimary(record.id);SelectOnly(record.id);if(ImGui::MenuItem("置頂","Ctrl+Shift+]"))ChangeSelectedLayer(LayerAction::BringToFront);if(ImGui::MenuItem("上移一層","Ctrl+]"))ChangeSelectedLayer(LayerAction::BringForward);if(ImGui::MenuItem("下移一層","Ctrl+["))ChangeSelectedLayer(LayerAction::SendBackward);if(ImGui::MenuItem("置底","Ctrl+Shift+["))ChangeSelectedLayer(LayerAction::SendToBack);ImGui::EndMenu();}
        if(record.id!=RootId()&&ImGui::MenuItem("刪除","Delete"))RemoveSelected();
        ImGui::EndPopup();
    }
    if (open) {
        // In free-layout containers the runtime paints later children on top, so
        // show those children first like a conventional Layers panel. Managed
        // layouts retain canonical order because it represents spatial/tab order.
        if(!policy||*policy==ui::ChildLayoutPolicy::Free){
            for(auto childId=children.rbegin();childId!=children.rend();++childId)
                if(auto* child=View().Find(*Document(),*childId))RenderTreeNode(*child);
        }else{
            for(const Uuid& childId:children)
                if(auto* child=View().Find(*Document(),childId))RenderTreeNode(*child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void UIDesigner::RenderHierarchy() {
    if (!Document()) { widgets::EmptyState("尚未開啟 UI 文件","建立或開啟 .pxscene 後即可編輯 Layers。"); return; }
    widgets::SearchField("##tree-search","搜尋節點或型別…",m_session->panels.treeFilter,sizeof(m_session->panels.treeFilter));
    if (widgets::ToolbarButton("＋ Add","新增 Control")) ImGui::OpenPopup("AddUIControl"); ImGui::SameLine();
    if(widgets::ToolbarButton("更多…","複製、刪除與儲存命令"))ImGui::OpenPopup("##layers-more");
    if(ImGui::BeginPopup("##layers-more")){const bool selected=!Selected().Empty()&&Selected()!=RootId();if(ImGui::MenuItem("複製","Ctrl+D",false,selected))DuplicateSelected();if(ImGui::MenuItem("刪除","Delete",false,selected))RemoveSelected();ImGui::Separator();if(ImGui::MenuItem(Dirty()?"儲存目前文件 ●":"儲存目前文件","Ctrl+S",false,Dirty()))Save();ImGui::EndPopup();}
    if (ImGui::BeginPopup("AddUIControl")) {
        RenderAddControlPalette();
        ImGui::EndPopup();
    }
    if(!ImGui::GetIO().WantTextInput&&ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::IsKeyPressed(ImGuiKey_F2)){
        if(const auto* selected=View().Find(*Document(),Selected())){std::snprintf(m_session->panels.treeRename,sizeof(m_session->panels.treeRename),"%s",selected->name.c_str());m_session->panels.treeRenameOpen=true;}
    }
    if(m_session->panels.treeRenameOpen)ImGui::OpenPopup("重新命名節點");
    if(ImGui::BeginPopupModal("重新命名節點",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
        const bool submit=ImGui::InputText("名稱",m_session->panels.treeRename,sizeof(m_session->panels.treeRename),ImGuiInputTextFlags_EnterReturnsTrue);
        if((submit||ImGui::Button("重新命名"))&&m_session->panels.treeRename[0]){if(const auto* selected=View().Find(*Document(),Selected()))EditVariant("Name","$name",Variant(selected->name),Variant(std::string(m_session->panels.treeRename)),true,false);m_session->panels.treeRenameOpen=false;ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("取消")){m_session->panels.treeRenameOpen=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    if(m_session->panels.createComponentOpen)ImGui::OpenPopup("建立 UI Component");
    if(ImGui::BeginPopupModal("建立 UI Component",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::SetNextItemWidth(420);ImGui::InputText("Path",m_session->panels.componentPath,sizeof(m_session->panels.componentPath));
        const std::filesystem::path componentPath=m_session->panels.componentPath;const bool validPath=componentPath.extension()==".pxcomponent"&&componentPath.generic_string().starts_with("Content/");
        if(!validPath)widgets::InlineMessage(widgets::MessageKind::Warning,"路徑必須位於 Content/ 下並使用 .pxcomponent 副檔名。");
        ImGui::BeginDisabled(!validPath);if(ImGui::Button("建立 Component",ImVec2(150,0))){const Status status=CreateComponentFromSelected(componentPath);if(!status)EmitStatus(status);else{m_session->panels.createComponentOpen=false;ImGui::CloseCurrentPopup();}}ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("取消",ImVec2(100,0))){m_session->panels.createComponentOpen=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    ImGui::Separator();
    if (auto* root = View().Find(*Document(),RootId())) RenderTreeNode(*root);
}

void UIDesigner::RenderInsert(){
    if(!Document()){widgets::EmptyState("尚未開啟 UI 文件","開啟文件後可搜尋並加入 Control。");return;}
    widgets::SearchField("##insert-filter","搜尋 Control…",m_session->panels.paletteFilter,sizeof(m_session->panels.paletteFilter));
    RenderAddControlPalette();
}

void UIDesigner::EditVariant(const char* label, const std::string& property, Variant before, Variant value,
                             bool changed, bool continuous) {
    if (!Document() || !changed || before == value) return;
    DesignerDirtyFlags dirty = DirtyForProperty(property);
    if (const auto* node = View().Find(*Document(),Selected()))
        if (const auto* metadata = TypeRegistry::Global().FindProperty(node->type, property))
            dirty = DirtyForProperty(*metadata);
    const std::string commandLabel = label && *label ? label : "Change " + property;
    const std::array<Uuid, 1> targets{Selected()};
    if (continuous) {
        if (!m_session->Commands().GestureMatches(targets, property)) {
            const Status begin = m_session->Commands().BeginPropertyGesture(
                Selected(), property, commandLabel, dirty);
            if (!begin) return EmitStatus(begin);
        }
        const Status status = m_session->Commands().UpdatePropertyGesture(std::move(value));
        if (!status) EmitStatus(status);
    } else {
        const Status status = m_session->Commands().SetProperty(
            Selected(), property, std::move(value), commandLabel, dirty);
        if (!status) EmitStatus(status);
    }
    MarkEdited();
}

void UIDesigner::RenderInspector(const std::string& selectedAssetPath) {
    const DesignerUiStateSnapshot uiState=CaptureDesignerUiState(*m_session);
    if (uiState.selection==DesignerSelectionPresentation::NoDocument) { widgets::EmptyState("尚未開啟 UI 文件","Inspector 會顯示選取 Control 的屬性。"); return; }
    auto* node = View().Find(*Document(),Selected()); if (uiState.selection==DesignerSelectionPresentation::None||!node) { widgets::EmptyState("未選取 Control","從 Layers 或 Canvas 選取一個 Control。 "); return; }
    ImGui::TextUnformatted(node->name.c_str());ImGui::SameLine();ImGui::TextDisabled("· %s", node->type.c_str());
    if(uiState.selection==DesignerSelectionPresentation::Multiple){ImGui::SameLine();ImGui::TextDisabled("· %zu 個元件",uiState.selectionCount);}
    ImGui::SameLine();widgets::StatusChip(ui::ChildLayoutPolicyName(uiState.parentPolicy),uiState.positionManaged?widgets::MessageKind::Warning:widgets::MessageKind::Info);
    widgets::SearchField("##inspector-search","搜尋屬性...",m_session->inspectorSearch);
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

    std::vector<const PropertyInfo*> properties;
    std::unordered_set<std::string> seenProperties;
    std::string type = node->type;
    while (const auto* info = TypeRegistry::Global().Find(type)) {
        for (const auto& property : info->properties)
            if (HasFlag(property.flags, PropertyFlags::Editable) &&
                seenProperties.insert(property.name).second)
                properties.push_back(&property);
        type = info->base; if (type.empty()) break;
    }
    const auto lower=[](std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char character){return static_cast<char>(std::tolower(character));});return value;};
    const std::string query=lower(m_session->inspectorSearch);
    std::vector<const PropertyInfo*> visibleProperties;
    std::vector<std::string> propertyCategories;
    std::unordered_set<std::string> seenCategories;
    for(const auto* property:properties){
        const std::string searchable=lower(property->name+" "+property->editor.displayName+" "+property->category+" "+property->editor.description);
        if(!query.empty()&&searchable.find(query)==std::string::npos)continue;
        visibleProperties.push_back(property);
        if(seenCategories.insert(property->category).second)propertyCategories.push_back(property->category);
    }
    for(const std::string& propertyCategory:propertyCategories){
        const std::string categoryId=propertyCategory.empty()?"General":propertyCategory;
        const char* category=propertyCategory.empty()?"一般":propertyCategory=="Layout"?"配置":propertyCategory=="Appearance"?"外觀":propertyCategory=="Interaction"?"互動":propertyCategory=="Data"?"資料":propertyCategory.c_str();
        if(!widgets::Section(categoryId.c_str(),category,true))continue;
        for(const auto* property:visibleProperties){
        if(property->category!=propertyCategory)continue;
        if(property->ownership==PropertyOwnership::ParentLayout&&SelectedParentPolicy()!=ui::ChildLayoutPolicy::Free){
            ImGui::TextColored(EditorTheme().colors.warning,"位置與尺寸  由 %s 控制",ui::ChildLayoutPolicyName(SelectedParentPolicy()));
            if(ImGui::IsItemHovered())ImGui::SetTooltip("Container 子元件不寫入無效 offsets；請使用 minimum size、size flags 或拖曳排序。");
            continue;
        }
        const auto current = node->properties.find(property->name);
        Variant before = current == node->properties.end() ? Variant{} : current->second;
        Variant value = current == node->properties.end() ? property->defaultValue : current->second;
        std::vector<Uuid> propertyTargets;bool mixed=false;Variant effective=value.Clone();
        for(const Uuid& selectedId:Selection().OrderedItems()){const auto* selectedNode=View().Find(*Document(),selectedId);if(!selectedNode)continue;const auto* selectedProperty=TypeRegistry::Global().FindProperty(selectedNode->type,property->name);if(!selectedProperty||selectedProperty->type!=property->type||!HasFlag(selectedProperty->flags,PropertyFlags::Editable))continue;propertyTargets.push_back(selectedId);const auto found=selectedNode->properties.find(property->name);const Variant selectedValue=found==selectedNode->properties.end()?selectedProperty->defaultValue:found->second;if(selectedValue!=effective)mixed=true;}
        if(propertyTargets.empty())propertyTargets.push_back(Selected());
        bool changed = false, continuous = false;
        ImGui::PushID(property->name.c_str());
        if(mixed){ImGui::TextDisabled("— 混合值 (%zu)",propertyTargets.size());if(ImGui::IsItemHovered())ImGui::SetTooltip("修改後會批次套用到所有相容的已選元件。");}
        const char* propertyLabel=property->editor.displayName.empty()?property->name.c_str():property->editor.displayName.c_str();
        PropertyEditRequest editRequest{.property=*property,.value=value.Clone()};
        if(const auto* editor=PropertyEditorRegistry::Global().Resolve(*property)){changed=(*editor)(editRequest);value=std::move(editRequest.value);continuous=editRequest.continuous;}
        else if(auto* reference=value.TryGet<ResourceRefValue>()){ImGui::Text("%s: %s",propertyLabel,reference->lastKnownPath.empty()?"Select resource":reference->lastKnownPath.c_str());if(ImGui::BeginDragDropTarget()){if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")){reference->id={};reference->lastKnownPath=std::string(static_cast<const char*>(payload->Data),payload->DataSize?payload->DataSize-1:0);changed=true;}ImGui::EndDragDropTarget();}}
        else if(auto* token=value.TryGet<TokenRefValue>()){std::vector<std::string> tokenNames;if(const auto modern=Document()->Data().properties.find("styleSystem");modern!=Document()->Data().properties.end())if(const auto* system=modern->second.AsObject())if(const auto tokens=system->find("tokens");tokens!=system->end())if(const auto* list=tokens->second.AsArray())for(const auto& item:*list)if(const auto* definition=item.AsObject())if(const auto tokenName=definition->find("name");tokenName!=definition->end())if(const auto* text=tokenName->second.TryGet<std::string>())tokenNames.push_back(*text);if(ImGui::BeginCombo((std::string("Token: ")+propertyLabel).c_str(),token->name.empty()?"Select token":token->name.c_str())){for(std::size_t index=0;index<tokenNames.size();++index){const auto& tokenName=tokenNames[index];if(ImGui::Selectable((tokenName+"##token-"+std::to_string(index)).c_str(),tokenName==token->name)){token->name=tokenName;changed=true;}}ImGui::EndCombo();}}
        else ImGui::TextDisabled("%s  (unsupported value)", property->name.c_str());
        if(HasFlag(property->flags,PropertyFlags::ResourcePath)&&!selectedAssetPath.empty()){ImGui::SameLine();if(ImGui::SmallButton("使用選取素材")){if(auto* path=value.TryGet<std::string>())*path=selectedAssetPath;else if(auto* reference=value.TryGet<ResourceRefValue>()){reference->id={};reference->lastKnownPath=selectedAssetPath;}changed=true;continuous=false;}}
        const bool deactivated=ImGui::IsItemDeactivatedAfterEdit();
        if(!property->editor.description.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",property->editor.description.c_str());
        ImGui::SameLine();ImGui::TextColored(current==node->properties.end()?EditorTheme().colors.textMuted:EditorTheme().colors.info,current==node->properties.end()?"Default":"Local");ImGui::SameLine();
        if(ImGui::SmallButton("↶")){value=property->defaultValue.Clone();changed=true;continuous=false;}if(ImGui::IsItemHovered())ImGui::SetTooltip("重設為 TypeRegistry 預設值；目前來源：%s",current==node->properties.end()?"型別預設":"本地覆寫");
        const bool recordKey=m_session->timeline.autoKey&&propertyTargets.size()==1&&((changed&&!continuous)||(continuous&&deactivated));
        Variant keyValue=recordKey?value.Clone():Variant{};
        if(propertyTargets.size()==1){
            EditVariant(property->name.c_str(), property->name, std::move(before), std::move(value), changed, continuous);
            if (continuous && deactivated && m_session->Commands().GestureActive()) { const Status status = m_session->Commands().CommitPropertyGesture(); if (!status) EmitStatus(status); }
        }else if(continuous){
            if(changed){if(!m_session->Commands().GestureMatches(propertyTargets,property->name)){const Status begin=m_session->Commands().BeginPropertyGesture(propertyTargets,property->name,"Batch change "+property->name,DirtyForProperty(*property));if(!begin){EmitStatus(begin);ImGui::PopID();continue;}}const Status status=m_session->Commands().UpdatePropertyGesture(value.Clone());if(!status)EmitStatus(status);else MarkEdited();}
            if(deactivated&&m_session->Commands().GestureActive()){const Status status=m_session->Commands().CommitPropertyGesture();if(!status)EmitStatus(status);MarkEdited();}
        }else if(changed){const Status status=m_session->Commands().SetProperties(propertyTargets,property->name,value,"Batch change "+property->name,DirtyForProperty(*property));if(!status)EmitStatus(status);else MarkEdited();}
        if(recordKey)RecordAnimationKey(Selected(),property->name,keyValue);
        ImGui::PopID();
        }
    }

    ImGui::Separator();if(widgets::Section("interaction-triggers","Interaction · Triggers",false))RenderEvents();
    if(widgets::Section("binding","Data Binding",false)){
    VariantObject bindings;if(const auto found=node->properties.find("bindings");found!=node->properties.end()&&found->second.AsObject())bindings=*found->second.AsObject();
    for(auto& [target,value]:bindings){auto* definition=value.AsObject();if(!definition)continue;std::string path=definition->contains("path")&&definition->at("path").TryGet<std::string>()?*definition->at("path").TryGet<std::string>():"";std::string formatter=definition->contains("formatter")&&definition->at("formatter").TryGet<std::string>()?*definition->at("formatter").TryGet<std::string>():"";bool changedBinding=false;
        const auto* targetProperty=TypeRegistry::Global().FindProperty(node->type,target);if(ImGui::BeginCombo((target+" source").c_str(),path.empty()?"Select ViewModel property":path.c_str())){for(const auto& source:m_session->previewFixture.ViewModel().EnumerateProperties()){bool compatible=targetProperty&&source.type==targetProperty->type;for(const auto* candidate:m_formatters.Descriptors())if(candidate->input==source.type&&targetProperty&&candidate->output==targetProperty->type)compatible=true;if(!compatible)continue;if(ImGui::Selectable((source.path+"##binding-"+target).c_str(),path==source.path)){path=source.path;formatter.clear();changedBinding=true;}}ImGui::EndCombo();}
        if(ImGui::BeginCombo(("Formatter##"+target).c_str(),formatter.empty()?"None":formatter.c_str())){if(ImGui::Selectable("None",formatter.empty())){formatter.clear();changedBinding=true;}const auto source=m_session->previewFixture.ViewModel().Describe(path);for(const auto* candidate:m_formatters.Descriptors())if(source&&targetProperty&&candidate->input==source->type&&candidate->output==targetProperty->type)if(ImGui::Selectable((candidate->name+"##formatter-"+target).c_str(),formatter==candidate->name)){formatter=candidate->name;changedBinding=true;}ImGui::EndCombo();}
        if(changedBinding){VariantObject changed=bindings;auto* changedDefinition=changed[target].AsObject();(*changedDefinition)["path"]=path;(*changedDefinition)["formatter"]=formatter;EditVariant("bindings","bindings",node->properties.contains("bindings")?node->properties["bindings"]:Variant{},Variant(std::move(changed)),true,false);}}
    if (ImGui::Button("Add typed binding")) ImGui::OpenPopup("AddBinding");
    if (ImGui::BeginPopup("AddBinding")) { for (const auto* property : properties) if (property->bindable && ImGui::MenuItem(property->name.c_str())) {
        VariantObject changed=bindings;changed[property->name]=VariantObject{{"path",std::string{}},{"formatter",std::string{}}};EditVariant("bindings","bindings",node->properties.contains("bindings")?node->properties["bindings"]:Variant{},Variant(std::move(changed)),true,false); ImGui::CloseCurrentPopup();
    } ImGui::EndPopup(); }
    if (!selectedAssetPath.empty() && node->type == "TextureRect" && ImGui::Button("Use selected asset"))
        EditVariant("path", "path", node->properties.contains("path") ? node->properties["path"] : Variant{}, Variant(selectedAssetPath), true, false);
    ImGui::TextDisabled("條件式 UI 請繫結 ViewModel computed property；Binding 不執行表達式。");
    }
    if(widgets::Section("style-binding","Appearance · Style Binding",false))RenderTheme();
    if(widgets::Section("accessibility","Accessibility",false))ImGui::TextWrapped("Accessibility 屬性與焦點導覽直接顯示在上方對應分類；此區保留語意與驗證摘要。");
}

void UIDesigner::RebuildLayout() {
    View().ClearLayout();
    m_layoutRoot.reset();
    if (!Document()) return;
    resource::TypedDocument preview = Document()->Data();
    for (auto& node : preview.nodes) node.properties.erase("bindings");
    const ui::UIDocumentLoader loader=[this](const ResourceRefValue& reference){return LoadReferencedUI(reference);};
    auto scene = ui::InstantiateUIScene(preview, nullptr, m_formatters, loader); if (!scene) return;
    m_layoutRoot = std::move(scene.Value().root);
    UpdateLayoutCache();
}

void UIDesigner::Relayout(const DocumentChangeSet& changes) {
    if (!Document() || !m_layoutRoot || changes.properties.empty()) {
        RebuildLayout();
        return;
    }
    for (const auto& changed : changes.properties) {
        if (changed.node == Document()->DocumentId()) {
            RebuildLayout();
            return;
        }
        const auto* record = View().Find(*Document(), changed.node);
        auto* control = record
                            ? dynamic_cast<ui::Control*>(m_layoutRoot->Find(changed.node))
                            : nullptr;
        if (!record || !control) {
            RebuildLayout();
            return;
        }
        if (changed.property == "$name") {
            control->SetName(record->name);
            continue;
        }
        if (changed.property == "styleBinding") {
            const auto found = record->properties.find("styleBinding");
            if (found == record->properties.end()) {
                control->SetStyleBinding({});
                continue;
            }
            auto binding = ui::ParseStyleBinding(found->second);
            if (!binding) {
                RebuildLayout();
                return;
            }
            control->SetStyleBinding(binding.TakeValue());
            continue;
        }
        const auto* property =
            TypeRegistry::Global().FindProperty(record->type, changed.property);
        if (!property || !property->set) {
            RebuildLayout();
            return;
        }
        const auto value = record->properties.find(changed.property);
        const Variant& applied =
            value == record->properties.end() ? property->defaultValue : value->second;
        if (!property->set(*control, applied)) {
            RebuildLayout();
            return;
        }
    }
    UpdateLayoutCache();
}

void UIDesigner::UpdateLayoutCache() {
    if (!Document() || !m_layoutRoot) {
        View().ClearLayout();
        return;
    }
    Vec2 canvas = CanvasSize();
    (void)m_layoutRoot->Measure(canvas);
    m_layoutRoot->Arrange({0, 0, canvas.x, canvas.y});
    std::unordered_map<Uuid,Rect,UuidHash> layout;
    std::unordered_map<Uuid,ui::ChildLayoutPolicy,UuidHash> policies;
    for (const auto& record : Document()->Data().nodes) {
        if (auto* control = dynamic_cast<ui::Control*>(m_layoutRoot->Find(record.id))) {
            layout[record.id] = control->LayoutRect();
            policies[record.id] = control->ChildPolicy();
        }
    }
    for(const auto& record:Document()->Data().nodes){const auto visibility=record.properties.find("visibility");const auto* value=visibility==record.properties.end()?nullptr:visibility->second.TryGet<std::string>();if(!value||*value!="Collapsed")continue;const Rect parent=record.parent.Empty()?Rect{0,0,canvas.x,canvas.y}:(layout.contains(record.parent)?layout.at(record.parent):Rect{0,0,canvas.x,canvas.y});Rect anchors{},offsets{0,0,120,40};Vec2 minimum{120,40};if(const auto found=record.properties.find("anchors");found!=record.properties.end()&&found->second.TryGet<Rect>())anchors=*found->second.TryGet<Rect>();if(const auto found=record.properties.find("offsets");found!=record.properties.end()&&found->second.TryGet<Rect>())offsets=*found->second.TryGet<Rect>();if(const auto found=record.properties.find("minimumSize");found!=record.properties.end()&&found->second.TryGet<Vec2>())minimum=*found->second.TryGet<Vec2>();Rect authored=ui::ControlLayoutMath::ResolveChildRect(parent,anchors,offsets,minimum);authored.w=std::max(authored.w,16.0f);authored.h=std::max(authored.h,16.0f);layout[record.id]=authored;}
    View().ReplaceLayout(std::move(layout),std::move(policies));
}

Rect UIDesigner::SelectedRect() const { const auto rect=View().LayoutRect(Selected()); return rect?*rect:Rect{}; }

Vec2 UIDesigner::CanvasSize() const {
    if (Document()) {
        if (const auto it = Document()->Data().properties.find("canvasSize");
            it != Document()->Data().properties.end()) {
            if (const auto* value = it->second.TryGet<Vec2>()) return *value;
        }
    }
    return {1280, 720};
}

std::string UIDesigner::SelectionSummary() const {
    if (!Document()) return "未開啟 UI 文件";
    const auto* node = View().Find(*Document(),Selected());
    if (!node) return "未選取元件";
    if (Selection().Size() > 1) return std::to_string(Selection().Size()) + " 個元件";
    return node->name + "  ·  " + node->type;
}

ui::ChildLayoutPolicy UIDesigner::SelectedParentPolicy() const {
    if (!Document()) return ui::ChildLayoutPolicy::Free;
    const auto* node = View().Find(*Document(),Selected());
    if (!node || node->parent.Empty()) return ui::ChildLayoutPolicy::Free;
    const auto policy = View().ChildPolicy(node->parent);
    return policy ? *policy : ui::ChildLayoutPolicy::Free;
}

Rect UIDesigner::ParentRect(const Uuid& nodeId) const {
    if (!Document()) return {};
    const auto* node = View().Find(*Document(),nodeId);
    if (!node || node->parent.Empty()) return {0, 0, CanvasSize().x, CanvasSize().y};
    const auto rect = View().LayoutRect(node->parent);
    return rect ? *rect : Rect{0, 0, CanvasSize().x, CanvasSize().y};
}

void UIDesigner::RenderViewportToolbar() {
    if(m_session->viewport.interactivePreview&&ImGui::IsKeyPressed(ImGuiKey_Escape))m_session->viewport.interactivePreview=false;
    if(m_session->viewport.interactivePreview){
        ImGui::TextColored(EditorTheme().colors.success,"PREVIEW · 所有輸入交給 Runtime");ImGui::SameLine();
        if(widgets::ToolbarButton("返回 Edit  (Esc)","結束互動預覽並返回編輯"))m_session->viewport.interactivePreview=false;
        ImGui::SameLine();ImGui::Checkbox("像素精確",&m_session->viewport.pixelExactPreview);return;
    }
    ImGui::TextDisabled("MODE");ImGui::SameLine();widgets::StatusChip("EDIT",widgets::MessageKind::Info);ImGui::SameLine();if(widgets::ToolbarButton("進入 Preview","把 Artboard 輸入交給 Runtime"))m_session->viewport.interactivePreview=true;ImGui::SameLine();ImGui::TextDisabled("│");ImGui::SameLine();
    const auto toolButton = [&](const char* label, DesignerTool tool) {
        const bool active = m_session->viewport.tool == tool;
        if (widgets::ToolbarButton(label,tool==DesignerTool::Select?"選取、框選、移動與調整尺寸":"編輯錨點並保持視覺位置",active)) m_session->viewport.tool = tool;
    };
    toolButton("選取 V", DesignerTool::Select); ImGui::SameLine();
    toolButton("錨點 A", DesignerTool::Anchors); ImGui::SameLine();
    if (widgets::ToolbarButton("錨點預設","套用常用錨點並保持目前視覺位置",false,Selected()!=RootId(),"請先選取非根節點 Control")) ImGui::OpenPopup("DesignerAnchorPresets");
    if (ImGui::BeginPopup("DesignerAnchorPresets")) {
        const auto apply = [&](const char* label, Rect anchors) {
            if (!ImGui::MenuItem(label) || !Document() || Selected() == RootId()) return;
            auto* node = View().Find(*Document(),Selected()); if (!node) return;
            const Rect visual = SelectedRect(); const Rect parent = ParentRect(Selected());
            const Rect offsets = ui::ControlLayoutMath::OffsetsForRect(parent, anchors, visual);
            const Variant beforeAnchors = node->properties.contains("anchors") ? node->properties.at("anchors") : Variant{};
            const Variant beforeOffsets = node->properties.contains("offsets") ? node->properties.at("offsets") : Variant{};
            auto command = std::make_unique<CompositeEditCommand>("套用錨點預設");
            command->Add(std::make_unique<PropertyChangeCommand>("錨點",Selected(),"anchors",beforeAnchors,Variant(anchors),std::chrono::steady_clock::now(),false));
            command->Add(std::make_unique<PropertyChangeCommand>("保持位置",Selected(),"offsets",beforeOffsets,Variant(offsets),std::chrono::steady_clock::now(),false));
            DocumentChangeSet changes=DocumentChangeSet::Property(Selected(),"anchors",DesignerDirtyFlags::Layout|DesignerDirtyFlags::Paint);changes.Merge(DocumentChangeSet::Property(Selected(),"offsets",DesignerDirtyFlags::Layout|DesignerDirtyFlags::Paint));const Status status=m_session->Commands().Execute(std::move(command),std::move(changes));if(!status)EmitStatus(status);else MarkEdited();
        };
        apply("左上", {0,0,0,0}); apply("置中", {.5f,.5f,.5f,.5f});
        apply("水平延展", {0,.5f,1,.5f}); apply("垂直延展", {.5f,0,.5f,1});
        apply("全區域", {0,0,1,1}); ImGui::EndPopup();
    }
    ImGui::SameLine();if(widgets::ToolbarButton("對齊","對齊或平均分布目前選取項目",false,!Selected().Empty(),"請先選取 Control"))ImGui::OpenPopup("DesignerAlignmentMenu");
    if(ImGui::BeginPopup("DesignerAlignmentMenu")){
        if(ImGui::MenuItem("靠左"))AlignSelection(AlignAction::Left);if(ImGui::MenuItem("水平置中"))AlignSelection(AlignAction::HCenter);if(ImGui::MenuItem("靠右"))AlignSelection(AlignAction::Right);
        if(ImGui::MenuItem("靠上"))AlignSelection(AlignAction::Top);if(ImGui::MenuItem("垂直置中"))AlignSelection(AlignAction::VCenter);if(ImGui::MenuItem("靠下"))AlignSelection(AlignAction::Bottom);
        ImGui::Separator();if(ImGui::MenuItem("水平平均分布"))AlignSelection(AlignAction::DistributeH);if(ImGui::MenuItem("垂直平均分布"))AlignSelection(AlignAction::DistributeV);ImGui::EndPopup();}
    ImGui::SameLine();ImGui::TextDisabled("│");ImGui::SameLine();if(widgets::ToolbarButton("吸附…","智慧吸附、格線與格線吸附設定"))ImGui::OpenPopup("##designer-snap-options");
    if(ImGui::BeginPopup("##designer-snap-options")){ImGui::Checkbox("智慧吸附",&m_session->viewport.smartGuides);ImGui::Checkbox("顯示格線  G",&m_session->viewport.gridVisible);ImGui::Checkbox("格線吸附  Shift+G",&m_session->viewport.gridSnap);ImGui::TextDisabled("Alt+拖曳：暫時停用吸附");ImGui::EndPopup();}
    ImGui::SameLine();ImGui::TextDisabled("│ VIEW");ImGui::SameLine();
    if (ImGui::Button("Fit")) { m_session->viewport.fitToViewport=true; m_session->viewport.applyStoredScroll=true; }
    ImGui::SameLine();
    if (ImGui::Button("100%")) { m_session->viewport.fitToViewport=false; m_session->viewport.zoom=1.0f; m_session->viewport.applyStoredScroll=true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130);
    int zoomPercent = static_cast<int>(std::round(m_session->viewport.zoom * 100.0f));
    if (ImGui::SliderInt("##DesignerZoom", &zoomPercent, 25, 400, "%d%%")) {
        m_session->viewport.fitToViewport=false;
        m_session->viewport.zoom = std::clamp(zoomPercent / 100.0f, .25f, 4.0f);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", SelectionSummary().c_str());ImGui::SameLine();
    widgets::StatusChip(ui::ChildLayoutPolicyName(SelectedParentPolicy()),SelectedParentPolicy()==ui::ChildLayoutPolicy::Free?widgets::MessageKind::Info:widgets::MessageKind::Warning);
    // This row is always reserved and has a fixed height. Resize feedback only
    // changes its text, so per-frame digit changes cannot reflow the viewport.
    RenderCanvasHintBar(m_session->canvas.hint);
}

bool UIDesigner::ProcessCanvasInput(const ImRect& viewport, ImVec2 p0, float scale, bool hovered,
                                    const std::string& selectedAssetPath) {
    if (!Document() || scale <= 0.0f) return false;
    ImGuiIO& io = ImGui::GetIO();
    const CanvasTransform transform({p0.x, p0.y}, scale);
    const Vec2 canvas = transform.ScreenToCanvas({io.MousePos.x, io.MousePos.y});
    DesignerPointerEvent event;
    event.screenPosition = {io.MousePos.x, io.MousePos.y};
    event.canvasPosition = canvas;
    event.zoom = scale;
    event.button = DesignerMouseButton::Left;
    event.modifiers = {.shift = io.KeyShift, .alt = io.KeyAlt,
                       .controlOrCommand = io.KeyCtrl || io.KeySuper};
    auto& interaction = m_session->Interaction();
    interaction.SetAnchorTool(m_session->viewport.tool == DesignerTool::Anchors);
    if (hovered) interaction.UpdateHover(event);
    else {
        m_session->hoveredNode = {};
        m_session->canvas.hoveredResizeHandle = 0;
        m_session->canvas.hoveredAnchorHandle = 0;
        m_session->canvas.hoveredPivotHandle = false;
    }
    bool handled = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
        (interaction.HasCapture() || (hovered && !io.WantTextInput))) {
        interaction.Cancel();
        handled = true;
    }
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_V)||ImGui::IsKeyPressed(ImGuiKey_Q)) m_session->viewport.tool=DesignerTool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_A)) m_session->viewport.tool=DesignerTool::Anchors;
        if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            if (io.KeyShift) m_session->viewport.gridSnap=!m_session->viewport.gridSnap;
            else m_session->viewport.gridVisible=!m_session->viewport.gridVisible;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0)) {
            m_session->viewport.zoom=1.0f;m_session->viewport.fitToViewport=false;m_session->viewport.applyStoredScroll=true;return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !Selected().Empty()) {
            const Rect selected=SelectedRect();
            m_session->viewport.scrollX=std::max(0.0f,(selected.x+selected.w*.5f)*scale-viewport.GetWidth()*.5f);
            m_session->viewport.scrollY=std::max(0.0f,(selected.y+selected.h*.5f)*scale-viewport.GetHeight()*.5f);
            m_session->viewport.applyStoredScroll=true;
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)||ImGui::IsKeyPressed(ImGuiKey_Backspace)) RemoveSelected();
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_C))CopySelected();
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_V))PasteClipboard(canvas);
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_D))DuplicateSelected();
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_A)){Selection().Clear();for(const auto& node:Document()->Data().nodes)if(node.id!=RootId())Selection().Add(node.id,false);return true;}
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_RightBracket))ChangeSelectedLayer(io.KeyShift?LayerAction::BringToFront:LayerAction::BringForward);
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_LeftBracket))ChangeSelectedLayer(io.KeyShift?LayerAction::SendToBack:LayerAction::SendBackward);
        const bool left=ImGui::IsKeyPressed(ImGuiKey_LeftArrow),right=ImGui::IsKeyPressed(ImGuiKey_RightArrow),up=ImGui::IsKeyPressed(ImGuiKey_UpArrow),down=ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if((left||right||up||down)&&!Selection().Empty()){
            const float step=io.KeyShift?10.0f:1.0f;const Vec2 delta{(right?step:0)-(left?step:0),(down?step:0)-(up?step:0)};
            auto command=std::make_unique<CompositeEditCommand>("Nudge UI controls");DocumentChangeSet changes;
            for(const Uuid& id:Selection().OrderedItems()){const auto* selected=View().Find(*Document(),id);if(!selected||id==RootId())continue;const auto policy=View().ChildPolicy(selected->parent);if(policy&&*policy!=ui::ChildLayoutPolicy::Free)continue;const auto found=selected->properties.find("offsets");if(found==selected->properties.end())continue;if(const auto* before=found->second.TryGet<Rect>()){Rect after=*before;after.x+=delta.x;after.y+=delta.y;command->Add(std::make_unique<PropertyChangeCommand>("Nudge control",id,"offsets",Variant(*before),Variant(after),std::chrono::steady_clock::now(),false));changes.Merge(DocumentChangeSet::Property(id,"offsets",DesignerDirtyFlags::Layout|DesignerDirtyFlags::Paint));}}
            if(!command->Empty()){const Status status=m_session->Commands().Execute(std::move(command),std::move(changes));if(!status)EmitStatus(status);else MarkEdited();return true;}
        }
    }
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        m_session->canvas.contextPosition=canvas;m_session->canvas.contextTarget=m_session->hoveredNode;if(!m_session->canvas.contextTarget.Empty()){if(!Selection().Contains(m_session->canvas.contextTarget))SelectOnly(m_session->canvas.contextTarget);MakePrimary(m_session->canvas.contextTarget);}ImGui::OpenPopup("UIDesignerCanvasContext");
    }
    if(ImGui::BeginPopup("UIDesignerCanvasContext")){
        if(ImGui::BeginMenu("新增控制項")){RenderAddControlPalette(m_session->canvas.contextPosition);ImGui::EndMenu();}
        if(!selectedAssetPath.empty()&&ImGui::MenuItem("Use selected image as TextureRect"))AddNode("TextureRect",m_session->canvas.contextPosition,selectedAssetPath);
        if(!selectedAssetPath.empty()&&ImGui::MenuItem("將選取圖片設為背景")){AddNode("TextureRect",m_session->canvas.contextPosition,selectedAssetPath);SetSelectedAsBackground(false);}
        ImGui::Separator();if(ImGui::MenuItem("複製","Ctrl+C",false,!Selected().Empty()))CopySelected();if(ImGui::MenuItem("貼上","Ctrl+V",false,!m_session->clipboardSubtree.empty()))PasteClipboard(m_session->canvas.contextPosition);if(ImGui::MenuItem("建立副本","Ctrl+D",false,!Selected().Empty()))DuplicateSelected();if(ImGui::MenuItem("建立 Component…",nullptr,false,!Selected().Empty()&&Selected()!=RootId()))m_session->panels.createComponentOpen=true;
        if(const auto* context=View().Find(*Document(),m_session->canvas.contextTarget);context&&context->type=="TextureRect"){if(ImGui::MenuItem("設為背景"))SetSelectedAsBackground(false);if(ImGui::MenuItem("設為背景並鎖定"))SetSelectedAsBackground(true);if(ImGui::MenuItem("恢復圖片原始尺寸"))RestoreSelectedImageSize();}
        if(!m_session->canvas.contextTarget.Empty()&&m_session->canvas.contextTarget!=RootId()&&ImGui::BeginMenu("對齊")){if(ImGui::MenuItem("靠左"))AlignSelection(AlignAction::Left);if(ImGui::MenuItem("水平置中"))AlignSelection(AlignAction::HCenter);if(ImGui::MenuItem("靠右"))AlignSelection(AlignAction::Right);if(ImGui::MenuItem("靠上"))AlignSelection(AlignAction::Top);if(ImGui::MenuItem("垂直置中"))AlignSelection(AlignAction::VCenter);if(ImGui::MenuItem("靠下"))AlignSelection(AlignAction::Bottom);ImGui::EndMenu();}
        if(ImGui::BeginMenu("圖層順序",!Selected().Empty())){if(ImGui::MenuItem("置頂"))ChangeSelectedLayer(LayerAction::BringToFront);if(ImGui::MenuItem("上移一層"))ChangeSelectedLayer(LayerAction::BringForward);if(ImGui::MenuItem("下移一層"))ChangeSelectedLayer(LayerAction::SendBackward);if(ImGui::MenuItem("置底"))ChangeSelectedLayer(LayerAction::SendToBack);ImGui::EndMenu();}
        ImGui::Separator();const bool canDelete=!m_session->canvas.contextTarget.Empty()&&m_session->canvas.contextTarget!=RootId();if(ImGui::MenuItem("刪除","Delete",false,canDelete))RemoveSelected();if(!m_session->canvas.contextTarget.Empty()&&!canDelete&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("根節點不能刪除");ImGui::EndPopup();
    }
    if(m_session->viewport.interactivePreview)return handled;

    const int handle=m_session->canvas.hoveredResizeHandle;
    if(handle==2||handle==6)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);else if(handle==4||handle==8)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);else if(handle==1||handle==5)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);else if(handle==3||handle==7)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (interaction.PointerDown(event)) handled = true;
    }
    if (interaction.HasCapture() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const auto gesture=m_session->canvas.gesture;
        if (interaction.PointerMove(event)) {
            handled = true;
            if(gesture==DesignerCanvasGesture::Move||gesture==DesignerCanvasGesture::Resize||
               gesture==DesignerCanvasGesture::Anchors||gesture==DesignerCanvasGesture::Pivot)
                MarkEdited();
        }
    }
    if (interaction.HasCapture() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const auto gesture=m_session->canvas.gesture;
        if (interaction.PointerUp(event)) {
            handled = true;
            if(gesture==DesignerCanvasGesture::Reorder)MarkEdited(true);
        }
    }
    return handled;
}

void UIDesigner::RenderCanvasOverlay(ImVec2 p0, float scale) {
    if (!Document()) return; ImDrawList* draw=ImGui::GetWindowDrawList();const auto& colors=EditorTheme().colors;const auto packed=[](ImVec4 color){return ImGui::ColorConvertFloat4ToU32(color);};const ImU32 selectionColor=packed(colors.canvasSelection),handleColor=packed(colors.canvasHandle),anchorColor=packed(colors.canvasAnchor),guideColor=packed(colors.canvasGuide);const Vec2 canvas=CanvasSize();const ImVec2 max{p0.x+canvas.x*scale,p0.y+canvas.y*scale};draw->PushClipRect(p0,max,true);
    if(m_session->viewport.gridVisible){float step=static_cast<float>(m_session->viewport.gridSize);while(step*scale<12)step*=2;int line=0;for(float x=0;x<=canvas.x;x+=step,++line)draw->AddLine({p0.x+x*scale,p0.y},{p0.x+x*scale,max.y},packed(line%4==0?colors.canvasGridMajor:colors.canvasGridMinor));line=0;for(float y=0;y<=canvas.y;y+=step,++line)draw->AddLine({p0.x,p0.y+y*scale},{max.x,p0.y+y*scale},packed(line%4==0?colors.canvasGridMajor:colors.canvasGridMinor));}
    for(const auto& guide:m_session->canvas.snapGuides){
        if(guide.orientation==SnapGuideOrientation::Vertical)draw->AddLine({p0.x+guide.position*scale,p0.y+guide.from*scale},{p0.x+guide.position*scale,p0.y+guide.to*scale},guideColor,1.5f);
        else draw->AddLine({p0.x+guide.from*scale,p0.y+guide.position*scale},{p0.x+guide.to*scale,p0.y+guide.position*scale},guideColor,1.5f);
    }
    for(const auto& distance:m_session->canvas.snapDistances){const ImVec2 position{p0.x+distance.position.x*scale,p0.y+distance.position.y*scale};const std::string label=std::to_string(static_cast<int>(std::round(distance.distance)));draw->AddText(position,anchorColor,label.c_str());}
    for(const auto& node:Document()->Data().nodes){const auto rect=View().LayoutRect(node.id);if(!rect)continue;const bool selected=Selection().Contains(node.id),primary=node.id==Selected(),hover=node.id==m_session->hoveredNode;if(!selected&&!hover&&!m_session->viewport.showAllOutlines)continue;std::string visibility="Visible";if(const auto found=node.properties.find("visibility");found!=node.properties.end()&&found->second.TryGet<std::string>())visibility=*found->second.TryGet<std::string>();const Rect r=*rect;const ImU32 color=visibility=="Collapsed"?packed(WithAlpha(colors.canvasAnchor,.85f)):visibility=="Hidden"?packed(WithAlpha(colors.textMuted,.82f)):selected?selectionColor:packed(WithAlpha(colors.textSecondary,.6f));draw->AddRect({p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},color,3.0f,ImDrawFlags_None,selected?2.0f:1.0f);if(primary){const std::string label=node.name+(visibility=="Visible"?"":"  ["+visibility+"]");draw->AddText({p0.x+r.x*scale+5,p0.y+r.y*scale-20},packed(colors.textPrimary),label.c_str());}}
    if(!Selected().Empty()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const Rect r=SelectedRect();const ImVec2 points[8]{{p0.x+r.x*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+r.y*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h*.5f)*scale},{p0.x+(r.x+r.w)*scale,p0.y+(r.y+r.h)*scale},{p0.x+(r.x+r.w*.5f)*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h)*scale},{p0.x+r.x*scale,p0.y+(r.y+r.h*.5f)*scale}};for(const auto& point:points){draw->AddRectFilled({point.x-4,point.y-4},{point.x+4,point.y+4},handleColor,1);draw->AddRect({point.x-4,point.y-4},{point.x+4,point.y+4},selectionColor,1);}}
    if(m_session->viewport.tool==DesignerTool::Select&&!Selected().Empty()&&Selected()!=RootId()&&Selection().Size()==1&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=View().Find(*Document(),Selected());Vec2 pivot{.5f,.5f};if(node)if(const auto found=node->properties.find("pivot");found!=node->properties.end())if(const auto* value=found->second.TryGet<Vec2>())pivot=*value;const Rect selected=SelectedRect();const ImVec2 point{p0.x+(selected.x+selected.w*pivot.x)*scale,p0.y+(selected.y+selected.h*pivot.y)*scale};const ImU32 pivotColor=packed(colors.warning);draw->AddCircleFilled(point,5.5f,pivotColor);draw->AddCircle(point,8.0f,packed(colors.canvasBackground),0,2.0f);draw->AddLine({point.x-11,point.y},{point.x+11,point.y},pivotColor,1.5f);draw->AddLine({point.x,point.y-11},{point.x,point.y+11},pivotColor,1.5f);}
    if(m_session->viewport.tool==DesignerTool::Anchors&&!Selected().Empty()&&Selected()!=RootId()&&SelectedParentPolicy()==ui::ChildLayoutPolicy::Free){const auto* node=View().Find(*Document(),Selected());Rect anchors{};if(node)if(const auto found=node->properties.find("anchors");found!=node->properties.end())if(const auto* value=found->second.TryGet<Rect>())anchors=*value;const Rect parent=ParentRect(Selected()),selected=SelectedRect();const ImVec2 points[4]{{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.y)*scale},{p0.x+(parent.x+parent.w*anchors.w)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale},{p0.x+(parent.x+parent.w*anchors.x)*scale,p0.y+(parent.y+parent.h*anchors.h)*scale}};const ImVec2 corners[4]{{p0.x+selected.x*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+selected.y*scale},{p0.x+(selected.x+selected.w)*scale,p0.y+(selected.y+selected.h)*scale},{p0.x+selected.x*scale,p0.y+(selected.y+selected.h)*scale}};for(int index=0;index<4;++index){draw->AddLine(points[index],corners[index],packed(WithAlpha(colors.canvasAnchor,.6f)));draw->AddQuadFilled({points[index].x,points[index].y-6},{points[index].x+6,points[index].y},{points[index].x,points[index].y+6},{points[index].x-6,points[index].y},anchorColor);}}
    if(m_session->canvas.gesture==DesignerCanvasGesture::Reorder){const auto* node=View().Find(*Document(),Selected());if(node){std::vector<Uuid> siblings;for(const Uuid& sibling:View().Children(node->parent))if(sibling!=Selected())siblings.push_back(sibling);Rect marker{};const auto policy=SelectedParentPolicy();if(!siblings.empty()){if(m_session->canvas.reorderPreview<siblings.size())if(const auto rect=View().LayoutRect(siblings[m_session->canvas.reorderPreview]))marker=*rect;if(marker==Rect{})if(const auto rect=View().LayoutRect(siblings.back())){marker=*rect;if(policy==ui::ChildLayoutPolicy::LinearX)marker.x+=marker.w;else marker.y+=marker.h;}}if(policy==ui::ChildLayoutPolicy::LinearX)draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+marker.x*scale,p0.y+(marker.y+marker.h)*scale},selectionColor,3);else draw->AddLine({p0.x+marker.x*scale,p0.y+marker.y*scale},{p0.x+(marker.x+marker.w)*scale,p0.y+marker.y*scale},selectionColor,3);}}
    if(m_session->canvas.gesture==DesignerCanvasGesture::Marquee){const ImVec2 a{p0.x+m_session->canvas.dragStart.x*scale,p0.y+m_session->canvas.dragStart.y*scale},b{p0.x+m_session->canvas.marqueeCurrent.x*scale,p0.y+m_session->canvas.marqueeCurrent.y*scale};draw->AddRectFilled({std::min(a.x,b.x),std::min(a.y,b.y)},{std::max(a.x,b.x),std::max(a.y,b.y)},packed(WithAlpha(colors.selectionBackground,.4f)));draw->AddRect({std::min(a.x,b.x),std::min(a.y,b.y)},{std::max(a.x,b.x),std::max(a.y,b.y)},selectionColor);}
    if(m_session->canvas.gesture==DesignerCanvasGesture::Resize&&!m_session->canvas.hint.empty()){const ImVec2 mouse=ImGui::GetMousePos(),textSize=ImGui::CalcTextSize(m_session->canvas.hint.c_str());const ImVec2 a{mouse.x+14,mouse.y+14},b{a.x+textSize.x+12,a.y+textSize.y+8};draw->AddRectFilled(a,b,packed(colors.surfaceOverlay),EditorTheme().metrics.radius);draw->AddText({a.x+6,a.y+4},packed(colors.textPrimary),m_session->canvas.hint.c_str());}
    draw->PopClipRect();
}

void UIDesigner::AddImageAt(float x,float y,const std::string& image){AddNode("TextureRect",{x,y},image);}

void UIDesigner::RecordAnimationKey(const Uuid& node,const std::string& property,const Variant& value){
    if(!Document()||node.Empty()||property.empty())return;
    const auto found=Document()->Data().properties.find("animations");
    if(found==Document()->Data().properties.end())return;
    const Variant before=found->second.Clone();auto parsed=ui::ParseUIAnimationLibrary(before,Document()->Path().generic_string());
    if(!parsed){EmitStatus(Status::Fail(parsed.Diagnostics()));return;}
    auto library=parsed.TakeValue();
    auto clip=std::find_if(library.clips.begin(),library.clips.end(),[&](const ui::AnimationClip& candidate){return candidate.id==m_session->timeline.selectedClip;});
    if(clip==library.clips.end())clip=library.clips.begin();if(clip==library.clips.end())return;m_session->timeline.selectedClip=clip->id;
    auto track=std::find_if(clip->tracks.begin(),clip->tracks.end(),[&](const ui::AnimationTrack& candidate){return candidate.node==node&&candidate.property==property;});
    if(track==clip->tracks.end()){clip->tracks.push_back({.node=node,.property=property});track=std::prev(clip->tracks.end());m_session->timeline.selectedTrack=static_cast<int>(clip->tracks.size())-1;}
    const float time=std::max(0.0f,m_session->timeline.currentTime);auto key=std::find_if(track->keys.begin(),track->keys.end(),[&](const ui::AnimationKey& candidate){return std::abs(candidate.time-time)<.0005f;});
    if(key==track->keys.end())track->keys.push_back({time,value.Clone(),ui::Ease::Linear,ui::KeyInterpolation::Linear});else key->value=value.Clone();
    std::stable_sort(track->keys.begin(),track->keys.end(),[](const ui::AnimationKey& left,const ui::AnimationKey& right){return left.time<right.time;});
    const Variant after=ui::WriteUIAnimationLibrary(library);if(after==before)return;
    const Status status=m_session->Commands().SetProperty(Document()->DocumentId(),"animations",after,"Auto-key "+property,DesignerDirtyFlags::Animation|DesignerDirtyFlags::Paint);if(!status)EmitStatus(status);else MarkEdited();
}

void UIDesigner::RenderAnimation(){
    if(!Document()){ImGui::TextDisabled("開啟 UI Scene 以編輯 Clip。");return;}
    const auto found=Document()->Data().properties.find("animations");
    if(found==Document()->Data().properties.end()){
        ImGui::TextWrapped("此 Scene 尚未建立 Animation Library。建立後會同時產生 Default Clip、Entry 與 Default State。");
        if(ImGui::Button("建立 Animation Library")){
            ui::UIAnimationLibrary library;ui::AnimationClip clip;clip.id=Uuid::FromName(Document()->DocumentId().ToString()+"/animations/Default");clip.name="Default";clip.duration=.3f;
            const Uuid clipId=clip.id,state=Uuid::FromName(Document()->DocumentId().ToString()+"/animations/DefaultState");library.clips.push_back(std::move(clip));library.machine.entry=state;library.machine.states.push_back({state,"Default",clipId,{80,80}});
            const Status status=m_session->Commands().SetProperty(Document()->DocumentId(),"animations",ui::WriteUIAnimationLibrary(library),"Create Animation Library",DesignerDirtyFlags::Animation|DesignerDirtyFlags::Paint);if(!status)EmitStatus(status);else{m_session->timeline.selectedClip=clipId;MarkEdited();}
        }
        return;
    }
    const Variant before=found->second.Clone();auto parsed=ui::ParseUIAnimationLibrary(before,Document()->Path().generic_string());if(!parsed){EmitStatus(Status::Fail(parsed.Diagnostics()));widgets::InlineMessage(widgets::MessageKind::Error,"Animation Library 無法解析。");return;}auto library=parsed.TakeValue();
    if(library.clips.empty()){widgets::InlineMessage(widgets::MessageKind::Error,"Animation Library 沒有 Clip。");return;}
    auto selected=std::find_if(library.clips.begin(),library.clips.end(),[&](const ui::AnimationClip& clip){return clip.id==m_session->timeline.selectedClip;});if(selected==library.clips.end()){selected=library.clips.begin();m_session->timeline.selectedClip=selected->id;m_session->timeline.selectedTrack=m_session->timeline.selectedKey=-1;}
    bool changed=false,previewChanged=false;
    if(ImGui::BeginCombo("Clip",selected->name.c_str())){for(auto& clip:library.clips)if(ImGui::Selectable((clip.name+"##clip-"+clip.id.ToString()).c_str(),clip.id==selected->id)){m_session->timeline.selectedClip=clip.id;selected=std::find_if(library.clips.begin(),library.clips.end(),[&](const ui::AnimationClip& item){return item.id==m_session->timeline.selectedClip;});m_session->timeline.selectedTrack=m_session->timeline.selectedKey=-1;m_session->timeline.currentTime=0;previewChanged=true;}ImGui::EndCombo();}
    ImGui::SameLine();if(ImGui::Button("＋ Clip")){ui::AnimationClip clip;clip.id=Uuid::Random();clip.name="Clip "+std::to_string(library.clips.size()+1);clip.duration=.3f;m_session->timeline.selectedClip=clip.id;library.clips.push_back(std::move(clip));selected=std::prev(library.clips.end());changed=true;}
    ImGui::SetNextItemWidth(180);changed|=ImGui::InputText("Name",&selected->name);float duration=selected->duration;if(ImGui::DragFloat("Duration",&duration,.01f,0,120,"%.3fs")){selected->duration=std::max(0.0f,duration);m_session->timeline.currentTime=std::min(m_session->timeline.currentTime,selected->duration);changed=previewChanged=true;}changed|=ImGui::Checkbox("Loop",&selected->loop);
    if(ImGui::Button(m_session->timeline.playing?"Pause":"Play")){m_session->timeline.playing=!m_session->timeline.playing;previewChanged=true;}ImGui::SameLine();if(ImGui::Button("Stop")){m_session->timeline.playing=false;m_session->timeline.currentTime=0;previewChanged=true;}ImGui::SameLine();ImGui::Checkbox("Auto-key",&m_session->timeline.autoKey);
    if(m_session->timeline.playing){m_session->timeline.currentTime+=ImGui::GetIO().DeltaTime;if(m_session->timeline.currentTime>selected->duration){if(selected->loop&&selected->duration>0)m_session->timeline.currentTime=std::fmod(m_session->timeline.currentTime,selected->duration);else{m_session->timeline.currentTime=selected->duration;m_session->timeline.playing=false;}previewChanged=true;}}
    ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##clip-scrub",&m_session->timeline.currentTime,0,std::max(.001f,selected->duration),"%.3fs"))previewChanged=true;
    const auto animatable=[](const PropertyInfo& property){return property.animatable&&HasFlag(property.flags,PropertyFlags::Editable);};
    const auto propertiesFor=[&](const resource::NodeRecord& record){std::vector<const PropertyInfo*> result;std::unordered_set<std::string> seen;std::string type=record.type;while(const auto* info=TypeRegistry::Global().Find(type)){for(const auto& property:info->properties)if(animatable(property)&&seen.insert(property.name).second)result.push_back(&property);type=info->base;if(type.empty())break;}return result;};
    if(const auto* control=View().Find(*Document(),Selected())){const auto properties=propertiesFor(*control);if(!properties.empty()&&!TypeRegistry::Global().FindProperty(control->type,m_session->timeline.property))m_session->timeline.property=properties.front()->name;if(ImGui::BeginCombo("Track Property",m_session->timeline.property.c_str())){for(const auto* property:properties)if(ImGui::Selectable(property->name.c_str(),property->name==m_session->timeline.property))m_session->timeline.property=property->name;ImGui::EndCombo();}ImGui::SameLine();if(ImGui::Button("＋ Track")&&!m_session->timeline.property.empty()){auto current=Document()->ReadProperty(Selected(),m_session->timeline.property);const auto* descriptor=TypeRegistry::Global().FindProperty(control->type,m_session->timeline.property);ui::AnimationTrack track{.node=Selected(),.property=m_session->timeline.property};track.keys.push_back({m_session->timeline.currentTime,current&&current.Value().Type()!=VariantType::Null?current.Value():(descriptor?descriptor->defaultValue.Clone():Variant{}),ui::Ease::Linear,ui::KeyInterpolation::Linear});selected->tracks.push_back(std::move(track));m_session->timeline.selectedTrack=static_cast<int>(selected->tracks.size())-1;m_session->timeline.selectedKey=0;changed=previewChanged=true;}}
    ImGui::SeparatorText("Typed Tracks");
    for(int index=0;index<static_cast<int>(selected->tracks.size());++index){const auto& track=selected->tracks[static_cast<std::size_t>(index)];const auto* target=View().Find(*Document(),track.node);const std::string label=(target?target->name:track.node.ToString().substr(0,8))+" · "+track.property+"  ("+std::to_string(track.keys.size())+")";if(ImGui::Selectable((label+"##track-"+std::to_string(index)).c_str(),m_session->timeline.selectedTrack==index)){m_session->timeline.selectedTrack=index;m_session->timeline.selectedKey=-1;}}
    if(m_session->timeline.selectedTrack>=static_cast<int>(selected->tracks.size()))m_session->timeline.selectedTrack=selected->tracks.empty()?-1:static_cast<int>(selected->tracks.size()-1);
    if(m_session->timeline.selectedTrack>=0){auto& track=selected->tracks[static_cast<std::size_t>(m_session->timeline.selectedTrack)];if(ImGui::Button("＋ Key")){auto current=Document()->ReadProperty(track.node,track.property);track.keys.push_back({m_session->timeline.currentTime,current?current.Value():Variant{},ui::Ease::Linear,ui::KeyInterpolation::Linear});m_session->timeline.selectedKey=static_cast<int>(track.keys.size())-1;changed=previewChanged=true;}ImGui::SameLine();if(ImGui::Button("Delete Track")){selected->tracks.erase(selected->tracks.begin()+m_session->timeline.selectedTrack);m_session->timeline.selectedTrack=m_session->timeline.selectedKey=-1;changed=previewChanged=true;}else{for(int index=0;index<static_cast<int>(track.keys.size());++index){auto& key=track.keys[static_cast<std::size_t>(index)];ImGui::PushID(index);if(ImGui::Selectable((std::to_string(key.time)+"s##animation-key").c_str(),m_session->timeline.selectedKey==index)){m_session->timeline.selectedKey=index;m_session->timeline.currentTime=key.time;previewChanged=true;}ImGui::PopID();}if(m_session->timeline.selectedKey>=static_cast<int>(track.keys.size()))m_session->timeline.selectedKey=track.keys.empty()?-1:static_cast<int>(track.keys.size()-1);if(m_session->timeline.selectedKey>=0){auto& key=track.keys[static_cast<std::size_t>(m_session->timeline.selectedKey)];if(ImGui::DragFloat("Key Time",&key.time,.01f,0,selected->duration,"%.3fs")){key.time=std::clamp(key.time,0.0f,selected->duration);m_session->timeline.currentTime=key.time;changed=previewChanged=true;}ui::ActionArgumentDescriptor valueEditor{.name="value",.displayName="Value",.type=key.value.Type()};changed|=RenderActionArgument(valueEditor,key.value);int ease=static_cast<int>(key.ease);if(ImGui::Combo("Ease",&ease,"Linear\0Ease In\0Ease Out\0Ease In Out\0Step\0")){key.ease=static_cast<ui::Ease>(ease);changed=true;}bool discrete=key.interpolation==ui::KeyInterpolation::Discrete;if(ImGui::Checkbox("Discrete",&discrete)){key.interpolation=discrete?ui::KeyInterpolation::Discrete:ui::KeyInterpolation::Linear;changed=true;}if(ImGui::Button("Delete Key")){track.keys.erase(track.keys.begin()+m_session->timeline.selectedKey);m_session->timeline.selectedKey=-1;changed=previewChanged=true;}}}}
    if(changed){for(auto& track:selected->tracks)std::stable_sort(track.keys.begin(),track.keys.end(),[](const ui::AnimationKey& left,const ui::AnimationKey& right){return left.time<right.time;});const Status valid=library.Validate(Document()->Path().generic_string());if(!valid)EmitStatus(valid);else{const Status status=m_session->Commands().SetProperty(Document()->DocumentId(),"animations",ui::WriteUIAnimationLibrary(library),"Edit Animation Clip",DesignerDirtyFlags::Animation|DesignerDirtyFlags::Paint);if(!status)EmitStatus(status);else MarkEdited();}}
    if((previewChanged||m_session->timeline.playing)&&m_animationPreview){const Status status=m_animationPreview(m_session->timeline.selectedClip,m_session->timeline.currentTime,m_session->timeline.playing);if(!status)EmitStatus(status);}
}

void UIDesigner::RenderTheme(){
    if(!Document()){ImGui::TextDisabled("Open a UI scene to edit styles.");return;}
    ui::StyleThemeData theme;std::string source="Embedded styleSystem";bool foundTheme=false;
    if(const auto found=Document()->Data().properties.find("styleSystem");found!=Document()->Data().properties.end()){auto parsed=ui::ParseStyleTheme(found->second);if(parsed){theme=parsed.TakeValue();foundTheme=true;}else EmitStatus(Status::Fail(parsed.Diagnostics()));}
    if(!foundTheme)if(const auto found=Document()->Data().properties.find("theme");found!=Document()->Data().properties.end())if(const auto* reference=found->second.TryGet<ResourceRefValue>()){auto document=LoadReferencedUI(*reference);if(document){if(const auto style=document.Value().properties.find("styleSystem");style!=document.Value().properties.end()){auto parsed=ui::ParseStyleTheme(style->second);if(parsed){theme=parsed.TakeValue();foundTheme=true;source=reference->lastKnownPath;}else EmitStatus(Status::Fail(parsed.Diagnostics()));}}}
    ImGui::TextDisabled("Style System 3 · %s",source.c_str());if(!foundTheme)widgets::InlineMessage(widgets::MessageKind::Warning,"No styleSystem is available; local overrides still work.");
    auto* node=View().Find(*Document(),Selected());if(!node){ImGui::TextDisabled("Select a Control.");return;}
    const Variant before=node->properties.contains("styleBinding")?node->properties.at("styleBinding"):Variant{};ui::ControlStyleBinding binding;if(before.Type()!=VariantType::Null){auto parsed=ui::ParseStyleBinding(before);if(parsed)binding=parsed.TakeValue();else{EmitStatus(Status::Fail(parsed.Diagnostics()));return;}}const ui::ControlStyleBinding bindingBefore=binding;bool changed=false;
    ImGui::SeparatorText((node->name+" · "+node->type).c_str());const char* base="(none)";if(binding.baseStyle)if(const auto* style=theme.FindStyle(*binding.baseStyle))base=style->displayName.c_str();if(ImGui::BeginCombo("Base Style",base)){if(ImGui::Selectable("(none)",!binding.baseStyle)){binding.baseStyle.reset();changed=true;}for(const auto& style:theme.styles)if(ui::IsStyleCompatibleWith(style.compatibleTypes,node->type))if(ImGui::Selectable((style.displayName+"##base-"+style.id.ToString()).c_str(),binding.baseStyle&&*binding.baseStyle==style.id)){binding.baseStyle=style.id;changed=true;}ImGui::EndCombo();}
    if(ImGui::TreeNode("Applied Styles")){for(const auto& style:theme.styles)if(ui::IsStyleCompatibleWith(style.compatibleTypes,node->type)){bool selected=std::find(binding.appliedStyles.begin(),binding.appliedStyles.end(),style.id)!=binding.appliedStyles.end();if(ImGui::Checkbox((style.displayName+"##applied-"+style.id.ToString()).c_str(),&selected)){if(selected)binding.appliedStyles.push_back(style.id);else std::erase(binding.appliedStyles,style.id);changed=true;}}ImGui::TreePop();}
    for(const auto& axis:theme.variantAxes)if(ui::IsStyleCompatibleWith(axis.compatibleTypes,node->type)){const auto selected=binding.variants.contains(axis.id)?binding.variants.at(axis.id):axis.defaultValue;const auto* value=axis.FindValue(selected);if(ImGui::BeginCombo((axis.displayName+"##axis-"+axis.id.ToString()).c_str(),value?value->displayName.c_str():"Missing variant")){for(const auto& candidate:axis.values)if(ImGui::Selectable((candidate.displayName+"##variant-"+candidate.id.ToString()).c_str(),candidate.id==selected)){binding.variants[axis.id]=candidate.id;changed=true;}ImGui::EndCombo();}}
    ui::StylePropertyRegistry registry;const auto editStyleValue=[&](const ui::StylePropertyDescriptor& descriptor,ui::StyleValue& styleValue){bool edited=false;const char* kind=styleValue.IsTokenReference()?"Token":"Literal";if(ImGui::BeginCombo((descriptor.displayName+" source").c_str(),kind)){if(ImGui::Selectable("Literal",styleValue.IsLiteral())){styleValue=ui::StyleValue::Literal(DefaultValueFor(descriptor.valueType));edited=true;}if(ImGui::Selectable("Token",styleValue.IsTokenReference())&&!theme.tokens.empty()){const auto& token=theme.tokens.front();styleValue=ui::StyleValue::Token(token.id,token.displayName);edited=true;}ImGui::EndCombo();}if(styleValue.IsTokenReference()){const auto* current=theme.FindToken(styleValue.TokenReference());if(ImGui::BeginCombo(descriptor.displayName.c_str(),current?current->displayName.c_str():"Missing token")){for(const auto& token:theme.tokens)if(ui::IsStyleValueTypeCompatible(descriptor.valueType,token.type)&&ImGui::Selectable((token.displayName+"##token-"+token.id.ToString()).c_str(),token.id==styleValue.TokenReference())){styleValue=ui::StyleValue::Token(token.id,token.displayName);edited=true;}ImGui::EndCombo();}}else{Variant literal=styleValue.IsLiteral()?styleValue.LiteralValue().Clone():DefaultValueFor(descriptor.valueType);ui::ActionArgumentDescriptor argument{.name=descriptor.id,.displayName=descriptor.displayName,.type=descriptor.valueType};if(RenderActionArgument(argument,literal)){styleValue=ui::StyleValue::Literal(std::move(literal));edited=true;}}return edited;};
    ImGui::SeparatorText("Local Overrides");std::optional<ui::StylePropertyId> removeOverride;
    for(auto& [id,value]:binding.localOverrides){ImGui::PushID(id.c_str());if(const auto* descriptor=registry.Find(id);descriptor&&registry.RuntimeSupports(id,node->type))changed|=editStyleValue(*descriptor,value);else ImGui::TextColored(EditorTheme().colors.warning,"Unsupported by runtime: %s",id.c_str());ImGui::SameLine();if(ImGui::SmallButton("Remove"))removeOverride=id;ImGui::PopID();}
    if(removeOverride){binding.localOverrides.erase(*removeOverride);changed=true;}
    if(ImGui::Button("＋ Override"))ImGui::OpenPopup("AddStyleOverride");
    if(ImGui::BeginPopup("AddStyleOverride")){
        for(const auto* descriptor:registry.Descriptors())if(registry.RuntimeSupports(descriptor->id,node->type)&&!binding.localOverrides.contains(descriptor->id))if(ImGui::MenuItem((descriptor->category+" / "+descriptor->displayName+"##style-property-"+descriptor->id).c_str())){binding.localOverrides[descriptor->id]=ui::StyleValue::Literal(DefaultValueFor(descriptor->valueType));changed=true;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ui::StyleResolveRequest request{.controlType=node->type,.binding=binding};const auto resolved=ui::StyleResolver{}.Resolve(theme,request,registry);if(ImGui::TreeNode("Advanced · Resolved Source Trace")){if(resolved&&ImGui::BeginTable("##selected-style-trace",3,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)){ImGui::TableSetupColumn("Property");ImGui::TableSetupColumn("Source");ImGui::TableSetupColumn("Token chain");ImGui::TableHeadersRow();for(const auto& [id,value]:resolved.Value().properties){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(id.c_str());ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(value.source.label.c_str());ImGui::TableSetColumnIndex(2);ImGui::Text("%zu",value.tokenChain.size());}ImGui::EndTable();}else if(!resolved)for(const auto& diagnostic:resolved.Diagnostics())ImGui::TextColored(EditorTheme().colors.error,"%s %s",diagnostic.code.c_str(),diagnostic.message.c_str());ImGui::TreePop();}
    if(changed){DesignerDirtyFlags dirty=DesignerDirtyFlags::Theme|DesignerDirtyFlags::Paint;if(StyleBindingAffectsLayout(bindingBefore,binding))dirty|=DesignerDirtyFlags::Layout;const Status status=m_session->Commands().SetProperty(Selected(),"styleBinding",ui::WriteStyleBinding(binding),"Edit Control style binding",dirty);if(!status)EmitStatus(status);else MarkEdited();}
    ImGui::TextDisabled("Theme definitions are edited in the external .pxtheme document editor.");
}

void UIDesigner::RenderComponents(){
    if(!Document()){ImGui::TextDisabled("Open a UI scene to browse components.");return;}
    ComponentService components;components.SetLoader([this](const ResourceRefValue& reference){return LoadReferencedUI(reference);});
    const auto commitDocumentArray=[&](const char* property,Variant before,Variant after,const char* label){(void)before;const Status status=components.SetInterfaceDefinitions(*Document(),m_session->Commands(),property,std::move(after),label);if(!status)EmitStatus(status);else MarkEdited(true);};
    if(Document()->Data().type=="UIComponent"){
        ImGui::TextDisabled("Component public API · UI schema 5");auto* selected=View().Find(*Document(),Selected());if(!selected){ImGui::TextDisabled("Select a source Control.");return;}
        const auto editDefinitions=[&](const char* field,const char* title,const auto& candidates,const auto& addDefinition){
            Variant before=Document()->Data().properties.contains(field)?Document()->Data().properties.at(field).Clone():Variant(VariantArray{});
            Variant after=before.Clone();if(!after.AsArray())after=Variant(VariantArray{});auto* values=after.AsArray();bool changed=false;
            ImGui::SeparatorText(title);
            for(std::size_t index=0;index<values->size();++index){
                const auto* object=(*values)[index].AsObject();const auto id=object?object->find("id"):VariantObject::const_iterator{};const auto display=object?object->find("displayName"):VariantObject::const_iterator{};
                const std::string idText=object&&id!=object->end()&&id->second.TryGet<std::string>()?*id->second.TryGet<std::string>():"invalid";const std::string displayText=object&&display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():idText;
                ImGui::PushID((std::string(field)+std::to_string(index)).c_str());ImGui::Text("%s",displayText.c_str());ImGui::SameLine();ImGui::TextDisabled("%s",idText.c_str());ImGui::SameLine();
                if(ImGui::SmallButton("Remove")){values->erase(values->begin()+static_cast<std::ptrdiff_t>(index));changed=true;ImGui::PopID();break;}ImGui::PopID();
            }
            if(ImGui::BeginCombo((std::string("Expose##")+field).c_str(),"＋ Add")){
                for(std::size_t candidateIndex=0;candidateIndex<candidates.size();++candidateIndex){const auto& candidate=candidates[candidateIndex];if(ImGui::Selectable((candidate.first+"##candidate-"+std::to_string(candidateIndex)).c_str())){values->push_back(Variant(addDefinition(candidate)));changed=true;ImGui::CloseCurrentPopup();}}
                ImGui::EndCombo();
            }
            if(changed)commitDocumentArray(field,std::move(before),std::move(after),(std::string("Edit ")+title).c_str());
        };
        std::vector<std::pair<std::string,const PropertyInfo*>> properties;std::unordered_set<std::string> seen;std::string type=selected->type;while(const auto* info=TypeRegistry::Global().Find(type)){for(const auto& property:info->properties)if(HasFlag(property.flags,PropertyFlags::Editable)&&seen.insert(property.name).second)properties.push_back({property.editor.displayName.empty()?property.name:property.editor.displayName,&property});type=info->base;if(type.empty())break;}
        editDefinitions("component.exposedProperties","Exposed Properties",properties,[&](const auto& candidate){const auto* property=candidate.second;const std::string id=ComponentInterfaceId(selected->name+" "+property->name);return VariantObject{{"id",id},{"displayName",candidate.first},{"node",selected->id},{"property",property->name},{"type",std::string(ToString(property->type))}};});
        std::vector<std::pair<std::string,const SignalInfo*>> signals;for(const auto* signal:TypeRegistry::Global().SignalsForType(selected->type))signals.push_back({signal->displayName.empty()?signal->name:signal->displayName,signal});
        editDefinitions("component.exposedSignals","Exposed Signals",signals,[&](const auto& candidate){const auto* signal=candidate.second;const std::string id=ComponentInterfaceId(selected->name+" "+signal->name);return VariantObject{{"id",id},{"displayName",candidate.first},{"node",selected->id},{"signal",signal->name}};});
        std::vector<std::pair<std::string,int>> slotCandidate{{"Use selected Control",0}};editDefinitions("component.slots","Content Slots",slotCandidate,[&](const auto&){const std::string id=ComponentInterfaceId(selected->name+" content");return VariantObject{{"id",id},{"displayName",selected->name+" Content"},{"node",selected->id}};});
        ImGui::TextWrapped("Exposed IDs are stable English schema names. Rename the displayName in the typed definition when a localized label is needed.");return;
    }
    if(ImGui::Button("Create component from selection"))m_session->panels.createComponentOpen=true;
    ImGui::Separator();bool any=false;
    for(const auto& node:Document()->Data().nodes){if(node.type!="ComponentInstance")continue;any=true;
        std::string source="Missing source";if(const auto found=node.properties.find("component");found!=node.properties.end()){
            if(const auto* reference=found->second.TryGet<ResourceRefValue>())source=reference->lastKnownPath;}
        if(ImGui::Selectable((node.name+"##component-"+node.id.ToString()).c_str(),node.id==Selected())){
            MakePrimary(node.id);SelectOnly(node.id);}
        ImGui::SameLine();ImGui::TextDisabled("%s",source.c_str());}
    if(!any)ImGui::TextDisabled("No component instances in this scene.");
    auto* instance=View().Find(*Document(),Selected());if(!instance||instance->type!="ComponentInstance")return;const auto referenceIt=instance->properties.find("component");const auto* reference=referenceIt==instance->properties.end()?nullptr:referenceIt->second.TryGet<ResourceRefValue>();if(!reference)return;auto source=LoadReferencedUI(*reference);if(!source){EmitStatus(Status::Fail(source.Diagnostics()));return;}
    const auto definitions=[&](const char* field){const auto found=source.Value().properties.find(field);return found==source.Value().properties.end()?static_cast<const VariantArray*>(nullptr):found->second.AsArray();};
    if(const auto* exposed=definitions("component.exposedProperties")){ImGui::SeparatorText("Public Properties");Variant before=instance->properties.contains("componentProperties")?instance->properties.at("componentProperties").Clone():Variant(VariantObject{});Variant after=before.Clone();if(!after.AsObject())after=Variant(VariantObject{});auto* values=after.AsObject();bool changed=false;for(const auto& item:*exposed){const auto* object=item.AsObject();if(!object)continue;const auto id=object->find("id"),node=object->find("node"),property=object->find("property"),display=object->find("displayName");if(id==object->end()||node==object->end()||property==object->end()||!id->second.TryGet<std::string>()||!node->second.TryGet<Uuid>()||!property->second.TryGet<std::string>())continue;const auto sourceNode=std::find_if(source.Value().nodes.begin(),source.Value().nodes.end(),[&](const auto& candidate){return candidate.id==*node->second.TryGet<Uuid>();});if(sourceNode==source.Value().nodes.end())continue;const auto* descriptor=TypeRegistry::Global().FindProperty(sourceNode->type,*property->second.TryGet<std::string>());if(!descriptor)continue;const std::string& publicId=*id->second.TryGet<std::string>();if(!values->contains(publicId)){const auto current=sourceNode->properties.find(descriptor->name);(*values)[publicId]=current==sourceNode->properties.end()?descriptor->defaultValue.Clone():current->second.Clone();}ui::ActionArgumentDescriptor editor{.name=publicId,.displayName=display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():publicId,.type=descriptor->type};ImGui::PushID(publicId.c_str());changed|=RenderActionArgument(editor,(*values)[publicId]);ImGui::PopID();}if(changed){const Status status=components.SetInstanceInterface(*Document(),m_session->Commands(),instance->id,"componentProperties",std::move(after),"Edit exposed Component property");if(!status)EmitStatus(status);else MarkEdited(true);}}
    if(const auto* exposed=definitions("component.exposedSignals")){ImGui::SeparatorText("Public Signals");Variant before=instance->properties.contains("componentEvents")?instance->properties.at("componentEvents").Clone():Variant(VariantObject{});Variant after=before.Clone();if(!after.AsObject())after=Variant(VariantObject{});auto* bindings=after.AsObject();bool changed=false;for(const auto& item:*exposed){const auto* object=item.AsObject();if(!object)continue;const auto id=object->find("id"),display=object->find("displayName");if(id==object->end()||!id->second.TryGet<std::string>())continue;const std::string publicId=*id->second.TryGet<std::string>(),label=display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():publicId;ImGui::PushID(publicId.c_str());ImGui::TextUnformatted(label.c_str());auto bindingIt=bindings->find(publicId);auto* binding=bindingIt==bindings->end()?nullptr:bindingIt->second.AsObject();std::string action;if(binding)if(const auto found=binding->find("action");found!=binding->end()&&found->second.TryGet<std::string>())action=*found->second.TryGet<std::string>();if(ImGui::BeginCombo("Action",action.empty()?"(none)":action.c_str())){if(ImGui::Selectable("(none)",action.empty())){bindings->erase(publicId);changed=true;}for(const auto& descriptor:ui::ActionCatalog::Global().Descriptors())if(descriptor.available&&ImGui::Selectable((descriptor.category+" / "+descriptor.displayName+"##"+descriptor.id).c_str(),descriptor.id==action)){VariantObject arguments;for(const auto& argument:descriptor.arguments)arguments[argument.name]=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);(*bindings)[publicId]=VariantObject{{"kind",std::string("action")},{"action",descriptor.id},{"arguments",std::move(arguments)},{"reentry",std::string(ui::ActionReentryPolicyName(descriptor.reentryPolicy))}};binding=(*bindings)[publicId].AsObject();action=descriptor.id;changed=true;}ImGui::EndCombo();}if(binding&&!action.empty())if(const auto* descriptor=ui::ActionCatalog::Global().Find(action)){auto& arguments=(*binding)["arguments"];if(!arguments.AsObject())arguments=VariantObject{};for(const auto& argument:descriptor->arguments){auto& value=(*arguments.AsObject())[argument.name];if(value.Type()==VariantType::Null)value=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);changed|=RenderActionArgument(argument,value);}}ImGui::PopID();}if(changed){const Status status=components.SetInstanceInterface(*Document(),m_session->Commands(),instance->id,"componentEvents",std::move(after),"Bind exposed Component signal");if(!status)EmitStatus(status);else MarkEdited(true);}}
    if(const auto* slots=definitions("component.slots")){ImGui::SeparatorText("Slot Content");for(const Uuid& childId:View().Children(instance->id))if(auto* child=View().Find(*Document(),childId)){std::string current;if(const auto found=child->properties.find("componentSlot");found!=child->properties.end()&&found->second.TryGet<std::string>())current=*found->second.TryGet<std::string>();ImGui::PushID(child->id.ToString().c_str());if(ImGui::BeginCombo(child->name.c_str(),current.empty()?"(root)":current.c_str())){if(ImGui::Selectable("(root)",current.empty())){const Status status=components.AssignSlot(*Document(),m_session->Commands(),child->id,{});if(!status)EmitStatus(status);else MarkEdited(true);}for(const auto& item:*slots)if(const auto* object=item.AsObject()){const auto id=object->find("id"),display=object->find("displayName");if(id!=object->end()&&id->second.TryGet<std::string>()){const std::string slot=*id->second.TryGet<std::string>(),label=display!=object->end()&&display->second.TryGet<std::string>()?*display->second.TryGet<std::string>():slot;if(ImGui::Selectable((label+"##"+slot).c_str(),slot==current)){const Status status=components.AssignSlot(*Document(),m_session->Commands(),child->id,slot);if(!status)EmitStatus(status);else MarkEdited(true);}}}ImGui::EndCombo();}ImGui::PopID();}}
}

void UIDesigner::RenderInteractionNavigator(){
    if(!Document()){ImGui::TextDisabled("開啟 UI Scene 以編輯 Trigger。");return;}
    auto* selected=View().Find(*Document(),Selected());
    ImGui::SeparatorText("Selected Control");
    if(!selected){ImGui::TextDisabled("請先選取 Control。");}
    else{
        ImGui::Text("%s · %s",selected->name.c_str(),selected->type.c_str());
        const auto triggersIt=selected->properties.find("triggers");const auto* triggers=triggersIt==selected->properties.end()?nullptr:triggersIt->second.AsObject();
        for(const auto* signal:TypeRegistry::Global().SignalsForType(selected->type)){
            std::string state="未綁定";if(triggers)if(const auto binding=triggers->find(signal->name);binding!=triggers->end())if(const auto* object=binding->second.AsObject())if(const auto kind=object->find("kind");kind!=object->end()&&kind->second.TryGet<std::string>())state=*kind->second.TryGet<std::string>()=="flow"?"Flow":"Direct Action";
            const std::string label=(signal->displayName.empty()?signal->name:signal->displayName)+"  ["+state+"]##signal-"+signal->name;
            if(ImGui::Selectable(label.c_str(),m_session->selectedSignal==signal->name)){m_session->selectedSignal=signal->name;m_behaviorEditor->ClearSelection(m_session->behaviorGraph);}
        }
    }
    ImGui::SeparatorText("Scene Triggers");bool any=false;
    for(const auto& node:Document()->Data().nodes){const auto found=node.properties.find("triggers");const auto* triggers=found==node.properties.end()?nullptr:found->second.AsObject();if(!triggers)continue;for(const auto& [signal,value]:*triggers){const auto* binding=value.AsObject();const auto kind=binding?binding->find("kind"):VariantObject::const_iterator{};const std::string state=binding&&kind!=binding->end()&&kind->second.TryGet<std::string>()&&*kind->second.TryGet<std::string>()=="flow"?"Flow":"Direct Action";if(ImGui::Selectable((node.name+" · "+signal+"  ["+state+"]##scene-trigger-"+node.id.ToString()+signal).c_str(),node.id==Selected()&&signal==m_session->selectedSignal)){MakePrimary(node.id);SelectOnly(node.id);m_session->selectedSignal=signal;m_behaviorEditor->ClearSelection(m_session->behaviorGraph);}any=true;}}
    if(!any)ImGui::TextDisabled("Scene 尚未建立 Trigger binding。");
}

void UIDesigner::RenderEvents(){
    if(!Document()||Selected().Empty()){ImGui::TextDisabled("選取 Control 與 Signal 以設定 Trigger。");return;}
    auto* node=View().Find(*Document(),Selected());if(!node)return;const auto signals=TypeRegistry::Global().SignalsForType(node->type);if(signals.empty()){ImGui::TextDisabled("此 Control 沒有可綁定的 Signal。");return;}
    const auto signalIt=std::find_if(signals.begin(),signals.end(),[&](const SignalInfo* signal){return signal->name==m_session->selectedSignal;});const SignalInfo* signal=signalIt==signals.end()?signals.front():*signalIt;m_session->selectedSignal=signal->name;
    ImGui::TextDisabled("WHEN");ImGui::SameLine();ImGui::Text("%s · %s",node->name.c_str(),signal->displayName.empty()?signal->name.c_str():signal->displayName.c_str());if(!signal->description.empty())ImGui::TextWrapped("%s",signal->description.c_str());
    Variant before=node->properties.contains("triggers")?node->properties.at("triggers").Clone():Variant(VariantObject{});Variant after=before.Clone();if(!after.AsObject())after=Variant(VariantObject{});auto* triggers=after.AsObject();auto bindingIt=triggers->find(signal->name);auto* binding=bindingIt==triggers->end()?nullptr:bindingIt->second.AsObject();std::string kind,action,reentry="IgnoreWhileRunning";Uuid entry;if(binding){if(const auto found=binding->find("kind");found!=binding->end()&&found->second.TryGet<std::string>())kind=*found->second.TryGet<std::string>();if(const auto found=binding->find("action");found!=binding->end()&&found->second.TryGet<std::string>())action=*found->second.TryGet<std::string>();if(const auto found=binding->find("entry");found!=binding->end()&&found->second.TryGet<Uuid>())entry=*found->second.TryGet<Uuid>();if(const auto found=binding->find("reentry");found!=binding->end()&&found->second.TryGet<std::string>())reentry=*found->second.TryGet<std::string>();}
    const auto makeDirect=[&](const ui::ActionDescriptor& descriptor){VariantObject arguments;for(const auto& argument:descriptor.arguments)arguments[argument.name]=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);(*triggers)[signal->name]=VariantObject{{"kind",std::string("action")},{"action",descriptor.id},{"arguments",std::move(arguments)},{"reentry",std::string(ui::ActionReentryPolicyName(descriptor.reentryPolicy))}};};
    const auto makeEmptyDirect=[&]{(*triggers)[signal->name]=VariantObject{{"kind",std::string("action")},{"action",std::string{}},{"arguments",VariantObject{}},{"reentry",std::string("IgnoreWhileRunning")}};};
    const auto createFlow=[&](const bool includeAction){
        const Variant graphBefore=Document()->Data().properties.contains("interactionGraph")?Document()->Data().properties.at("interactionGraph").Clone():Variant{};ui::BehaviorGraph graph;if(graphBefore.Type()!=VariantType::Null){auto parsed=ui::ParseBehaviorGraph(graphBefore,Document()->Path().generic_string());if(!parsed){EmitStatus(Status::Fail(parsed.Diagnostics()));return false;}graph=parsed.TakeValue();}
        const Uuid entryId=Uuid::Random();graph.nodes.push_back({entryId,ui::BehaviorNodeKind::SignalEntry,{40,80},{{"control",node->id},{"signal",signal->name}}});
        if(includeAction&&!action.empty()){VariantObject arguments;if(binding)if(const auto found=binding->find("arguments");found!=binding->end()&&found->second.AsObject())arguments=*found->second.AsObject();const Uuid actionId=Uuid::Random();graph.nodes.push_back({actionId,ui::BehaviorNodeKind::Action,{360,80},{{"action",action},{"arguments",arguments},{"wait",true}}});graph.links.push_back({Uuid::Random(),entryId,"out",actionId,"in"});}
        (*triggers)[signal->name]=VariantObject{{"kind",std::string("flow")},{"entry",entryId},{"reentry",reentry}};
        auto command=std::make_unique<CompositeEditCommand>(includeAction?"Convert Direct Action to Flow":"Create visual Flow");command->Add(std::make_unique<PropertyChangeCommand>("Create Flow entry",Document()->DocumentId(),"interactionGraph",graphBefore,ui::WriteBehaviorGraph(graph),std::chrono::steady_clock::now(),false));command->Add(std::make_unique<PropertyChangeCommand>("Bind Trigger to Flow",node->id,"triggers",before,after,std::chrono::steady_clock::now(),false));DocumentChangeSet changes=DocumentChangeSet::Property(Document()->DocumentId(),"interactionGraph",DesignerDirtyFlags::Binding);changes.Merge(DocumentChangeSet::Property(node->id,"triggers",DesignerDirtyFlags::Binding));const Status status=m_session->Commands().Execute(std::move(command),std::move(changes));if(!status){EmitStatus(status);return false;}MarkEdited();m_behaviorEditor->FocusNode(m_session->behaviorGraph,entryId);m_session->viewport.authorMode=1;if(m_requestAuthorMode)m_requestAuthorMode(1);return true;
    };
    if(!binding){ImGui::Spacing();widgets::InlineMessage(widgets::MessageKind::Info,"基本互動使用 Direct Action；需要 Delay、Branch 或多步驟時再建立 Visual Flow。");if(widgets::ToolbarButton("選擇 Direct Action","從可用的 typed actions 中搜尋並設定",false,!ui::ActionCatalog::Global().Descriptors().empty(),"目前沒有可用 Action")){makeEmptyDirect();EditVariant("Create Direct Trigger","triggers",before,after,true,false);return;}ImGui::SameLine();if(widgets::ToolbarButton("進階：建立 Visual Flow","建立可加入分支與非同步步驟的 Flow")){(void)createFlow(false);return;}return;}
    if(kind=="flow"){
        ImGui::SeparatorText("DO  Visual Flow");ImGui::Text("Entry  %s",entry.Empty()?"Invalid":entry.ToString().c_str());if(ImGui::Button("在 Flow Graph 中開啟")){m_behaviorEditor->FocusNode(m_session->behaviorGraph,entry);if(m_requestAuthorMode)m_requestAuthorMode(1);}ImGui::SameLine();if(ImGui::Button("移除 Trigger")){triggers->erase(signal->name);EditVariant("Remove Flow Trigger","triggers",before,after,true,false);}return;
    }
    ImGui::SeparatorText("DO  Direct Action");widgets::SearchField("##action-filter","搜尋 Action 名稱、分類或 ID…",m_session->panels.actionFilter,sizeof(m_session->panels.actionFilter));const char* preview=action.empty()?"選擇 Action":action.c_str();bool changed=false;
    if(ImGui::BeginCombo("Action",preview)){const std::string filter=m_session->panels.actionFilter;for(const auto& descriptor:ui::ActionCatalog::Global().Descriptors()){if(!filter.empty()&&descriptor.id.find(filter)==std::string::npos&&descriptor.displayName.find(filter)==std::string::npos&&descriptor.category.find(filter)==std::string::npos)continue;ImGui::BeginDisabled(!descriptor.available);if(ImGui::Selectable((descriptor.category+" / "+descriptor.displayName+"##trigger-action-"+descriptor.id).c_str(),descriptor.id==action)){makeDirect(descriptor);binding=(*triggers)[signal->name].AsObject();action=descriptor.id;changed=true;}ImGui::EndDisabled();if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){ImGui::BeginTooltip();ImGui::Text("%s",descriptor.displayName.c_str());if(!descriptor.description.empty())ImGui::TextWrapped("%s",descriptor.description.c_str());ImGui::TextDisabled("%s",descriptor.id.c_str());if(!descriptor.available&&!descriptor.unavailableReason.empty())ImGui::TextColored(EditorTheme().colors.warning,"%s",descriptor.unavailableReason.c_str());ImGui::EndTooltip();}}ImGui::EndCombo();}
    if(binding&&!action.empty())if(const auto* descriptor=ui::ActionCatalog::Global().Find(action)){auto& arguments=(*binding)["arguments"];if(!arguments.AsObject())arguments=VariantObject{};for(const auto& argument:descriptor->arguments){auto& value=(*arguments.AsObject())[argument.name];if(value.Type()==VariantType::Null)value=argument.defaultValue?argument.defaultValue->Clone():DefaultValueFor(argument.type);changed|=RenderActionArgument(argument,value);}}
    if(binding){if(ImGui::BeginCombo("Reentry",reentry.c_str())){for(const char* option:{"Allow","IgnoreWhileRunning","Restart"})if(ImGui::Selectable(option,reentry==option)){(*binding)["reentry"]=std::string(option);reentry=option;changed=true;}ImGui::EndCombo();}}
    if(changed)EditVariant("Edit Direct Trigger","triggers",before,after,true,false);
    if(ImGui::Button("一鍵轉換成 Flow")){(void)createFlow(true);return;}ImGui::SameLine();if(ImGui::Button("移除 Trigger")){triggers->erase(signal->name);EditVariant("Remove Direct Trigger","triggers",before,after,true,false);}
}

void UIDesigner::RenderInteractionInspector(){if(!Document())return;if(!BehaviorGraphEditor::SelectedNode(m_session->behaviorGraph).Empty()){ImGui::TextDisabled("Flow Step Inspector");RenderBehaviorInspector();return;}ImGui::TextDisabled("Trigger Inspector");RenderEvents();}

void UIDesigner::RenderBehaviorGraph(){if(!Document()){ImGui::TextDisabled("Open a UI scene to edit behavior.");return;}if(m_behaviorDebugProvider)m_behaviorEditor->SetDebugState(m_behaviorDebugProvider());if(m_behaviorEditor->Render(*Document(),m_session->Commands(),m_session->behaviorGraph,Selected()))MarkEdited();}
void UIDesigner::RenderBehaviorInspector(){if(!Document())return;if(m_behaviorDebugProvider)m_behaviorEditor->SetDebugState(m_behaviorDebugProvider());if(m_behaviorEditor->RenderInspector(*Document(),m_session->Commands(),m_session->behaviorGraph,Selected()))MarkEdited();}
void UIDesigner::RenderAnimationNavigator(){if(!Document())return;if(m_animationDebugProvider)m_animationStateEditor->SetDebugState(m_animationDebugProvider());if(m_animationStateEditor->RenderNavigator(*Document(),m_session->Commands(),m_session->animationMachine))MarkEdited();}
void UIDesigner::RenderAnimationStateMachine(){if(!Document())return;if(m_animationDebugProvider)m_animationStateEditor->SetDebugState(m_animationDebugProvider());if(m_animationStateEditor->Render(*Document(),m_session->Commands(),m_session->animationMachine))MarkEdited();}
void UIDesigner::RenderAnimationInspector(){if(!Document())return;if(m_animationDebugProvider)m_animationStateEditor->SetDebugState(m_animationDebugProvider());if(m_animationStateEditor->RenderInspector(*Document(),m_session->Commands(),m_session->animationMachine))MarkEdited();}

void UIDesigner::RenderProblems(){
    if(!Document()){widgets::EmptyState("尚未開啟 UI 文件","Problems 會列出目前 UI 文件的驗證結果。");return;}
    DesignerDiagnostics diagnostics;DesignerDiagnostics::ValidationContext context;ComponentService components;components.SetLoader([this](const ResourceRefValue& reference){return LoadReferencedUI(reference);});context.components=&components;
    context.validateAction=[](std::string_view action,const VariantObject& arguments,const diag::Source& source){std::vector<diag::Diagnostic> result;const auto* descriptor=ui::ActionCatalog::Global().Find(action);if(!descriptor){result.push_back({.severity=diag::Severity::Error,.code="PXEDUIP5022",.category="Editor.UIDesigner",.message="Action is missing or its extension is disabled",.details=std::string(action),.source=source});return result;}if(!descriptor->available){result.push_back({.severity=diag::Severity::Warning,.code="PXEDUIP5023",.category="Editor.UIDesigner",.message="Action is currently unavailable",.details=descriptor->unavailableReason,.source=source});return result;}ui::ActionInvocation invocation{.action=std::string(action),.arguments=arguments};const auto checked=ui::ActionCatalog::Global().ValidateAndNormalize(invocation);for(auto item:checked.Diagnostics()){item.source=source;result.push_back(std::move(item));}return result;};
    diagnostics.Refresh(*Document(),context);
    if(diagnostics.ErrorCount())widgets::StatusChip((std::to_string(diagnostics.ErrorCount())+" errors").c_str(),widgets::MessageKind::Error);else widgets::StatusChip("0 errors",widgets::MessageKind::Success);ImGui::SameLine();widgets::StatusChip((std::to_string(diagnostics.WarningCount())+" warnings").c_str(),diagnostics.WarningCount()?widgets::MessageKind::Warning:widgets::MessageKind::Success);
    ImGui::Separator();if(diagnostics.Items().empty()){widgets::EmptyState("沒有 Designer 問題","目前文件通過結構、Action、Component 與屬性驗證。");return;}
    for(const auto& item:diagnostics.Items()){
        const ImVec4 color=item.severity>=diag::Severity::Error?EditorTheme().colors.error:EditorTheme().colors.warning;
        if(ImGui::Selectable((item.code+"  "+item.message+"##problem-"+item.code+item.source.nodeId).c_str())){
            if(const auto id=Uuid::Parse(item.source.nodeId);id&&View().Contains(*id)){MakePrimary(*id);SelectOnly(*id);}}
        if(ImGui::IsItemHovered()&&!item.details.empty())ImGui::SetTooltip("%s",item.details.c_str());
        ImGui::SameLine();ImGui::TextColored(color,"%s",item.source.property.c_str());
    }
}

}  // namespace px::editor
