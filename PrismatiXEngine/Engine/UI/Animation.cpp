#include "Engine/UI/Animation.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace px::ui {
namespace {
diag::Diagnostic AnimationError(std::string code, std::string message, const AnimationTrack* track = nullptr) {
    diag::Diagnostic d{.severity = diag::Severity::Error, .code = std::move(code),
                       .category = "UI.Animation", .message = std::move(message)};
    if (track) { d.source.nodeId = track->node.ToString(); d.source.property = track->property; }
    diag::Emit(d); return d;
}
float EaseValue(float value, Ease ease) {
    value = std::clamp(value, 0.0f, 1.0f);
    switch (ease) {
        case Ease::EaseIn: return value * value;
        case Ease::EaseOut: return 1.0f - (1.0f - value) * (1.0f - value);
        case Ease::EaseInOut: return value < 0.5f ? 2.0f * value * value : 1.0f - std::pow(-2.0f * value + 2.0f, 2.0f) * 0.5f;
        case Ease::Step: return 0.0f;
        default: return value;
    }
}
Result<Variant> Interpolate(const Variant& a, const Variant& b, float t) {
    if (a.Type() != b.Type()) return Result<Variant>::Failure(AnimationError("PXUI2701", "Animation key types do not match"));
    switch (a.Type()) {
        case VariantType::Number: {
            const double x = *a.TryGet<double>(), y = *b.TryGet<double>();
            return Result<Variant>::Success(Variant(x + (y - x) * t));
        }
        case VariantType::Vec2: {
            const Vec2 x = *a.TryGet<Vec2>(), y = *b.TryGet<Vec2>();
            return Result<Variant>::Success(Variant(Vec2{x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t}));
        }
        case VariantType::Rect: {
            const Rect x = *a.TryGet<Rect>(), y = *b.TryGet<Rect>();
            return Result<Variant>::Success(Variant(Rect{x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                x.w + (y.w - x.w) * t, x.h + (y.h - x.h) * t}));
        }
        case VariantType::Color: {
            const Color x = *a.TryGet<Color>(), y = *b.TryGet<Color>();
            auto channel = [t](std::uint8_t p, std::uint8_t q) { return static_cast<std::uint8_t>(std::clamp(std::lround(static_cast<float>(p) + (static_cast<float>(q) - p) * t), 0l, 255l)); };
            return Result<Variant>::Success(Variant(Color{channel(x.r,y.r), channel(x.g,y.g), channel(x.b,y.b), channel(x.a,y.a)}));
        }
        default: return Result<Variant>::Success(t < 1.0f ? a : b);
    }
}
}

Status AnimationClip::Validate() const {
    if (duration < 0.0f) return Status::Fail(AnimationError("PXUI2702", "Animation duration cannot be negative"));
    for (const auto& track : tracks) {
        if (track.node.Empty() || track.property.empty() || track.keys.empty())
            return Status::Fail(AnimationError("PXUI2703", "Animation track is incomplete", &track));
        float previous = -1.0f;
        VariantType type = track.keys.front().value.Type();
        for (const auto& key : track.keys) {
            if (key.time < previous || key.time < 0.0f || key.time > duration || key.value.Type() != type)
                return Status::Fail(AnimationError("PXUI2704", "Animation keys must be ordered and use one type", &track));
            previous = key.time;
        }
    }
    return Status::Ok();
}

