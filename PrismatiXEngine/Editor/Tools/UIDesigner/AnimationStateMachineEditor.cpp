#include "Editor/Tools/UIDesigner/AnimationStateMachineEditor.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <imgui.h>
#include <imgui_node_editor.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ed = ax::NodeEditor;

namespace px::editor {
namespace {

std::uintptr_t StableId(const Uuid& id,const std::string_view suffix={}){
    const Uuid stable=suffix.empty()?id:Uuid::FromName(id.ToString()+"/"+std::string(suffix));
    const auto value=static_cast<std::uintptr_t>(UuidHash{}(stable));return value?value:1;
}
ed::NodeId StateNode(const Uuid& id){return ed::NodeId(StableId(id,"animation-state"));}
ed::PinId StateInput(const Uuid& id){return ed::PinId(StableId(id,"animation-in"));}
ed::PinId StateOutput(const Uuid& id){return ed::PinId(StableId(id,"animation-out"));}
ed::LinkId TransitionLink(const Uuid& id){return ed::LinkId(StableId(id,"animation-transition"));}

Variant AnimationBefore(const UISceneDocument& document){const auto found=document.Data().properties.find("animations");return found==document.Data().properties.end()?Variant{}:found->second.Clone();}
Result<ui::UIAnimationLibrary> ReadLibrary(const UISceneDocument& document){const Variant value=AnimationBefore(document);if(value.Type()==VariantType::Null)return Result<ui::UIAnimationLibrary>::Failure(diag::Diagnostic{.severity=diag::Severity::Error,.code="PXEDUIAN1001",.category="Editor.UIDesigner",.message="Scene has no Animation Library"});return ui::ParseUIAnimationLibrary(value,document.Path().generic_string());}
bool Commit(UISceneDocument& document,DesignerCommandService& commands,const Variant& before,const ui::UIAnimationLibrary& library,const char* label){const Status valid=library.Validate(document.Path().generic_string());if(!valid){for(const auto& diagnostic:valid.Diagnostics())diag::Emit(diagnostic);return false;}auto command=std::make_unique<PropertyChangeCommand>(label,document.DocumentId(),"animations",before,ui::WriteUIAnimationLibrary(library),std::chrono::steady_clock::now(),false);const Status status=commands.Execute(std::move(command),DocumentChangeSet::Property(document.DocumentId(),"animations",DesignerDirtyFlags::Animation));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);return static_cast<bool>(status);}
std::string UniqueName(const auto& values,const std::string& base){for(int suffix=1;suffix<10000;++suffix){const std::string candidate=suffix==1?base:base+" "+std::to_string(suffix);if(std::none_of(values.begin(),values.end(),[&](const auto& value){return value.name==candidate;}))return candidate;}return base+" Copy";}
const char* ParameterTypeName(const ui::AnimationParameterType type){switch(type){case ui::AnimationParameterType::Trigger:return "Trigger";case ui::AnimationParameterType::Bool:return "Bool";case ui::AnimationParameterType::Number:return "Number";}return "Trigger";}
const char* OperatorName(const ui::AnimationConditionOperator operation){switch(operation){case ui::AnimationConditionOperator::Triggered:return "Triggered";case ui::AnimationConditionOperator::Equal:return "==";case ui::AnimationConditionOperator::NotEqual:return "!=";case ui::AnimationConditionOperator::Less:return "<";case ui::AnimationConditionOperator::LessEqual:return "<=";case ui::AnimationConditionOperator::Greater:return ">";case ui::AnimationConditionOperator::GreaterEqual:return ">=";}return "Triggered";}

}  // namespace

AnimationStateMachineEditor::AnimationStateMachineEditor(){EnsureContext();}
AnimationStateMachineEditor::~AnimationStateMachineEditor(){if(m_context)ed::DestroyEditor(m_context);}
void AnimationStateMachineEditor::EnsureContext(){if(m_context)return;ed::Config config;config.SettingsFile=nullptr;m_context=ed::CreateEditor(&config);}

