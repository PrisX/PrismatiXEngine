#include "Engine/Animation/Timeline.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>

namespace px::animation {
namespace {
using Json = nlohmann::json;

diag::Diagnostic TimelineError(std::string code, std::string message,
                               const std::string& path = {}) {
    diag::Diagnostic diagnostic;
    diagnostic.severity = diag::Severity::Error;
    diagnostic.code = std::move(code);
    diagnostic.category = "Animation.Timeline";
    diagnostic.message = std::move(message);
    diagnostic.source.path = path;
    return diagnostic;
}

float Ease(const Curve curve, const float value) {
    const float t=std::clamp(value,0.0f,1.0f);
    switch(curve){
        case Curve::Step:return 0.0f;
        case Curve::Linear:return t;
        case Curve::EaseIn:return t*t;
        case Curve::EaseOut:return 1.0f-(1.0f-t)*(1.0f-t);
        case Curve::EaseInOut:return t<0.5f?2.0f*t*t:1.0f-std::pow(-2.0f*t+2.0f,2.0f)/2.0f;
        case Curve::BackOut:{constexpr float c1=1.70158f,c3=c1+1.0f;return 1.0f+c3*std::pow(t-1.0f,3.0f)+c1*std::pow(t-1.0f,2.0f);}
    }
    return t;
}

Variant Interpolate(const Variant& from,const Variant& to,const float amount) {
    if(from.Type()!=to.Type())return amount<1.0f?from.Clone():to.Clone();
    if(const auto* a=from.TryGet<std::int64_t>()){const auto b=*to.TryGet<std::int64_t>();return Variant(static_cast<std::int64_t>(std::llround(static_cast<double>(*a)+(static_cast<double>(b)-*a)*amount)));}
    if(const auto* a=from.TryGet<double>()){const auto b=*to.TryGet<double>();return Variant(*a+(b-*a)*amount);}
    if(const auto* a=from.TryGet<Vec2>()){const auto b=*to.TryGet<Vec2>();return Variant(Vec2{a->x+(b.x-a->x)*amount,a->y+(b.y-a->y)*amount});}
    if(const auto* a=from.TryGet<Color>()){const auto b=*to.TryGet<Color>();const auto channel=[amount](std::uint8_t x,std::uint8_t y){return static_cast<std::uint8_t>(std::clamp(std::lround(x+(y-x)*amount),0l,255l));};return Variant(Color{channel(a->r,b.r),channel(a->g,b.g),channel(a->b,b.b),channel(a->a,b.a)});}
    return amount<1.0f?from.Clone():to.Clone();
}

Variant Sample(const Track& track,const float time) {
    if(track.keys.empty())return {};
    if(time<=track.keys.front().time)return track.keys.front().value.Clone();
    if(time>=track.keys.back().time)return track.keys.back().value.Clone();
    const auto upper=std::upper_bound(track.keys.begin(),track.keys.end(),time,[](float value,const Keyframe& key){return value<key.time;});
    const auto& to=*upper;const auto& from=*(upper-1);const float span=to.time-from.time;
    return Interpolate(from.value,to.value,Ease(to.curve,span<=0.0f?1.0f:(time-from.time)/span));
}

AnimationClip Preset(const std::string& category,const std::string& name,const std::string& property,
                     Variant from,Variant to,float duration=0.45f,bool loop=false) {
    AnimationClip clip;clip.id=Uuid::FromName("PrismatiX.OfficialPreset."+category+"."+name);
    clip.name=category+"/"+name;clip.duration=duration;clip.loop=loop;
    TargetKind kind=category=="Text"?TargetKind::Text:category=="UI"?TargetKind::UI:
                    category=="Screen"?TargetKind::Camera:TargetKind::Stage;
    clip.tracks.push_back({{kind,"$selection",property},{{0.0f,std::move(from),Curve::Linear},{duration,std::move(to),Curve::EaseOut}}});return clip;
}

const char* KindName(const TargetKind kind) {
    switch (kind) {
        case TargetKind::Stage: return "stage";
        case TargetKind::UI: return "ui";
        case TargetKind::Camera: return "camera";
        case TargetKind::Text: return "text";
        case TargetKind::Audio: return "audio";
        case TargetKind::Shader: return "shader";
    }
    return "stage";
}

std::optional<TargetKind> ParseKind(const std::string_view name) {
    if (name == "stage") return TargetKind::Stage;
    if (name == "ui") return TargetKind::UI;
    if (name == "camera") return TargetKind::Camera;
    if (name == "text") return TargetKind::Text;
    if (name == "audio") return TargetKind::Audio;
    if (name == "shader") return TargetKind::Shader;
    return std::nullopt;
}

const char* CurveName(const Curve curve) {
    switch (curve) {
        case Curve::Step: return "step";
        case Curve::Linear: return "linear";
        case Curve::EaseIn: return "easeIn";
        case Curve::EaseOut: return "easeOut";
        case Curve::EaseInOut: return "easeInOut";
        case Curve::BackOut: return "backOut";
    }
    return "linear";
}

std::optional<Curve> ParseCurve(const std::string_view name) {
    if (name == "step") return Curve::Step;
    if (name == "linear") return Curve::Linear;
    if (name == "easeIn" || name == "ease-in") return Curve::EaseIn;
    if (name == "easeOut" || name == "ease-out") return Curve::EaseOut;
    if (name == "easeInOut" || name == "ease-in-out")
        return Curve::EaseInOut;
    if (name == "backOut" || name == "back-out") return Curve::BackOut;
    return std::nullopt;
}

bool OnlyFields(const Json& value,
                const std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() > fields.size()) return false;
    return std::ranges::all_of(value.items(), [&](const auto& item) {
        return std::ranges::find(fields, std::string_view(item.key())) !=
               fields.end();
    });
}

bool Identifier(const std::string_view value) {
    if (value.empty() || value.size() > 256 ||
        !std::isalnum(static_cast<unsigned char>(value.front())))
        return false;
    return std::ranges::all_of(value.substr(1), [](const char character) {
        return std::isalnum(static_cast<unsigned char>(character)) ||
               character == '.' || character == '_' || character == '-';
    });
}

bool SafePath(const std::string_view value) {
    if (value.empty() || value.size() > 4096 || value.starts_with("asset:") ||
        value.front() == '/' || value.find('\\') != std::string_view::npos ||
        (value.size() > 1 && std::isalpha(static_cast<unsigned char>(value[0])) &&
         value[1] == ':'))
        return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto slash = value.find('/', start);
        const auto part = value.substr(
            start, slash == std::string_view::npos ? value.size() - start
                                                    : slash - start);
        if (part == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

Json ValueJson(const Variant& value) {
    switch (value.Type()) {
        case VariantType::Null: return nullptr;
        case VariantType::Bool: return *value.TryGet<bool>();
        case VariantType::Integer: return *value.TryGet<std::int64_t>();
        case VariantType::Number: return *value.TryGet<double>();
        case VariantType::String: return *value.TryGet<std::string>();
        case VariantType::Vec2: {
            const auto& item = *value.TryGet<Vec2>();
            return Json{{"$type", "vec2"}, {"x", item.x}, {"y", item.y}};
        }
        case VariantType::Rect: {
            const auto& item = *value.TryGet<Rect>();
            return Json{{"$type", "rect"}, {"x", item.x}, {"y", item.y},
                        {"width", item.w}, {"height", item.h}};
        }
        case VariantType::Color: {
            const auto& item = *value.TryGet<Color>();
            return Json{{"$type", "color"}, {"r", item.r}, {"g", item.g},
                        {"b", item.b}, {"a", item.a}};
        }
        case VariantType::Uuid:
            return Json{{"$type", "uuid"},
                        {"value", value.TryGet<Uuid>()->ToString()}};
        case VariantType::ResourceRef: {
            const auto& item = *value.TryGet<ResourceRefValue>();
            return Json{{"$type", "resource"}, {"id", item.id.ToString()},
                        {"path", item.lastKnownPath}};
        }
        case VariantType::TokenRef:
            return Json{{"$type", "token"},
                        {"value", value.TryGet<TokenRefValue>()->name}};
        case VariantType::Array: {
            Json result = Json::array();
            for (const auto& item : *value.AsArray())
                result.push_back(ValueJson(item));
            return result;
        }
        case VariantType::Object: {
            Json result = Json::object();
            for (const auto& [name, item] : *value.AsObject())
                result[name] = ValueJson(item);
            return result;
        }
    }
    return nullptr;
}

std::optional<Variant> ParseValue(const Json& json, const std::size_t depth,
                                  std::size_t& nodes) {
    constexpr std::size_t kMaxDepth = 32;
    constexpr std::size_t kMaxNodes = 1'000'000;
    constexpr std::size_t kMaxEntries = 100'000;
    if (depth > kMaxDepth || ++nodes > kMaxNodes) return std::nullopt;
    try {
        if (json.is_null()) return Variant{};
        if (json.is_boolean()) return Variant(json.get<bool>());
        if (json.is_number_unsigned()) {
            const auto number = json.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(
                             (std::numeric_limits<std::int64_t>::max)()))
                return std::nullopt;
            return Variant(static_cast<std::int64_t>(number));
        }
        if (json.is_number_integer()) return Variant(json.get<std::int64_t>());
        if (json.is_number_float()) {
            const double number = json.get<double>();
            return std::isfinite(number) ? std::optional<Variant>{number}
                                         : std::nullopt;
        }
        if (json.is_string()) {
            const auto& text = json.get_ref<const std::string&>();
            return text.size() <= 1'048'576
                       ? std::optional<Variant>{Variant(text)}
                       : std::nullopt;
        }
        if (json.is_array()) {
            if (json.size() > kMaxEntries) return std::nullopt;
            VariantArray result;
            result.reserve(json.size());
            for (const auto& item : json) {
                auto parsed = ParseValue(item, depth + 1, nodes);
                if (!parsed) return std::nullopt;
                result.push_back(std::move(*parsed));
            }
            return Variant(std::move(result));
        }
        if (!json.is_object() || json.size() > kMaxEntries)
            return std::nullopt;
        const std::string type = json.value("$type", std::string{});
        if (type == "vec2" && OnlyFields(json, {"$type", "x", "y"}) &&
            json.size() == 3)
            return Variant(Vec2{json.at("x").get<float>(),
                                json.at("y").get<float>()});
        if (type == "rect" &&
            OnlyFields(json, {"$type", "x", "y", "width", "height"}) &&
            json.size() == 5)
            return Variant(Rect{json.at("x").get<float>(),
                                json.at("y").get<float>(),
                                json.at("width").get<float>(),
                                json.at("height").get<float>()});
        if (type == "color" &&
            OnlyFields(json, {"$type", "r", "g", "b", "a"}) &&
            json.size() == 5)
            return Variant(Color{json.at("r").get<std::uint8_t>(),
                                 json.at("g").get<std::uint8_t>(),
                                 json.at("b").get<std::uint8_t>(),
                                 json.at("a").get<std::uint8_t>()});
        if (type == "uuid" && OnlyFields(json, {"$type", "value"}) &&
            json.size() == 2) {
            const auto id = Uuid::Parse(json.at("value").get<std::string>());
            return id ? std::optional<Variant>{Variant(*id)} : std::nullopt;
        }
        if (type == "resource" &&
            OnlyFields(json, {"$type", "id", "path"}) && json.size() >= 2) {
            const auto id = Uuid::Parse(json.at("id").get<std::string>());
            const std::string path = json.value("path", std::string{});
            return id && (path.empty() || SafePath(path))
                       ? std::optional<Variant>{
                             Variant(ResourceRefValue{*id, path})}
                       : std::nullopt;
        }
        if (type == "token" && OnlyFields(json, {"$type", "value"}) &&
            json.size() == 2)
            return Variant(TokenRefValue{json.at("value").get<std::string>()});
        VariantObject result;
        for (const auto& [name, item] : json.items()) {
            if (name.empty() || name.size() > 256) return std::nullopt;
            auto parsed = ParseValue(item, depth + 1, nodes);
            if (!parsed) return std::nullopt;
            result.emplace(name, std::move(*parsed));
        }
        return Variant(std::move(result));
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<Variant> ParseValue(const Json& json) {
    std::size_t nodes = 0;
    return ParseValue(json, 0, nodes);
}

std::optional<ResourceRefValue> ParseResourceRef(const Json& json) {
    if (json.is_string()) {
        const std::string value = json.get<std::string>();
        if (value.starts_with("asset:")) {
            const auto id = Uuid::Parse(std::string_view(value).substr(6));
            return id ? std::optional<ResourceRefValue>{{*id, {}}}
                      : std::nullopt;
        }
        return SafePath(value)
                   ? std::optional<ResourceRefValue>{{{}, value}}
                   : std::nullopt;
    }
    if (!OnlyFields(json, {"id", "path", "kind", "name"}) ||
        !json.contains("id") || !json["id"].is_string())
        return std::nullopt;
    const auto id = Uuid::Parse(json["id"].get<std::string>());
    const std::string path = json.value("path", std::string{});
    if (!id || (!path.empty() && !SafePath(path)) ||
        (json.contains("kind") &&
         (!json["kind"].is_string() || json["kind"].get<std::string>().empty() ||
          json["kind"].get<std::string>().size() > 64)) ||
        (json.contains("name") &&
         (!json["name"].is_string() ||
          json["name"].get<std::string>().size() > 512)))
        return std::nullopt;
    return ResourceRefValue{*id, path};
}

}  // namespace

Status AnimationClip::Validate(const std::string& sourcePath) const {
    Status status;if(id.Empty())status.Add(TimelineError("PXANIM7501","Animation clip id is empty",sourcePath));
    if(duration<0.0f||!std::isfinite(duration))status.Add(TimelineError("PXANIM7502","Animation duration is invalid",sourcePath));
    for(const auto& track:tracks){if(track.binding.target.empty()||track.binding.property.empty()||track.keys.empty())status.Add(TimelineError("PXANIM7503","Animation track target, property, and keys are required",sourcePath));float previous=-1.0f;const VariantType type=track.keys.empty()?VariantType::Null:track.keys.front().value.Type();for(const auto& key:track.keys){if(!std::isfinite(key.time)||key.time<0.0f||key.time<previous||key.time>duration||key.value.Type()!=type)status.Add(TimelineError("PXANIM7504","Animation key times must be sorted within duration and use one value type",sourcePath));previous=key.time;}}
    for(const auto& marker:markers)if(!std::isfinite(marker.time)||marker.time<0.0f||marker.time>duration||marker.name.empty())status.Add(TimelineError("PXANIM7505","Animation marker is invalid or outside duration",sourcePath));
    for(const auto& child:nested){if(child.clip.Empty()||child.start<0.0f||child.start>duration||!std::isfinite(child.speed)||child.speed<=0.0f)status.Add(TimelineError("PXANIM7506","Nested animation reference is invalid",sourcePath));}
    return status;
}

Status TimelinePlayer::Register(AnimationClip clip) {const Status valid=clip.Validate();if(!valid)return valid;if(m_clips.contains(clip.id))return Status::Fail(TimelineError("PXANIM7510","Duplicate animation clip id"));m_clips.emplace(clip.id,std::move(clip));return Status::Ok();}
Status TimelinePlayer::RegisterOrReplace(AnimationClip clip) {const Status valid=clip.Validate();if(!valid)return valid;const auto id=clip.id;m_clips.insert_or_assign(id,std::move(clip));return Status::Ok();}
Status TimelinePlayer::ReplaceRegisteredClips(std::vector<AnimationClip> clips) {
    std::unordered_map<resource::ResourceId, AnimationClip, UuidHash> candidate;
    candidate.reserve(clips.size());
    for (auto& clip : clips) {
        if (const Status valid = clip.Validate(); !valid) return valid;
        const auto id = clip.id;
        if (!candidate.emplace(id, std::move(clip)).second)
            return Status::Fail(TimelineError(
                "PXANIM7510", "Duplicate animation clip id in restore library"));
    }
    for (const auto& [_, clip] : candidate)
        for (const auto& nested : clip.nested)
            if (!candidate.contains(nested.clip))
                return Status::Fail(TimelineError(
                    "PXANIM7518",
                    "Nested animation clip is missing from restore library"));
    m_clips = std::move(candidate);
    return Status::Ok();
}
const AnimationClip* TimelinePlayer::Find(const resource::ResourceId& id) const {const auto found=m_clips.find(id);return found==m_clips.end()?nullptr:&found->second;}
Status TimelinePlayer::Unregister(const resource::ResourceId& id){if(std::any_of(m_playbacks.begin(),m_playbacks.end(),[&](const auto& state){return state.clip==id&&state.playing;}))return Status::Fail(TimelineError("PXANIM7515","Cannot remove an animation while it is playing"));if(!m_clips.erase(id))return Status::Fail(TimelineError("PXANIM7516","Animation clip is not registered"));return Status::Ok();}

PlaybackHandle TimelinePlayer::Play(const resource::ResourceId& clip,const bool await,const float speed) {if(!Find(clip)||!std::isfinite(speed)||speed<=0.0f)return 0;PlaybackState state;state.handle=m_nextHandle++;state.clip=clip;state.speed=speed;state.playing=true;state.awaiting=await;m_playbacks.push_back(state);(void)ApplyAt(*Find(clip),0.0f);return state.handle;}

Status TimelinePlayer::Seek(const PlaybackHandle handle,const float position,const bool apply){if(!std::isfinite(position))return Status::Fail(TimelineError("PXANIM7514","Playback position must be finite"));for(auto& state:m_playbacks)if(state.handle==handle){const auto* clip=Find(state.clip);if(!clip)return Status::Fail(TimelineError("PXANIM7511","Playback clip is missing"));state.position=std::clamp(position,0.0f,clip->duration);return apply?ApplyAt(*clip,state.position):Status::Ok();}return Status::Fail(TimelineError("PXANIM7512","Playback handle is unknown"));}
Status TimelinePlayer::Cancel(const PlaybackHandle handle){for(auto& state:m_playbacks)if(state.handle==handle&&state.playing){state.playing=false;if(m_completion)m_completion(handle,true);return Status::Ok();}return Status::Fail(TimelineError("PXANIM7512","Playback handle is unknown or stopped"));}
bool TimelinePlayer::Playing(const PlaybackHandle handle) const {return std::any_of(m_playbacks.begin(),m_playbacks.end(),[handle](const auto& state){return state.handle==handle&&state.playing;});}

Status TimelinePlayer::ApplyAt(const AnimationClip& clip,const float position){std::vector<resource::ResourceId> stack;return ApplyAtRecursive(clip,position,stack);}
Status TimelinePlayer::ApplyAtRecursive(const AnimationClip& clip,const float position,std::vector<resource::ResourceId>& stack){Status status;if(std::find(stack.begin(),stack.end(),clip.id)!=stack.end())return Status::Fail(TimelineError("PXANIM7517","Nested animation cycle detected"));stack.push_back(clip.id);if(m_apply)for(const auto& track:clip.tracks){const Status applied=m_apply(track.binding,Sample(track,position));for(const auto& diagnostic:applied.Diagnostics())status.Add(diagnostic);}for(const auto& nested:clip.nested){if(position<nested.start)continue;const auto* child=Find(nested.clip);if(!child){status.Add(TimelineError("PXANIM7518","Nested animation clip is not registered"));continue;}float childTime=(position-nested.start)*nested.speed;if(child->loop&&child->duration>0.0f)childTime=std::fmod(childTime,child->duration);else childTime=std::clamp(childTime,0.0f,child->duration);const Status applied=ApplyAtRecursive(*child,childTime,stack);for(const auto& diagnostic:applied.Diagnostics())status.Add(diagnostic);}stack.pop_back();return status;}
void TimelinePlayer::EmitMarkers(const AnimationClip& clip,const float from,const float to){if(!m_event)return;for(const auto& marker:clip.markers)if(marker.time>from&&marker.time<=to)m_event(marker);}

void TimelinePlayer::Update(const float deltaSeconds){if(!std::isfinite(deltaSeconds)||deltaSeconds<0.0f)return;for(auto& state:m_playbacks){if(!state.playing)continue;const auto* clip=Find(state.clip);if(!clip){state.playing=false;continue;}const float previous=state.position;state.position+=deltaSeconds*state.speed;if(clip->loop&&clip->duration>0.0f){while(state.position>=clip->duration){EmitMarkers(*clip,previous,clip->duration);state.position-=clip->duration;++state.loopIteration;}EmitMarkers(*clip,0.0f,state.position);}else{const float target=std::min(state.position,clip->duration);EmitMarkers(*clip,previous,target);state.position=target;if(state.position>=clip->duration){state.playing=false;if(m_completion)m_completion(state.handle,false);}}(void)ApplyAt(*clip,state.position);}}
void TimelinePlayer::Clear(){for(auto& state:m_playbacks)if(state.playing&&m_completion)m_completion(state.handle,true);m_playbacks.clear();}
Status TimelinePlayer::RestoreState(std::vector<PlaybackState> state,const bool apply){std::unordered_set<PlaybackHandle> handles;for(const auto& playback:state){const auto* clip=Find(playback.clip);if(!clip)return Status::Fail(TimelineError("PXANIM7513","Saved playback references a missing clip"));if(!playback.handle||!handles.insert(playback.handle).second||!std::isfinite(playback.position)||!std::isfinite(playback.speed)||playback.speed<=0.0f||playback.position<0.0f||playback.position>clip->duration)return Status::Fail(TimelineError("PXANIM7514","Saved playback state is invalid"));}m_playbacks=std::move(state);for(const auto& playback:m_playbacks)m_nextHandle=std::max(m_nextHandle,playback.handle+1);if(apply)for(const auto& playback:m_playbacks){const Status status=ApplyAt(*Find(playback.clip),playback.position);if(!status)return status;}return Status::Ok();}

std::vector<AnimationClip> OfficialPresets(){std::vector<AnimationClip> clips;const auto add=[&](const char* c,const char* n,const char* p,Variant a,Variant b,float d=0.45f,bool loop=false){clips.push_back(Preset(c,n,p,std::move(a),std::move(b),d,loop));};
    for(const char* name:{"typewriter","fade","slide","pop","shake","wave","rainbow","glitch"})add("Text",name,name,0.0,1.0,name==std::string("typewriter")?1.2f:0.6f,name==std::string("wave")||name==std::string("rainbow"));
    for(const char* name:{"enter-left","enter-right","exit-left","exit-right","fade","hop","shake","breathing","expression-crossfade","blink","lip-sync"})add("Actor",name,name,0.0,1.0,0.55f,name==std::string("breathing")||name==std::string("blink")||name==std::string("lip-sync"));
    for(const char* name:{"shake","flash","fade","zoom","pan","blur","vignette","color-grade","rule-dissolve"})add("Screen",name,name,0.0,1.0,0.5f,false);
    for(const char* name:{"panel-slide","panel-fade","panel-scale","modal-open","modal-close","button-hover","button-press"})
        add("UI",name,name,0.0,1.0,0.25f,false);
    return clips;
}

std::string WriteAnimationClip(const AnimationClip& clip) {
    Json root{{"format", "PrismatiXAnimation"},
              {"schemaRevision", 2},
              {"version", 4},
              {"id", clip.id.ToString()},
              {"name", clip.name},
              {"duration", clip.duration},
              {"loop", clip.loop}};
    root["tracks"] = Json::array();
    for (const auto& track : clip.tracks) {
        Json item{{"binding",
                   {{"kind", KindName(track.binding.kind)},
                    {"target", track.binding.target},
                    {"property", track.binding.property}}}};
        item["keys"] = Json::array();
        for (const auto& key : track.keys)
            item["keys"].push_back({{"time", key.time},
                                     {"value", ValueJson(key.value)},
                                     {"curve", CurveName(key.curve)}});
        root["tracks"].push_back(std::move(item));
    }
    root["markers"] = Json::array();
    for (const auto& marker : clip.markers) {
        Json payload = Json::object();
        for (const auto& [name, value] : marker.payload)
            payload[name] = ValueJson(value);
        root["markers"].push_back({{"time", marker.time},
                                    {"name", marker.name},
                                    {"payload", std::move(payload)}});
    }
    root["nested"] = Json::array();
    for (const auto& nested : clip.nested)
        root["nested"].push_back({{"start", nested.start},
                                   {"clip", nested.clip.ToString()},
                                   {"speed", nested.speed}});
    return root.dump(2) + "\n";
}

Result<AnimationClip> ParseAnimationClip(const std::string_view text,
                                         const std::string& sourcePath) {
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() ||
        !OnlyFields(root, {"format", "schemaRevision", "version", "id",
                           "name", "duration", "loop", "tracks", "markers",
                           "nested"}) ||
        root.size() != 10 ||
        root.value("format", std::string{}) != "PrismatiXAnimation" ||
        root.value("schemaRevision", 0) != 2 || root.value("version", 0) != 4)
        return Result<AnimationClip>::Failure(TimelineError(
            "PXANIM7520",
            "Animation must be strict PrismatiXAnimation schema 2 version 4",
            sourcePath));
    try {
        if (!root["name"].is_string() ||
            root["name"].get_ref<const std::string&>().size() > 512 ||
            !root["duration"].is_number() || !root["loop"].is_boolean() ||
            !root["tracks"].is_array() || root["tracks"].size() > 100'000 ||
            !root["markers"].is_array() || root["markers"].size() > 100'000 ||
            !root["nested"].is_array() || root["nested"].size() > 100'000)
            return Result<AnimationClip>::Failure(TimelineError(
                "PXANIM7526", "Animation fields are malformed", sourcePath));
        AnimationClip clip;
        const auto id = Uuid::Parse(root.at("id").get<std::string>());
        if (!id)
            return Result<AnimationClip>::Failure(TimelineError(
                "PXANIM7521", "Animation id is invalid", sourcePath));
        clip.id = *id;
        clip.name = root.at("name").get<std::string>();
        clip.duration = root.at("duration").get<float>();
        clip.loop = root.at("loop").get<bool>();
        for (const auto& item : root.at("tracks")) {
            if (!OnlyFields(item, {"binding", "keys"}) || item.size() != 2 ||
                !item.contains("binding") || !item.contains("keys") ||
                !OnlyFields(item["binding"], {"kind", "target", "property"}) ||
                item["binding"].size() != 3 || !item["keys"].is_array() ||
                item["keys"].empty() || item["keys"].size() > 100'000)
                return Result<AnimationClip>::Failure(TimelineError(
                    "PXANIM7522", "Animation track is malformed", sourcePath));
            const auto& binding = item.at("binding");
            const auto kind = ParseKind(binding.at("kind").get<std::string>());
            const std::string target = binding.at("target").get<std::string>();
            const std::string property =
                binding.at("property").get<std::string>();
            if (!kind || target.empty() || target.size() > 512 ||
                property.empty() || property.size() > 512)
                return Result<AnimationClip>::Failure(TimelineError(
                    "PXANIM7522", "Animation target binding is invalid",
                    sourcePath));
            Track track{{*kind, target, property}, {}};
            for (const auto& key : item.at("keys")) {
                if (!OnlyFields(key, {"time", "value", "curve"}) ||
                    key.size() != 3 || !key["time"].is_number() ||
                    !key["curve"].is_string())
                    return Result<AnimationClip>::Failure(TimelineError(
                        "PXANIM7523", "Animation key is malformed", sourcePath));
                auto value = ParseValue(key.at("value"));
                auto curve =
                    ParseCurve(key.at("curve").get<std::string>());
                if (!value || !curve)
                    return Result<AnimationClip>::Failure(TimelineError(
                        "PXANIM7523", "Animation key is invalid", sourcePath));
                track.keys.push_back({key.at("time").get<float>(),
                                      std::move(*value), *curve});
            }
            clip.tracks.push_back(std::move(track));
        }
        for (const auto& item : root.at("markers")) {
            if (!OnlyFields(item, {"time", "name", "payload"}) ||
                item.size() != 3 || !item["time"].is_number() ||
                !item["name"].is_string() ||
                item["name"].get_ref<const std::string&>().empty() ||
                !item["payload"].is_object() ||
                item["payload"].size() > 100'000)
                return Result<AnimationClip>::Failure(TimelineError(
                    "PXANIM7524", "Animation marker is malformed", sourcePath));
            Marker marker;
            marker.time = item.at("time").get<float>();
            marker.name = item.at("name").get<std::string>();
            for (auto it = item.at("payload").begin();
                 it != item.at("payload").end(); ++it) {
                auto value = ParseValue(it.value());
                if (!value)
                    return Result<AnimationClip>::Failure(TimelineError(
                        "PXANIM7524", "Animation marker payload is invalid",
                        sourcePath));
                marker.payload[it.key()] = std::move(*value);
            }
            clip.markers.push_back(std::move(marker));
        }
        for (const auto& item : root.at("nested")) {
            if (!OnlyFields(item, {"start", "clip", "speed"}) ||
                item.size() != 3 || !item["start"].is_number() ||
                !item["clip"].is_string() || !item["speed"].is_number())
                return Result<AnimationClip>::Failure(TimelineError(
                    "PXANIM7525", "Nested animation is malformed", sourcePath));
            const auto idValue =
                Uuid::Parse(item.at("clip").get<std::string>());
            if (!idValue)
                return Result<AnimationClip>::Failure(TimelineError(
                    "PXANIM7525", "Nested clip id is invalid", sourcePath));
            clip.nested.push_back({item.at("start").get<float>(), *idValue,
                                   item.at("speed").get<float>()});
        }
        const Status valid = clip.Validate(sourcePath);
        return valid ? Result<AnimationClip>::Success(std::move(clip))
                     : Result<AnimationClip>::Failure(valid.Diagnostics());
    } catch (const Json::exception& error) {
        return Result<AnimationClip>::Failure(TimelineError(
            "PXANIM7526",
            std::string("Animation fields are malformed: ") + error.what(),
            sourcePath));
    }
}

Result<TimelineDocument> ParseTimeline(const std::string_view text,
                                       const std::string& sourcePath) {
    const Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() ||
        !OnlyFields(root, {"format", "schemaRevision", "id", "name",
                           "duration", "tracks", "markers", "nestedClips"}) ||
        root.value("format", std::string{}) != "PrismatiXTimeline" ||
        root.value("schemaRevision", 0) != 2 || !root.contains("id") ||
        !root.contains("duration") || !root.contains("tracks") ||
        !root.contains("markers") || !root.contains("nestedClips"))
        return Result<TimelineDocument>::Failure(TimelineError(
            "PXTIMELINE7601",
            "Timeline must be strict PrismatiXTimeline schema 2", sourcePath));
    try {
        if (!root["id"].is_string() ||
            !Identifier(root["id"].get_ref<const std::string&>()) ||
            (root.contains("name") &&
             (!root["name"].is_string() ||
              root["name"].get_ref<const std::string&>().size() > 512)) ||
            !root["duration"].is_number() || !root["tracks"].is_array() ||
            root["tracks"].size() > 100'000 || !root["markers"].is_array() ||
            root["markers"].size() > 100'000 ||
            !root["nestedClips"].is_array() ||
            root["nestedClips"].size() > 100'000)
            return Result<TimelineDocument>::Failure(TimelineError(
                "PXTIMELINE7602", "Timeline fields are malformed", sourcePath));
        TimelineDocument document;
        document.id = root["id"].get<std::string>();
        document.clip.id =
            Uuid::FromName("PrismatiX.Timeline." + document.id);
        document.clip.name = root.value("name", document.id);
        document.clip.duration = root["duration"].get<float>();
        document.clip.loop = false;
        if (!std::isfinite(document.clip.duration) ||
            document.clip.duration < 0.0f || document.clip.duration > 86'400.0f)
            return Result<TimelineDocument>::Failure(TimelineError(
                "PXTIMELINE7603", "Timeline duration is invalid", sourcePath));

        std::unordered_set<std::string> trackIds;
        for (const auto& item : root["tracks"]) {
            if (!OnlyFields(item, {"id", "binding", "keyframes"}) ||
                item.size() != 3 || !item["id"].is_string() ||
                !Identifier(item["id"].get_ref<const std::string&>()) ||
                !trackIds.insert(item["id"].get<std::string>()).second ||
                !OnlyFields(item["binding"], {"kind", "target", "property"}) ||
                item["binding"].size() != 3 ||
                !item["keyframes"].is_array() || item["keyframes"].empty() ||
                item["keyframes"].size() > 100'000)
                return Result<TimelineDocument>::Failure(TimelineError(
                    "PXTIMELINE7604", "Timeline track is invalid", sourcePath));
            const auto& binding = item["binding"];
            const auto kind = ParseKind(binding.at("kind").get<std::string>());
            const std::string target = binding.at("target").get<std::string>();
            const std::string property =
                binding.at("property").get<std::string>();
            if (!kind || target.empty() || target.size() > 512 ||
                property.empty() || property.size() > 512)
                return Result<TimelineDocument>::Failure(TimelineError(
                    "PXTIMELINE7605", "Timeline binding is invalid", sourcePath));
            Track track{{*kind, target, property}, {}};
            for (const auto& key : item["keyframes"]) {
                if (!OnlyFields(key, {"time", "value", "easing"}) ||
                    key.size() != 3 || !key["time"].is_number() ||
                    !key["easing"].is_string())
                    return Result<TimelineDocument>::Failure(TimelineError(
                        "PXTIMELINE7606", "Timeline keyframe is malformed",
                        sourcePath));
                auto value = ParseValue(key["value"]);
                const auto curve =
                    ParseCurve(key["easing"].get<std::string>());
                if (!value || !curve)
                    return Result<TimelineDocument>::Failure(TimelineError(
                        "PXTIMELINE7606", "Timeline keyframe is invalid",
                        sourcePath));
                track.keys.push_back({key["time"].get<float>(),
                                      std::move(*value), *curve});
            }
            document.clip.tracks.push_back(std::move(track));
        }

        std::unordered_set<std::string> markerIds;
        for (const auto& item : root["markers"]) {
            if (!OnlyFields(item, {"id", "time", "name", "payload"}) ||
                item.size() != 4 || !item["id"].is_string() ||
                !Identifier(item["id"].get_ref<const std::string&>()) ||
                !markerIds.insert(item["id"].get<std::string>()).second ||
                !item["time"].is_number() || !item["name"].is_string() ||
                item["name"].get_ref<const std::string&>().empty() ||
                item["name"].get_ref<const std::string&>().size() > 512 ||
                !item["payload"].is_object() ||
                item["payload"].size() > 100'000)
                return Result<TimelineDocument>::Failure(TimelineError(
                    "PXTIMELINE7607", "Timeline marker is invalid", sourcePath));
            Marker marker;
            marker.id = item["id"].get<std::string>();
            marker.time = item["time"].get<float>();
            marker.name = item["name"].get<std::string>();
            for (const auto& [name, payload] : item["payload"].items()) {
                auto value = ParseValue(payload);
                if (!value)
                    return Result<TimelineDocument>::Failure(TimelineError(
                        "PXTIMELINE7608", "Timeline marker payload is invalid",
                        sourcePath));
                marker.payload.emplace(name, std::move(*value));
            }
            document.clip.markers.push_back(std::move(marker));
        }

        for (const auto& item : root["nestedClips"]) {
            if (!OnlyFields(item, {"start", "clip", "speed"}) ||
                item.size() != 3 || !item["start"].is_number() ||
                !item["speed"].is_number())
                return Result<TimelineDocument>::Failure(TimelineError(
                    "PXTIMELINE7609", "Nested Timeline clip is malformed",
                    sourcePath));
            auto resource = ParseResourceRef(item["clip"]);
            const float start = item["start"].get<float>();
            const float speed = item["speed"].get<float>();
            if (!resource || !std::isfinite(start) || start < 0.0f ||
                start > document.clip.duration || !std::isfinite(speed) ||
                speed <= 0.0f)
                return Result<TimelineDocument>::Failure(TimelineError(
                    "PXTIMELINE7609", "Nested Timeline clip is invalid",
                    sourcePath));
            document.nestedClips.push_back(
                {start, std::move(*resource), speed});
        }
        const Status valid = document.clip.Validate(sourcePath);
        return valid ? Result<TimelineDocument>::Success(std::move(document))
                     : Result<TimelineDocument>::Failure(valid.Diagnostics());
    } catch (const Json::exception& error) {
        return Result<TimelineDocument>::Failure(TimelineError(
            "PXTIMELINE7610",
            std::string("Timeline fields are malformed: ") + error.what(),
            sourcePath));
    }
}

}  // namespace px::animation