Status AnimationPlayer::Play(const AnimationClip& clip, float blendSeconds) {
    const Status valid = clip.Validate(); if (!valid) return valid;
    m_blendFrom.clear();
    if (m_clip && blendSeconds > 0.0f) {
        for (const auto& track : clip.tracks) {
            auto* node=m_root.Find(track.node);
            const auto* property=node?TypeRegistry::Global().FindProperty(std::string(node->TypeName()),track.property):nullptr;
            if(node&&property&&property->get)m_blendFrom.push_back({track.node,track.property,property->get(*node)});
        }
    }
    for (const auto& track : clip.tracks) {
        auto* node = m_root.Find(track.node);
        if (!node) return Status::Fail(AnimationError("PXUI2706", "Animation target node does not exist", &track));
        const auto* property = TypeRegistry::Global().FindProperty(std::string(node->TypeName()), track.property);
        if (!property || !property->get || !property->set ||
            !property->animatable)
            return Status::Fail(AnimationError("PXUI2707", "Animation target property is not animatable Runtime metadata", &track));
        const bool captured=std::any_of(m_originals.begin(),m_originals.end(),[&](const OriginalValue& value){return value.node==track.node&&value.property==track.property;});
        if(!captured)m_originals.push_back({track.node, track.property, property->get(*node)});
    }
    m_clip = &clip;m_position = 0.0f;m_paused = false;m_finished=false;
    m_blendDuration=std::max(0.0f,blendSeconds);m_blendElapsed=0.0f;
    return Apply();
}
Status AnimationPlayer::Pause() {
    if (!m_clip) return Status::Fail(AnimationError("PXUI2717", "No animation is playing"));
    m_paused = true; return Status::Ok();
}
Status AnimationPlayer::Resume() {
    if (!m_clip) return Status::Fail(AnimationError("PXUI2717", "No animation is playing"));
    if(m_finished){if(m_position>=m_clip->duration)m_position=0.0f;m_finished=false;}
    m_paused = false; return Apply();
}
Status AnimationPlayer::Stop(bool restoreDesignState) {
    Status result;
    if (restoreDesignState) {
        for (const auto& original : m_originals) {
            auto* node = m_root.Find(original.node);
            if (!node) continue;
            const auto* property = TypeRegistry::Global().FindProperty(
                std::string(node->TypeName()), original.property);
            if (!property || !property->set) continue;
            const Status set = property->set(*node, original.value);
            for (const auto& diagnostic : set.Diagnostics()) result.Add(diagnostic);
        }
    }
    m_clip = nullptr;m_position = 0.0f;m_paused = false;m_finished=false;
    m_blendDuration=0.0f;m_blendElapsed=0.0f;m_originals.clear();m_blendFrom.clear();
    return result;
}
Status AnimationPlayer::Seek(float time, bool apply) {
    if (!m_clip) return Status::Fail(AnimationError("PXUI2705", "No animation is playing"));
    m_position = std::clamp(time, 0.0f, m_clip->duration);
    m_finished=!m_clip->loop&&m_clip->duration>0.0f&&m_position>=m_clip->duration;
    return apply ? Apply() : Status::Ok();
}
Status AnimationPlayer::Update(float deltaSeconds) {
    if (!m_clip || m_finished) return Status::Ok();
    if (m_paused) return Status::Ok();
    m_position += std::max(0.0f, deltaSeconds);
    m_blendElapsed += std::max(0.0f,deltaSeconds);
    if (m_position > m_clip->duration) {
        if (m_clip->loop && m_clip->duration > 0.0f) m_position = std::fmod(m_position, m_clip->duration);
        else m_position = m_clip->duration;
    }
    const bool finished = !m_clip->loop && m_position >= m_clip->duration;
    const Status result = Apply();
    if (!result) return result;
    if(finished){m_position=m_clip->duration;m_finished=true;m_paused=false;}
    return result;
}

Result<Variant> AnimationPlayer::Sample(const AnimationTrack& track, float time) {
    if (time <= track.keys.front().time) return Result<Variant>::Success(track.keys.front().value);
    if (time >= track.keys.back().time) return Result<Variant>::Success(track.keys.back().value);
    auto right = std::upper_bound(track.keys.begin(), track.keys.end(), time,
        [](float value, const AnimationKey& key) { return value < key.time; });
    const auto& b = *right; const auto& a = *(right - 1);
    const float span = b.time - a.time;
    if (a.interpolation == KeyInterpolation::Discrete || b.interpolation == KeyInterpolation::Discrete)
        return Result<Variant>::Success(a.value);
    return Interpolate(a.value, b.value, EaseValue(span > 0.0f ? (time - a.time) / span : 1.0f, b.ease));
}

Status AnimationPlayer::Apply() {
    if (!m_clip) return Status::Ok();
    for (const auto& track : m_clip->tracks) {
        auto* node = m_root.Find(track.node);
        if (!node) return Status::Fail(AnimationError("PXUI2706", "Animation target node does not exist", &track));
        const auto* property = TypeRegistry::Global().FindProperty(std::string(node->TypeName()), track.property);
        if (!property || !property->set || !property->animatable) return Status::Fail(AnimationError("PXUI2707", "Animation target property is not animatable Runtime metadata", &track));
        auto sample = Sample(track, m_position); if (!sample) return Status::Fail(sample.Diagnostics());
        Variant value=sample.TakeValue();
        if(m_blendDuration>0.0f&&m_blendElapsed<m_blendDuration){const auto from=std::find_if(m_blendFrom.begin(),m_blendFrom.end(),[&](const OriginalValue& item){return item.node==track.node&&item.property==track.property;});if(from!=m_blendFrom.end()){auto blended=Interpolate(from->value,value,std::clamp(m_blendElapsed/m_blendDuration,0.0f,1.0f));if(!blended)return Status::Fail(blended.Diagnostics());value=blended.TakeValue();}}
        const Status set = property->set(*node, value); if (!set) return set;
    }
    return Status::Ok();
}

namespace {

diag::Diagnostic LibraryError(std::string code,std::string message,const std::string& sourcePath={}){
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),
        .category="UI.Animation",.message=std::move(message)};
    diagnostic.source.path=sourcePath;diag::Emit(diagnostic);return diagnostic;
}

