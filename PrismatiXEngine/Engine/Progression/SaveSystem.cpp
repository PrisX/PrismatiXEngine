#include "Engine/Progression/SaveSystem.h"

#include "Engine/Progression/Persist.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <filesystem>
#include <limits>
#include <stdexcept>

namespace px::progress {

namespace {
constexpr std::string_view kSaveFormat = "PrismatiXSave";
constexpr int kSaveSchemaRevision = 4;
constexpr int kOldestSaveSchemaRevision = 4;
constexpr std::size_t kMaxSaveCollectionItems = 1'000'000;

void SaveLoadError(const std::string& path, std::string message, std::string details = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXSAVE6101",
                                .category="Persistence.Save",.message=std::move(message),
                                .details=std::move(details)};
    diagnostic.source.path=path;diag::Emit(std::move(diagnostic));
}

bool HasSupportedHeader(const Json& json) {
    if (!json.is_object() ||
        json.value("format", std::string{}) != kSaveFormat)
        return false;
    const int revision = json.value("schemaRevision", 0);
    return revision >= kOldestSaveSchemaRevision &&
           revision <= kSaveSchemaRevision;
}

Json ColorToJson(const Color color) {
    return Json::array({color.r, color.g, color.b, color.a});
}

Color ColorFromJson(const Json& json, const Color fallback) {
    if (!json.is_array() || json.size() != 4) {
        return fallback;
    }
    Color color;
    const auto channel = [&json](const std::size_t index) {
        const int value = json.at(index).get<int>();
        if (value < 0 || value > 255) {
            throw std::out_of_range("color channel must be in the range 0..255");
        }
        return static_cast<std::uint8_t>(value);
    };
    color.r = channel(0);
    color.g = channel(1);
    color.b = channel(2);
    color.a = channel(3);
    return color;
}

const char* VMStateName(const vn::VMState state) {
    switch (state) {
        case vn::VMState::Idle: return "idle";
        case vn::VMState::Running: return "running";
        case vn::VMState::WaitingClick: return "waiting-click";
        case vn::VMState::WaitingChoice: return "waiting-choice";
        case vn::VMState::WaitingTimer: return "waiting-timer";
        case vn::VMState::WaitingVideo: return "waiting-video";
        case vn::VMState::WaitingExternal: return "waiting-external";
        case vn::VMState::Paused: return "paused";
        case vn::VMState::Finished: return "finished";
    }
    return "idle";
}

vn::VMState VMExecutionStateFromJson(const Json& json) {
    const std::string value = json.get<std::string>();
    if (value == "idle") return vn::VMState::Idle;
    if (value == "running") return vn::VMState::Running;
    if (value == "waiting-click") return vn::VMState::WaitingClick;
    if (value == "waiting-choice") return vn::VMState::WaitingChoice;
    if (value == "waiting-timer") return vn::VMState::WaitingTimer;
    if (value == "waiting-video") return vn::VMState::WaitingVideo;
    if (value == "waiting-external") return vn::VMState::WaitingExternal;
    if (value == "paused") return vn::VMState::Paused;
    if (value == "finished") return vn::VMState::Finished;
    throw std::invalid_argument("unknown VM state: " + value);
}

Json VMStateToJson(const vn::VMRuntimeState& state) {
    Json callStack = Json::array();
    for (const auto& frame : state.callStack) {
        callStack.push_back({{"script", frame.script}, {"pc", frame.pc}});
    }
    Json choices = Json::array();
    for (const auto& choice : state.choices) {
        choices.push_back({{"text", choice.text}, {"target", choice.target},
                           {"sourceId", choice.sourceId},
                           {"operationId", choice.operationId}});
    }
    return {{"scriptPath", state.scriptPath},
            {"pc", state.pc},
            {"state", VMStateName(state.state)},
            {"callStack", std::move(callStack)},
            {"choices", std::move(choices)},
            {"speaker", state.speaker},
            {"pendingVoice", state.pendingVoice},
            {"textColor", ColorToJson(state.textColor)},
            {"outlineColor", ColorToJson(state.outlineColor)},
            {"textSpeed", state.textSpeed},
            {"textEffect", state.textEffect},
            {"chapter", state.chapter},
            {"currentBgm", state.currentBgm},
            {"timerRemainingMs", state.timerRemainingMs},
            {"currentLineSeen", state.currentLineSeen}};
}

vn::VMRuntimeState VMStateFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("vm must be an object");
    vn::VMRuntimeState state;
    state.scriptPath = json.value("scriptPath", std::string{});
    state.pc = json.value("pc", 0);
    state.state = VMExecutionStateFromJson(json.value("state", Json("idle")));
    const Json& stack = json.value("callStack", Json::array());
    if (!stack.is_array() || stack.size() > kMaxSaveCollectionItems) {
        throw std::invalid_argument("vm.callStack is not a valid bounded array");
    }
    for (const auto& frame : stack) {
        if (!frame.is_object()) throw std::invalid_argument("VM call frame must be an object");
        state.callStack.push_back(
            {frame.value("script", std::string{}), frame.value("pc", 0)});
    }
    const Json& choices = json.value("choices", Json::array());
    if (!choices.is_array() || choices.size() > kMaxSaveCollectionItems) {
        throw std::invalid_argument("vm.choices is not a valid bounded array");
    }
    for (const auto& choice : choices) {
        if (!choice.is_object()) throw std::invalid_argument("VM choice must be an object");
        state.choices.push_back(
            {choice.value("text", std::string{}), choice.value("target", std::string{}),
             choice.value("sourceId", std::string{}),
             choice.value("operationId", std::string{})});
    }
    state.speaker = json.value("speaker", std::string{});
    state.pendingVoice = json.value("pendingVoice", std::string{});
    state.textColor = ColorFromJson(json.value("textColor", Json::array()), state.textColor);
    state.outlineColor =
        ColorFromJson(json.value("outlineColor", Json::array()), state.outlineColor);
    state.textSpeed = json.value("textSpeed", state.textSpeed);
    state.textEffect = json.value("textEffect", std::string{});
    state.chapter = json.value("chapter", std::string{});
    state.currentBgm = json.value("currentBgm", std::string{});
    state.timerRemainingMs = json.value("timerRemainingMs", std::uint64_t{0});
    state.currentLineSeen = json.value("currentLineSeen", true);
    return state;
}

Json DialogueToJson(const vn::DialogueSnapshot& snapshot) {
    const auto& state = snapshot.state;
    return {{"speaker", state.speaker},
            {"fullText", state.fullText},
            {"displayText", state.displayText},
            {"voice", state.voice},
            {"currentChar", state.currentChar},
            {"totalChars", state.totalChars},
            {"finished", state.finished},
            {"textColor", ColorToJson(state.textColor)},
            {"outlineColor", ColorToJson(state.outlineColor)},
            {"effect", state.effect},
            {"effectProgress", state.effectProgress},
            {"speedMs", snapshot.speedMs}};
}

Json RouteStateToJson(const ui::RouteState& state) {
    return {{"stack", state.stack}, {"modals", state.modals}};
}

ui::RouteState RouteStateFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("routes must be an object");
    ui::RouteState state;
    const auto readRoutes = [](const Json& values, std::vector<std::string>& output) {
        if (!values.is_array() || values.size() > kMaxSaveCollectionItems) {
            throw std::invalid_argument("route state must be a bounded array");
        }
        for (const auto& value : values) {
            if (!value.is_string()) throw std::invalid_argument("route id must be a string");
            output.push_back(value.get<std::string>());
        }
    };
    readRoutes(json.value("stack", Json::array()), state.stack);
    readRoutes(json.value("modals", Json::array()), state.modals);
    return state;
}

Json RuntimeValueToJson(const vn::Value& value, const int depth = 0) {
    if (depth > 64) throw std::invalid_argument("variable value nesting exceeds 64 levels");
    switch (value.Type()) {
        case VariantType::Null: return nullptr;
        case VariantType::Bool: return *value.TryGet<bool>();
        case VariantType::Integer: return *value.TryGet<std::int64_t>();
        case VariantType::Number: return *value.TryGet<double>();
        case VariantType::String: return *value.TryGet<std::string>();
        case VariantType::Array: {
            const auto* array = value.AsArray();
            if (array->size() > kMaxSaveCollectionItems) {
                throw std::invalid_argument("variable list exceeds save limits");
            }
            Json json = Json::array();
            for (const auto& item : *array) json.push_back(RuntimeValueToJson(item, depth + 1));
            return json;
        }
        case VariantType::Object: {
            const auto* object = value.AsObject();
            if (object->size() > kMaxSaveCollectionItems) {
                throw std::invalid_argument("variable map exceeds save limits");
            }
            Json json = Json::object();
            for (const auto& [name, item] : *object) {
                json[name] = RuntimeValueToJson(item, depth + 1);
            }
            return json;
        }
        default:
            throw std::invalid_argument("runtime variables cannot contain editor resource types");
    }
}

