#include "Engine/Lua/LuaHost.h"

#include "Engine/IO/VFS.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/Actions/ActionCatalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace px::lua {
namespace {
std::string Lower(std::string value){std::transform(value.begin(),value.end(),value.begin(),[](const unsigned char c){return static_cast<char>(std::tolower(c));});return value;}
std::optional<VariantType> ManifestType(const std::string& raw){const auto type=Lower(raw);if(type=="null")return VariantType::Null;if(type=="bool"||type=="boolean")return VariantType::Bool;if(type=="int"||type=="integer")return VariantType::Integer;if(type=="number"||type=="float")return VariantType::Number;if(type=="string")return VariantType::String;if(type=="vec2"||type=="vector2")return VariantType::Vec2;if(type=="rect")return VariantType::Rect;if(type=="color")return VariantType::Color;if(type=="uuid"||type=="node")return VariantType::Uuid;if(type=="list"||type=="array")return VariantType::Array;if(type=="map"||type=="object"||type=="expression")return VariantType::Object;if(type=="resource")return VariantType::ResourceRef;if(type=="token")return VariantType::TokenRef;return std::nullopt;}
std::optional<ui::ActionEditorHint> ManifestEditorHint(const std::string& raw){const auto hint=Lower(raw);if(hint.empty()||hint=="default")return ui::ActionEditorHint::Default;if(hint=="multiline")return ui::ActionEditorHint::Multiline;if(hint=="enum")return ui::ActionEditorHint::Enum;if(hint=="color")return ui::ActionEditorHint::Color;if(hint=="resource")return ui::ActionEditorHint::Resource;if(hint=="route")return ui::ActionEditorHint::Route;if(hint=="node")return ui::ActionEditorHint::Node;if(hint=="animation")return ui::ActionEditorHint::Animation;if(hint=="token")return ui::ActionEditorHint::Token;return std::nullopt;}
std::optional<vn::CommandEditorWidget> ManifestCommandEditorHint(const std::string& raw){const auto hint=Lower(raw);if(hint.empty()||hint=="default")return vn::CommandEditorWidget::Default;if(hint=="multiline")return vn::CommandEditorWidget::Multiline;if(hint=="enum")return vn::CommandEditorWidget::Enum;if(hint=="color")return vn::CommandEditorWidget::Color;if(hint=="resource")return vn::CommandEditorWidget::Resource;if(hint=="route")return vn::CommandEditorWidget::Route;if(hint=="node")return vn::CommandEditorWidget::Node;if(hint=="animation")return vn::CommandEditorWidget::Animation;if(hint=="token")return vn::CommandEditorWidget::Token;if(hint=="character")return vn::CommandEditorWidget::Character;if(hint=="expression")return vn::CommandEditorWidget::Expression;if(hint=="target")return vn::CommandEditorWidget::Target;if(hint=="preset")return vn::CommandEditorWidget::Preset;if(hint=="hidden")return vn::CommandEditorWidget::Hidden;return std::nullopt;}
bool SupportedCapability(const std::string_view capability){return capability=="runtime"||capability=="animation"||capability=="ui"||capability=="audio";}
std::string NormalizeDebugSource(std::string source) {
    if (!source.empty() && source.front() == '@') source.erase(0, 1);
    std::replace(source.begin(), source.end(), '\\', '/');
    return source;
}
std::string DebugValue(lua_State* state, const int index) {
    switch (lua_type(state, index)) {
        case LUA_TNIL: return "nil";
        case LUA_TBOOLEAN: return lua_toboolean(state, index) ? "true" : "false";
        case LUA_TNUMBER:
        case LUA_TSTRING: {
            std::size_t size = 0;
            const char* text = lua_tolstring(state, index, &size);
            if (!text) return {};
            constexpr std::size_t limit = 160;
            return std::string(text, std::min(size, limit)) +
                   (size > limit ? "…" : "");
        }
        default: return std::string("<") + lua_typename(state, lua_type(state, index)) + ">";
    }
}
std::string ConsoleText(lua_State* state) {
    constexpr std::size_t limit = 4096;
    std::string result;
    const int count = lua_gettop(state);
    for (int index = 1; index <= count; ++index) {
        std::size_t size = 0;
        const char* value = luaL_tolstring(state, index, &size);
        if (index > 1 && result.size() < limit) result.push_back('\t');
        if (value && result.size() < limit) {
            const std::size_t remaining = limit - result.size();
            result.append(value, std::min(size, remaining));
        }
        lua_pop(state, 1);
    }
    if (result.size() == limit) result.append("…");
    return result;
}
std::pair<std::string, int> ConsoleSource(lua_State* state) {
    lua_Debug caller{};
    if (!lua_getstack(state, 1, &caller) || !lua_getinfo(state, "Sl", &caller)) {
        return {};
    }
    return {NormalizeDebugSource(caller.source ? caller.source : ""),
            caller.currentline};
}
std::optional<Variant> UntypedManifestValue(const nlohmann::json& value,int depth=0){if(depth>32)return std::nullopt;if(value.is_null())return Variant{};if(value.is_boolean())return Variant(value.get<bool>());if(value.is_number_integer())return Variant(value.get<std::int64_t>());if(value.is_number())return Variant(value.get<double>());if(value.is_string())return Variant(value.get<std::string>());if(value.is_array()){VariantArray result;for(const auto& item:value){auto converted=UntypedManifestValue(item,depth+1);if(!converted)return std::nullopt;result.push_back(std::move(*converted));}return Variant(std::move(result));}if(value.is_object()){VariantObject result;for(auto item=value.begin();item!=value.end();++item){auto converted=UntypedManifestValue(item.value(),depth+1);if(!converted)return std::nullopt;result.emplace(item.key(),std::move(*converted));}return Variant(std::move(result));}return std::nullopt;}
std::optional<Variant> ManifestValue(const nlohmann::json& value,const VariantType type){
    if(type==VariantType::Bool&&value.is_boolean())return Variant(value.get<bool>());
    if(type==VariantType::Integer&&value.is_number_integer())return Variant(value.get<std::int64_t>());
    if(type==VariantType::Number&&value.is_number())return Variant(value.get<double>());
    if(type==VariantType::String&&value.is_string())return Variant(value.get<std::string>());
    if(type==VariantType::Uuid&&value.is_string()){const auto parsed=Uuid::Parse(value.get<std::string>());if(parsed)return Variant(*parsed);}
    if(type==VariantType::TokenRef&&value.is_string())return Variant(TokenRefValue{value.get<std::string>()});
    if(type==VariantType::Vec2){if(value.is_array()&&value.size()==2&&value[0].is_number()&&value[1].is_number())return Variant(Vec2{value[0].get<float>(),value[1].get<float>()});if(value.is_object()&&value.contains("x")&&value.contains("y"))return Variant(Vec2{value["x"].get<float>(),value["y"].get<float>()});}
    if(type==VariantType::Rect){if(value.is_array()&&value.size()==4)return Variant(Rect{value[0].get<float>(),value[1].get<float>(),value[2].get<float>(),value[3].get<float>()});}
    if(type==VariantType::Color&&value.is_array()&&value.size()==4){int channels[4]{};for(int index=0;index<4;++index){if(!value[index].is_number_integer())return std::nullopt;channels[index]=value[index].get<int>();if(channels[index]<0||channels[index]>255)return std::nullopt;}return Variant(Color{static_cast<std::uint8_t>(channels[0]),static_cast<std::uint8_t>(channels[1]),static_cast<std::uint8_t>(channels[2]),static_cast<std::uint8_t>(channels[3])});}
    if(type==VariantType::ResourceRef){if(value.is_string()){const auto path=value.get<std::string>();return Variant(ResourceRefValue{Uuid::FromName(path),path});}if(value.is_object()&&value.contains("path")&&value["path"].is_string()){const auto path=value["path"].get<std::string>();Uuid id=Uuid::FromName(path);if(value.contains("id")&&value["id"].is_string()){const auto parsed=Uuid::Parse(value["id"].get<std::string>());if(!parsed)return std::nullopt;id=*parsed;}return Variant(ResourceRefValue{id,path});}}
    if(type==VariantType::Array&&value.is_array())return UntypedManifestValue(value);
    if(type==VariantType::Object&&value.is_object())return UntypedManifestValue(value);
    if(type==VariantType::Null&&value.is_null())return Variant{};
    return std::nullopt;
}
bool HasManifestDefault(const nlohmann::json& parameter, const VariantType type) {
    const auto found = parameter.find("default");
    if (found == parameter.end()) return false;
    // Studio serializes an empty optional default as JSON null.  Keep an
    // explicit null only for the null Variant type, where it is actual data.
    return !found->is_null() || type == VariantType::Null;
}
bool ReadManifestRange(const nlohmann::json& parameter,
                       std::optional<double>& minimum,
                       std::optional<double>& maximum,
                       std::string& error) {
    const auto readBound = [&](const nlohmann::json& source,
                               const char* name,
                               std::optional<double>& destination) {
        const auto found = source.find(name);
        if (found == source.end() || found->is_null()) return true;
        if (!found->is_number()) {
            error = std::string("parameter ") + name + " must be a finite number";
            return false;
        }
        const double value = found->get<double>();
        if (!std::isfinite(value)) {
            error = std::string("parameter ") + name + " must be a finite number";
            return false;
        }
        destination = value;
        return true;
    };

    const auto range = parameter.find("range");
    // Studio serializes an empty optional range as JSON null.
    if (range != parameter.end() && !range->is_null()) {
        if (!range->is_object()) {
            error = "parameter range must be an object or null";
            return false;
        }
        if (!readBound(*range, "minimum", minimum) ||
            !readBound(*range, "maximum", maximum)) {
            return false;
        }
    }
    return readBound(parameter, "minimum", minimum) &&
           readBound(parameter, "maximum", maximum);
}

bool ActionHintMatchesType(const ui::ActionEditorHint hint,
                           const VariantType type,
                           const bool hasEnum) {
    switch (hint) {
        case ui::ActionEditorHint::Default: return true;
        case ui::ActionEditorHint::Multiline: return type == VariantType::String;
        case ui::ActionEditorHint::Enum:
            return type == VariantType::String && hasEnum;
        case ui::ActionEditorHint::Color: return type == VariantType::Color;
        case ui::ActionEditorHint::Resource: return type == VariantType::ResourceRef;
        case ui::ActionEditorHint::Route: return type == VariantType::String;
        case ui::ActionEditorHint::Node: return type == VariantType::Uuid;
        case ui::ActionEditorHint::Animation:
            return type == VariantType::ResourceRef || type == VariantType::String;
        case ui::ActionEditorHint::Token: return type == VariantType::TokenRef;
    }
    return false;
}

bool DefaultMatchesActionConstraints(const ui::ActionArgumentDescriptor& argument) {
    if (!argument.defaultValue) return true;
    if (!argument.enumValues.empty()) {
        const auto* enumValue = argument.defaultValue->TryGet<std::string>();
        if (!enumValue || std::ranges::find(argument.enumValues, *enumValue) ==
                          argument.enumValues.end()) {
            return false;
        }
    }
    if (argument.minimum || argument.maximum) {
        std::optional<double> numeric;
        if (const auto* numberValue = argument.defaultValue->TryGet<double>()) {
            numeric = *numberValue;
        } else if (const auto* integerValue =
                       argument.defaultValue->TryGet<std::int64_t>()) {
            numeric = static_cast<double>(*integerValue);
        }
        if (!numeric || (argument.minimum && *numeric < *argument.minimum) ||
            (argument.maximum && *numeric > *argument.maximum)) {
            return false;
        }
    }
    return true;
}
bool HasUniqueNonEmptyValues(const std::vector<std::string>& values) {
    std::unordered_set<std::string> unique;
    return std::ranges::all_of(values, [&](const std::string& value) {
        return !value.empty() && unique.insert(value).second;
    });
}
sol::object ToLua(sol::state_view state,const Variant& value){switch(value.Type()){case VariantType::Null:return sol::make_object(state,sol::lua_nil);case VariantType::Bool:return sol::make_object(state,*value.TryGet<bool>());case VariantType::Integer:return sol::make_object(state,*value.TryGet<std::int64_t>());case VariantType::Number:return sol::make_object(state,*value.TryGet<double>());case VariantType::String:return sol::make_object(state,*value.TryGet<std::string>());case VariantType::Uuid:return sol::make_object(state,value.TryGet<Uuid>()->ToString());case VariantType::TokenRef:return sol::make_object(state,value.TryGet<TokenRefValue>()->name);case VariantType::ResourceRef:{const auto& reference=*value.TryGet<ResourceRefValue>();sol::table table=state.create_table();table["id"]=reference.id.ToString();table["path"]=reference.lastKnownPath;return sol::make_object(state,table);}case VariantType::Array:{sol::table table=state.create_table();std::size_t index=1;for(const auto& item:*value.AsArray())table[index++]=ToLua(state,item);return sol::make_object(state,table);}case VariantType::Object:{sol::table table=state.create_table();for(const auto& [name,item]:*value.AsObject())table[name]=ToLua(state,item);return sol::make_object(state,table);}case VariantType::Vec2:{const auto& vector=*value.TryGet<Vec2>();sol::table table=state.create_table();table["x"]=vector.x;table["y"]=vector.y;return sol::make_object(state,table);}case VariantType::Rect:{const auto& rectangle=*value.TryGet<Rect>();sol::table table=state.create_table();table["x"]=rectangle.x;table["y"]=rectangle.y;table["w"]=rectangle.w;table["h"]=rectangle.h;return sol::make_object(state,table);}case VariantType::Color:{const auto& color=*value.TryGet<Color>();sol::table table=state.create_table();table["r"]=color.r;table["g"]=color.g;table["b"]=color.b;table["a"]=color.a;return sol::make_object(state,table);}}return sol::make_object(state,sol::lua_nil);}
class LuaActionProvider final : public ui::IActionProvider {
public:
    explicit LuaActionProvider(LuaHost& host):m_host(host){}
    std::string_view ProviderId()const override{return "lua";}
    ui::ActionOrigin Origin()const override{return ui::ActionOrigin::ScriptExtension;}
    bool CanInvoke(std::string_view action)const override{return m_host.HasAction(action);}
    Status Invoke(const ui::ActionInvocation& invocation)override{return m_host.InvokeAction(invocation);}
    ui::ProviderActionStart Start(const ui::ActionInvocation& invocation)override{return m_host.StartAction(invocation);}
    ui::ActionExecutionState Poll(std::uint64_t handle)const override{return m_host.ActionState(handle);}
    void Cancel(std::uint64_t handle)override{m_host.CancelAction(handle);}
private:LuaHost& m_host;
};
}