const char* ParameterTypeName(const AnimationParameterType type){
    switch(type){case AnimationParameterType::Trigger:return "Trigger";case AnimationParameterType::Bool:return "Bool";case AnimationParameterType::Number:return "Number";}return "Trigger";
}
std::optional<AnimationParameterType> ParseParameterType(const std::string_view name){
    if(name=="Trigger")return AnimationParameterType::Trigger;if(name=="Bool")return AnimationParameterType::Bool;if(name=="Number")return AnimationParameterType::Number;return std::nullopt;
}
const char* ConditionName(const AnimationConditionOperator operation){
    switch(operation){case AnimationConditionOperator::Triggered:return "Triggered";case AnimationConditionOperator::Equal:return "Equal";case AnimationConditionOperator::NotEqual:return "NotEqual";case AnimationConditionOperator::Less:return "Less";case AnimationConditionOperator::LessEqual:return "LessEqual";case AnimationConditionOperator::Greater:return "Greater";case AnimationConditionOperator::GreaterEqual:return "GreaterEqual";}return "Triggered";
}
std::optional<AnimationConditionOperator> ParseCondition(const std::string_view name){
    if(name=="Triggered")return AnimationConditionOperator::Triggered;if(name=="Equal")return AnimationConditionOperator::Equal;if(name=="NotEqual")return AnimationConditionOperator::NotEqual;if(name=="Less")return AnimationConditionOperator::Less;if(name=="LessEqual")return AnimationConditionOperator::LessEqual;if(name=="Greater")return AnimationConditionOperator::Greater;if(name=="GreaterEqual")return AnimationConditionOperator::GreaterEqual;return std::nullopt;
}
const char* EaseName(const Ease ease){switch(ease){case Ease::EaseIn:return "EaseIn";case Ease::EaseOut:return "EaseOut";case Ease::EaseInOut:return "EaseInOut";case Ease::Step:return "Step";default:return "Linear";}}
bool Numeric(const Variant& value,double& result);

Result<AnimationClip> ParseClipObject(const VariantObject& object,const std::string& sourcePath){
    const auto idIt=object.find("id"),nameIt=object.find("name"),sourceIt=object.find("source");
    const auto* id=idIt==object.end()?nullptr:idIt->second.TryGet<Uuid>();const auto* name=nameIt==object.end()?nullptr:nameIt->second.TryGet<std::string>();
    if(!id||id->Empty()||!name||name->empty())return Result<AnimationClip>::Failure(LibraryError("PXUI2720","Animation clip requires a stable id and name",sourcePath));
    AnimationClip clip;clip.id=*id;clip.name=*name;
    if(sourceIt!=object.end()){
        const auto* source=sourceIt->second.TryGet<ResourceRefValue>();if(!source)return Result<AnimationClip>::Failure(LibraryError("PXUI2721","External animation source must be a ResourceRef",sourcePath));clip.source=*source;return Result<AnimationClip>::Success(std::move(clip));
    }
    const auto durationIt=object.find("duration"),loopIt=object.find("loop"),tracksIt=object.find("tracks");
    double duration=0.0;const auto* loop=loopIt==object.end()?nullptr:loopIt->second.TryGet<bool>();const auto* tracks=tracksIt==object.end()?nullptr:tracksIt->second.AsArray();
    if(durationIt==object.end()||!Numeric(durationIt->second,duration)||!loop||!tracks)return Result<AnimationClip>::Failure(LibraryError("PXUI2722","Embedded animation clip requires duration, loop, and tracks",sourcePath));
    clip.duration=static_cast<float>(duration);clip.loop=*loop;
    for(const auto& trackValue:*tracks){const auto* definition=trackValue.AsObject();if(!definition)return Result<AnimationClip>::Failure(LibraryError("PXUI2723","Animation track must be an Object",sourcePath));AnimationTrack track;
        const auto kindIt=definition->find("kind");if(kindIt!=definition->end()){const auto* kind=kindIt->second.TryGet<std::string>();if(!kind||*kind!="Property")return Result<AnimationClip>::Failure(LibraryError("PXUI2724","Animation v5 only supports typed Property tracks",sourcePath));}
        const auto nodeIt=definition->find("node"),propertyIt=definition->find("property"),keysIt=definition->find("keys");const auto* node=nodeIt==definition->end()?nullptr:nodeIt->second.TryGet<Uuid>();const auto* property=propertyIt==definition->end()?nullptr:propertyIt->second.TryGet<std::string>();const auto* keys=keysIt==definition->end()?nullptr:keysIt->second.AsArray();if(!keys)return Result<AnimationClip>::Failure(LibraryError("PXUI2725","Animation track requires keys",sourcePath));if(node)track.node=*node;if(property)track.property=*property;
        for(const auto& keyValue:*keys){const auto* key=keyValue.AsObject();if(!key)return Result<AnimationClip>::Failure(LibraryError("PXUI2729","Animation key must be an Object",sourcePath));const auto timeIt=key->find("time"),valueIt=key->find("value");double time=0.0;if(timeIt==key->end()||!Numeric(timeIt->second,time)||valueIt==key->end())return Result<AnimationClip>::Failure(LibraryError("PXUI2730","Animation key requires numeric time and value",sourcePath));Ease ease=Ease::Linear;KeyInterpolation interpolation=KeyInterpolation::Linear;if(const auto found=key->find("ease");found!=key->end())if(const auto* text=found->second.TryGet<std::string>()){if(*text=="EaseIn")ease=Ease::EaseIn;else if(*text=="EaseOut")ease=Ease::EaseOut;else if(*text=="EaseInOut")ease=Ease::EaseInOut;else if(*text=="Step")ease=Ease::Step;}if(const auto found=key->find("interpolation");found!=key->end())if(const auto* text=found->second.TryGet<std::string>())if(*text=="Discrete")interpolation=KeyInterpolation::Discrete;track.keys.push_back({static_cast<float>(time),valueIt->second.Clone(),ease,interpolation});}
        clip.tracks.push_back(std::move(track));
    }
    const Status valid=clip.Validate();return valid?Result<AnimationClip>::Success(std::move(clip)):Result<AnimationClip>::Failure(valid.Diagnostics());
}