bool AnimationStateMachineEditor::Render(UISceneDocument& document,DesignerCommandService& commands,DesignerAnimationMachineState& viewState){
    auto parsed=ReadLibrary(document);
    if(!parsed){ImGui::TextWrapped("先在 Clip 工作面建立 Animation Library。");return false;}
    auto library=parsed.TakeValue();
    const Variant before=AnimationBefore(document);
    bool dirty=false;
    EnsureContext();

    if(ImGui::Button("＋ State")){
        ui::AnimationClip clip;
        clip.id=Uuid::Random();
        clip.name=UniqueName(library.clips,"Clip");
        clip.duration=.3f;
        const Uuid clipId=clip.id;
        const Uuid stateId=Uuid::Random();
        library.clips.push_back(std::move(clip));
        library.machine.states.push_back({stateId,UniqueName(library.machine.states,"State"),clipId,{100.0f+40.0f*static_cast<float>(library.machine.states.size()),100.0f}});
        if(library.machine.entry.Empty())library.machine.entry=stateId;
        viewState.selectedState=stateId;
        viewState.selectedTransition={};
        dirty=true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("拖曳圓點建立 Transition；Any State 優先於目前 State。");

    const Uuid anyState=Uuid::FromName(document.DocumentId().ToString()+"/animations/AnyState");
    struct Pin {Uuid state;bool input=false;bool any=false;};
    std::unordered_map<std::uintptr_t,Pin> pins;
    ed::SetCurrentEditor(m_context);
    ed::Begin("##animation-state-machine",{0,0});

    if(m_initializedNodes.insert(anyState).second)ed::SetNodePosition(StateNode(anyState),{30,30});
    ed::PushStyleColor(ed::StyleColor_NodeBg,ImColor(80,48,88,250));
    ed::BeginNode(StateNode(anyState));
    ImGui::TextColored({.92f,.72f,1,1},"ANY STATE");
    ed::BeginPin(StateOutput(anyState),ed::PinKind::Output);
    ImGui::TextColored({.92f,.72f,1,1},"Transition  ●");
    ed::EndPin();
    pins.emplace(StateOutput(anyState).Get(),Pin{{},false,true});
    ed::EndNode();
    ed::PopStyleColor();

    for(const auto& state:library.machine.states){
        if(m_initializedNodes.insert(state.id).second)ed::SetNodePosition(StateNode(state.id),{state.position.x,state.position.y});
        const bool active=m_debugState.state==state.id;
        ed::PushStyleColor(ed::StyleColor_NodeBg,active?ImColor(34,105,72,250):ImColor(24,31,44,250));
        ed::BeginNode(StateNode(state.id));
        ed::BeginPin(StateInput(state.id),ed::PinKind::Input);
        ImGui::TextColored({.55f,.75f,1,1},"●  In");
        ed::EndPin();
        ImGui::SameLine();
        if(state.id==library.machine.entry)ImGui::TextColored({1,.75f,.25f,1},"ENTRY");
        else if(active)ImGui::TextColored({.35f,1,.58f,1},"ACTIVE");
        ImGui::Text("%s",state.name.c_str());
        const auto* clip=library.FindClip(state.clip);
        ImGui::TextDisabled("Clip · %s",clip?clip->name.c_str():"Missing");
        ed::BeginPin(StateOutput(state.id),ed::PinKind::Output);
        ImGui::TextColored({1,.66f,.34f,1},"Transition  ●");
        ed::EndPin();
        pins.emplace(StateInput(state.id).Get(),Pin{state.id,true,false});
        pins.emplace(StateOutput(state.id).Get(),Pin{state.id,false,false});
        ed::EndNode();
        ed::PopStyleColor();
    }
    for(const auto& transition:library.machine.transitions){
        const ImColor color=m_debugState.transition==transition.id?ImColor(80,255,145):ImColor(244,161,76);
        ed::Link(TransitionLink(transition.id),transition.from?StateOutput(*transition.from):StateOutput(anyState),StateInput(transition.to),color,m_debugState.transition==transition.id?4.0f:2.0f);
    }

    if(ed::BeginCreate()){
        ed::PinId start=0,end=0;
        if(ed::QueryNewLink(&start,&end)){
            auto from=pins.find(start.Get()),to=pins.find(end.Get());
            if(from!=pins.end()&&to!=pins.end()){
                Pin source=from->second,target=to->second;
                if(source.input){std::swap(source,target);std::swap(start,end);}
                if(!source.input&&target.input&&!target.any&&source.state!=target.state){
                    if(ed::AcceptNewItem()){
                        ui::AnimationTransition transition;
                        transition.id=Uuid::Random();
                        if(!source.any)transition.from=source.state;
                        transition.to=target.state;
                        transition.duration=.15f;
                        library.machine.transitions.push_back(transition);
                        viewState.selectedTransition=transition.id;
                        viewState.selectedState={};
                        dirty=true;
                    }
                }else ed::RejectNewItem(ImColor(255,80,80),2);
            }
        }
    }
    ed::EndCreate();

    if(ed::BeginDelete()){
        ed::LinkId link=0;
        while(ed::QueryDeletedLink(&link)){
            if(!ed::AcceptDeletedItem())continue;
            const auto found=std::find_if(library.machine.transitions.begin(),library.machine.transitions.end(),[&](const ui::AnimationTransition& transition){return TransitionLink(transition.id)==link;});
            if(found!=library.machine.transitions.end()){library.machine.transitions.erase(found);viewState.selectedTransition={};dirty=true;}
        }
        ed::NodeId node=0;
        while(ed::QueryDeletedNode(&node)){
            const auto found=std::find_if(library.machine.states.begin(),library.machine.states.end(),[&](const ui::AnimationState& state){return StateNode(state.id)==node;});
            if(found!=library.machine.states.end()&&found->id!=library.machine.entry&&ed::AcceptDeletedItem()){
                const Uuid id=found->id;
                library.machine.states.erase(found);
                std::erase_if(library.machine.transitions,[&](const ui::AnimationTransition& transition){return transition.to==id||(transition.from&&*transition.from==id);});
                m_initializedNodes.erase(id);
                viewState.selectedState={};
                dirty=true;
            }else ed::RejectDeletedItem();
        }
    }
    ed::EndDelete();

    const int selectedCount=ed::GetSelectedObjectCount();
    if(selectedCount>0){
        std::vector<ed::NodeId> nodes(static_cast<std::size_t>(selectedCount));
        if(ed::GetSelectedNodes(nodes.data(),selectedCount)>0){
            const auto selectedState=std::find_if(library.machine.states.begin(),library.machine.states.end(),[&](const ui::AnimationState& value){return StateNode(value.id)==nodes.front();});
            if(selectedState!=library.machine.states.end()){viewState.selectedState=selectedState->id;viewState.selectedTransition={};}
        }
        std::vector<ed::LinkId> links(static_cast<std::size_t>(selectedCount));
        if(ed::GetSelectedLinks(links.data(),selectedCount)>0){
            const auto transition=std::find_if(library.machine.transitions.begin(),library.machine.transitions.end(),[&](const ui::AnimationTransition& value){return TransitionLink(value.id)==links.front();});
            if(transition!=library.machine.transitions.end()){viewState.selectedTransition=transition->id;viewState.selectedState={};}
        }
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        for(auto& state:library.machine.states){
            const ImVec2 position=ed::GetNodePosition(StateNode(state.id));
            if(std::abs(position.x-state.position.x)>.5f||std::abs(position.y-state.position.y)>.5f){state.position={position.x,position.y};dirty=true;}
        }
    }
    ed::End();
    ed::SetCurrentEditor(nullptr);
    return dirty&&Commit(document,commands,before,library,"Edit Animation State Machine");
}

bool AnimationStateMachineEditor::RenderNavigator(UISceneDocument& document,DesignerCommandService& commands,DesignerAnimationMachineState& viewState){
    auto parsed=ReadLibrary(document);if(!parsed){ImGui::TextDisabled("尚無 Animation Library。");return false;}auto library=parsed.TakeValue();const Variant before=AnimationBefore(document);bool changed=false;
    if(!m_debugState.transition.Empty())ImGui::TextColored(ImVec4(.3f,.8f,1,.9f),"Transition · %.0f%%",std::clamp(m_debugState.transitionProgress,0.0f,1.0f)*100.0f);
    ImGui::SeparatorText("States");for(const auto& item:library.machine.states){const bool active=m_debugState.state==item.id;const std::string label=(item.id==library.machine.entry?"▶ ":active?"● ":"")+item.name+"##state-nav-"+item.id.ToString();if(ImGui::Selectable(label.c_str(),viewState.selectedState==item.id)){viewState.selectedState=item.id;viewState.selectedTransition={};}}
    ImGui::SeparatorText("Clips");for(const auto& clip:library.clips)ImGui::BulletText("%s · %.2fs%s",clip.name.c_str(),clip.duration,clip.loop?" · Loop":"");
    ImGui::SeparatorText("Parameters");
    for(std::size_t parameterIndex=0;parameterIndex<library.machine.parameters.size();++parameterIndex){
        auto& parameter=library.machine.parameters[parameterIndex];
        ImGui::PushID(static_cast<int>(parameterIndex));
        ImGui::Text("%s",parameter.name.c_str());ImGui::SameLine();ImGui::TextDisabled("%s",ParameterTypeName(parameter.type));
        const auto live=m_debugState.parameters.find(parameter.name);
        if(parameter.type==ui::AnimationParameterType::Trigger){
            ImGui::SameLine();ImGui::BeginDisabled(!m_parameterTester);if(ImGui::SmallButton("Fire")&&m_parameterTester){const Status status=m_parameterTester(parameter.name,true);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}ImGui::EndDisabled();
        }else if(parameter.type==ui::AnimationParameterType::Bool){
            if(auto* value=parameter.defaultValue.TryGet<bool>())changed|=ImGui::Checkbox("Default",value);
            bool test=live!=m_debugState.parameters.end()&&live->second.TryGet<bool>()?*live->second.TryGet<bool>():false;
            ImGui::SameLine();ImGui::BeginDisabled(!m_parameterTester);if(ImGui::Checkbox("Live",&test)&&m_parameterTester){const Status status=m_parameterTester(parameter.name,test);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}ImGui::EndDisabled();
        }else if(parameter.type==ui::AnimationParameterType::Number){
            if(auto* value=parameter.defaultValue.TryGet<double>()){float number=static_cast<float>(*value);if(ImGui::DragFloat("Default",&number,.05f)){*value=number;changed=true;}}
            double liveNumber=0.0;if(live!=m_debugState.parameters.end()){if(const auto* numberValue=live->second.TryGet<double>())liveNumber=*numberValue;else if(const auto* integerValue=live->second.TryGet<std::int64_t>())liveNumber=static_cast<double>(*integerValue);}
            float test=static_cast<float>(liveNumber);ImGui::BeginDisabled(!m_parameterTester);if(ImGui::DragFloat("Live",&test,.05f)&&m_parameterTester){const Status status=m_parameterTester(parameter.name,static_cast<double>(test));if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if(ImGui::Button("＋ Trigger")){library.machine.parameters.push_back({UniqueName(library.machine.parameters,"Trigger"),ui::AnimationParameterType::Trigger,false});changed=true;}ImGui::SameLine();if(ImGui::Button("＋ Bool")){library.machine.parameters.push_back({UniqueName(library.machine.parameters,"Bool"),ui::AnimationParameterType::Bool,false});changed=true;}ImGui::SameLine();if(ImGui::Button("＋ Number")){library.machine.parameters.push_back({UniqueName(library.machine.parameters,"Number"),ui::AnimationParameterType::Number,0.0});changed=true;}
    return changed&&Commit(document,commands,before,library,"Edit Animation Parameters");
}

bool AnimationStateMachineEditor::RenderInspector(UISceneDocument& document,DesignerCommandService& commands,DesignerAnimationMachineState& viewState){
    auto parsed=ReadLibrary(document);
    if(!parsed)return false;
    auto library=parsed.TakeValue();
    const Variant before=AnimationBefore(document);
    bool changed=false;

    if(!viewState.selectedTransition.Empty()){
        auto transitionIt=std::find_if(library.machine.transitions.begin(),library.machine.transitions.end(),[&](const ui::AnimationTransition& value){return value.id==viewState.selectedTransition;});
        if(transitionIt==library.machine.transitions.end()){viewState.selectedTransition={};return false;}
        auto& transition=*transitionIt;
        ImGui::TextDisabled("Transition Inspector");
        const auto* fromState=transition.from?library.machine.FindState(*transition.from):nullptr;
        const auto* toState=library.machine.FindState(transition.to);
        ImGui::Text("%s  →  %s",transition.from?(fromState?fromState->name.c_str():"Missing"):"Any State",toState?toState->name.c_str():"Missing");
        if(ImGui::BeginCombo("Target",toState?toState->name.c_str():"Missing")){
            for(const auto& state:library.machine.states){
                if(ImGui::Selectable((state.name+"##target-"+state.id.ToString()).c_str(),state.id==transition.to)){transition.to=state.id;changed=true;}
            }
            ImGui::EndCombo();
        }
        changed|=ImGui::Checkbox("Has Exit Time",&transition.hasExitTime);
        if(transition.hasExitTime)changed|=ImGui::SliderFloat("Exit Time",&transition.exitTime,0,1,"%.3f");
        changed|=ImGui::DragFloat("Transition Duration",&transition.duration,.01f,0,10,"%.3fs");
        changed|=ImGui::DragInt("Priority",&transition.priority);

        ImGui::SeparatorText("Conditions · AND");
        for(std::size_t index=0;index<transition.conditions.size();++index){
            auto& condition=transition.conditions[index];
            ImGui::PushID(static_cast<int>(index));
            if(ImGui::BeginCombo("Parameter",condition.parameter.c_str())){
                for(std::size_t candidateIndex=0;candidateIndex<library.machine.parameters.size();++candidateIndex){
                    const auto& candidate=library.machine.parameters[candidateIndex];
                    if(ImGui::Selectable((candidate.name+"##parameter-"+std::to_string(candidateIndex)).c_str(),candidate.name==condition.parameter)){
                        condition.parameter=candidate.name;
                        condition.operation=candidate.type==ui::AnimationParameterType::Trigger?ui::AnimationConditionOperator::Triggered:ui::AnimationConditionOperator::Equal;
                        condition.value=candidate.type==ui::AnimationParameterType::Number?Variant(0.0):Variant(false);
                        changed=true;
                    }
                }
                ImGui::EndCombo();
            }
            const auto parameterIt=std::find_if(library.machine.parameters.begin(),library.machine.parameters.end(),[&](const ui::AnimationParameter& value){return value.name==condition.parameter;});
            if(parameterIt!=library.machine.parameters.end()){
                if(parameterIt->type==ui::AnimationParameterType::Bool){
                    if(ImGui::BeginCombo("Operator",OperatorName(condition.operation))){
                        for(const auto operation:{ui::AnimationConditionOperator::Equal,ui::AnimationConditionOperator::NotEqual}){
                            if(ImGui::Selectable(OperatorName(operation),condition.operation==operation)){condition.operation=operation;changed=true;}
                        }
                        ImGui::EndCombo();
                    }
                    if(auto* value=condition.value.TryGet<bool>())changed|=ImGui::Checkbox("Value",value);
                }else if(parameterIt->type==ui::AnimationParameterType::Number){
                    if(ImGui::BeginCombo("Operator",OperatorName(condition.operation))){
                        for(const auto operation:{ui::AnimationConditionOperator::Equal,ui::AnimationConditionOperator::NotEqual,ui::AnimationConditionOperator::Less,ui::AnimationConditionOperator::LessEqual,ui::AnimationConditionOperator::Greater,ui::AnimationConditionOperator::GreaterEqual}){
                            if(ImGui::Selectable(OperatorName(operation),condition.operation==operation)){condition.operation=operation;changed=true;}
                        }
                        ImGui::EndCombo();
                    }
                    if(auto* value=condition.value.TryGet<double>()){
                        float number=static_cast<float>(*value);
                        if(ImGui::DragFloat("Value",&number,.05f)){*value=number;changed=true;}
                    }
                }else ImGui::TextDisabled("Trigger 在成功轉場時才消耗。");
            }
            if(ImGui::Button("Remove Condition")){
                transition.conditions.erase(transition.conditions.begin()+static_cast<std::ptrdiff_t>(index));
                changed=true;
                ImGui::PopID();
                break;
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::BeginDisabled(library.machine.parameters.empty());
        if(ImGui::Button("＋ Condition")&&!library.machine.parameters.empty()){
            const auto& firstParameter=library.machine.parameters.front();
            transition.conditions.push_back({firstParameter.name,firstParameter.type==ui::AnimationParameterType::Trigger?ui::AnimationConditionOperator::Triggered:ui::AnimationConditionOperator::Equal,firstParameter.type==ui::AnimationParameterType::Number?Variant(0.0):Variant(false)});
            changed=true;
        }
        ImGui::EndDisabled();
        if(ImGui::Button("Delete Transition")){library.machine.transitions.erase(transitionIt);viewState.selectedTransition={};changed=true;}
    }else if(!viewState.selectedState.Empty()){
        auto stateIt=std::find_if(library.machine.states.begin(),library.machine.states.end(),[&](const ui::AnimationState& value){return value.id==viewState.selectedState;});
        if(stateIt==library.machine.states.end()){viewState.selectedState={};return false;}
        auto& state=*stateIt;
        ImGui::TextDisabled("State Inspector");
        changed|=ImGui::InputText("Name",&state.name);
        const auto* clip=library.FindClip(state.clip);
        if(ImGui::BeginCombo("Clip",clip?clip->name.c_str():"Missing")){
            for(const auto& candidate:library.clips){
                if(ImGui::Selectable((candidate.name+"##clip-"+candidate.id.ToString()).c_str(),candidate.id==state.clip)){state.clip=candidate.id;changed=true;}
            }
            ImGui::EndCombo();
        }
        if(state.id==library.machine.entry)ImGui::TextColored({1,.75f,.25f,1},"Entry State");
        else if(ImGui::Button("Set as Entry")){library.machine.entry=state.id;changed=true;}
        if(m_debugState.state==state.id){
            ImGui::SeparatorText("Runtime");
            ImGui::TextColored({.35f,1,.58f,1},"Active · %.3fs",m_debugState.position);
            if(!m_debugState.transition.Empty())ImGui::Text("Transition %.0f%%",m_debugState.transitionProgress*100);
        }
    }else ImGui::TextDisabled("選取 State 或 Transition。");

    if(!m_debugState.parameters.empty()&&ImGui::CollapsingHeader("Runtime Parameters")){
        for(const auto& [name,value]:m_debugState.parameters)ImGui::BulletText("%s · %s",name.c_str(),ToString(value.Type()));
    }
    return changed&&Commit(document,commands,before,library,"Edit Animation State Machine");
}

}  // namespace px::editor