LuaHost::LuaHost(const LuaServices& services) : m_runner(sol::thread::create(m_lua)), m_services(services) {
    *static_cast<LuaHost**>(lua_getextraspace(m_lua.lua_state())) = this;
    *static_cast<LuaHost**>(lua_getextraspace(m_runner.state().lua_state())) = this;
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                         sol::lib::coroutine, sol::lib::utf8);
    // Base Lua exposes host-filesystem helpers even when io/os are not opened.
    // Extensions receive only VFS-aware require unless a future platform
    // capability provider explicitly grants a broader API.
    m_lua["dofile"] = sol::lua_nil;
    m_lua["loadfile"] = sol::lua_nil;
    m_lua.set_function("print", &LuaHost::ConsolePrint);
    m_lua.set_function("warn", &LuaHost::ConsoleWarn);
    BindVfsRequire();
    BindApi();
}
LuaHost::~LuaHost(){for(const auto& source:m_loadedActionSources)(void)ui::ActionCatalog::Global().RemoveSource(ui::ActionOrigin::ScriptExtension,source);}

void LuaHost::HandleError(const std::string& where, const std::string& message) {
    EmitConsole(LuaConsoleLevel::Error, message, where);
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXLUA7401",
                                .category="Lua.Runtime",.message="Lua extension failed",
                                .details=message};
    diagnostic.source.path=where;diag::Emit(std::move(diagnostic));
}