Variant WriteClip(const AnimationClip& clip){
    VariantObject object{{"id",clip.id},{"name",clip.name}};if(clip.source){object["source"]=*clip.source;return object;}object["duration"]=static_cast<double>(clip.duration);object["loop"]=clip.loop;VariantArray tracks;for(const auto& track:clip.tracks){VariantObject definition{{"kind",std::string("Property")},{"node",track.node},{"property",track.property}};VariantArray keys;for(const auto& key:track.keys)keys.emplace_back(VariantObject{{"time",static_cast<double>(key.time)},{"value",key.value.Clone()},{"ease",std::string(EaseName(key.ease))},{"interpolation",std::string(key.interpolation==KeyInterpolation::Discrete?"Discrete":"Linear")}});definition["keys"]=std::move(keys);tracks.emplace_back(std::move(definition));}object["tracks"]=std::move(tracks);return object;
}

bool Numeric(const Variant& value,double& result){if(const auto* number=value.TryGet<double>()){result=*number;return true;}if(const auto* integer=value.TryGet<std::int64_t>()){result=static_cast<double>(*integer);return true;}return false;}

} // namespace

const AnimationState* AnimationStateMachine::FindState(const Uuid& id)const{const auto found=std::find_if(states.begin(),states.end(),[&](const AnimationState& state){return state.id==id;});return found==states.end()?nullptr:&*found;}
const AnimationState* AnimationStateMachine::FindState(const std::string_view name)const{const auto found=std::find_if(states.begin(),states.end(),[&](const AnimationState& state){return state.name==name;});return found==states.end()?nullptr:&*found;}
const AnimationClip* UIAnimationLibrary::FindClip(const Uuid& id)const{const auto found=std::find_if(clips.begin(),clips.end(),[&](const AnimationClip& clip){return clip.id==id;});return found==clips.end()?nullptr:&*found;}
const AnimationClip* UIAnimationLibrary::FindClip(const std::string_view name)const{const auto found=std::find_if(clips.begin(),clips.end(),[&](const AnimationClip& clip){return clip.name==name;});return found==clips.end()?nullptr:&*found;}

Status UIAnimationLibrary::Validate(const std::string& sourcePath)const{
    if(version!=CurrentVersion||machine.version!=AnimationStateMachine::CurrentVersion)return Status::Fail(LibraryError("PXUI2740","Animation library and state machine must use version 1",sourcePath));
    std::unordered_set<Uuid,UuidHash> clipIds,stateIds,transitionIds;std::unordered_set<std::string> clipNames,stateNames,parameterNames;
    for(const auto& clip:clips){if(clip.id.Empty()||clip.name.empty()||!clipIds.insert(clip.id).second||!clipNames.insert(clip.name).second)return Status::Fail(LibraryError("PXUI2741","Animation clips require unique ids and names",sourcePath));if(clip.source){if(clip.source->id.Empty()&&clip.source->lastKnownPath.empty())return Status::Fail(LibraryError("PXUI2742","External animation clip reference is empty",sourcePath));}else{const Status valid=clip.Validate();if(!valid)return valid;}}
    for(const auto& parameter:machine.parameters){if(parameter.name.empty()||!parameterNames.insert(parameter.name).second)return Status::Fail(LibraryError("PXUI2743","Animation parameters require unique names",sourcePath));const bool validDefault=parameter.type==AnimationParameterType::Number?(parameter.defaultValue.Type()==VariantType::Number||parameter.defaultValue.Type()==VariantType::Integer):parameter.defaultValue.Type()==VariantType::Bool;if(!validDefault)return Status::Fail(LibraryError("PXUI2744","Animation parameter default type does not match its declaration",sourcePath));}
    for(const auto& state:machine.states){if(state.id.Empty()||state.name.empty()||!stateIds.insert(state.id).second||!stateNames.insert(state.name).second||!FindClip(state.clip))return Status::Fail(LibraryError("PXUI2745","Animation states require unique identity and an existing clip",sourcePath));}
    if(machine.states.empty()||machine.entry.Empty()||!machine.FindState(machine.entry))return Status::Fail(LibraryError("PXUI2746","Animation state machine requires a valid Entry state",sourcePath));
    for(const auto& transition:machine.transitions){if(transition.id.Empty()||!transitionIds.insert(transition.id).second||!machine.FindState(transition.to)||(transition.from&&!machine.FindState(*transition.from))||transition.duration<0.0f||transition.exitTime<0.0f||transition.exitTime>1.0f)return Status::Fail(LibraryError("PXUI2747","Animation transition is invalid",sourcePath));for(const auto& condition:transition.conditions){const auto parameter=std::find_if(machine.parameters.begin(),machine.parameters.end(),[&](const AnimationParameter& value){return value.name==condition.parameter;});if(parameter==machine.parameters.end())return Status::Fail(LibraryError("PXUI2748","Animation condition references a missing parameter",sourcePath));if(parameter->type==AnimationParameterType::Trigger&&condition.operation!=AnimationConditionOperator::Triggered)return Status::Fail(LibraryError("PXUI2749","Trigger parameters only support Triggered conditions",sourcePath));if(parameter->type==AnimationParameterType::Bool&&condition.operation!=AnimationConditionOperator::Equal&&condition.operation!=AnimationConditionOperator::NotEqual)return Status::Fail(LibraryError("PXUI2750","Bool parameters only support equality conditions",sourcePath));if(parameter->type==AnimationParameterType::Number&&(condition.operation==AnimationConditionOperator::Triggered||(!condition.value.TryGet<double>()&&!condition.value.TryGet<std::int64_t>())))return Status::Fail(LibraryError("PXUI2751","Number condition is invalid",sourcePath));}}
    return Status::Ok();
}