vn::Value RuntimeValueFromJson(const Json& json, const int depth = 0) {
    if (depth > 64) throw std::invalid_argument("variable value nesting exceeds 64 levels");
    if (json.is_null()) return vn::Value{};
    if (json.is_boolean()) return vn::Value(json.get<bool>());
    if (json.is_number_integer()) return vn::Value(json.get<std::int64_t>());
    if (json.is_number_unsigned()) {
        const auto value = json.get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            throw std::out_of_range("unsigned variable value exceeds int64");
        }
        return vn::Value(static_cast<std::int64_t>(value));
    }
    if (json.is_number_float()) return vn::Value(json.get<double>());
    if (json.is_string()) return vn::Value(json.get<std::string>());
    if (json.is_array()) {
        if (json.size() > kMaxSaveCollectionItems) {
            throw std::invalid_argument("variable list exceeds save limits");
        }
        vn::ValueList array;
        array.reserve(json.size());
        for (const auto& item : json) array.push_back(RuntimeValueFromJson(item, depth + 1));
        return vn::Value(std::move(array));
    }
    if (json.is_object()) {
        if (json.size() > kMaxSaveCollectionItems) {
            throw std::invalid_argument("variable map exceeds save limits");
        }
        vn::ValueMap object;
        for (auto item = json.begin(); item != json.end(); ++item) {
            object.emplace(item.key(), RuntimeValueFromJson(item.value(), depth + 1));
        }
        return vn::Value(std::move(object));
    }
    throw std::invalid_argument("unsupported runtime variable JSON value");
}

Json TimelinesToJson(const std::vector<animation::PlaybackState>& timelines) {
    Json values = Json::array();
    for (const auto& state : timelines) {
        values.push_back({{"handle", state.handle},
                          {"clip", state.clip.ToString()},
                          {"position", state.position},
                          {"speed", state.speed},
                          {"loopIteration", state.loopIteration},
                          {"playing", state.playing},
                          {"awaiting", state.awaiting}});
    }
    return values;
}

std::vector<animation::PlaybackState> TimelinesFromJson(const Json& json) {
    if (!json.is_array() || json.size() > kMaxSaveCollectionItems) {
        throw std::invalid_argument("timelines must be a bounded array");
    }
    std::vector<animation::PlaybackState> timelines;
    timelines.reserve(json.size());
    for (const auto& value : json) {
        if (!value.is_object()) throw std::invalid_argument("timeline state must be an object");
        const auto clip = Uuid::Parse(value.value("clip", std::string{}));
        if (!clip) throw std::invalid_argument("timeline clip id is invalid");
        animation::PlaybackState state;
        state.handle = value.value("handle", animation::PlaybackHandle{0});
        state.clip = *clip;
        state.position = value.value("position", 0.0f);
        state.speed = value.value("speed", 1.0f);
        state.loopIteration = value.value("loopIteration", std::uint64_t{0});
        state.playing = value.value("playing", false);
        state.awaiting = value.value("awaiting", false);
        timelines.push_back(state);
    }
    return timelines;
}