void LuaHost::EmitConsole(const LuaConsoleLevel level, std::string message,
                          std::string source, const int line) const {
    LuaConsoleMessage output{level, std::move(message), std::move(source), line};
    if (m_services.console) {
        m_services.console(output);
        return;
    }
    switch (level) {
        case LuaConsoleLevel::Info:
            PX_LOG_INFO("[lua] {}", output.text);
            break;
        case LuaConsoleLevel::Warning:
            PX_LOG_WARN("[lua] {}", output.text);
            break;
        case LuaConsoleLevel::Error:
            PX_LOG_ERROR("Lua error in {}: {}", output.source, output.text);
            break;
    }
}

int LuaHost::ConsolePrint(lua_State* state) {
    auto* host = *static_cast<LuaHost**>(lua_getextraspace(state));
    if (!host) return 0;
    auto [source, line] = ConsoleSource(state);
    host->EmitConsole(LuaConsoleLevel::Info, ConsoleText(state),
                      std::move(source), line);
    return 0;
}

int LuaHost::ConsoleWarn(lua_State* state) {
    auto* host = *static_cast<LuaHost**>(lua_getextraspace(state));
    if (!host) return 0;
    auto [source, line] = ConsoleSource(state);
    host->EmitConsole(LuaConsoleLevel::Warning, ConsoleText(state),
                      std::move(source), line);
    return 0;
}

void LuaHost::PrepareDebugHook(lua_State* state) {
    if (!state) return;
    *static_cast<LuaHost**>(lua_getextraspace(state)) = this;
    const bool enabled = !m_debugBreakpoints.empty() || m_debugPauseRequested ||
                         m_debugStepRequested;
    lua_sethook(state, enabled ? &LuaHost::DebugHook : nullptr,
                enabled ? LUA_MASKLINE : 0, 0);
}

void LuaHost::CaptureDebugStack(lua_State* state, lua_Debug* event,
                                std::string reason) {
    m_debugSnapshot = {};
    m_debugSnapshot.paused = true;
    m_debugSnapshot.reason = std::move(reason);
    m_debugPauseRequested = false;
    m_debugStepRequested = false;
    m_debugPausedState = state;

    for (int level = 0; level < 32; ++level) {
        lua_Debug frame{};
        if (!lua_getstack(state, level, &frame)) break;
        if (!lua_getinfo(state, "nSl", &frame)) continue;
        DebugFrame captured;
        captured.source = NormalizeDebugSource(
            frame.source ? frame.source : (event && event->source ? event->source : ""));
        captured.function = frame.name ? frame.name : "<anonymous>";
        captured.line = frame.currentline;
        for (int local = 1; local <= 128; ++local) {
            const char* name = lua_getlocal(state, &frame, local);
            if (!name) break;
            if (name[0] != '(') {
                captured.locals.push_back({name, DebugValue(state, -1)});
            }
            lua_pop(state, 1);
        }
        m_debugSnapshot.frames.push_back(std::move(captured));
    }
}

void LuaHost::DebugHook(lua_State* state, lua_Debug* event) {
    auto* host = *static_cast<LuaHost**>(lua_getextraspace(state));
    if (!host || !event || event->event != LUA_HOOKLINE) return;
    if (!lua_getinfo(state, "Sl", event)) return;

    const std::string source = NormalizeDebugSource(event->source ? event->source : "");
    if (event->currentline == host->m_debugSkipLine &&
        source == host->m_debugSkipSource) {
        host->m_debugSkipSource.clear();
        host->m_debugSkipLine = 0;
        return;
    }
    bool atBreakpoint = false;
    for (const auto& [configuredSource, lines] : host->m_debugBreakpoints) {
        const bool sourceMatches =
            configuredSource.empty() || source == configuredSource ||
            (source.size() > configuredSource.size() &&
             source.ends_with(configuredSource));
        if (sourceMatches && lines.contains(event->currentline)) {
            atBreakpoint = true;
            break;
        }
    }
    if (!atBreakpoint && !host->m_debugPauseRequested && !host->m_debugStepRequested) {
        return;
    }
    const std::string reason = atBreakpoint ? "breakpoint"
                               : host->m_debugStepRequested ? "step"
                                                          : "pause";
    // A Lua line hook may yield, but must not inspect or otherwise mutate the
    // active stack before doing so. The suspended coroutine is inspected by
    // the caller immediately after lua_resume returns.
    host->m_debugSnapshot = {};
    host->m_debugSnapshot.paused = true;
    host->m_debugSnapshot.reason = reason;
    host->m_debugPauseRequested = false;
    host->m_debugStepRequested = false;
    lua_yield(state, 0);
}

std::vector<LuaHost::DebugBreakpoint> LuaHost::SetDebugBreakpoints(
    std::vector<DebugBreakpoint> breakpoints) {
    m_debugBreakpoints.clear();
    std::vector<DebugBreakpoint> accepted;
    for (auto& breakpoint : breakpoints) {
        breakpoint.source = NormalizeDebugSource(std::move(breakpoint.source));
        if (breakpoint.line <= 0) continue;
        if (m_debugBreakpoints[breakpoint.source].insert(breakpoint.line).second) {
            accepted.push_back(std::move(breakpoint));
        }
    }
    PrepareDebugHook(m_runner.state().lua_state());
    return accepted;
}

bool LuaHost::DebugPause() {
    if (m_debugSnapshot.paused) return false;
    m_debugPauseRequested = true;
    PrepareDebugHook(m_runner.state().lua_state());
    return true;
}