Result<UIAnimationLibrary> ParseUIAnimationLibrary(const Variant& value,const std::string& sourcePath){
    const auto* root=value.AsObject();if(!root)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2760","animations must be an Object",sourcePath));const auto versionIt=root->find("version"),clipsIt=root->find("clips"),machineIt=root->find("machine");const auto* version=versionIt==root->end()?nullptr:versionIt->second.TryGet<std::int64_t>();const auto* clips=clipsIt==root->end()?nullptr:clipsIt->second.AsArray();const auto* machine=machineIt==root->end()?nullptr:machineIt->second.AsObject();if(!version||!clips||!machine)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2761","animations requires version, clips, and machine",sourcePath));UIAnimationLibrary library;library.version=*version;
    for(const auto& clipValue:*clips){const auto* object=clipValue.AsObject();if(!object)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2762","Animation clip must be an Object",sourcePath));auto parsed=ParseClipObject(*object,sourcePath);if(!parsed)return Result<UIAnimationLibrary>::Failure(parsed.Diagnostics());library.clips.push_back(parsed.TakeValue());}
    const auto machineVersionIt=machine->find("version"),entryIt=machine->find("entry"),parametersIt=machine->find("parameters"),statesIt=machine->find("states"),transitionsIt=machine->find("transitions");const auto* machineVersion=machineVersionIt==machine->end()?nullptr:machineVersionIt->second.TryGet<std::int64_t>();const auto* entry=entryIt==machine->end()?nullptr:entryIt->second.TryGet<Uuid>();const auto* parameters=parametersIt==machine->end()?nullptr:parametersIt->second.AsArray();const auto* states=statesIt==machine->end()?nullptr:statesIt->second.AsArray();const auto* transitions=transitionsIt==machine->end()?nullptr:transitionsIt->second.AsArray();if(!machineVersion||!entry||!parameters||!states||!transitions)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2763","Animation state machine fields are incomplete",sourcePath));library.machine.version=*machineVersion;library.machine.entry=*entry;
    for(const auto& parameterValue:*parameters){const auto* object=parameterValue.AsObject();if(!object)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2764","Animation parameter must be an Object",sourcePath));const auto nameIt=object->find("name"),typeIt=object->find("type"),defaultIt=object->find("default");const auto* name=nameIt==object->end()?nullptr:nameIt->second.TryGet<std::string>();const auto* typeName=typeIt==object->end()?nullptr:typeIt->second.TryGet<std::string>();const auto type=typeName?ParseParameterType(*typeName):std::nullopt;if(!name||!type||defaultIt==object->end())return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2765","Animation parameter fields are invalid",sourcePath));library.machine.parameters.push_back({*name,*type,defaultIt->second.Clone()});}
    for(const auto& stateValue:*states){const auto* object=stateValue.AsObject();if(!object)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2766","Animation state must be an Object",sourcePath));const auto idIt=object->find("id"),nameIt=object->find("name"),clipIt=object->find("clip"),positionIt=object->find("position");const auto* id=idIt==object->end()?nullptr:idIt->second.TryGet<Uuid>();const auto* name=nameIt==object->end()?nullptr:nameIt->second.TryGet<std::string>();const auto* clip=clipIt==object->end()?nullptr:clipIt->second.TryGet<Uuid>();const auto* position=positionIt==object->end()?nullptr:positionIt->second.TryGet<Vec2>();if(!id||!name||!clip||!position)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2767","Animation state fields are invalid",sourcePath));library.machine.states.push_back({*id,*name,*clip,*position});}
    for(const auto& transitionValue:*transitions){const auto* object=transitionValue.AsObject();if(!object)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2768","Animation transition must be an Object",sourcePath));AnimationTransition transition;const auto idIt=object->find("id"),fromIt=object->find("from"),toIt=object->find("to"),conditionsIt=object->find("conditions"),exitEnabledIt=object->find("hasExitTime"),exitIt=object->find("exitTime"),durationIt=object->find("duration"),priorityIt=object->find("priority");const auto* id=idIt==object->end()?nullptr:idIt->second.TryGet<Uuid>();const auto* from=fromIt==object->end()?nullptr:fromIt->second.TryGet<Uuid>();const auto* to=toIt==object->end()?nullptr:toIt->second.TryGet<Uuid>();const auto* conditions=conditionsIt==object->end()?nullptr:conditionsIt->second.AsArray();const auto* exitEnabled=exitEnabledIt==object->end()?nullptr:exitEnabledIt->second.TryGet<bool>();const auto* priority=priorityIt==object->end()?nullptr:priorityIt->second.TryGet<std::int64_t>();double exit=0.0,duration=0.0;if(!id||!from||!to||!conditions||!exitEnabled||exitIt==object->end()||!Numeric(exitIt->second,exit)||durationIt==object->end()||!Numeric(durationIt->second,duration)||!priority)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2769","Animation transition fields are invalid",sourcePath));transition.id=*id;if(!from->Empty())transition.from=*from;transition.to=*to;transition.hasExitTime=*exitEnabled;transition.exitTime=static_cast<float>(exit);transition.duration=static_cast<float>(duration);transition.priority=static_cast<int>(*priority);
        for(const auto& conditionValue:*conditions){const auto* condition=conditionValue.AsObject();if(!condition)return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2770","Animation condition must be an Object",sourcePath));const auto parameterIt=condition->find("parameter"),operatorIt=condition->find("operator"),valueIt=condition->find("value");const auto* parameter=parameterIt==condition->end()?nullptr:parameterIt->second.TryGet<std::string>();const auto* operationName=operatorIt==condition->end()?nullptr:operatorIt->second.TryGet<std::string>();const auto operation=operationName?ParseCondition(*operationName):std::nullopt;if(!parameter||!operation||valueIt==condition->end())return Result<UIAnimationLibrary>::Failure(LibraryError("PXUI2771","Animation condition fields are invalid",sourcePath));transition.conditions.push_back({*parameter,*operation,valueIt->second.Clone()});}library.machine.transitions.push_back(std::move(transition));}
    const Status valid=library.Validate(sourcePath);return valid?Result<UIAnimationLibrary>::Success(std::move(library)):Result<UIAnimationLibrary>::Failure(valid.Diagnostics());
}