Json AnimationClipsToJson(const std::vector<animation::AnimationClip>& clips) {
    if (clips.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("animation clip collection exceeds save limits");
    Json values = Json::array();
    for (const auto& clip : clips) values.push_back(animation::WriteAnimationClip(clip));
    return values;
}

std::vector<animation::AnimationClip> AnimationClipsFromJson(const Json& json) {
    if (!json.is_array() || json.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("animationClips must be a bounded array");
    std::vector<animation::AnimationClip> clips;
    clips.reserve(json.size());
    for (const auto& encoded : json) {
        if (!encoded.is_string()) throw std::invalid_argument("saved animation clip must be text");
        auto parsed = animation::ParseAnimationClip(encoded.get<std::string>(), "save.animationClip");
        if (!parsed) throw std::invalid_argument("saved animation clip is invalid");
        clips.push_back(parsed.TakeValue());
    }
    return clips;
}

Json VariantToSaveJson(const Variant& value, int depth);
Variant VariantFromSaveJson(const Json& json, int depth);

Json ScriptJournalToJson(const script::EngineOperationJournal& journal) {
    Json values = Json::array();
    for (const auto& entry : journal) {
        values.push_back({
            {"operation", entry.operation},
            {"arguments", VariantToSaveJson(Variant(entry.arguments), 0)},
            {"result", VariantToSaveJson(entry.result, 0)},
            {"resultUndefined", entry.resultUndefined}});
    }
    return values;
}

script::EngineOperationJournal ScriptJournalFromJson(const Json& json) {
    if (!json.is_array() || json.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument(
            "Script operation journal must be a bounded array");
    script::EngineOperationJournal journal;
    journal.reserve(json.size());
    for (const auto& encoded : json) {
        if (!encoded.is_object())
            throw std::invalid_argument(
                "Script operation journal entry must be an object");
        script::EngineOperationJournalEntry entry;
        entry.operation = encoded.at("operation").get<std::string>();
        Variant arguments = VariantFromSaveJson(encoded.at("arguments"), 0);
        if (!arguments.AsArray())
            throw std::invalid_argument(
                "Script journal arguments must be an array");
        entry.arguments = *arguments.AsArray();
        entry.result = VariantFromSaveJson(encoded.at("result"), 0);
        entry.resultUndefined = encoded.at("resultUndefined").get<bool>();
        journal.push_back(std::move(entry));
    }
    return journal;
}

Json ScriptPendingToJson(const script::PendingCommandsState& state) {
    Json values = Json::array();
    for (const auto& pending : state) {
        Json arguments = Json::array();
        for (const auto& argument : pending.command.args)
            arguments.push_back({{"key", argument.key}, {"value", argument.value}});
        values.push_back({{"sourceId", pending.sourceId},
                          {"command", pending.command.type}, {"arguments", std::move(arguments)},
                          {"typedArguments", VariantToSaveJson(
                              Variant(pending.command.typedArgs), 0)},
                          {"line", pending.command.line}, {"yieldIndex", pending.yieldIndex},
                          {"waitKind", pending.waitKind}, {"handle", pending.handle},
                          {"remainingSeconds", pending.remainingSeconds},
                          {"journal", ScriptJournalToJson(pending.journal)}});
    }
    return values;
}

script::PendingCommandsState ScriptPendingFromJson(const Json& json) {
    if (!json.is_array() || json.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("scriptPending must be a bounded array");
    script::PendingCommandsState state;
    for (const auto& value : json) {
        if (!value.is_object()) throw std::invalid_argument("Script checkpoint must be an object");
        script::PendingCommandState pending;
        pending.sourceId = value.at("sourceId").get<std::string>();
        pending.command.type = value.at("command").get<std::string>();
        pending.command.line = value.value("line", 0);
        const auto& arguments = value.at("arguments");
        if (!arguments.is_array() || arguments.size() > kMaxSaveCollectionItems)
            throw std::invalid_argument("Script checkpoint arguments must be a bounded array");
        for (const auto& argument : arguments) {
            if (!argument.is_object()) throw std::invalid_argument("Script argument must be an object");
            pending.command.args.push_back({argument.at("key").get<std::string>(),
                                             argument.at("value").get<std::string>()});
        }
        Variant typedArguments =
            VariantFromSaveJson(value.at("typedArguments"), 0);
        if (!typedArguments.AsObject())
            throw std::invalid_argument(
                "Script checkpoint typedArguments must be an object");
        pending.command.typedArgs = *typedArguments.AsObject();
        pending.yieldIndex = value.at("yieldIndex").get<std::uint32_t>();
        pending.waitKind = value.at("waitKind").get<std::string>();
        pending.handle = value.value("handle", std::uint64_t{0});
        pending.remainingSeconds = value.value("remainingSeconds", 0.0f);
        pending.journal = ScriptJournalFromJson(value.at("journal"));
        state.push_back(std::move(pending));
    }
    return state;
}

Json VariantToSaveJson(const Variant& value, const int depth = 0) {
    if (depth > 64) throw std::invalid_argument("Variant checkpoint nesting exceeds limits");
    switch (value.Type()) {
        case VariantType::Null: return Json{{"type", "null"}};
        case VariantType::Bool: return Json{{"type", "bool"}, {"value", *value.TryGet<bool>()}};
        case VariantType::Integer: return Json{{"type", "integer"}, {"value", *value.TryGet<std::int64_t>()}};
        case VariantType::Number: return Json{{"type", "number"}, {"value", *value.TryGet<double>()}};
        case VariantType::String: return Json{{"type", "string"}, {"value", *value.TryGet<std::string>()}};
        case VariantType::Vec2: { const auto v=*value.TryGet<Vec2>(); return Json{{"type","vec2"},{"value",Json::array({v.x,v.y})}}; }
        case VariantType::Rect: { const auto v=*value.TryGet<Rect>(); return Json{{"type","rect"},{"value",Json::array({v.x,v.y,v.w,v.h})}}; }
        case VariantType::Color: return Json{{"type","color"},{"value",ColorToJson(*value.TryGet<Color>())}};
        case VariantType::Uuid: return Json{{"type","uuid"},{"value",value.TryGet<Uuid>()->ToString()}};
        case VariantType::ResourceRef: { const auto& ref=*value.TryGet<ResourceRefValue>(); return Json{{"type","resource"},{"id",ref.id.ToString()},{"path",ref.lastKnownPath}}; }
        case VariantType::TokenRef: return Json{{"type","token"},{"value",value.TryGet<TokenRefValue>()->name}};
        case VariantType::Array: {
            const auto* array=value.AsArray();if(!array||array->size()>kMaxSaveCollectionItems)throw std::invalid_argument("Variant array exceeds save limits");
            Json encoded=Json::array();for(const auto& item:*array)encoded.push_back(VariantToSaveJson(item,depth+1));
            return Json{{"type","array"},{"value",std::move(encoded)}};
        }
        case VariantType::Object: {
            const auto* object=value.AsObject();if(!object||object->size()>kMaxSaveCollectionItems)throw std::invalid_argument("Variant object exceeds save limits");
            Json encoded=Json::object();for(const auto& [name,item]:*object)encoded[name]=VariantToSaveJson(item,depth+1);
            return Json{{"type","object"},{"value",std::move(encoded)}};
        }
    }
    throw std::invalid_argument("Unknown Variant type in checkpoint");
}

Uuid RequiredUuid(const Json& value, const char* field) {
    if(!value.is_string())throw std::invalid_argument(std::string(field)+" must be a UUID string");
    const auto parsed=Uuid::Parse(value.get<std::string>());if(!parsed)throw std::invalid_argument(std::string(field)+" is not a UUID");return *parsed;
}

Variant VariantFromSaveJson(const Json& json, const int depth = 0) {
    if(depth>64||!json.is_object()||!json.contains("type")||!json["type"].is_string())throw std::invalid_argument("Variant checkpoint is malformed");
    const std::string type=json["type"].get<std::string>();
    if(type=="null")return Variant{};
    if(type=="bool")return Variant(json.at("value").get<bool>());
    if(type=="integer")return Variant(json.at("value").get<std::int64_t>());
    if(type=="number")return Variant(json.at("value").get<double>());
    if(type=="string")return Variant(json.at("value").get<std::string>());
    if(type=="vec2"){const auto& v=json.at("value");if(!v.is_array()||v.size()!=2)throw std::invalid_argument("Vec2 checkpoint is malformed");return Variant(Vec2{v[0].get<float>(),v[1].get<float>()});}
    if(type=="rect"){const auto& v=json.at("value");if(!v.is_array()||v.size()!=4)throw std::invalid_argument("Rect checkpoint is malformed");return Variant(Rect{v[0].get<float>(),v[1].get<float>(),v[2].get<float>(),v[3].get<float>()});}
    if(type=="color")return Variant(ColorFromJson(json.at("value"),Color{}));
    if(type=="uuid")return Variant(RequiredUuid(json.at("value"),"Variant UUID"));
    if(type=="resource")return Variant(ResourceRefValue{RequiredUuid(json.at("id"),"Resource id"),json.at("path").get<std::string>()});
    if(type=="token")return Variant(TokenRefValue{json.at("value").get<std::string>()});
    if(type=="array"){
        const auto& encoded=json.at("value");if(!encoded.is_array()||encoded.size()>kMaxSaveCollectionItems)throw std::invalid_argument("Variant array checkpoint is malformed");
        VariantArray result;result.reserve(encoded.size());for(const auto& item:encoded)result.push_back(VariantFromSaveJson(item,depth+1));return Variant(std::move(result));
    }
    if(type=="object"){
        const auto& encoded=json.at("value");if(!encoded.is_object()||encoded.size()>kMaxSaveCollectionItems)throw std::invalid_argument("Variant object checkpoint is malformed");
        VariantObject result;for(auto item=encoded.begin();item!=encoded.end();++item)result.emplace(item.key(),VariantFromSaveJson(item.value(),depth+1));return Variant(std::move(result));
    }
    throw std::invalid_argument("Unknown Variant checkpoint type: "+type);
}

Json VariantObjectToSaveJson(const VariantObject& object) {
    return VariantToSaveJson(Variant(object));
}

VariantObject VariantObjectFromSaveJson(const Json& json) {
    Variant value=VariantFromSaveJson(json);const auto* object=value.AsObject();
    if(!object)throw std::invalid_argument("Expected a Variant object checkpoint");return *object;
}

Json ActionContextToJson(const ui::ActionContext& context) {
    return {{"sourceScene",context.sourceScene},{"sourceNode",context.sourceNode.ToString()},
            {"signal",context.signal},{"currentRoute",context.currentRoute},{"preview",context.preview}};
}

ui::ActionContext ActionContextFromJson(const Json& json) {
    if(!json.is_object())throw std::invalid_argument("Action context must be an object");
    ui::ActionContext context;context.sourceScene=json.at("sourceScene").get<std::string>();
    context.sourceNode=RequiredUuid(json.at("sourceNode"),"Action source node");
    context.signal=json.at("signal").get<std::string>();context.currentRoute=json.at("currentRoute").get<std::string>();
    context.preview=json.at("preview").get<bool>();return context;
}

Json ScriptActionsToJson(const script::PendingActionsState& state) {
    Json values = Json::array();
    for (const auto& pending : state) {
        values.push_back({
            {"sourceId", pending.sourceId},
            {"id", pending.id},
            {"action", pending.invocation.action},
            {"arguments", VariantObjectToSaveJson(pending.invocation.arguments)},
            {"context", ActionContextToJson(pending.invocation.context)},
            {"yieldIndex", pending.yieldIndex},
            {"waitKind", pending.waitKind},
            {"handle", pending.handle},
            {"remainingSeconds", pending.remainingSeconds},
            {"journal", ScriptJournalToJson(pending.journal)}});
    }
    return values;
}

script::PendingActionsState ScriptActionsFromJson(const Json& json) {
    if(!json.is_array()||json.size()>kMaxSaveCollectionItems)throw std::invalid_argument("scriptActions must be a bounded array");
    script::PendingActionsState state;state.reserve(json.size());for(const auto& value:json){if(!value.is_object())throw std::invalid_argument("Script Action checkpoint must be an object");script::PendingActionState pending;
        pending.sourceId=value.at("sourceId").get<std::string>();pending.id=value.at("id").get<std::uint64_t>();pending.invocation.action=value.at("action").get<std::string>();
        pending.invocation.arguments=VariantObjectFromSaveJson(value.at("arguments"));pending.invocation.context=ActionContextFromJson(value.at("context"));
        pending.yieldIndex=value.at("yieldIndex").get<std::uint32_t>();pending.waitKind=value.at("waitKind").get<std::string>();
        pending.handle=value.at("handle").get<std::uint64_t>();pending.remainingSeconds=value.at("remainingSeconds").get<float>();pending.journal=ScriptJournalFromJson(value.at("journal"));state.push_back(std::move(pending));}return state;
}

Json ExtensionStateToJson(const script::ExtensionStates& state) {
    if (state.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument(
            "Extension state provider collection exceeds save limits");
    Json values = Json::array();
    for (const auto& provider : state) {
        if (provider.providerId.empty() || provider.providerId.size() > 128 ||
            provider.sourceId.size() > 128 || provider.version == 0)
            throw std::invalid_argument(
                "Extension state provider identity is invalid");
        values.push_back({{"sourceId", provider.sourceId},
                          {"providerId", provider.providerId},
                          {"version", provider.version},
                          {"state", RuntimeValueToJson(provider.state, 0)}});
    }
    return values;
}

script::ExtensionStates ExtensionStateFromJson(const Json& json) {
    if (!json.is_array() || json.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument(
            "extensionState must be a bounded array");
    script::ExtensionStates state;
    state.reserve(json.size());
    std::set<std::pair<std::string, std::string>> identities;
    for (const auto& value : json) {
        if (!value.is_object())
            throw std::invalid_argument(
                "Extension state provider must be an object");
        script::ExtensionStateSnapshot provider;
        provider.sourceId = value.at("sourceId").get<std::string>();
        provider.providerId = value.at("providerId").get<std::string>();
        provider.version = value.at("version").get<std::uint32_t>();
        provider.state = RuntimeValueFromJson(value.at("state"), 0);
        if (provider.sourceId.size() > 128 || provider.providerId.empty() ||
            provider.providerId.size() > 128 || provider.version == 0 ||
            !identities.emplace(provider.sourceId, provider.providerId).second)
            throw std::invalid_argument(
                "Extension state provider identity is invalid or duplicated");
        state.push_back(std::move(provider));
    }
    return state;
}

Json BehaviorStateToJson(const ui::BehaviorRuntimeState& state) {
    Json fibers=Json::array();for(const auto& fiber:state.fibers){Json continuation=Json::array();for(const auto& node:fiber.continuation)continuation.push_back(node.ToString());fibers.push_back({
        {"id",fiber.id},{"entry",fiber.entry.ToString()},{"current",fiber.current.ToString()},
        {"continuation",std::move(continuation)},{"delayRemaining",fiber.delayRemaining},
        {"actionExecution",fiber.actionExecution},{"animationHandle",fiber.animationHandle},
        {"signalArguments",VariantObjectToSaveJson(fiber.signalArguments)}});}
    Json actions=Json::array();for(const auto& action:state.actions)actions.push_back({
        {"execution",action.execution},{"action",action.invocation.action},
        {"arguments",VariantObjectToSaveJson(action.invocation.arguments)},
        {"context",ActionContextToJson(action.invocation.context)},
        {"provider",action.providerId},{"providerHandle",action.providerHandle},{"autoForget",action.autoForget}});
    return {{"fibers",std::move(fibers)},{"actions",std::move(actions)}};
}

ui::BehaviorRuntimeState BehaviorStateFromJson(const Json& json) {
    if(!json.is_object())throw std::invalid_argument("behavior must be an object");const auto& fibers=json.at("fibers");const auto& actions=json.at("actions");
    if(!fibers.is_array()||!actions.is_array()||fibers.size()>kMaxSaveCollectionItems||actions.size()>kMaxSaveCollectionItems)throw std::invalid_argument("behavior collections exceed save limits");
    ui::BehaviorRuntimeState state;for(const auto& value:fibers){if(!value.is_object())throw std::invalid_argument("Behavior fiber must be an object");ui::BehaviorFiberState fiber;
        fiber.id=value.at("id").get<ui::BehaviorFiberId>();fiber.entry=RequiredUuid(value.at("entry"),"Behavior entry");fiber.current=RequiredUuid(value.at("current"),"Behavior current node");
        const auto& continuation=value.at("continuation");if(!continuation.is_array()||continuation.size()>kMaxSaveCollectionItems)throw std::invalid_argument("Behavior continuation exceeds save limits");for(const auto& node:continuation)fiber.continuation.push_back(RequiredUuid(node,"Behavior continuation node"));
        fiber.delayRemaining=value.at("delayRemaining").get<float>();fiber.actionExecution=value.at("actionExecution").get<ui::ActionExecutionId>();fiber.animationHandle=value.at("animationHandle").get<std::uint64_t>();fiber.signalArguments=VariantObjectFromSaveJson(value.at("signalArguments"));state.fibers.push_back(std::move(fiber));}
    for(const auto& value:actions){if(!value.is_object())throw std::invalid_argument("Behavior Action checkpoint must be an object");ui::ActionExecutionCheckpoint action;
        action.execution=value.at("execution").get<ui::ActionExecutionId>();action.invocation.action=value.at("action").get<std::string>();action.invocation.arguments=VariantObjectFromSaveJson(value.at("arguments"));action.invocation.context=ActionContextFromJson(value.at("context"));
        action.providerId=value.at("provider").get<std::string>();action.providerHandle=value.at("providerHandle").get<std::uint64_t>();action.autoForget=value.at("autoForget").get<bool>();state.actions.push_back(std::move(action));}return state;
}

const char* VisualEaseName(const ui::VisualStateEase ease) {
    switch (ease) {
        case ui::VisualStateEase::Step: return "step";
        case ui::VisualStateEase::Linear: return "linear";
        case ui::VisualStateEase::EaseIn: return "easeIn";
        case ui::VisualStateEase::EaseOut: return "easeOut";
        case ui::VisualStateEase::EaseInOut: return "easeInOut";
        case ui::VisualStateEase::BackOut: return "backOut";
    }
    return "linear";
}

ui::VisualStateEase VisualEaseFromName(const std::string_view name) {
    if(name=="step")return ui::VisualStateEase::Step;
    if(name=="linear")return ui::VisualStateEase::Linear;
    if(name=="easeIn")return ui::VisualStateEase::EaseIn;
    if(name=="easeOut")return ui::VisualStateEase::EaseOut;
    if(name=="easeInOut")return ui::VisualStateEase::EaseInOut;
    if(name=="backOut")return ui::VisualStateEase::BackOut;
    throw std::invalid_argument("Visual State easing is invalid");
}

Json UIAnimationStateToJson(const ui::UIAnimationRuntimeState& state) {
    Json parameters=Json::object();
    for(const auto& [name,value]:state.parameters)
        parameters[name]=VariantToSaveJson(value);
    return {{"state",state.state.ToString()},
            {"transition",state.transition.ToString()},
            {"position",state.position},
            {"transitionProgress",state.transitionProgress},
            {"paused",state.paused},
            {"parameters",std::move(parameters)}};
}

ui::UIAnimationRuntimeState UIAnimationStateFromJson(const Json& json) {
    if(!json.is_object())throw std::invalid_argument("UI animation state must be an object");
    ui::UIAnimationRuntimeState state;
    state.state=RequiredUuid(json.at("state"),"UI animation state");
    state.transition=RequiredUuid(json.at("transition"),"UI animation transition");
    state.position=json.at("position").get<float>();
    state.transitionProgress=json.at("transitionProgress").get<float>();
    state.paused=json.at("paused").get<bool>();
    const auto& parameters=json.at("parameters");
    if(!parameters.is_object()||parameters.size()>kMaxSaveCollectionItems)
        throw std::invalid_argument("UI animation parameters exceed save limits");
    for(auto item=parameters.begin();item!=parameters.end();++item)
        state.parameters.emplace(item.key(),VariantFromSaveJson(item.value()));
    return state;
}

Json VisualStateToJson(const ui::VisualStateRuntimeState& state) {
    Json groups=Json::array();
    for(const auto& group:state.groups){
        Json transitionFrom=Json::array();
        for(const auto& value:group.transitionFrom)
            transitionFrom.push_back({{"node",value.node.ToString()},
                                      {"property",value.property},
                                      {"value",VariantToSaveJson(value.value)}});
        groups.push_back({{"group",group.group},{"state",group.state},
                          {"from",group.from},{"elapsed",group.elapsed},
                          {"duration",group.duration},
                          {"easing",VisualEaseName(group.easing)},
                          {"transitionFrom",std::move(transitionFrom)},
                          {"animationClip",group.animationClip
                              ? Json(group.animationClip->ToString()):Json(nullptr)},
                          {"animationPosition",group.animationPosition}});
    }
    return {{"groups",std::move(groups)}};
}

ui::VisualStateRuntimeState VisualStateFromJson(const Json& json) {
    if(!json.is_object()||!json.contains("groups")||!json["groups"].is_array()||
       json["groups"].size()>kMaxSaveCollectionItems)
        throw std::invalid_argument("Visual State checkpoint is malformed");
    ui::VisualStateRuntimeState state;
    for(const auto& encoded:json["groups"]){
        if(!encoded.is_object())throw std::invalid_argument("Visual State group checkpoint must be an object");
        ui::VisualStateGroupRuntimeState group;
        group.group=encoded.at("group").get<std::string>();
        group.state=encoded.at("state").get<std::string>();
        group.from=encoded.at("from").get<std::string>();
        group.elapsed=encoded.at("elapsed").get<float>();
        group.duration=encoded.at("duration").get<float>();
        group.easing=VisualEaseFromName(encoded.at("easing").get<std::string>());
        const auto& transitionFrom=encoded.at("transitionFrom");
        if(!transitionFrom.is_array()||transitionFrom.size()>kMaxSaveCollectionItems)
            throw std::invalid_argument("Visual State transition checkpoint exceeds save limits");
        for(const auto& value:transitionFrom)
            group.transitionFrom.push_back({
                RequiredUuid(value.at("node"),"Visual State node"),
                value.at("property").get<std::string>(),
                VariantFromSaveJson(value.at("value"))});
        const auto& animationClip=encoded.at("animationClip");
        if(!animationClip.is_null())
            group.animationClip=RequiredUuid(animationClip,"Visual State animation clip");
        group.animationPosition=encoded.at("animationPosition").get<float>();
        state.groups.push_back(std::move(group));
    }
    return state;
}

Json UIRuntimeStateToJson(const ui::UIRuntimeState& state) {
    return {{"surfaceId",state.surfaceId},
            {"behavior",BehaviorStateToJson(state.behavior)},
            {"animation",state.animation
                ? UIAnimationStateToJson(*state.animation):Json(nullptr)},
            {"visualState",state.visualState
                ? VisualStateToJson(*state.visualState):Json(nullptr)}};
}

ui::UIRuntimeState UIRuntimeStateFromJson(const Json& json) {
    if(!json.is_object())throw std::invalid_argument("UI runtime state must be an object");
    ui::UIRuntimeState state;
    state.surfaceId=json.at("surfaceId").get<std::string>();
    if(state.surfaceId.empty()||state.surfaceId.size()>128)
        throw std::invalid_argument("UI surface identity is invalid");
    state.behavior=BehaviorStateFromJson(json.at("behavior"));
    if(!json.at("animation").is_null())
        state.animation=UIAnimationStateFromJson(json.at("animation"));
    if(!json.at("visualState").is_null())
        state.visualState=VisualStateFromJson(json.at("visualState"));
    return state;
}

vn::DialogueSnapshot DialogueFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("dialogue must be an object");
    vn::DialogueSnapshot snapshot;
    auto& state = snapshot.state;
    state.speaker = json.value("speaker", std::string{});
    state.fullText = json.value("fullText", std::string{});
    state.displayText = json.value("displayText", std::string{});
    state.voice = json.value("voice", std::string{});
    state.currentChar = json.value("currentChar", 0);
    state.totalChars = json.value("totalChars", 0);
    state.finished = json.value("finished", false);
    state.textColor = ColorFromJson(json.value("textColor", Json::array()), state.textColor);
    state.outlineColor =
        ColorFromJson(json.value("outlineColor", Json::array()), state.outlineColor);
    state.effect = json.value("effect", std::string{});
    state.effectProgress = json.value("effectProgress", 0.0f);
    snapshot.speedMs = json.value("speedMs", 30);
    if (state.currentChar < 0 || state.totalChars < 0 || state.currentChar > state.totalChars ||
        snapshot.speedMs < 0) {
        throw std::invalid_argument("dialogue progress is invalid");
    }
    return snapshot;
}

Json ActorsToJson(const std::vector<vn::Stage::SavedActor>& actors) {
    Json arr = Json::array();
    for (const auto& a : actors) {
        arr.push_back({ { "name", a.name },
                        { "image", a.imagePath },
                        { "slot", a.slot },
                        { "ox", a.offsetX },
                        { "oy", a.offsetY },
                        { "scale", a.scale },
                        { "previousImage", a.previousImagePath },
                        { "alpha", a.alpha },
                        { "targetAlpha", a.targetAlpha },
                        { "previousAlpha", a.previousAlpha },
                        { "x", a.x },
                        { "targetX", a.targetX },
                        { "exiting", a.exiting },
                        { "effectOffsetX", a.effectOffsetX },
                        { "effectOffsetY", a.effectOffsetY },
                        { "effectScale", a.effectScale },
                        { "effectAlpha", a.effectAlpha } });
    }
    return arr;
}

std::vector<vn::Stage::SavedActor> ActorsFromJson(const Json& arr) {
    std::vector<vn::Stage::SavedActor> out;
    if (!arr.is_array() || arr.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("stage actors must be a bounded array");
    for (const auto& j : arr) {
        if (!j.is_object()) throw std::invalid_argument("stage actor must be an object");
        out.push_back(vn::Stage::SavedActor{ j.value("name", std::string{}),
                                             j.value("image", std::string{}), j.value("slot", 2),
                                             j.value("ox", 0.0f), j.value("oy", 0.0f),
                                             j.value("scale", 1.0f),
                                             j.value("previousImage", std::string{}),
                                             j.value("alpha", 255.0f),
                                             j.value("targetAlpha", 255.0f),
                                             j.value("previousAlpha", 0.0f),
                                             j.value("x", 0.0f), j.value("targetX", 0.0f),
                                             j.value("exiting", false),
                                             j.value("effectOffsetX", 0.0f),
                                             j.value("effectOffsetY", 0.0f),
                                             j.value("effectScale", 1.0f),
                                             j.value("effectAlpha", 1.0f) });
    }
    return out;
}

Json LayersToJson(const std::vector<vn::Stage::SavedLayer>& layers) {
    Json arr = Json::array();
    for (const auto& l : layers) {
        arr.push_back({ { "name", l.name },
                        { "image", l.imagePath },
                        { "x", l.x },
                        { "y", l.y },
                        { "scale", l.scale },
                        { "scaleY", l.scaleY },
                        { "rotation", l.rotation },
                        { "alpha", l.alpha },
                        { "z", l.z } });
    }
    return arr;
}

std::vector<vn::Stage::SavedLayer> LayersFromJson(const Json& arr) {
    std::vector<vn::Stage::SavedLayer> out;
    if (!arr.is_array() || arr.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("stage layers must be a bounded array");
    for (const auto& j : arr) {
        if (!j.is_object()) throw std::invalid_argument("stage layer must be an object");
        out.push_back(vn::Stage::SavedLayer{ j.value("name", std::string{}),
                                             j.value("image", std::string{}), j.value("x", 0.0f),
                                             j.value("y", 0.0f), j.value("scale", 1.0f),
                                             j.value("alpha", 255), j.value("z", 0),
                                             j.value("scaleY", j.value("scale", 1.0f)),
                                             j.value("rotation", 0.0f) });
    }
    return out;
}

Json TweenSpecToJson(const vn::Stage::TweenSpec& spec) {
    return {{"hasX", spec.hasX}, {"x", spec.x}, {"hasY", spec.hasY}, {"y", spec.y},
            {"hasScale", spec.hasScale}, {"scale", spec.scale},
            {"hasAlpha", spec.hasAlpha}, {"alpha", spec.alpha},
            {"durationMs", spec.durationMs}, {"ease", spec.ease}};
}

vn::Stage::TweenSpec TweenSpecFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("stage tween spec must be an object");
    vn::Stage::TweenSpec spec;
    spec.hasX = json.value("hasX", false); spec.x = json.value("x", 0.0f);
    spec.hasY = json.value("hasY", false); spec.y = json.value("y", 0.0f);
    spec.hasScale = json.value("hasScale", false); spec.scale = json.value("scale", 1.0f);
    spec.hasAlpha = json.value("hasAlpha", false); spec.alpha = json.value("alpha", 255.0f);
    spec.durationMs = json.value("durationMs", 600); spec.ease = json.value("ease", std::string("outCubic"));
    return spec;
}

std::string_view NodeKindName(const vn::Stage::NodeKind kind) {
    switch (kind) {
        case vn::Stage::NodeKind::Group: return "group";
        case vn::Stage::NodeKind::Image: return "image";
        case vn::Stage::NodeKind::Character: return "character";
    }
    return "group";
}

vn::Stage::NodeKind NodeKindFromJson(const Json& json) {
    if (!json.is_string()) throw std::invalid_argument("stage node kind must be a string");
    const auto kind = json.get<std::string>();
    if (kind == "group") return vn::Stage::NodeKind::Group;
    if (kind == "image") return vn::Stage::NodeKind::Image;
    if (kind == "character") return vn::Stage::NodeKind::Character;
    throw std::invalid_argument("stage node kind is unsupported");
}

Json NodesToJson(const std::vector<vn::Stage::SavedNode>& nodes) {
    Json values = Json::array();
    for (const auto& node : nodes) {
        values.push_back({
            {"name", node.name}, {"kind", NodeKindName(node.kind)},
            {"parent", node.parent}, {"children", node.children},
            {"transform", {{"x", node.transform.x}, {"y", node.transform.y},
                           {"scaleX", node.transform.scaleX},
                           {"scaleY", node.transform.scaleY},
                           {"rotation", node.transform.rotation},
                           {"opacity", node.transform.opacity}}},
            {"z", node.z}, {"order", node.order}, {"visible", node.visible},
            {"effect", {{"id", node.effect},
                        {"progress", node.effectProgress},
                        {"seed", node.effectSeed},
                        {"parameters", node.effectParameters}}}});
    }
    return values;
}

std::vector<vn::Stage::SavedNode> NodesFromJson(const Json& values) {
    if (!values.is_array() || values.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("stage nodes must be a bounded array");
    std::vector<vn::Stage::SavedNode> nodes;
    for (const auto& value : values) {
        if (!value.is_object() || !value.contains("kind") ||
            !value.contains("transform") || !value.at("transform").is_object())
            throw std::invalid_argument("stage node must be an object");
        const auto& transform = value.at("transform");
        vn::Stage::SavedNode node;
        node.name = value.value("name", std::string{});
        node.kind = NodeKindFromJson(value.at("kind"));
        node.parent = value.value("parent", std::string{});
        if (!value.value("children", Json::array()).is_array())
            throw std::invalid_argument("stage node children must be an array");
        node.children = value.value("children", Json::array())
                            .get<std::vector<std::string>>();
        node.transform = {transform.value("x", 0.0f),
                          transform.value("y", 0.0f),
                          transform.value("scaleX", 1.0f),
                          transform.value("scaleY", 1.0f),
                          transform.value("rotation", 0.0f),
                          transform.value("opacity", 1.0f)};
        node.z = value.value("z", 0);
        node.order = value.value("order", 0);
        node.visible = value.value("visible", true);
        if (const auto effect = value.find("effect"); effect != value.end()) {
            if (!effect->is_object())
                throw std::invalid_argument("stage node effect must be an object");
            node.effect = effect->value("id", std::string{});
            node.effectProgress = effect->value("progress", 0.0f);
            node.effectSeed = effect->value("seed", std::uint32_t{0});
            if (effect->contains("parameters"))
                node.effectParameters =
                    effect->at("parameters")
                        .get<std::array<std::array<float, 4>, 8>>();
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

Json ParticleRangeToJson(const vn::ParticleRange& range) {
    return Json::array({range.minimum, range.maximum});
}

vn::ParticleRange ParticleRangeFromJson(const Json& value,
                                        const vn::ParticleRange fallback) {
    if (value.is_null()) return fallback;
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() ||
        !value[1].is_number())
        throw std::invalid_argument("particle range must be [minimum, maximum]");
    return {value[0].get<float>(), value[1].get<float>()};
}

Json ParticleCurveToJson(const std::vector<vn::ParticleCurvePoint>& curve) {
    Json result = Json::array();
    for (const auto& point : curve)
        result.push_back({{"time", point.time}, {"value", point.value}});
    return result;
}

std::vector<vn::ParticleCurvePoint> ParticleCurveFromJson(const Json& value) {
    if (!value.is_array() || value.size() > 8)
        throw std::invalid_argument("particle curve must be a bounded array");
    std::vector<vn::ParticleCurvePoint> result;
    for (const auto& point : value) {
        if (!point.is_object() || !point.contains("time") ||
            !point.contains("value"))
            throw std::invalid_argument("particle curve point is invalid");
        result.push_back(
            {point.at("time").get<float>(), point.at("value").get<float>()});
    }
    return result;
}

Json ParticleColorCurveToJson(
    const std::vector<vn::ParticleColorPoint>& curve) {
    Json result = Json::array();
    for (const auto& point : curve)
        result.push_back(
            {{"time", point.time},
             {"value", {point.value.r, point.value.g, point.value.b,
                        point.value.a}}});
    return result;
}

std::vector<vn::ParticleColorPoint> ParticleColorCurveFromJson(
    const Json& value) {
    if (!value.is_array() || value.size() > 8)
        throw std::invalid_argument("particle color curve must be bounded");
    std::vector<vn::ParticleColorPoint> result;
    for (const auto& point : value) {
        if (!point.is_object() || !point.contains("time") ||
            !point.contains("value") || !point.at("value").is_array() ||
            point.at("value").size() != 4)
            throw std::invalid_argument("particle color curve point is invalid");
        const auto& color = point.at("value");
        std::array<int, 4> channels{};
        for (std::size_t index = 0; index < channels.size(); ++index) {
            channels[index] = color[index].get<int>();
            if (channels[index] < 0 || channels[index] > 255)
                throw std::invalid_argument("particle color channel is invalid");
        }
        result.push_back(
            {point.at("time").get<float>(),
             {static_cast<std::uint8_t>(channels[0]),
              static_cast<std::uint8_t>(channels[1]),
              static_cast<std::uint8_t>(channels[2]),
              static_cast<std::uint8_t>(channels[3])}});
    }
    return result;
}

Json ParticleEmittersToJson(
    const std::vector<vn::ParticleEmitterState>& emitters) {
    Json values = Json::array();
    for (const auto& emitter : emitters) {
        const auto& spec = emitter.spec;
        values.push_back({{"name", emitter.name},
                          {"preset", vn::ParticlePresetName(spec.preset)},
                          {"seed", spec.seed}, {"rate", spec.rate},
                          {"maxParticles", spec.maxParticles}, {"z", spec.z},
                          {"opacity", spec.opacity}, {"wind", spec.wind},
                          {"speed", spec.speed}, {"size", spec.size},
                          {"texture", spec.texture},
                          {"atlas", {{"columns", spec.atlasColumns},
                                     {"rows", spec.atlasRows},
                                     {"firstFrame", spec.atlasFirstFrame},
                                     {"frameCount", spec.atlasFrameCount}}},
                          {"spawnShape", vn::ParticleSpawnShapeName(spec.spawnShape)},
                          {"position", {{"x", ParticleRangeToJson(spec.positionX)},
                                        {"y", ParticleRangeToJson(spec.positionY)}}},
                          {"velocity", {{"x", ParticleRangeToJson(spec.velocityX)},
                                        {"y", ParticleRangeToJson(spec.velocityY)}}},
                          {"acceleration", {{"x", ParticleRangeToJson(spec.accelerationX)},
                                            {"y", ParticleRangeToJson(spec.accelerationY)}}},
                          {"lifetime", ParticleRangeToJson(spec.lifetime)},
                          {"rotation", ParticleRangeToJson(spec.rotation)},
                          {"angularVelocity", ParticleRangeToJson(spec.angularVelocity)},
                          {"scale", ParticleRangeToJson(spec.scale)},
                          {"initialOpacity", ParticleRangeToJson(spec.initialOpacity)},
                          {"scaleOverLifetime", ParticleCurveToJson(spec.scaleOverLifetime)},
                          {"opacityOverLifetime", ParticleCurveToJson(spec.opacityOverLifetime)},
                          {"colorOverLifetime", ParticleColorCurveToJson(spec.colorOverLifetime)},
                          {"gravity", spec.gravity}, {"variation", spec.variation},
                          {"burst", spec.burst}, {"loop", spec.loop},
                          {"duration", spec.duration}, {"advanced", spec.advanced},
                          {"ticks", emitter.ticks},
                          {"tickRemainder", emitter.tickRemainder}});
    }
    return values;
}

std::vector<vn::ParticleEmitterState> ParticleEmittersFromJson(
    const Json& values) {
    if (!values.is_array() || values.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("particle emitters must be a bounded array");
    std::vector<vn::ParticleEmitterState> emitters;
    for (const auto& value : values) {
        if (!value.is_object())
            throw std::invalid_argument("particle emitter must be an object");
        const auto preset = vn::ParticlePresetFromName(
            value.value("preset", std::string{}));
        if (!preset) throw std::invalid_argument("particle preset is unsupported");
        vn::ParticleEmitterSpec spec;
        spec.preset = *preset;
        spec.seed = value.value("seed", std::uint32_t{0});
        spec.rate = value.value("rate", 0.0f);
        spec.maxParticles = value.value("maxParticles", std::uint32_t{0});
        spec.z = value.value("z", 0);
        spec.opacity = value.value("opacity", 1.0f);
        spec.wind = value.value("wind", 0.0f);
        spec.speed = value.value("speed", 1.0f);
        spec.size = value.value("size", 1.0f);
        spec.texture = value.value("texture", std::string{});
        if (const auto atlas = value.find("atlas"); atlas != value.end()) {
            if (!atlas->is_object())
                throw std::invalid_argument("particle atlas must be an object");
            spec.atlasColumns = atlas->value("columns", std::uint32_t{1});
            spec.atlasRows = atlas->value("rows", std::uint32_t{1});
            spec.atlasFirstFrame = atlas->value("firstFrame", std::uint32_t{0});
            spec.atlasFrameCount = atlas->value("frameCount", std::uint32_t{1});
        }
        const auto shape = vn::ParticleSpawnShapeFromName(
            value.value("spawnShape", std::string("box")));
        if (!shape) throw std::invalid_argument("particle spawn shape is invalid");
        spec.spawnShape = *shape;
        const auto vectorRange = [&](const char* name, vn::ParticleRange& x,
                                     vn::ParticleRange& y) {
            const auto object = value.find(name);
            if (object == value.end()) return;
            if (!object->is_object())
                throw std::invalid_argument("particle vector range is invalid");
            x = ParticleRangeFromJson(object->value("x", Json{}), x);
            y = ParticleRangeFromJson(object->value("y", Json{}), y);
        };
        vectorRange("position", spec.positionX, spec.positionY);
        vectorRange("velocity", spec.velocityX, spec.velocityY);
        vectorRange("acceleration", spec.accelerationX, spec.accelerationY);
        spec.lifetime = ParticleRangeFromJson(
            value.value("lifetime", Json{}), spec.lifetime);
        spec.rotation = ParticleRangeFromJson(
            value.value("rotation", Json{}), spec.rotation);
        spec.angularVelocity = ParticleRangeFromJson(
            value.value("angularVelocity", Json{}), spec.angularVelocity);
        spec.scale = ParticleRangeFromJson(value.value("scale", Json{}), spec.scale);
        spec.initialOpacity = ParticleRangeFromJson(
            value.value("initialOpacity", Json{}), spec.initialOpacity);
        if (value.contains("scaleOverLifetime"))
            spec.scaleOverLifetime =
                ParticleCurveFromJson(value.at("scaleOverLifetime"));
        if (value.contains("opacityOverLifetime"))
            spec.opacityOverLifetime =
                ParticleCurveFromJson(value.at("opacityOverLifetime"));
        if (value.contains("colorOverLifetime"))
            spec.colorOverLifetime =
                ParticleColorCurveFromJson(value.at("colorOverLifetime"));
        spec.gravity = value.value("gravity", 0.0f);
        spec.variation = value.value("variation", 0.0f);
        spec.burst = value.value("burst", std::uint32_t{0});
        spec.loop = value.value("loop", true);
        spec.duration = value.value("duration", 0.0f);
        spec.advanced = value.value("advanced", false);
        emitters.push_back({value.value("name", std::string{}), spec,
                            value.value("ticks", std::uint64_t{0}),
                            value.value("tickRemainder", 0.0)});
    }
    return emitters;
}

Json StageToJson(const vn::Stage::RuntimeState& state) {
    Json tweens = Json::array();
    for (const auto& tween : state.tweens) {
        tweens.push_back({{"layer", tween.layer}, {"target", tween.target},
                          {"spec", TweenSpecToJson(tween.spec)}, {"fromX", tween.fromX},
                          {"fromY", tween.fromY}, {"fromScale", tween.fromScale},
                          {"fromAlpha", tween.fromAlpha}, {"elapsed", tween.elapsed},
                          {"duration", tween.duration}});
    }
    return {{"background", state.background},
            {"previousBackground", state.previousBackground},
            {"backgroundFade", state.backgroundFade},
            {"ruleTransition", {{"active", state.ruleActive},
                                {"oldBackground", state.ruleOldBackground},
                                {"newBackground", state.ruleNewBackground},
                                {"mask", state.ruleMask},
                                {"progress", state.ruleProgress},
                                {"duration", state.ruleDuration},
                                {"vague", state.ruleVague}}},
            {"camera", {{"x", state.cameraX}, {"y", state.cameraY}, {"zoom", state.cameraZoom}}},
            {"shake", {{"remaining", state.shakeRemaining}, {"duration", state.shakeDuration},
                        {"amplitude", state.shakeAmplitude}, {"phase", state.shakePhase}}},
            {"customEffect", {{"id", state.customEffect},
                              {"progress", state.customEffectProgress},
                              {"seed", state.customEffectSeed},
                              {"sequence", state.customEffectSequence},
                              {"parameters", state.customEffectParameters}}},
            {"effects", state.screenEffects}, {"actors", ActorsToJson(state.actors)},
            {"layers", LayersToJson(state.layers)},
            {"nodes", NodesToJson(state.nodes)},
            {"particleEmitters", ParticleEmittersToJson(state.particleEmitters)},
            {"tweens", std::move(tweens)}};
}

vn::Stage::RuntimeState StageFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("stage must be an object");
    vn::Stage::RuntimeState state;
    state.background = json.value("background", std::string{});
    state.previousBackground = json.value("previousBackground", std::string{});
    state.backgroundFade = json.value("backgroundFade", 1.0f);
    const auto& rule = json.at("ruleTransition");
    const auto& camera = json.at("camera");
    const auto& shake = json.at("shake");
    if (!rule.is_object() || !camera.is_object() || !shake.is_object())
        throw std::invalid_argument("stage rule/camera/shake must be objects");
    state.ruleActive = rule.value("active", false);
    state.ruleOldBackground = rule.value("oldBackground", std::string{});
    state.ruleNewBackground = rule.value("newBackground", std::string{});
    state.ruleMask = rule.value("mask", std::string{});
    state.ruleProgress = rule.value("progress", 0.0f);
    state.ruleDuration = rule.value("duration", 0.6f);
    state.ruleVague = rule.value("vague", 64);
    state.cameraX = camera.value("x", 0.0f); state.cameraY = camera.value("y", 0.0f);
    state.cameraZoom = camera.value("zoom", 1.0f);
    state.shakeRemaining = shake.value("remaining", 0.0f);
    state.shakeDuration = shake.value("duration", 0.0f);
    state.shakeAmplitude = shake.value("amplitude", 0.0f);
    state.shakePhase = shake.value("phase", 0.0f);
    if (const auto custom = json.find("customEffect"); custom != json.end()) {
        if (!custom->is_object())
            throw std::invalid_argument("stage customEffect must be an object");
        state.customEffect = custom->value("id", std::string{});
        state.customEffectProgress = custom->value("progress", 0.0f);
        state.customEffectSeed = custom->value("seed", std::uint32_t{0});
        state.customEffectSequence =
            custom->value("sequence", std::uint32_t{0});
        if (custom->contains("parameters"))
            state.customEffectParameters =
                custom->at("parameters")
                    .get<std::array<std::array<float, 4>, 8>>();
    }
    if (!json.at("effects").is_object()) throw std::invalid_argument("stage effects must be an object");
    state.screenEffects = json.at("effects").get<std::unordered_map<std::string, float>>();
    state.actors = ActorsFromJson(json.at("actors"));
    state.layers = LayersFromJson(json.at("layers"));
    if (const auto nodes = json.find("nodes"); nodes != json.end())
        state.nodes = NodesFromJson(*nodes);
    if (const auto emitters = json.find("particleEmitters"); emitters != json.end())
        state.particleEmitters = ParticleEmittersFromJson(*emitters);
    const auto& tweens = json.at("tweens");
    if (!tweens.is_array() || tweens.size() > kMaxSaveCollectionItems)
        throw std::invalid_argument("stage tweens must be a bounded array");
    for (const auto& value : tweens) {
        if (!value.is_object()) throw std::invalid_argument("stage tween must be an object");
        state.tweens.push_back(vn::Stage::SavedTween{
            value.value("layer", false), value.value("target", std::string{}),
            TweenSpecFromJson(value.at("spec")), value.value("fromX", 0.0f),
            value.value("fromY", 0.0f), value.value("fromScale", 1.0f),
            value.value("fromAlpha", 255.0f), value.value("elapsed", 0.0f),
            value.value("duration", 0.6f)});
    }
    return state;
}

Json AudioTrackToJson(const audio::AudioEngine::TrackState& state) {
    return {{"path", state.path}, {"loop", state.loop}, {"playing", state.playing},
            {"playbackFrame", state.playbackFrame}};
}

audio::AudioEngine::TrackState AudioTrackFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("audio track must be an object");
    return {.path=json.at("path").get<std::string>(), .loop=json.at("loop").get<bool>(),
            .playing=json.at("playing").get<bool>(),
            .playbackFrame=json.at("playbackFrame").get<std::int64_t>()};
}

Json AudioToJson(const audio::AudioEngine::RuntimeState& state) {
    return {{"music", AudioTrackToJson(state.music)}, {"voice", AudioTrackToJson(state.voice)},
            {"ambience", AudioTrackToJson(state.ambience)},
            {"pendingMusicLoop", state.pendingMusicLoop}, {"mainVolume", state.mainVolume},
            {"musicVolume", state.musicVolume}, {"voiceVolume", state.voiceVolume},
            {"sfxVolume", state.sfxVolume}, {"ambienceVolume", state.ambienceVolume}};
}

audio::AudioEngine::RuntimeState AudioFromJson(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("audio must be an object");
    audio::AudioEngine::RuntimeState state;
    state.music = AudioTrackFromJson(json.at("music"));
    state.voice = AudioTrackFromJson(json.at("voice"));
    state.ambience = AudioTrackFromJson(json.at("ambience"));
    state.pendingMusicLoop = json.at("pendingMusicLoop").get<std::string>();
    state.mainVolume = json.at("mainVolume").get<int>();
    state.musicVolume = json.at("musicVolume").get<int>();
    state.voiceVolume = json.at("voiceVolume").get<int>();
    state.sfxVolume = json.at("sfxVolume").get<int>();
    state.ambienceVolume = json.at("ambienceVolume").get<int>();
    return state;
}

Json BacklogToJson(const std::vector<vn::BacklogEntry>& log) {
    Json arr = Json::array();
    for (const auto& e : log) {
        arr.push_back({ { "speaker", e.speaker },
                        { "text", e.text },
                        { "voice", e.voice },
                        { "choice", e.isChoice },
                        { "sourceId", e.sourceId },
                        { "operationId", e.operationId } });
    }
    return arr;
}

std::vector<vn::BacklogEntry> BacklogFromJson(const Json& arr) {
    std::vector<vn::BacklogEntry> out;
    if (!arr.is_array()) return out;
    for (const auto& j : arr) {
        out.push_back(vn::BacklogEntry{ j.value("speaker", std::string{}),
                                        j.value("text", std::string{}),
                                        j.value("voice", std::string{}),
                                        j.value("choice", false),
                                        j.value("sourceId", std::string{}),
                                        j.value("operationId", std::string{}) });
    }
    return out;
}
}

void SaveSystem::Configure(const std::string& directory, const crypto::Key* key) {
    m_dir = directory;
    m_encrypt = key != nullptr;
    if (key) {
        m_key = *key;
    }
}

std::string SaveSystem::SlotPath(int slot) const {
    // Negative slots are reserved for the automatic save.
    const std::string file = slot < 0 ? "autosave.pxsav"
                                      : "save_" + std::to_string(slot) + ".pxsav";
    return (std::filesystem::path(m_dir) / file).string();
}

bool SaveSystem::Save(int slot, const SaveSnapshot& s) {
    Json j;
    try {
    if (s.engineVersion.empty() || s.gameId.empty() ||
        s.packageFingerprint.empty() || s.contentVersion.empty() ||
        s.anchor.runtimeDocumentId.empty() || s.anchor.sourceId.empty() ||
        s.anchor.operationId.empty()) {
        SaveLoadError(SlotPath(slot),
                      "Save identity or stable execution anchor is missing");
        return false;
    }
    j["format"] = kSaveFormat;
    j["schemaRevision"] = kSaveSchemaRevision;
    j["engineVersion"] = s.engineVersion;
    j["gameId"] = s.gameId;
    j["packageFingerprint"] = s.packageFingerprint;
    j["contentVersion"] = s.contentVersion;
    j["saveVersion"] = s.saveVersion;
    j["anchor"] = {{"runtimeDocumentId", s.anchor.runtimeDocumentId},
                   {"sourceId", s.anchor.sourceId},
                   {"operationId", s.anchor.operationId}};
    Json state;
    state["scriptPath"] = s.scriptPath;
    state["pc"] = s.pc;
    state["chapter"] = s.chapter;
    state["bgmPath"] = s.bgmPath;
    state["stage"] = StageToJson(s.stage);
    state["audio"] = AudioToJson(s.audio);
    state["variables"] = s.variables;
    Json typedVariables = Json::object();
    for (const auto& [name, value] : s.typedVariables)
        typedVariables[name] = RuntimeValueToJson(value);
    state["values"] = std::move(typedVariables);
    state["vm"] = VMStateToJson(s.vm);
    state["dialogue"] = DialogueToJson(s.dialogue);
    state["routes"] = RouteStateToJson(s.routes);
    state["timelines"] = TimelinesToJson(s.timelines);
    state["animationClips"] = AnimationClipsToJson(s.animationClips);
    state["scriptPending"] = ScriptPendingToJson(s.scriptPending);
    state["scriptActions"] = ScriptActionsToJson(s.scriptActions);
    state["extensionState"] = ExtensionStateToJson(s.extensionState);
    state["ui"] = UIRuntimeStateToJson(s.ui);
    state["backlog"] = BacklogToJson(s.backlog);
    state["nvlMode"] = s.nvlMode;
    state["nvlLines"] = BacklogToJson(s.nvlLines);
    state["timestamp"] = s.timestamp;
    state["playtimeMs"] = s.playtimeMs;
    if (!s.thumbnailPng.empty()) {
        state["thumb"] = Base64Encode(s.thumbnailPng);
    }
    j["state"] = std::move(state);
    j["integrityHash"] = crypto::Sha256Hex(j.dump());
    return SaveJson(SlotPath(slot), j, m_encrypt ? &m_key : nullptr);
    } catch (const std::exception& error) {
        SaveLoadError(SlotPath(slot), "Save contains unsupported runtime state", error.what());
        return false;
    }
}

std::optional<SaveSnapshot> ParseSaveEnvelopeV2(
    const std::string_view text, const std::string_view sourcePath) {
    constexpr std::size_t kMaxSaveEnvelopeBytes = 64u * 1024u * 1024u;
    const std::string path(sourcePath);
    if (text.empty() || text.size() > kMaxSaveEnvelopeBytes) {
        SaveLoadError(path, "Save envelope has an invalid or excessive size");
        return std::nullopt;
    }
    const Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        SaveLoadError(path, "Save envelope is not valid JSON");
        return std::nullopt;
    }
    if(!HasSupportedHeader(j)){SaveLoadError(path,"Unsupported or corrupt save schema");return std::nullopt;}
    try {
        if (!j.contains("integrityHash") || !j["integrityHash"].is_string() ||
            !j.contains("state") || !j["state"].is_object() ||
            !j.contains("anchor") || !j["anchor"].is_object()) {
            SaveLoadError(path, "Save envelope is incomplete");
            return std::nullopt;
        }
        Json authenticated = j;
        const std::string expectedHash = authenticated["integrityHash"].get<std::string>();
        authenticated.erase("integrityHash");
        if (expectedHash.empty() ||
            crypto::Sha256Hex(authenticated.dump()) != expectedHash) {
            SaveLoadError(path, "Save integrity verification failed");
            return std::nullopt;
        }
        const Json& payload = j["state"];
        const Json& anchor = j["anchor"];
        SaveSnapshot s;
        s.engineVersion = j.value("engineVersion", std::string{});
        s.gameId = j.value("gameId", std::string{});
        s.packageFingerprint = j.value("packageFingerprint", std::string{});
        s.contentVersion = j.value("contentVersion", std::string{});
        s.saveVersion = j.value("saveVersion", std::uint32_t{0});
        s.anchor.runtimeDocumentId = anchor.value("runtimeDocumentId", std::string{});
        s.anchor.sourceId = anchor.value("sourceId", std::string{});
        s.anchor.operationId = anchor.value("operationId", std::string{});
        if (s.engineVersion.empty() || s.gameId.empty() ||
            s.packageFingerprint.empty() || s.contentVersion.empty() ||
            s.saveVersion == 0 || s.anchor.runtimeDocumentId.empty() ||
            s.anchor.sourceId.empty() || s.anchor.operationId.empty()) {
            SaveLoadError(path, "Save identity or stable execution anchor is invalid");
            return std::nullopt;
        }
        s.scriptPath = payload.value("scriptPath", std::string{});
        s.pc = payload.value("pc", 0);
        s.chapter = payload.value("chapter", std::string{});
        s.bgmPath = payload.value("bgmPath", std::string{});
        s.nvlMode = payload.value("nvlMode", false);
        s.nvlLines = BacklogFromJson(payload.value("nvlLines", Json::array()));
        if (payload.contains("variables")) {
            if(!payload["variables"].is_object()){SaveLoadError(path,"Save variables field must be an object");return std::nullopt;}
            for (auto it = payload["variables"].begin(); it != payload["variables"].end(); ++it) {
                if(!it.value().is_number_integer()){SaveLoadError(path,"Save variable values must be integers",it.key());return std::nullopt;}
                s.variables[it.key()] = it.value().get<int>();
            }
        }
        if (!payload.contains("values") || !payload["values"].is_object() ||
            payload["values"].size() > kMaxSaveCollectionItems) {
            SaveLoadError(path, "Save values field must be a bounded object");
            return std::nullopt;
        }
        for (auto value = payload["values"].begin(); value != payload["values"].end(); ++value)
            s.typedVariables.emplace(value.key(), RuntimeValueFromJson(value.value()));
        if (!payload.contains("vm") || !payload.contains("dialogue") || !payload.contains("routes") ||
            !payload.contains("timelines") || !payload.contains("animationClips") || !payload.contains("stage") || !payload.contains("audio") ||
            !payload.contains("scriptPending") || !payload.contains("scriptActions") ||
            (!payload.contains("ui") && !payload.contains("behavior"))) {
            SaveLoadError(path, "Save is missing exact runtime state");
            return std::nullopt;
        }
        s.vm = VMStateFromJson(payload["vm"]);
        s.stage = StageFromJson(payload["stage"]);
        s.audio = AudioFromJson(payload["audio"]);
        s.dialogue = DialogueFromJson(payload["dialogue"]);
        s.routes = RouteStateFromJson(payload["routes"]);
        s.timelines = TimelinesFromJson(payload["timelines"]);
        s.animationClips = AnimationClipsFromJson(payload["animationClips"]);
        s.scriptPending = ScriptPendingFromJson(payload["scriptPending"]);
        s.scriptActions = ScriptActionsFromJson(payload["scriptActions"]);
        s.extensionState = ExtensionStateFromJson(
            payload.value("extensionState", Json::array()));
        if(payload.contains("ui"))s.ui=UIRuntimeStateFromJson(payload["ui"]);
        else s.ui.behavior=BehaviorStateFromJson(payload["behavior"]);
        s.backlog = BacklogFromJson(payload.value("backlog", Json::array()));
        s.timestamp = payload.value("timestamp", std::uint64_t{ 0 });
        s.playtimeMs = payload.value("playtimeMs", std::uint64_t{ 0 });
        if (payload.contains("thumb")) {
            if(!payload["thumb"].is_string()){SaveLoadError(path,"Save thumbnail must be a string");return std::nullopt;}
            s.thumbnailPng = Base64Decode(payload["thumb"].get<std::string>());
        }
        return s;
    } catch(const Json::exception& error) {
        SaveLoadError(path,"Save payload has invalid field types",error.what());
        return std::nullopt;
    } catch(const std::exception& error) {
        SaveLoadError(path,"Save payload is invalid",error.what());
        return std::nullopt;
    }
}

std::optional<SaveSnapshot> SaveSystem::Load(int slot) const {
    auto json = LoadJson(SlotPath(slot), m_encrypt ? &m_key : nullptr);
    if (!json) return std::nullopt;
    return ParseSaveEnvelopeV2(json->dump(), SlotPath(slot));
}

SlotInfo SaveSystem::Peek(int slot) const {
    SlotInfo info;
    auto json = LoadJson(SlotPath(slot), m_encrypt ? &m_key : nullptr);
    if (!json) {
        return info;
    }
    const Json& j = *json;
    const std::string path=SlotPath(slot);
    if(!HasSupportedHeader(j)){SaveLoadError(path,"Unsupported or corrupt save schema");return info;}
    try {
        if (!j.contains("integrityHash") || !j["integrityHash"].is_string() ||
            !j.contains("state") || !j["state"].is_object()) {
            SaveLoadError(path, "Save envelope is incomplete");
            return info;
        }
        Json authenticated = j;
        const std::string expectedHash = authenticated["integrityHash"].get<std::string>();
        authenticated.erase("integrityHash");
        if (crypto::Sha256Hex(authenticated.dump()) != expectedHash) {
            SaveLoadError(path, "Save integrity verification failed");
            return info;
        }
        const Json& payload = j["state"];
        info.chapter = payload.value("chapter", std::string{});
        info.timestamp = payload.value("timestamp", std::uint64_t{ 0 });
        if (payload.contains("thumb")) {
            if(!payload["thumb"].is_string()){SaveLoadError(path,"Save thumbnail must be a string");return info;}
            info.thumbnailPng = Base64Decode(payload["thumb"].get<std::string>());
        }
        info.exists = true;
    } catch(const Json::exception& error) {
        SaveLoadError(path,"Save metadata has invalid field types",error.what());
    }
    return info;
}

std::vector<SlotInfo> SaveSystem::List(int count) const {
    std::vector<SlotInfo> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        out.push_back(Peek(i));
    }
    return out;
}

bool SaveSystem::Delete(int slot) {
    std::error_code ec;
    return std::filesystem::remove(SlotPath(slot), ec);
}

}