bool LuaHost::DebugContinue() {
    if (!m_debugSnapshot.paused) return false;
    if (!m_debugSnapshot.frames.empty()) {
        m_debugSkipSource = m_debugSnapshot.frames.front().source;
        m_debugSkipLine = m_debugSnapshot.frames.front().line;
    }
    m_debugSnapshot.paused = false;
    m_debugSnapshot.reason.clear();
    m_debugSnapshot.frames.clear();
    m_debugPausedState = nullptr;
    for (auto& pending : m_pending) {
        if (pending.waitKind == "debug") pending.waitKind = "debug-resume";
    }
    for (auto& pending : m_pendingActions) {
        if (pending.waitKind == "debug") pending.waitKind = "debug-resume";
    }
    PrepareDebugHook(m_runner.state().lua_state());
    return true;
}

bool LuaHost::DebugStep() {
    if (!m_debugSnapshot.paused) return false;
    if (!m_debugSnapshot.frames.empty()) {
        m_debugSkipSource = m_debugSnapshot.frames.front().source;
        m_debugSkipLine = m_debugSnapshot.frames.front().line;
    }
    m_debugStepRequested = true;
    m_debugSnapshot.paused = false;
    m_debugSnapshot.reason.clear();
    m_debugSnapshot.frames.clear();
    m_debugPausedState = nullptr;
    for (auto& pending : m_pending) {
        if (pending.waitKind == "debug") pending.waitKind = "debug-resume";
    }
    for (auto& pending : m_pendingActions) {
        if (pending.waitKind == "debug") pending.waitKind = "debug-resume";
    }
    PrepareDebugHook(m_runner.state().lua_state());
    return true;
}