Variant WriteUIAnimationLibrary(const UIAnimationLibrary& library){
    VariantArray clips;for(const auto& clip:library.clips)clips.push_back(WriteClip(clip));VariantArray parameters;for(const auto& parameter:library.machine.parameters)parameters.emplace_back(VariantObject{{"name",parameter.name},{"type",std::string(ParameterTypeName(parameter.type))},{"default",parameter.defaultValue.Clone()}});VariantArray states;for(const auto& state:library.machine.states)states.emplace_back(VariantObject{{"id",state.id},{"name",state.name},{"clip",state.clip},{"position",state.position}});VariantArray transitions;for(const auto& transition:library.machine.transitions){VariantArray conditions;for(const auto& condition:transition.conditions)conditions.emplace_back(VariantObject{{"parameter",condition.parameter},{"operator",std::string(ConditionName(condition.operation))},{"value",condition.value.Clone()}});transitions.emplace_back(VariantObject{{"id",transition.id},{"from",transition.from?*transition.from:Uuid{}},{"to",transition.to},{"conditions",std::move(conditions)},{"hasExitTime",transition.hasExitTime},{"exitTime",static_cast<double>(transition.exitTime)},{"duration",static_cast<double>(transition.duration)},{"priority",static_cast<std::int64_t>(transition.priority)}});}VariantObject machine{{"version",library.machine.version},{"entry",library.machine.entry},{"parameters",std::move(parameters)},{"states",std::move(states)},{"transitions",std::move(transitions)}};return VariantObject{{"version",library.version},{"clips",std::move(clips)},{"machine",std::move(machine)}};
}

UIAnimationController::UIAnimationController(Control& sceneRoot):m_player(sceneRoot){}
const AnimationParameter* UIAnimationController::Parameter(const std::string_view name)const{if(!m_library)return nullptr;const auto found=std::find_if(m_library->machine.parameters.begin(),m_library->machine.parameters.end(),[&](const AnimationParameter& parameter){return parameter.name==name;});return found==m_library->machine.parameters.end()?nullptr:&*found;}
const AnimationClip* UIAnimationController::ResolveClip(const Uuid& id){if(!m_library)return nullptr;const auto* clip=m_library->FindClip(id);if(!clip)return nullptr;if(!clip->source)return clip;if(const auto found=m_externalClips.find(id);found!=m_externalClips.end())return &found->second;if(!m_resolver)return nullptr;auto resolved=m_resolver(*clip->source);if(!resolved)return nullptr;AnimationClip value=resolved.TakeValue();value.id=clip->id;value.name=clip->name;const auto [found,_]=m_externalClips.emplace(id,std::move(value));return &found->second;}
Status UIAnimationController::SetLibrary(UIAnimationLibrary library,const bool autoplay){const Status valid=library.Validate();if(!valid)return valid;(void)m_player.Stop(true);m_library=std::move(library);m_externalClips.clear();m_parameters.clear();m_state={};m_transition={};m_transitionElapsed=0.0f;m_transitionDuration=0.0f;for(const auto& parameter:m_library->machine.parameters)m_parameters[parameter.name]=parameter.defaultValue.Clone();return autoplay?Travel(m_library->machine.entry):Status::Ok();}
Status UIAnimationController::SetTrigger(const std::string_view parameter){const auto* descriptor=Parameter(parameter);if(!descriptor||descriptor->type!=AnimationParameterType::Trigger)return Status::Fail(LibraryError("PXUI2780","Animation Trigger parameter does not exist: "+std::string(parameter)));m_parameters[std::string(parameter)]=true;return Status::Ok();}
Status UIAnimationController::SetBool(const std::string_view parameter,const bool value){const auto* descriptor=Parameter(parameter);if(!descriptor||descriptor->type!=AnimationParameterType::Bool)return Status::Fail(LibraryError("PXUI2781","Animation Bool parameter does not exist: "+std::string(parameter)));m_parameters[std::string(parameter)]=value;return Status::Ok();}
Status UIAnimationController::SetNumber(const std::string_view parameter,const double value){const auto* descriptor=Parameter(parameter);if(!descriptor||descriptor->type!=AnimationParameterType::Number)return Status::Fail(LibraryError("PXUI2782","Animation Number parameter does not exist: "+std::string(parameter)));m_parameters[std::string(parameter)]=value;return Status::Ok();}
Status UIAnimationController::SetParameter(const std::string_view parameter,const Variant& value){const auto* descriptor=Parameter(parameter);if(!descriptor)return Status::Fail(LibraryError("PXUI2780","Animation parameter does not exist: "+std::string(parameter)));if(descriptor->type==AnimationParameterType::Trigger)return SetTrigger(parameter);if(descriptor->type==AnimationParameterType::Bool){const auto* boolean=value.TryGet<bool>();return boolean?SetBool(parameter,*boolean):Status::Fail(LibraryError("PXUI2781","Animation Bool parameter requires a Bool value"));}double number=0;return Numeric(value,number)?SetNumber(parameter,number):Status::Fail(LibraryError("PXUI2782","Animation Number parameter requires a numeric value"));}
bool UIAnimationController::ConditionsPass(const AnimationTransition& transition)const{for(const auto& condition:transition.conditions){const auto found=m_parameters.find(condition.parameter);if(found==m_parameters.end())return false;const auto* descriptor=Parameter(condition.parameter);if(!descriptor)return false;if(descriptor->type==AnimationParameterType::Trigger){const auto* active=found->second.TryGet<bool>();if(!active||!*active)return false;continue;}if(descriptor->type==AnimationParameterType::Bool){const auto* current=found->second.TryGet<bool>();const auto* expected=condition.value.TryGet<bool>();if(!current||!expected)return false;const bool equal=*current==*expected;if((condition.operation==AnimationConditionOperator::Equal&&!equal)||(condition.operation==AnimationConditionOperator::NotEqual&&equal))return false;continue;}double current=0,expected=0;if(!Numeric(found->second,current)||!Numeric(condition.value,expected))return false;switch(condition.operation){case AnimationConditionOperator::Equal:if(current!=expected)return false;break;case AnimationConditionOperator::NotEqual:if(current==expected)return false;break;case AnimationConditionOperator::Less:if(!(current<expected))return false;break;case AnimationConditionOperator::LessEqual:if(!(current<=expected))return false;break;case AnimationConditionOperator::Greater:if(!(current>expected))return false;break;case AnimationConditionOperator::GreaterEqual:if(!(current>=expected))return false;break;default:return false;}}return true;}
bool UIAnimationController::ExitTimePasses(const AnimationTransition& transition)const{if(!transition.hasExitTime)return true;const float duration=m_player.Duration();return duration<=0.0f||m_player.Position()/duration>=transition.exitTime;}
void UIAnimationController::ConsumeTriggers(const AnimationTransition& transition){for(const auto& condition:transition.conditions)if(const auto* descriptor=Parameter(condition.parameter);descriptor&&descriptor->type==AnimationParameterType::Trigger)m_parameters[condition.parameter]=false;}
Status UIAnimationController::Enter(const AnimationState& state,const float duration,const Uuid& transition){const auto* clip=ResolveClip(state.clip);if(!clip)return Status::Fail(LibraryError("PXUI2783","Animation state clip could not be resolved: "+state.name));const Status played=m_player.Play(*clip,duration);if(!played)return played;m_state=state.id;m_transition=transition;m_transitionElapsed=0.0f;m_transitionDuration=std::max(0.0f,duration);return Status::Ok();}
Status UIAnimationController::Travel(const Uuid& state,const float duration){if(!m_library)return Status::Fail(LibraryError("PXUI2784","Animation library is not configured"));const auto* target=m_library->machine.FindState(state);if(!target)return Status::Fail(LibraryError("PXUI2785","Animation state does not exist"));return Enter(*target,duration);}
Status UIAnimationController::Travel(const std::string_view state,const float duration){if(!m_library)return Status::Fail(LibraryError("PXUI2784","Animation library is not configured"));const auto* target=m_library->machine.FindState(state);if(!target)return Status::Fail(LibraryError("PXUI2785","Animation state does not exist: "+std::string(state)));return Enter(*target,duration);}
Status UIAnimationController::Update(const float deltaSeconds){if(!m_library||m_state.Empty())return Status::Ok();const Status updated=m_player.Update(deltaSeconds);if(!updated)return updated;if(!m_transition.Empty()){m_transitionElapsed+=std::max(0.0f,deltaSeconds);if(m_transitionDuration<=0.0f||m_transitionElapsed>=m_transitionDuration)m_transition={};}
    const auto tryGroup=[this](std::vector<const AnimationTransition*> candidates)->std::optional<Status>{
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto* left,const auto* right){return left->priority>right->priority;});
        for(const auto* transition:candidates){
            if(!ExitTimePasses(*transition)||!ConditionsPass(*transition))continue;
            const auto* target=m_library->machine.FindState(transition->to);
            if(!target)return Status::Fail(LibraryError("PXUI2786","Animation transition target disappeared"));
            ConsumeTriggers(*transition);
            return Enter(*target,transition->duration,transition->id);
        }
        return std::nullopt;
    };
    std::vector<const AnimationTransition*> anyState;
    std::vector<const AnimationTransition*> currentState;
    for(const auto& transition:m_library->machine.transitions){
        if(!transition.from)anyState.push_back(&transition);
        else if(*transition.from==m_state)currentState.push_back(&transition);
    }
    if(auto result=tryGroup(std::move(anyState)))return *result;
    if(auto result=tryGroup(std::move(currentState)))return *result;
    return Status::Ok();}