std::optional<LuaHost::DebugVariable> LuaHost::EvaluateDebugWatch(
    const std::string_view expression) const {
    if (!m_debugSnapshot.paused || !m_debugPausedState || expression.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> path;
    std::size_t start = 0;
    while (start < expression.size()) {
        const std::size_t end = expression.find('.', start);
        const std::string part(expression.substr(
            start, end == std::string_view::npos ? expression.size() - start
                                                  : end - start));
        if (part.empty() ||
            !(std::isalpha(static_cast<unsigned char>(part.front())) ||
              part.front() == '_') ||
            !std::ranges::all_of(part.substr(1), [](const unsigned char value) {
                return std::isalnum(value) || value == '_';
            })) {
            return std::nullopt;
        }
        path.push_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    lua_Debug frame{};
    if (!lua_getstack(m_debugPausedState, 0, &frame)) return std::nullopt;
    bool found = false;
    for (int local = 1; local <= 128; ++local) {
        const char* name = lua_getlocal(m_debugPausedState, &frame, local);
        if (!name) break;
        if (path.front() == name) {
            found = true;
            break;
        }
        lua_pop(m_debugPausedState, 1);
    }
    if (!found) return std::nullopt;
    for (std::size_t index = 1; index < path.size(); ++index) {
        if (!lua_istable(m_debugPausedState, -1)) {
            lua_pop(m_debugPausedState, 1);
            return std::nullopt;
        }
        lua_pushlstring(m_debugPausedState, path[index].data(),
                        path[index].size());
        lua_rawget(m_debugPausedState, -2);
        lua_remove(m_debugPausedState, -2);
        if (lua_isnil(m_debugPausedState, -1)) {
            lua_pop(m_debugPausedState, 1);
            return std::nullopt;
        }
    }
    DebugVariable value{std::string(expression),
                        DebugValue(m_debugPausedState, -1)};
    lua_pop(m_debugPausedState, 1);
    return value;
}

void LuaHost::BindVfsRequire() {
    m_lua.set_function("require", [this](const std::string& module) -> sol::object {
        if (module.empty() || module.starts_with('/') || module.find(':') != std::string::npos ||
            module.find("..") != std::string::npos || module.find('\\') != std::string::npos) {
            throw sol::error("unsafe module name: " + module);
        }
        if (const auto found = m_modules.find(module); found != m_modules.end()) {
            return found->second;
        }
        std::string relative = module;
        std::replace(relative.begin(), relative.end(), '.', '/');
        const std::string path = "Content/Extensions/" + relative + ".lua";
        if (!m_services.vfs) throw sol::error("VFS is unavailable");
        const auto source = m_services.vfs->ReadText(path);
        if (!source) throw sol::error("VFS module not found: " + path);
        sol::load_result loaded = m_lua.load(*source, path);
        if (!loaded.valid()) {
            const sol::error error = loaded;
            throw sol::error(error.what());
        }
        sol::protected_function function = loaded;
        sol::protected_function_result result = function();
        if (!result.valid()) {
            const sol::error error = result;
            throw sol::error(error.what());
        }
        sol::object value = result.return_count() > 0 ? result.get<sol::object>()
                                                       : sol::make_object(m_lua, true);
        m_modules.emplace(module, value);
        return value;
    });
}

bool LuaHost::LoadExtensionManifest(const std::string& manifestPath) {
    if (!m_services.vfs) return false;
    const auto text = m_services.vfs->ReadText(manifestPath);
    if (!text) {
        HandleError(manifestPath, "extension manifest was not found");
        return false;
    }
    const auto json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        HandleError(manifestPath, "extension manifest is corrupt");
        return false;
    }
    if (json.value("format", std::string{}) != "PrismatiXExtension" ||
        json.value("version", 0) != 4) {
        HandleError(manifestPath, "extension manifest must be strict PrismatiXExtension version 4");
        return false;
    }
    try {
        ExtensionManifest manifest;
        manifest.id = json.at("id").get<std::string>();
        manifest.order = json.value("order", 0);
        manifest.entry = json.at("entry").get<std::string>();
        if (manifest.id.empty() || manifest.entry.empty() || manifest.entry.starts_with('/') ||
            manifest.entry.find("..") != std::string::npos ||
            manifest.entry.find(':') != std::string::npos) {
            HandleError(manifestPath, "extension id or entry is unsafe");
            return false;
        }
        std::unordered_set<std::string> declaredCapabilities;
        for (const auto& capability : json.value("capabilities", nlohmann::json::array())) {
            if (!capability.is_string()) {
                HandleError(manifestPath, "capability must be a string");
                return false;
            }
            const std::string name=capability.get<std::string>();
            if(!declaredCapabilities.insert(name).second){HandleError(manifestPath,"duplicate capability: "+name);return false;}
            manifest.capabilities.push_back(name);
        }
        // File/network/native access are deliberately not implemented by the
        // sandbox provider yet; declaring them produces a clear hard failure.
        for (const auto& capability : manifest.capabilities) {
            if (!SupportedCapability(capability)) {
                HandleError(manifestPath, "unsupported capability: " + capability);
                return false;
            }
        }
        std::vector<vn::CommandDescriptor> commandDescriptors;
        vn::CommandRegistry stagedCommands;
        for (const auto& command : json.value("commands", nlohmann::json::array())) {
            if (!command.is_object()) {
                HandleError(manifestPath, "commands must be typed descriptor objects");
                return false;
            }
            vn::CommandDescriptor descriptor;descriptor.id=command.at("id").get<std::string>();descriptor.displayName=command.value("displayName",descriptor.id);descriptor.description=command.value("description",std::string{});descriptor.category=command.value("category",std::string("Extension"));descriptor.waitPolicy=command.value("await",false)?vn::CommandWaitPolicy::Async:vn::CommandWaitPolicy::Immediate;descriptor.allowAdditionalParameters=command.value("allowAdditionalParameters",false);
            const auto rollback=Lower(command.value("rollback",std::string("boundary")));if(rollback=="reversible")descriptor.rollbackPolicy=vn::RollbackPolicy::Reversible;else if(rollback=="boundary")descriptor.rollbackPolicy=vn::RollbackPolicy::Boundary;else if(rollback=="transient")descriptor.rollbackPolicy=vn::RollbackPolicy::Transient;else{HandleError(manifestPath,"command rollback policy is invalid: "+rollback);return false;}
            if(descriptor.id.empty()){HandleError(manifestPath,"command id is empty");return false;}
            for (const auto& parameter :
                 command.value("parameters", nlohmann::json::array())) {
                if (!parameter.is_object()) {
                    HandleError(manifestPath,
                                "command parameter must be an object");
                    return false;
                }
                const auto type =
                    ManifestType(parameter.value("type", std::string{}));
                if (!type) {
                    HandleError(manifestPath,
                                "unsupported command parameter type");
                    return false;
                }
                vn::CommandParameterDescriptor value;
                value.name = parameter.at("name").get<std::string>();
                value.label = parameter.value(
                    "displayName", parameter.value("label", value.name));
                value.description =
                    parameter.value("description", std::string{});
                value.type = *type;
                value.required = parameter.value("required", false);
                if (value.name.empty()) {
                    HandleError(manifestPath,
                                "command parameter name is empty");
                    return false;
                }
                if (HasManifestDefault(parameter, value.type)) {
                    const auto parsed =
                        ManifestValue(parameter["default"], value.type);
                    if (!parsed) {
                        HandleError(
                            manifestPath,
                            "Command parameter default has the wrong type: " +
                                descriptor.id + "." + value.name);
                        return false;
                    }
                    value.defaultValue = parsed->Clone();
                    value.hasDefault = true;
                }
                const auto optionsJson = parameter.contains("enum")
                                             ? parameter["enum"]
                                             : parameter.value(
                                                   "options",
                                                   nlohmann::json::array());
                if (!optionsJson.is_array()) {
                    HandleError(
                        manifestPath,
                        "Command parameter enum/options must be an array");
                    return false;
                }
                for (const auto& option : optionsJson) {
                    if (!option.is_string()) {
                        HandleError(manifestPath,
                                    "Command parameter options must be strings");
                        return false;
                    }
                    value.options.push_back(option.get<std::string>());
                }
                std::string rangeError;
                if (!ReadManifestRange(parameter, value.minimum, value.maximum,
                                       rangeError)) {
                    HandleError(manifestPath, "Command " + descriptor.id + "." +
                                                  value.name + " " + rangeError);
                    return false;
                }
                value.resourceType = parameter.value(
                    "resourceFilter",
                    parameter.value("resourceType", std::string{}));
                const auto hint = ManifestCommandEditorHint(
                    parameter.value("editorHint", std::string{}));
                if (!hint) {
                    HandleError(manifestPath,
                                "Command parameter editorHint is unknown");
                    return false;
                }
                value.widget = *hint;
                if (!value.options.empty() &&
                    value.widget == vn::CommandEditorWidget::Default) {
                    value.widget = vn::CommandEditorWidget::Enum;
                }
                if (value.type == VariantType::ResourceRef &&
                    value.widget == vn::CommandEditorWidget::Default) {
                    value.widget = vn::CommandEditorWidget::Resource;
                }
                descriptor.parameters.push_back(std::move(value));
            }
            if(vn::CommandRegistry::Global().Find(descriptor.id)){HandleError(manifestPath,"command id conflicts with an existing runtime command: "+descriptor.id);return false;}if(!manifest.commands.insert(descriptor.id).second){HandleError(manifestPath,"duplicate command id in extension manifest: "+descriptor.id);return false;}const Status staged=stagedCommands.Register(descriptor);if(!staged){HandleError(manifestPath,diag::Describe(staged.Diagnostics().front()));return false;}commandDescriptors.push_back(std::move(descriptor));
        }
        std::vector<ui::ActionDescriptor> actionDescriptors;
        for (const auto& action : json.value("actions", nlohmann::json::array())) {
            if (!action.is_object()) { HandleError(manifestPath,"actions must be typed descriptor objects"); return false; }
            ui::ActionDescriptor descriptor;
            descriptor.id=action.at("id").get<std::string>();
            descriptor.label=action.value("displayName",descriptor.id);descriptor.displayName=descriptor.label;
            descriptor.description=action.value("description",std::string{});
            descriptor.category=action.value("category",std::string("Extension"));
            descriptor.origin=ui::ActionOrigin::ScriptExtension;descriptor.sourceId=manifest.id;descriptor.providerId="script";
            descriptor.destructiveInPreview=action.value("destructiveInPreview",false);
            descriptor.allowAdditionalArguments=action.value("allowAdditionalArguments",false);
            const auto reentry=ui::ParseActionReentryPolicy(action.value("reentry",std::string("allow")));
            if(descriptor.id.empty()||!reentry){HandleError(manifestPath,"action id or reentry policy is invalid");return false;}
            descriptor.reentryPolicy=*reentry;
            for(const auto& capability:action.value("capabilities",nlohmann::json::array())){
                if(!capability.is_string()){HandleError(manifestPath,"Action capability must be a string");return false;}
                const std::string name=capability.get<std::string>();
                if(!SupportedCapability(name)||!declaredCapabilities.contains(name)){HandleError(manifestPath,"Action capability is unsupported or not declared by the extension: "+name);return false;}
                if(std::find(descriptor.capabilities.begin(),descriptor.capabilities.end(),name)!=descriptor.capabilities.end()){HandleError(manifestPath,"duplicate Action capability: "+name);return false;}
                descriptor.capabilities.push_back(name);
            }
            for(const auto& parameter:action.value("parameters",nlohmann::json::array())){
                if(!parameter.is_object()){HandleError(manifestPath,"action parameter must be an object");return false;}
                const auto type=ManifestType(parameter.value("type",std::string{}));
                if(!type){HandleError(manifestPath,"unsupported action parameter type");return false;}
                ui::ActionArgumentDescriptor value;value.name=parameter.at("name").get<std::string>();
                value.displayName=parameter.value("displayName",value.name);value.type=*type;
                value.description=parameter.value("description",std::string{});
                value.required=parameter.value("required",false);
                if(value.name.empty()){HandleError(manifestPath,"action parameter name is empty");return false;}
                if(HasManifestDefault(parameter,value.type)){const auto parsed=ManifestValue(parameter["default"],value.type);if(!parsed){HandleError(manifestPath,"Action parameter default has the wrong type: "+descriptor.id+"."+value.name);return false;}value.defaultValue=parsed->Clone();}
                const auto enumJson=parameter.contains("enum")?parameter["enum"]:nlohmann::json::array();
                if(!enumJson.is_array()){HandleError(manifestPath,"Action parameter enum must be an array");return false;}
                if(!enumJson.empty()&&value.type!=VariantType::String){HandleError(manifestPath,"Action parameter enum requires string type");return false;}
                for(const auto& option:enumJson){if(!option.is_string()){HandleError(manifestPath,"Action enum values must be strings");return false;}value.enumValues.push_back(option.get<std::string>());}
                if (!HasUniqueNonEmptyValues(value.enumValues)) {
                    HandleError(manifestPath,
                                "Action enum values must be non-empty and unique: " +
                                    descriptor.id + "." + value.name);
                    return false;
                }
                std::string rangeError;
                if (!ReadManifestRange(parameter, value.minimum, value.maximum,
                                       rangeError)) {
                    HandleError(manifestPath, "Action " + descriptor.id + "." +
                                                  value.name + " " + rangeError);
                    return false;
                }
                if((value.minimum||value.maximum)&&value.type!=VariantType::Integer&&value.type!=VariantType::Number){HandleError(manifestPath,"Action parameter range requires numeric type");return false;}
                value.resourceType=parameter.value("resourceFilter",parameter.value("resourceType",std::string{}));
                const auto hint=ManifestEditorHint(parameter.value("editorHint",std::string{}));if(!hint){HandleError(manifestPath,"Action parameter editorHint is unknown");return false;}value.editorHint=*hint;
                if(!value.enumValues.empty()&&value.editorHint==ui::ActionEditorHint::Default)value.editorHint=ui::ActionEditorHint::Enum;
                if(value.type==VariantType::ResourceRef&&value.editorHint==ui::ActionEditorHint::Default)value.editorHint=ui::ActionEditorHint::Resource;
                if (!ActionHintMatchesType(value.editorHint, value.type,
                                           !value.enumValues.empty())) {
                    HandleError(manifestPath,
                                "Action parameter editorHint does not match its type: " +
                                    descriptor.id + "." + value.name);
                    return false;
                }
                if (!value.resourceType.empty() &&
                    value.type != VariantType::ResourceRef) {
                    HandleError(manifestPath,
                                "Action resourceFilter requires resource type: " +
                                    descriptor.id + "." + value.name);
                    return false;
                }
                if (!DefaultMatchesActionConstraints(value)) {
                    HandleError(manifestPath,
                                "Action parameter default does not satisfy its schema: " +
                                    descriptor.id + "." + value.name);
                    return false;
                }
                descriptor.arguments.push_back(std::move(value));
            }
            manifest.actions.insert(descriptor.id);actionDescriptors.push_back(std::move(descriptor));
        }
        const Status actionRegistration=ui::ActionCatalog::Global().ReplaceSource(
            ui::ActionOrigin::ScriptExtension,manifest.id,std::move(actionDescriptors));
        if(!actionRegistration){HandleError(manifestPath,diag::Describe(actionRegistration.Diagnostics().front()));return false;}
        m_loadedActionSources.insert(manifest.id);
        m_activeExtension = manifest.id;
        m_declaredCommands.insert(manifest.commands.begin(), manifest.commands.end());
        m_declaredActions.insert(manifest.actions.begin(), manifest.actions.end());
        const auto discardManifestRegistration=[&](){for(const auto& command:manifest.commands){m_declaredCommands.erase(command);m_commands.erase(command);}for(const auto& action:manifest.actions){m_declaredActions.erase(action);m_actions.erase(action);}(void)ui::ActionCatalog::Global().RemoveSource(ui::ActionOrigin::ScriptExtension,manifest.id);m_loadedActionSources.erase(manifest.id);};
        const bool loaded = RunFile("Content/Extensions/" + manifest.entry);
        m_activeExtension.clear();
        if(!loaded){discardManifestRegistration();return false;}
        for(const auto& command:manifest.commands)if(!m_commands.contains(command)){
            discardManifestRegistration();HandleError(manifestPath,"command '"+command+"' has no Engine.RegisterCommand callback");return false;
        }
        for(const auto& action:manifest.actions)if(!m_actions.contains(action)){
            discardManifestRegistration();HandleError(manifestPath,"action '"+action+"' has no Engine.RegisterAction callback");return false;
        }
        for(auto& descriptor:commandDescriptors){const Status registered=vn::CommandRegistry::Global().Register(std::move(descriptor));if(!registered){HandleError(manifestPath,diag::Describe(registered.Diagnostics().front()));return false;}}
        return true;
    } catch (const nlohmann::json::exception& error) {
        HandleError(manifestPath, error.what());
        return false;
    }
}

bool LuaHost::LoadExtensionIndex(const std::string& indexPath) {
    if (!m_services.vfs) return false;
    const auto text = m_services.vfs->ReadText(indexPath);
    if (!text) return false;
    const auto json = nlohmann::json::parse(*text, nullptr, false);
    if (!json.is_array()) {
        HandleError(indexPath, "extension index must be an array of manifest paths");
        return false;
    }
    struct OrderedManifest { int order=0; std::string id; std::string path; };
    std::vector<OrderedManifest> manifests;
    for (const auto& item : json) {
        if (!item.is_string()) {
            HandleError(indexPath, "extension index contains a non-string path");
            return false;
        }
        const std::string path=item.get<std::string>();const auto manifestText=m_services.vfs->ReadText(path);
        if(!manifestText){HandleError(path,"extension manifest was not found");return false;}
        const auto manifestJson=nlohmann::json::parse(*manifestText,nullptr,false);
        if(manifestJson.is_discarded()||!manifestJson.is_object()){HandleError(path,"extension manifest is corrupt");return false;}
        manifests.push_back({manifestJson.value("order",0),manifestJson.value("id",std::string{}),path});
    }
    std::sort(manifests.begin(), manifests.end(),[](const auto& a,const auto& b){return std::tie(a.order,a.id,a.path)<std::tie(b.order,b.id,b.path);});
    for (const auto& manifest : manifests) {
        if (!LoadExtensionManifest(manifest.path)) return false;
    }
    return true;
}

bool LuaHost::RunString(const std::string& code, const std::string& chunkName) {
    sol::protected_function_result result =
        m_lua.safe_script(code, sol::script_pass_on_error, chunkName);
    if (!result.valid()) {
        const sol::error err = result;
        HandleError(chunkName, err.what());
        return false;
    }
    return true;
}

bool LuaHost::RunFile(const std::string& vfsPath) {
    if (!m_services.vfs) {
        return false;
    }
    auto text = m_services.vfs->ReadText(vfsPath);
    if (!text) {
        PX_LOG_ERROR("LuaHost: script not found '{}'", vfsPath);
        return false;
    }
    return RunString(*text, vfsPath);
}

bool LuaHost::HasAction(std::string_view action)const{return m_actions.contains(std::string(action));}
std::shared_ptr<ui::IActionProvider> LuaHost::CreateActionProvider(){return std::make_shared<LuaActionProvider>(*this);}

Status LuaHost::InvokeAction(const ui::ActionInvocation& invocation){
    return StartAction(invocation).status;
}

ui::ProviderActionStart LuaHost::StartAction(const ui::ActionInvocation& invocation){
    const auto function=m_actions.find(invocation.action);
    if(function==m_actions.end())return {.status=Status::Fail(diag::Diagnostic{
        .severity=diag::Severity::Error,.code="PXLUA7420",.category="Lua.Action",
        .message="Lua action callback is missing",.details=invocation.action})};
    sol::table args=m_lua.create_table();for(const auto& [name,value]:invocation.arguments)args[name]=ToLua(m_lua,value);
    sol::table context=m_lua.create_table();context["scene"]=invocation.context.sourceScene;
    context["node"]=invocation.context.sourceNode.ToString();context["signal"]=invocation.context.signal;
    context["route"]=invocation.context.currentRoute;context["preview"]=invocation.context.preview;
    auto runner = std::make_shared<sol::thread>(sol::thread::create(m_lua));
    sol::state_view runnerState=runner->state();
    PrepareDebugHook(runnerState.lua_state());
    sol::coroutine coroutine(runnerState,function->second);
    sol::protected_function_result result=coroutine(args,context);
    if(!result.valid()){const sol::error error=result;HandleError("action:"+invocation.action,error.what());return {.status=Status::Fail(diag::Diagnostic{
        .severity=diag::Severity::Error,.code="PXLUA7421",.category="Lua.Action",
        .message="Lua action failed",.details=error.what()})};}
    if(result.status()==sol::call_status::yielded){const std::uint64_t handle=m_nextActionHandle++;if(m_debugSnapshot.paused){CaptureDebugStack(runnerState.lua_state(),nullptr,m_debugSnapshot.reason);result.abandon();}PendingActionCoroutine pending{runner,std::move(coroutine)};pending.id=handle;pending.action=invocation.action;pending.invocation=invocation;pending.yieldIndex=1;
        if(m_debugSnapshot.paused) pending.waitKind="debug";
        else if(result.return_count()>=1)pending.waitKind=result.get<std::string>(0);
        if(result.return_count()>=2){if(pending.waitKind=="animation")pending.handle=result.get<std::uint64_t>(1);else if(pending.waitKind=="timer")pending.remainingSeconds=result.get<float>(1);}
        m_pendingActions.push_back(std::move(pending));return {.status=Status::Ok(),.handle=handle,.pending=true};}
    return {.status=Status::Ok()};
}

ui::ActionExecutionState LuaHost::ActionState(const std::uint64_t handle) const {
    if(std::ranges::find(m_pendingActions,handle,&PendingActionCoroutine::id)!=m_pendingActions.end())
        return ui::ActionExecutionState::Running;
    const auto terminal=m_actionTerminalStates.find(handle);
    return terminal==m_actionTerminalStates.end()?ui::ActionExecutionState::Unknown:terminal->second;
}

void LuaHost::CancelAction(const std::uint64_t handle){
    const auto found=std::ranges::find(m_pendingActions,handle,&PendingActionCoroutine::id);
    if(found!=m_pendingActions.end()){
        if(m_debugPausedState==found->runner->state().lua_state()){
            m_debugSnapshot={};m_debugPauseRequested=false;m_debugStepRequested=false;
            m_debugSkipSource.clear();m_debugSkipLine=0;m_debugPausedState=nullptr;
        }
        m_pendingActions.erase(found);
    }
    m_actionTerminalStates[handle]=ui::ActionExecutionState::Cancelled;
}

bool LuaHost::InvokeCommand(const vn::Command& cmd) {
    auto it = m_commands.find(cmd.type);
    if (it == m_commands.end()) {
        return false;
    }
    sol::table args = m_lua.create_table();
    for (const vn::Arg& a : cmd.args) {
        args[a.key] = a.value;
    }
    for (const auto& [name, value] : cmd.typedArgs) args[name] = ToLua(m_lua, value);
    auto runner = std::make_shared<sol::thread>(sol::thread::create(m_lua));
    sol::state_view runnerState=runner->state();
    PrepareDebugHook(runnerState.lua_state());
    sol::coroutine coroutine(runnerState,it->second);
    sol::protected_function_result result = coroutine(args);
    if (!result.valid()) {
        const sol::error err = result;
        HandleError("command:" + cmd.type, err.what());
        return true;
    }
    if(result.status()==sol::call_status::yielded){if(m_debugSnapshot.paused){CaptureDebugStack(runnerState.lua_state(),nullptr,m_debugSnapshot.reason);result.abandon();}PendingCoroutine pending{runner,std::move(coroutine)};pending.command=cmd;pending.yieldIndex=1;if(m_debugSnapshot.paused)pending.waitKind="debug";else if(result.return_count()>=1)pending.waitKind=result.get<std::string>(0);if(result.return_count()>=2){if(pending.waitKind=="animation")pending.handle=result.get<std::uint64_t>(1);else if(pending.waitKind=="timer")pending.remainingSeconds=result.get<float>(1);}m_pending.push_back(std::move(pending));}
    return true;
}

void LuaHost::Update(const float deltaSeconds) {
    enum class ResumeResult { Yielded, Finished, Failed };
    const auto resumeDebug = [this](auto& pending,
                                    const std::string& errorContext) {
        lua_State* state = pending.runner->state().lua_state();
        PrepareDebugHook(state);
        int resultCount = 0;
        const int status = lua_resume(state, nullptr, 0, &resultCount);
        if (status != LUA_OK && status != LUA_YIELD) {
            const char* error = lua_tostring(state, -1);
            HandleError(errorContext, error ? error : "Lua coroutine resume failed");
            if (lua_gettop(state) > 0) lua_pop(state, 1);
            return ResumeResult::Failed;
        }
        if (status == LUA_OK) {
            if (resultCount > 0) lua_pop(state, resultCount);
            return ResumeResult::Finished;
        }

        ++pending.yieldIndex;
        if (m_debugSnapshot.paused) {
            CaptureDebugStack(state, nullptr, m_debugSnapshot.reason);
            pending.waitKind = "debug";
        } else {
            const int firstResult = lua_gettop(state) - resultCount + 1;
            pending.waitKind =
                resultCount >= 1 && lua_isstring(state, firstResult)
                    ? lua_tostring(state, firstResult)
                    : "";
            pending.handle =
                pending.waitKind == "animation" && resultCount >= 2
                    ? static_cast<std::uint64_t>(
                          lua_tointeger(state, firstResult + 1))
                    : 0;
            pending.remainingSeconds =
                pending.waitKind == "timer" && resultCount >= 2
                    ? static_cast<float>(lua_tonumber(state, firstResult + 1))
                    : 0.0f;
        }
        if (resultCount > 0) lua_pop(state, resultCount);
        return ResumeResult::Yielded;
    };

    for (auto iterator = m_pending.begin(); iterator != m_pending.end();) {
        if (iterator->waitKind == "debug") {
            ++iterator;
            continue;
        }
        if (iterator->waitKind == "debug-resume") {
            const auto result = resumeDebug(*iterator, "await");
            if (result == ResumeResult::Finished ||
                result == ResumeResult::Failed) {
                iterator = m_pending.erase(iterator);
            } else {
                ++iterator;
            }
            continue;
        }
        bool ready = false;
        if (iterator->waitKind == "animation") {
            ready = !m_services.timeline ||
                    !m_services.timeline->Playing(iterator->handle);
        } else if (iterator->waitKind == "timer") {
            iterator->remainingSeconds -= std::max(0.0f, deltaSeconds);
            ready = iterator->remainingSeconds <= 0.0f;
        } else {
            ready = true;
        }
        if (!ready) {
            ++iterator;
            continue;
        }
        const auto result = resumeDebug(*iterator, "await");
        if (result == ResumeResult::Finished || result == ResumeResult::Failed) {
            iterator = m_pending.erase(iterator);
        } else {
            ++iterator;
        }
    }

    for (auto iterator = m_pendingActions.begin();
         iterator != m_pendingActions.end();) {
        if (iterator->waitKind == "debug") {
            ++iterator;
            continue;
        }
        if (iterator->waitKind == "debug-resume") {
            const auto result =
                resumeDebug(*iterator, "action:" + iterator->action);
            if (result == ResumeResult::Finished) {
                m_actionTerminalStates[iterator->id] =
                    ui::ActionExecutionState::Completed;
                iterator = m_pendingActions.erase(iterator);
            } else if (result == ResumeResult::Failed) {
                m_actionTerminalStates[iterator->id] =
                    ui::ActionExecutionState::Failed;
                iterator = m_pendingActions.erase(iterator);
            } else {
                ++iterator;
            }
            continue;
        }
        bool ready = false;
        if (iterator->waitKind == "animation") {
            ready = !m_services.timeline ||
                    !m_services.timeline->Playing(iterator->handle);
        } else if (iterator->waitKind == "timer") {
            iterator->remainingSeconds -= std::max(0.0f, deltaSeconds);
            ready = iterator->remainingSeconds <= 0.0f;
        } else {
            ready = true;
        }
        if (!ready) {
            ++iterator;
            continue;
        }
        const auto result =
            resumeDebug(*iterator, "action:" + iterator->action);
        if (result == ResumeResult::Finished) {
            m_actionTerminalStates[iterator->id] =
                ui::ActionExecutionState::Completed;
            iterator = m_pendingActions.erase(iterator);
        } else if (result == ResumeResult::Failed) {
            m_actionTerminalStates[iterator->id] =
                ui::ActionExecutionState::Failed;
            iterator = m_pendingActions.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

PendingCommandsState LuaHost::CapturePending() const {
    PendingCommandsState state;
    state.reserve(m_pending.size());
    for (const PendingCoroutine& pending : m_pending) {
        state.push_back(PendingCommandState{pending.command, pending.yieldIndex,
                                             pending.waitKind, pending.handle,
                                             pending.remainingSeconds});
    }
    return state;
}

Status LuaHost::RestorePending(const PendingCommandsState& state) {
    m_pending.clear();
    m_pendingActions.clear();
    m_actionTerminalStates.clear();
    m_runner = sol::thread::create(m_lua);
    PrepareDebugHook(m_runner.state().lua_state());
    for (const PendingCommandState& saved : state) {
        if (saved.yieldIndex == 0 ||
            (saved.waitKind != "timer" && saved.waitKind != "animation")) {
            return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                                 .code = "PXLUA7410",
                                                 .category = "Lua.Save",
                                                 .message = "Saved Lua await checkpoint is invalid",
                                                 .details = saved.command.type});
        }
        const auto function = m_commands.find(saved.command.type);
        if (function == m_commands.end()) {
            return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                                 .code = "PXLUA7411",
                                                 .category = "Lua.Save",
                                                 .message = "Saved Lua command is no longer registered",
                                                 .details = saved.command.type});
        }
        sol::table args = m_lua.create_table();
        for (const vn::Arg& argument : saved.command.args) args[argument.key] = argument.value;
        for (const auto& [name, value] : saved.command.typedArgs) {
            args[name] = ToLua(m_lua, value);
        }
        auto runner = std::make_shared<sol::thread>(sol::thread::create(m_lua));
        sol::state_view runnerState = runner->state();
        sol::coroutine coroutine(runnerState, function->second);
        sol::protected_function_result result = coroutine(args);
        std::uint32_t yieldIndex = 1;
        while (result.valid() && result.status() == sol::call_status::yielded &&
               yieldIndex < saved.yieldIndex) {
            result = coroutine();
            ++yieldIndex;
        }
        if (!result.valid() || result.status() != sol::call_status::yielded ||
            yieldIndex != saved.yieldIndex) {
            std::string details = saved.command.type;
            if (!result.valid()) {
                const sol::error error = result;
                details = error.what();
            }
            return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                                 .code = "PXLUA7412",
                                                 .category = "Lua.Save",
                                                 .message = "Lua command await structure changed",
                                                 .details = details});
        }
        PendingCoroutine pending{runner, std::move(coroutine)};
        pending.command = saved.command;
        pending.yieldIndex = saved.yieldIndex;
        pending.waitKind = saved.waitKind;
        pending.handle = saved.handle;
        pending.remainingSeconds = saved.remainingSeconds;
        m_pending.push_back(std::move(pending));
    }
    return Status::Ok();
}