Status UIAnimationController::PreviewClip(const Uuid& clipId,const float time,const bool playing){if(!m_library)return Status::Fail(LibraryError("PXUI2784","Animation library is not configured"));const auto* clip=ResolveClip(clipId);if(!clip)return Status::Fail(LibraryError("PXUI2787","Animation preview clip does not exist"));if(m_player.CurrentClip()!=clip){const Status started=m_player.Play(*clip);if(!started)return started;}const Status seek=m_player.Seek(time,true);if(!seek)return seek;return playing?m_player.Resume():m_player.Pause();}
Status UIAnimationController::Stop(const bool restoreDesignState){m_state={};m_transition={};m_transitionElapsed=0.0f;m_transitionDuration=0.0f;return m_player.Stop(restoreDesignState);}
UIAnimationRuntimeState UIAnimationController::CaptureState()const{UIAnimationRuntimeState result{.state=m_state,.transition=m_transition,.position=m_player.Position(),.transitionProgress=m_transitionDuration>0.0f?std::clamp(m_transitionElapsed/m_transitionDuration,0.0f,1.0f):0.0f,.paused=m_player.Paused()};for(const auto& [name,value]:m_parameters)result.parameters[name]=value.Clone();return result;}
Status UIAnimationController::RestoreState(const UIAnimationRuntimeState& state){
    if(!m_library||!m_library->machine.FindState(state.state))return Status::Fail(LibraryError("PXUI2788","Animation checkpoint references a missing state"));
    const AnimationTransition* restoredTransition=nullptr;
    if(!state.transition.Empty()){
        const auto found=std::find_if(m_library->machine.transitions.begin(),m_library->machine.transitions.end(),[&](const AnimationTransition& transition){return transition.id==state.transition;});
        if(found==m_library->machine.transitions.end())return Status::Fail(LibraryError("PXUI2790","Animation checkpoint references a missing transition"));
        restoredTransition=&*found;
    }
    for(const auto& [name,value]:state.parameters){const auto* descriptor=Parameter(name);if(!descriptor)return Status::Fail(LibraryError("PXUI2789","Animation checkpoint parameter is missing: "+name));m_parameters[name]=value.Clone();}
    const Status entered=Travel(state.state);if(!entered)return entered;
    const Status seek=m_player.Seek(state.position,true);if(!seek)return seek;
    if(restoredTransition){m_transition=restoredTransition->id;m_transitionDuration=std::max(0.0f,restoredTransition->duration);m_transitionElapsed=m_transitionDuration*std::clamp(state.transitionProgress,0.0f,1.0f);}
    if(state.paused){const Status paused=m_player.Pause();if(!paused)return paused;}
    return Status::Ok();}

}  // namespace px::ui