PendingActionsState LuaHost::CapturePendingActions() const {
    PendingActionsState state;
    state.reserve(m_pendingActions.size());
    for (const auto& pending : m_pendingActions) {
        state.push_back({.id=pending.id,
                         .invocation=pending.invocation,
                         .yieldIndex=pending.yieldIndex,
                         .waitKind=pending.waitKind,
                         .handle=pending.handle,
                         .remainingSeconds=pending.remainingSeconds});
    }
    return state;
}

Status LuaHost::RestorePendingActions(const PendingActionsState& state) {
    m_pendingActions.clear();
    m_actionTerminalStates.clear();
    std::unordered_set<std::uint64_t> ids;
    for (const PendingActionState& saved : state) {
        if (!saved.id || !ids.insert(saved.id).second || saved.yieldIndex == 0 ||
            (saved.waitKind != "timer" && saved.waitKind != "animation")) {
            return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
                .code="PXLUA7422",.category="Lua.Save",
                .message="Saved Lua Action checkpoint is invalid",
                .details=saved.invocation.action});
        }
        const auto function = m_actions.find(saved.invocation.action);
        if (function == m_actions.end()) {
            return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
                .code="PXLUA7423",.category="Lua.Save",
                .message="Saved Lua Action is no longer registered",
                .details=saved.invocation.action});
        }
        sol::table args=m_lua.create_table();
        for(const auto& [name,value]:saved.invocation.arguments)args[name]=ToLua(m_lua,value);
        sol::table context=m_lua.create_table();
        context["scene"]=saved.invocation.context.sourceScene;
        context["node"]=saved.invocation.context.sourceNode.ToString();
        context["signal"]=saved.invocation.context.signal;
        context["route"]=saved.invocation.context.currentRoute;
        context["preview"]=saved.invocation.context.preview;
        auto runner = std::make_shared<sol::thread>(sol::thread::create(m_lua));
        sol::state_view runnerState=runner->state();
        sol::coroutine coroutine(runnerState,function->second);
        sol::protected_function_result result=coroutine(args,context);
        std::uint32_t yieldIndex=1;
        while(result.valid()&&result.status()==sol::call_status::yielded&&yieldIndex<saved.yieldIndex){result=coroutine();++yieldIndex;}
        if(!result.valid()||result.status()!=sol::call_status::yielded||yieldIndex!=saved.yieldIndex){
            std::string details=saved.invocation.action;if(!result.valid()){const sol::error error=result;details=error.what();}
            return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
                .code="PXLUA7424",.category="Lua.Save",
                .message="Lua Action await structure changed",.details=std::move(details)});
        }
        PendingActionCoroutine pending{runner,std::move(coroutine)};
        pending.id=saved.id;pending.action=saved.invocation.action;pending.invocation=saved.invocation;
        pending.waitKind=saved.waitKind;pending.handle=saved.handle;
        pending.remainingSeconds=saved.remainingSeconds;pending.yieldIndex=saved.yieldIndex;
        m_pendingActions.push_back(std::move(pending));
        m_nextActionHandle=std::max(m_nextActionHandle,saved.id+1);
    }
    return Status::Ok();
}

void LuaHost::CancelPending() {
    m_pending.clear();
    m_pendingActions.clear();
    m_actionTerminalStates.clear();
    m_debugSnapshot = {};
    m_debugPauseRequested = false;
    m_debugStepRequested = false;
    m_debugSkipSource.clear();
    m_debugSkipLine = 0;
    m_debugPausedState = nullptr;
    m_runner = sol::thread::create(m_lua);
    PrepareDebugHook(m_runner.state().lua_state());
}

}
