#include "Engine/Script/JavaScriptHost.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/IO/VFS.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/VN/Commands/CommandRegistry.h"

#include <nlohmann/json.hpp>
#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace px::script {
namespace {

constexpr std::size_t kJavaScriptMemoryLimit = 64U * 1024U * 1024U;
constexpr std::size_t kJavaScriptStackLimit = 1024U * 1024U;
constexpr auto kExecutionBudget = std::chrono::milliseconds(250);

diag::Diagnostic ScriptDiagnostic(std::string code, std::string message,
                                  std::string details = {},
                                  std::string source = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "Script.JavaScript",
                                .message = std::move(message),
                                .details = std::move(details)};
    diagnostic.source.path = std::move(source);
    return diagnostic;
}

std::string Lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::optional<VariantType> ManifestType(const std::string& raw) {
    const auto type = Lower(raw);
    if (type == "null") return VariantType::Null;
    if (type == "bool" || type == "boolean") return VariantType::Bool;
    if (type == "int" || type == "integer") return VariantType::Integer;
    if (type == "number" || type == "float") return VariantType::Number;
    if (type == "string") return VariantType::String;
    if (type == "vec2" || type == "vector2") return VariantType::Vec2;
    if (type == "rect") return VariantType::Rect;
    if (type == "color") return VariantType::Color;
    if (type == "uuid" || type == "node") return VariantType::Uuid;
    if (type == "list" || type == "array") return VariantType::Array;
    if (type == "map" || type == "object" || type == "expression")
        return VariantType::Object;
    if (type == "resource") return VariantType::ResourceRef;
    if (type == "token") return VariantType::TokenRef;
    return std::nullopt;
}

bool SupportedCapability(const std::string_view capability) {
    return capability == "runtime" || capability == "animation" ||
           capability == "ui" || capability == "audio";
}

bool SafeRelativePath(const std::string_view path) {
    return !path.empty() && path.front() != '/' && path.front() != '\\' &&
           path.find(':') == std::string_view::npos &&
           path.find("..") == std::string_view::npos &&
           path.find('\\') == std::string_view::npos;
}

std::optional<Variant> JsonVariant(const nlohmann::json& value,
                                   const VariantType expected,
                                   const int depth = 0) {
    if (depth > 32) return std::nullopt;
    if (expected == VariantType::Null && value.is_null()) return Variant{};
    if (expected == VariantType::Bool && value.is_boolean())
        return Variant(value.get<bool>());
    if (expected == VariantType::Integer && value.is_number_integer())
        return Variant(value.get<std::int64_t>());
    if (expected == VariantType::Number && value.is_number())
        return Variant(value.get<double>());
    if (expected == VariantType::String && value.is_string())
        return Variant(value.get<std::string>());
    if (expected == VariantType::Uuid && value.is_string()) {
        if (const auto id = Uuid::Parse(value.get<std::string>())) return Variant(*id);
        return std::nullopt;
    }
    if (expected == VariantType::TokenRef && value.is_string())
        return Variant(TokenRefValue{value.get<std::string>()});
    if (expected == VariantType::ResourceRef && value.is_string()) {
        const auto path = value.get<std::string>();
        return Variant(ResourceRefValue{Uuid::FromName(path), path});
    }
    if (expected == VariantType::ResourceRef && value.is_object() &&
        value.contains("path") && value["path"].is_string()) {
        const auto path = value["path"].get<std::string>();
        Uuid id = Uuid::FromName(path);
        if (value.contains("id")) {
            if (!value["id"].is_string()) return std::nullopt;
            const auto parsed = Uuid::Parse(value["id"].get<std::string>());
            if (!parsed) return std::nullopt;
            id = *parsed;
        }
        return Variant(ResourceRefValue{id, path});
    }
    if (expected == VariantType::Vec2 && value.is_array() && value.size() == 2 &&
        value[0].is_number() && value[1].is_number()) {
        return Variant(Vec2{value[0].get<float>(), value[1].get<float>()});
    }
    if (expected == VariantType::Rect && value.is_array() && value.size() == 4) {
        return Variant(Rect{value[0].get<float>(), value[1].get<float>(),
                            value[2].get<float>(), value[3].get<float>()});
    }
    if (expected == VariantType::Color && value.is_array() && value.size() == 4) {
        int channels[4]{};
        for (int index = 0; index < 4; ++index) {
            if (!value[index].is_number_integer()) return std::nullopt;
            channels[index] = value[index].get<int>();
            if (channels[index] < 0 || channels[index] > 255) return std::nullopt;
        }
        return Variant(Color{static_cast<std::uint8_t>(channels[0]),
                             static_cast<std::uint8_t>(channels[1]),
                             static_cast<std::uint8_t>(channels[2]),
                             static_cast<std::uint8_t>(channels[3])});
    }
    if (expected == VariantType::Array && value.is_array()) {
        VariantArray result;
        for (const auto& item : value) {
            if (item.is_string()) result.emplace_back(item.get<std::string>());
            else if (item.is_boolean()) result.emplace_back(item.get<bool>());
            else if (item.is_number_integer()) result.emplace_back(item.get<std::int64_t>());
            else if (item.is_number()) result.emplace_back(item.get<double>());
            else return std::nullopt;
        }
        return Variant(std::move(result));
    }
    if (expected == VariantType::Object && value.is_object()) {
        VariantObject result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (item.value().is_string()) result.emplace(item.key(), Variant(item.value().get<std::string>()));
            else if (item.value().is_boolean()) result.emplace(item.key(), Variant(item.value().get<bool>()));
            else if (item.value().is_number_integer()) result.emplace(item.key(), Variant(item.value().get<std::int64_t>()));
            else if (item.value().is_number()) result.emplace(item.key(), Variant(item.value().get<double>()));
            else return std::nullopt;
        }
        return Variant(std::move(result));
    }
    return std::nullopt;
}

std::optional<double> FiniteNumber(const nlohmann::json& object,
                                   const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return std::nullopt;
    if (!found->is_number()) throw std::invalid_argument(std::string(key) + " must be numeric");
    const double number = found->get<double>();
    if (!std::isfinite(number)) throw std::invalid_argument(std::string(key) + " must be finite");
    return number;
}

class JavaScriptActionProvider final : public ui::IActionProvider {
public:
    explicit JavaScriptActionProvider(JavaScriptHost& host) : m_host(host) {}
    [[nodiscard]] std::string_view ProviderId() const override { return "script"; }
    [[nodiscard]] ui::ActionOrigin Origin() const override {
        return ui::ActionOrigin::ScriptExtension;
    }
    [[nodiscard]] bool CanInvoke(const std::string_view action) const override {
        return m_host.HasAction(action);
    }
    Status Invoke(const ui::ActionInvocation& invocation) override {
        return m_host.InvokeAction(invocation);
    }

private:
    JavaScriptHost& m_host;
};

}  // namespace

class JavaScriptHost::Impl {
public:
    Impl(JavaScriptHost& owner, ScriptServices services)
        : host(owner), services(std::move(services)) {
        runtime = JS_NewRuntime();
        if (!runtime) return;
        JS_SetMemoryLimit(runtime, kJavaScriptMemoryLimit);
        JS_SetMaxStackSize(runtime, kJavaScriptStackLimit);
        JS_SetInterruptHandler(runtime, &Interrupt, this);
        context = JS_NewContext(runtime);
        if (!context) return;
        JS_SetContextOpaque(context, this);
        BindEngine();
    }

    ~Impl() {
        if (context) {
            for (auto& [_, callback] : commands) JS_FreeValue(context, callback);
            for (auto& [_, callback] : actions) JS_FreeValue(context, callback);
            for (auto& [_, callbacks] : events)
                for (auto& callback : callbacks) JS_FreeValue(context, callback);
            for (const auto& source : loadedActionSources)
                (void)ui::ActionCatalog::Global().RemoveSource(
                    ui::ActionOrigin::ScriptExtension, source);
            JS_FreeContext(context);
        }
        if (runtime) JS_FreeRuntime(runtime);
    }

    [[nodiscard]] bool Ready() const { return runtime && context; }

    static int Interrupt(JSRuntime*, void* opaque) {
        const auto* self = static_cast<Impl*>(opaque);
        return self && self->executing &&
               std::chrono::steady_clock::now() >= self->deadline;
    }

    void BeginExecution() {
        executing = true;
        deadline = std::chrono::steady_clock::now() + kExecutionBudget;
    }

    void EndExecution() { executing = false; }

    [[nodiscard]] std::optional<std::string> String(JSValueConst value) const {
        std::size_t size = 0;
        const char* text = JS_ToCStringLen(context, &size, value);
        if (!text) return std::nullopt;
        std::string result(text, size);
        JS_FreeCString(context, text);
        return result;
    }

    [[nodiscard]] std::string Exception() {
        JSValue exception = JS_GetException(context);
        std::string message = String(exception).value_or("JavaScript exception");
        JSValue stack = JS_GetPropertyStr(context, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            if (const auto trace = String(stack); trace && *trace != message)
                message.append("\n").append(*trace);
        }
        JS_FreeValue(context, stack);
        JS_FreeValue(context, exception);
        return message;
    }

    void Console(const ConsoleLevel level, std::string message,
                 std::string source = {}, const int line = 0) const {
        if (message.size() > 4096) {
            message.resize(4096);
            message.append("…");
        }
        ConsoleMessage output{level, std::move(message), std::move(source), line};
        if (services.console) services.console(output);
        if (level == ConsoleLevel::Warning) PX_LOG_WARN("[javascript] {}", output.text);
        else if (level == ConsoleLevel::Error) PX_LOG_ERROR("[javascript] {}", output.text);
        else PX_LOG_INFO("[javascript] {}", output.text);
    }

    void Error(const std::string& source, std::string message) const {
        Console(ConsoleLevel::Error, message, source);
        diag::Emit(ScriptDiagnostic("PXJS7001", "JavaScript execution failed",
                                    std::move(message), source));
    }

    static Impl* From(JSContext* context) {
        return static_cast<Impl*>(JS_GetContextOpaque(context));
    }

    static JSValue Log(JSContext* context, JSValueConst, const int count,
                       JSValueConst* values) {
        auto* self = From(context);
        std::string text;
        for (int index = 0; index < count; ++index) {
            if (index) text.push_back(' ');
            text.append(self->String(values[index]).value_or("<unprintable>"));
        }
        self->Console(ConsoleLevel::Info, std::move(text), "Engine.log");
        return JS_UNDEFINED;
    }

    static JSValue Warn(JSContext* context, JSValueConst, const int count,
                        JSValueConst* values) {
        auto* self = From(context);
        std::string text;
        for (int index = 0; index < count; ++index) {
            if (index) text.push_back(' ');
            text.append(self->String(values[index]).value_or("<unprintable>"));
        }
        self->Console(ConsoleLevel::Warning, std::move(text), "console.warn");
        return JS_UNDEFINED;
    }

    static JSValue ErrorLog(JSContext* context, JSValueConst, const int count,
                            JSValueConst* values) {
        auto* self = From(context);
        std::string text;
        for (int index = 0; index < count; ++index) {
            if (index) text.push_back(' ');
            text.append(self->String(values[index]).value_or("<unprintable>"));
        }
        self->Console(ConsoleLevel::Error, std::move(text), "console.error");
        return JS_UNDEFINED;
    }

    static JSValue RegisterCommand(JSContext* context, JSValueConst, const int count,
                                   JSValueConst* values) {
        auto* self = From(context);
        if (count != 2 || !JS_IsString(values[0]) ||
            !JS_IsFunction(context, values[1]))
            return JS_ThrowTypeError(context,
                                     "Engine.RegisterCommand requires an id and function");
        const auto name = self->String(values[0]);
        if (!name || name->empty()) return JS_ThrowTypeError(context, "command id is empty");
        if (!self->activeExtension.empty() &&
            !self->declaredCommands.contains(*name))
            return JS_ThrowTypeError(context, "command is not declared by the active extension");
        if (const auto found = self->commands.find(*name); found != self->commands.end())
            JS_FreeValue(context, found->second);
        self->commands[*name] = JS_DupValue(context, values[1]);
        return JS_UNDEFINED;
    }

    static JSValue RegisterAction(JSContext* context, JSValueConst, const int count,
                                  JSValueConst* values) {
        auto* self = From(context);
        if (count != 2 || !JS_IsString(values[0]) ||
            !JS_IsFunction(context, values[1]))
            return JS_ThrowTypeError(context,
                                     "Engine.RegisterAction requires an id and function");
        const auto name = self->String(values[0]);
        if (!name || name->empty()) return JS_ThrowTypeError(context, "action id is empty");
        if (!self->activeExtension.empty() &&
            !self->declaredActions.contains(*name))
            return JS_ThrowTypeError(context, "action is not declared by the active extension");
        if (const auto found = self->actions.find(*name); found != self->actions.end())
            JS_FreeValue(context, found->second);
        self->actions[*name] = JS_DupValue(context, values[1]);
        return JS_UNDEFINED;
    }

    static JSValue On(JSContext* context, JSValueConst, const int count,
                      JSValueConst* values) {
        auto* self = From(context);
        if (count != 2 || !JS_IsString(values[0]) ||
            !JS_IsFunction(context, values[1]))
            return JS_ThrowTypeError(context, "Engine.On requires an event and function");
        const auto event = self->String(values[0]);
        if (!event || event->empty()) return JS_ThrowTypeError(context, "event id is empty");
        self->events[*event].push_back(JS_DupValue(context, values[1]));
        return JS_UNDEFINED;
    }

    static JSValue EmitEvent(JSContext* context, JSValueConst, const int count,
                             JSValueConst* values) {
        auto* self = From(context);
        if (count < 1 || !JS_IsString(values[0]))
            return JS_ThrowTypeError(context, "Engine.Emit requires an event id");
        const auto event = self->String(values[0]);
        if (!event || event->empty()) return JS_ThrowTypeError(context, "event id is empty");
        self->host.Emit(*event);
        return JS_UNDEFINED;
    }

    void BindEngine() {
        JSValue global = JS_GetGlobalObject(context);
        JSValue engine = JS_NewObject(context);
        JS_SetPropertyStr(context, engine, "log",
                          JS_NewCFunction(context, &Log, "log", 1));
        JS_SetPropertyStr(context, engine, "RegisterCommand",
                          JS_NewCFunction(context, &RegisterCommand, "RegisterCommand", 2));
        JS_SetPropertyStr(context, engine, "RegisterAction",
                          JS_NewCFunction(context, &RegisterAction, "RegisterAction", 2));
        JS_SetPropertyStr(context, engine, "On",
                          JS_NewCFunction(context, &On, "On", 2));
        JS_SetPropertyStr(context, engine, "Emit",
                          JS_NewCFunction(context, &EmitEvent, "Emit", 2));
        JS_SetPropertyStr(context, global, "Engine", engine);

        JSValue console = JS_NewObject(context);
        JS_SetPropertyStr(context, console, "log",
                          JS_NewCFunction(context, &Log, "log", 1));
        JS_SetPropertyStr(context, console, "warn",
                          JS_NewCFunction(context, &Warn, "warn", 1));
        JS_SetPropertyStr(context, console, "error",
                          JS_NewCFunction(context, &ErrorLog, "error", 1));
        JS_SetPropertyStr(context, global, "console", console);
        JS_FreeValue(context, global);

        constexpr std::string_view sandbox = R"js(
            globalThis.px = Engine;
            globalThis.Date = undefined;
            globalThis.eval = undefined;
            globalThis.Function = undefined;
            Object.defineProperty(Math, "random", {
                value() { throw new Error("Math.random is disabled; use deterministic engine operations"); },
                writable: false,
                configurable: false
            });
            Object.freeze(Engine);
            Object.freeze(console);
        )js";
        BeginExecution();
        JSValue result = JS_Eval(context, sandbox.data(), sandbox.size(),
                                 "<prismatix-sandbox>", JS_EVAL_TYPE_GLOBAL);
        EndExecution();
        if (JS_IsException(result)) Error("<prismatix-sandbox>", Exception());
        JS_FreeValue(context, result);
    }

    [[nodiscard]] JSValue ToJavaScript(const Variant& value, const int depth = 0) {
        if (depth > 32) return JS_ThrowRangeError(context, "Variant nesting exceeds 32 levels");
        switch (value.Type()) {
            case VariantType::Null: return JS_NULL;
            case VariantType::Bool: return JS_NewBool(context, *value.TryGet<bool>());
            case VariantType::Integer: return JS_NewInt64(context, *value.TryGet<std::int64_t>());
            case VariantType::Number: return JS_NewFloat64(context, *value.TryGet<double>());
            case VariantType::String: return JS_NewString(context, value.TryGet<std::string>()->c_str());
            case VariantType::Uuid: return JS_NewString(context, value.TryGet<Uuid>()->ToString().c_str());
            case VariantType::TokenRef: return JS_NewString(context, value.TryGet<TokenRefValue>()->name.c_str());
            case VariantType::ResourceRef: {
                const auto& reference = *value.TryGet<ResourceRefValue>();
                JSValue object = JS_NewObject(context);
                JS_SetPropertyStr(context, object, "id", JS_NewString(context, reference.id.ToString().c_str()));
                JS_SetPropertyStr(context, object, "path", JS_NewString(context, reference.lastKnownPath.c_str()));
                return object;
            }
            case VariantType::Array: {
                JSValue array = JS_NewArray(context);
                std::uint32_t index = 0;
                for (const auto& item : *value.AsArray())
                    JS_SetPropertyUint32(context, array, index++, ToJavaScript(item, depth + 1));
                return array;
            }
            case VariantType::Object: {
                JSValue object = JS_NewObject(context);
                for (const auto& [name, item] : *value.AsObject())
                    JS_SetPropertyStr(context, object, name.c_str(), ToJavaScript(item, depth + 1));
                return object;
            }
            case VariantType::Vec2: {
                const auto vector = *value.TryGet<Vec2>();
                JSValue object = JS_NewObject(context);
                JS_SetPropertyStr(context, object, "x", JS_NewFloat64(context, vector.x));
                JS_SetPropertyStr(context, object, "y", JS_NewFloat64(context, vector.y));
                return object;
            }
            case VariantType::Rect: {
                const auto rectangle = *value.TryGet<Rect>();
                JSValue object = JS_NewObject(context);
                JS_SetPropertyStr(context, object, "x", JS_NewFloat64(context, rectangle.x));
                JS_SetPropertyStr(context, object, "y", JS_NewFloat64(context, rectangle.y));
                JS_SetPropertyStr(context, object, "w", JS_NewFloat64(context, rectangle.w));
                JS_SetPropertyStr(context, object, "h", JS_NewFloat64(context, rectangle.h));
                return object;
            }
            case VariantType::Color: {
                const auto color = *value.TryGet<Color>();
                JSValue object = JS_NewObject(context);
                JS_SetPropertyStr(context, object, "r", JS_NewInt32(context, color.r));
                JS_SetPropertyStr(context, object, "g", JS_NewInt32(context, color.g));
                JS_SetPropertyStr(context, object, "b", JS_NewInt32(context, color.b));
                JS_SetPropertyStr(context, object, "a", JS_NewInt32(context, color.a));
                return object;
            }
        }
        return JS_UNDEFINED;
    }

    JavaScriptHost& host;
    ScriptServices services;
    JSRuntime* runtime = nullptr;
    JSContext* context = nullptr;
    std::unordered_map<std::string, JSValue> commands;
    std::unordered_map<std::string, JSValue> actions;
    std::unordered_map<std::string, std::vector<JSValue>> events;
    std::unordered_set<std::string> declaredCommands;
    std::unordered_set<std::string> declaredActions;
    std::unordered_set<std::string> loadedActionSources;
    std::string activeExtension;
    DebugSnapshot debug;
    bool executing = false;
    std::chrono::steady_clock::time_point deadline{};
};

JavaScriptHost::JavaScriptHost(const ScriptServices& services)
    : m_impl(std::make_unique<Impl>(*this, services)) {
    if (!m_impl->Ready()) {
        diag::Emit(ScriptDiagnostic("PXJS7000", "JavaScript runtime could not initialize"));
    }
}

JavaScriptHost::~JavaScriptHost() = default;

bool JavaScriptHost::RunString(const std::string& source,
                               const std::string& sourceName) {
    if (!m_impl->Ready()) return false;
    m_impl->BeginExecution();
    JSValue result = JS_Eval(m_impl->context, source.data(), source.size(),
                             sourceName.c_str(), JS_EVAL_TYPE_GLOBAL);
    m_impl->EndExecution();
    if (JS_IsException(result)) {
        m_impl->Error(sourceName, m_impl->Exception());
        JS_FreeValue(m_impl->context, result);
        return false;
    }
    JS_FreeValue(m_impl->context, result);
    return true;
}

bool JavaScriptHost::RunFile(const std::string& vfsPath) {
    if (!m_impl->services.vfs) return false;
    const auto source = m_impl->services.vfs->ReadText(vfsPath);
    if (!source) {
        m_impl->Error(vfsPath, "JavaScript source was not found");
        return false;
    }
    return RunString(*source, vfsPath);
}

bool JavaScriptHost::LoadExtensionManifest(const std::string& manifestPath) {
    if (!m_impl->services.vfs) return false;
    const auto text = m_impl->services.vfs->ReadText(manifestPath);
    if (!text) {
        m_impl->Error(manifestPath, "extension manifest was not found");
        return false;
    }
    const auto json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        m_impl->Error(manifestPath, "extension manifest is corrupt");
        return false;
    }
    std::unordered_set<std::string> stagedCommandIds;
    std::unordered_set<std::string> stagedActionIds;
    std::string stagedExtensionId;
    bool stagedActionSource = false;
    const auto discardStagedCallbacks = [&] {
        if (!m_impl->context) return;
        for (const auto& command : stagedCommandIds) {
            if (const auto found = m_impl->commands.find(command);
                found != m_impl->commands.end()) {
                JS_FreeValue(m_impl->context, found->second);
                m_impl->commands.erase(found);
            }
            m_impl->declaredCommands.erase(command);
        }
        for (const auto& action : stagedActionIds) {
            if (const auto found = m_impl->actions.find(action);
                found != m_impl->actions.end()) {
                JS_FreeValue(m_impl->context, found->second);
                m_impl->actions.erase(found);
            }
            m_impl->declaredActions.erase(action);
        }
        if (stagedActionSource && !stagedExtensionId.empty()) {
            (void)ui::ActionCatalog::Global().RemoveSource(
                ui::ActionOrigin::ScriptExtension, stagedExtensionId);
            m_impl->loadedActionSources.erase(stagedExtensionId);
        }
    };
    try {
        if (json.value("format", std::string{}) != "PrismatiXExtension" ||
            json.value("schemaRevision", 0) != 1 ||
            Lower(json.value("language", std::string{})) != "javascript") {
            m_impl->Error(manifestPath,
                          "extension manifest must be PrismatiXExtension schemaRevision 1 with language javascript");
            return false;
        }
        const std::string id = json.at("id").get<std::string>();
        const std::string entry = json.at("entry").get<std::string>();
        if (id.empty() || !SafeRelativePath(entry) || !entry.ends_with(".js")) {
            m_impl->Error(manifestPath, "extension id or JavaScript entry is unsafe");
            return false;
        }
        stagedExtensionId = id;
        std::unordered_set<std::string> capabilities;
        for (const auto& capability : json.value("capabilities", nlohmann::json::array())) {
            if (!capability.is_string()) throw std::invalid_argument("capability must be a string");
            const auto name = capability.get<std::string>();
            if (!SupportedCapability(name) || !capabilities.insert(name).second)
                throw std::invalid_argument("unsupported or duplicate capability: " + name);
        }

        std::vector<vn::CommandDescriptor> commands;
        std::unordered_set<std::string> commandIds;
        for (const auto& encoded : json.value("commands", nlohmann::json::array())) {
            if (!encoded.is_object()) throw std::invalid_argument("command descriptor must be an object");
            vn::CommandDescriptor descriptor;
            descriptor.id = encoded.at("id").get<std::string>();
            descriptor.displayName = encoded.value("displayName", descriptor.id);
            descriptor.description = encoded.value("description", std::string{});
            descriptor.category = encoded.value("category", std::string("Extension"));
            descriptor.allowAdditionalParameters = encoded.value("allowAdditionalParameters", false);
            if (encoded.value("await", false))
                throw std::invalid_argument("async JavaScript commands are not enabled until continuation parity is installed");
            const auto rollback = Lower(encoded.value("rollback", std::string("boundary")));
            if (rollback == "reversible") descriptor.rollbackPolicy = vn::RollbackPolicy::Reversible;
            else if (rollback == "boundary") descriptor.rollbackPolicy = vn::RollbackPolicy::Boundary;
            else if (rollback == "transient") descriptor.rollbackPolicy = vn::RollbackPolicy::Transient;
            else throw std::invalid_argument("invalid command rollback policy");
            if (descriptor.id.empty() || !commandIds.insert(descriptor.id).second ||
                vn::CommandRegistry::Builtins().Find(descriptor.id) ||
                vn::CommandRegistry::Global().Find(descriptor.id))
                throw std::invalid_argument("empty, duplicate, or conflicting command id: " + descriptor.id);
            for (const auto& encodedParameter :
                 encoded.value("parameters", nlohmann::json::array())) {
                if (!encodedParameter.is_object())
                    throw std::invalid_argument("command parameter must be an object");
                const auto type = ManifestType(encodedParameter.at("type").get<std::string>());
                if (!type) throw std::invalid_argument("unsupported command parameter type");
                vn::CommandParameterDescriptor parameter;
                parameter.name = encodedParameter.at("name").get<std::string>();
                parameter.label = encodedParameter.value("displayName", parameter.name);
                parameter.description = encodedParameter.value("description", std::string{});
                parameter.type = *type;
                parameter.required = encodedParameter.value("required", false);
                parameter.resourceType = encodedParameter.value("resourceFilter", std::string{});
                if (encodedParameter.contains("default") && !encodedParameter["default"].is_null()) {
                    const auto value = JsonVariant(encodedParameter["default"], *type);
                    if (!value) throw std::invalid_argument("command parameter default type mismatch");
                    parameter.defaultValue = value->Clone();
                    parameter.hasDefault = true;
                }
                const auto options = encodedParameter.contains("enum")
                                         ? encodedParameter["enum"]
                                         : nlohmann::json::array();
                if (!options.is_array()) throw std::invalid_argument("command enum must be an array");
                for (const auto& option : options) parameter.options.push_back(option.get<std::string>());
                if (const auto range = encodedParameter.find("range");
                    range != encodedParameter.end() && !range->is_null()) {
                    if (!range->is_object()) throw std::invalid_argument("command range must be an object");
                    parameter.minimum = FiniteNumber(*range, "minimum");
                    parameter.maximum = FiniteNumber(*range, "maximum");
                }
                descriptor.parameters.push_back(std::move(parameter));
            }
            commands.push_back(std::move(descriptor));
        }

        std::vector<ui::ActionDescriptor> actions;
        std::unordered_set<std::string> actionIds;
        for (const auto& encoded : json.value("actions", nlohmann::json::array())) {
            if (!encoded.is_object()) throw std::invalid_argument("action descriptor must be an object");
            ui::ActionDescriptor descriptor;
            descriptor.id = encoded.at("id").get<std::string>();
            descriptor.label = encoded.value("displayName", descriptor.id);
            descriptor.displayName = descriptor.label;
            descriptor.description = encoded.value("description", std::string{});
            descriptor.category = encoded.value("category", std::string("Extension"));
            descriptor.origin = ui::ActionOrigin::ScriptExtension;
            descriptor.sourceId = id;
            descriptor.providerId = "script";
            descriptor.destructiveInPreview = encoded.value("destructiveInPreview", false);
            descriptor.allowAdditionalArguments = encoded.value("allowAdditionalArguments", false);
            const auto reentry = ui::ParseActionReentryPolicy(
                encoded.value("reentry", std::string("allow")));
            if (descriptor.id.empty() || !reentry || !actionIds.insert(descriptor.id).second)
                throw std::invalid_argument("empty, duplicate, or invalid action id: " + descriptor.id);
            descriptor.reentryPolicy = *reentry;
            for (const auto& encodedParameter :
                 encoded.value("parameters", nlohmann::json::array())) {
                if (!encodedParameter.is_object())
                    throw std::invalid_argument("action parameter must be an object");
                const auto type = ManifestType(encodedParameter.at("type").get<std::string>());
                if (!type) throw std::invalid_argument("unsupported action parameter type");
                ui::ActionArgumentDescriptor parameter;
                parameter.name = encodedParameter.at("name").get<std::string>();
                parameter.displayName = encodedParameter.value("displayName", parameter.name);
                parameter.description = encodedParameter.value("description", std::string{});
                parameter.type = *type;
                parameter.required = encodedParameter.value("required", false);
                parameter.resourceType = encodedParameter.value("resourceFilter", std::string{});
                if (encodedParameter.contains("default") && !encodedParameter["default"].is_null()) {
                    const auto value = JsonVariant(encodedParameter["default"], *type);
                    if (!value) throw std::invalid_argument("action parameter default type mismatch");
                    parameter.defaultValue = value->Clone();
                }
                const auto options = encodedParameter.contains("enum")
                                         ? encodedParameter["enum"]
                                         : nlohmann::json::array();
                if (!options.is_array()) throw std::invalid_argument("action enum must be an array");
                for (const auto& option : options) parameter.enumValues.push_back(option.get<std::string>());
                if (const auto range = encodedParameter.find("range");
                    range != encodedParameter.end() && !range->is_null()) {
                    if (!range->is_object()) throw std::invalid_argument("action range must be an object");
                    parameter.minimum = FiniteNumber(*range, "minimum");
                    parameter.maximum = FiniteNumber(*range, "maximum");
                }
                descriptor.arguments.push_back(std::move(parameter));
            }
            actions.push_back(std::move(descriptor));
        }

        vn::CommandRegistry stagedCommands;
        for (const auto& descriptor : commands) {
            const Status status = stagedCommands.Register(descriptor);
            if (!status) throw std::invalid_argument(
                "command descriptor failed schema validation: " + descriptor.id);
        }
        ui::ActionCatalog stagedActions;
        for (const auto& descriptor : actions) {
            const Status status = stagedActions.Register(descriptor);
            if (!status) throw std::invalid_argument(
                "action descriptor failed schema validation: " + descriptor.id);
        }
        for (const auto& command : commandIds)
            if (m_impl->commands.contains(command))
                throw std::invalid_argument("command callback already exists: " + command);
        for (const auto& action : actionIds)
            if (m_impl->actions.contains(action))
                throw std::invalid_argument("action callback already exists: " + action);

        stagedCommandIds = commandIds;
        stagedActionIds = actionIds;
        m_impl->declaredCommands.insert(commandIds.begin(), commandIds.end());
        m_impl->declaredActions.insert(actionIds.begin(), actionIds.end());
        m_impl->activeExtension = id;
        const auto separator = manifestPath.find_last_of('/');
        const std::string scriptPath = separator == std::string::npos
                                           ? entry
                                           : manifestPath.substr(0, separator + 1) + entry;
        const bool loaded = RunFile(scriptPath);
        m_impl->activeExtension.clear();
        if (!loaded) throw std::runtime_error("JavaScript extension entry failed");
        for (const auto& command : commandIds)
            if (!m_impl->commands.contains(command))
                throw std::invalid_argument("command has no Engine.RegisterCommand callback: " + command);
        for (const auto& action : actionIds)
            if (!m_impl->actions.contains(action))
                throw std::invalid_argument("action has no Engine.RegisterAction callback: " + action);
        const Status actionStatus = ui::ActionCatalog::Global().ReplaceSource(
            ui::ActionOrigin::ScriptExtension, id, std::move(actions));
        if (!actionStatus) throw std::invalid_argument("action catalog rejected extension descriptors");
        stagedActionSource = true;
        m_impl->loadedActionSources.insert(id);
        for (auto& descriptor : commands) {
            const Status commandStatus = vn::CommandRegistry::Global().Register(std::move(descriptor));
            if (!commandStatus) throw std::invalid_argument("command registry rejected extension descriptor");
        }
        return true;
    } catch (const std::exception& error) {
        m_impl->activeExtension.clear();
        discardStagedCallbacks();
        m_impl->Error(manifestPath, error.what());
        return false;
    }
}

bool JavaScriptHost::LoadExtensionIndex(const std::string& indexPath) {
    if (!m_impl->services.vfs) return false;
    const auto text = m_impl->services.vfs->ReadText(indexPath);
    if (!text) return false;
    const auto json = nlohmann::json::parse(*text, nullptr, false);
    if (!json.is_array()) {
        m_impl->Error(indexPath, "extension index must be an array of manifest paths");
        return false;
    }
    struct Entry {
        int order = 0;
        std::string id;
        std::string path;
    };
    std::vector<Entry> entries;
    for (const auto& item : json) {
        if (!item.is_string()) {
            m_impl->Error(indexPath, "extension index contains a non-string path");
            return false;
        }
        const auto path = item.get<std::string>();
        const auto manifest = m_impl->services.vfs->ReadText(path);
        if (!manifest) return false;
        const auto decoded = nlohmann::json::parse(*manifest, nullptr, false);
        if (!decoded.is_object()) return false;
        entries.push_back({decoded.value("order", 0),
                           decoded.value("id", std::string{}), path});
    }
    std::ranges::sort(entries, {}, [](const Entry& entry) {
        return std::tie(entry.order, entry.id, entry.path);
    });
    return std::ranges::all_of(entries, [this](const Entry& entry) {
        return LoadExtensionManifest(entry.path);
    });
}

void JavaScriptHost::Emit(const std::string& event, const EventArgs& args) {
    if (!m_impl->Ready()) return;
    const auto found = m_impl->events.find(event);
    if (found == m_impl->events.end()) return;
    JSValue payload = JS_NewObject(m_impl->context);
    for (const auto& [name, value] : args)
        JS_SetPropertyStr(m_impl->context, payload, name.c_str(),
                          JS_NewString(m_impl->context, value.c_str()));
    for (const auto& callback : found->second) {
        m_impl->BeginExecution();
        JSValue result = JS_Call(m_impl->context, callback, JS_UNDEFINED, 1, &payload);
        m_impl->EndExecution();
        if (JS_IsException(result)) m_impl->Error("event:" + event, m_impl->Exception());
        JS_FreeValue(m_impl->context, result);
    }
    JS_FreeValue(m_impl->context, payload);
}

bool JavaScriptHost::InvokeCommand(const vn::Command& command) {
    const auto found = m_impl->commands.find(command.type);
    if (found == m_impl->commands.end()) return false;
    JSValue arguments = JS_NewObject(m_impl->context);
    for (const auto& argument : command.args)
        JS_SetPropertyStr(m_impl->context, arguments, argument.key.c_str(),
                          JS_NewString(m_impl->context, argument.value.c_str()));
    for (const auto& [name, value] : command.typedArgs)
        JS_SetPropertyStr(m_impl->context, arguments, name.c_str(),
                          m_impl->ToJavaScript(value));
    m_impl->BeginExecution();
    JSValue result = JS_Call(m_impl->context, found->second, JS_UNDEFINED, 1, &arguments);
    m_impl->EndExecution();
    JS_FreeValue(m_impl->context, arguments);
    if (JS_IsException(result)) {
        m_impl->Error("command:" + command.type, m_impl->Exception());
        JS_FreeValue(m_impl->context, result);
        return false;
    }
    JS_FreeValue(m_impl->context, result);
    return true;
}

std::shared_ptr<ui::IActionProvider> JavaScriptHost::CreateActionProvider() {
    return std::make_shared<JavaScriptActionProvider>(*this);
}

bool JavaScriptHost::HasAction(const std::string_view action) const {
    return m_impl->actions.contains(std::string(action));
}

Status JavaScriptHost::InvokeAction(const ui::ActionInvocation& invocation) {
    const auto found = m_impl->actions.find(invocation.action);
    if (found == m_impl->actions.end())
        return Status::Fail(ScriptDiagnostic("PXJS7420", "JavaScript action callback is missing",
                                             invocation.action));
    JSValue arguments = JS_NewObject(m_impl->context);
    for (const auto& [name, value] : invocation.arguments)
        JS_SetPropertyStr(m_impl->context, arguments, name.c_str(),
                          m_impl->ToJavaScript(value));
    JSValue context = JS_NewObject(m_impl->context);
    JS_SetPropertyStr(m_impl->context, context, "scene",
                      JS_NewString(m_impl->context, invocation.context.sourceScene.c_str()));
    JS_SetPropertyStr(m_impl->context, context, "node",
                      JS_NewString(m_impl->context,
                                   invocation.context.sourceNode.ToString().c_str()));
    JS_SetPropertyStr(m_impl->context, context, "signal",
                      JS_NewString(m_impl->context, invocation.context.signal.c_str()));
    JS_SetPropertyStr(m_impl->context, context, "route",
                      JS_NewString(m_impl->context, invocation.context.currentRoute.c_str()));
    JS_SetPropertyStr(m_impl->context, context, "preview",
                      JS_NewBool(m_impl->context, invocation.context.preview));
    JSValue parameters[] = {arguments, context};
    m_impl->BeginExecution();
    JSValue result = JS_Call(m_impl->context, found->second, JS_UNDEFINED, 2, parameters);
    m_impl->EndExecution();
    JS_FreeValue(m_impl->context, arguments);
    JS_FreeValue(m_impl->context, context);
    if (JS_IsException(result)) {
        const auto error = m_impl->Exception();
        m_impl->Error("action:" + invocation.action, error);
        JS_FreeValue(m_impl->context, result);
        return Status::Fail(ScriptDiagnostic("PXJS7421", "JavaScript action failed", error));
    }
    JS_FreeValue(m_impl->context, result);
    return Status::Ok();
}

void JavaScriptHost::Update(float) {
    if (!m_impl->runtime) return;
    int jobs = 0;
    while (JS_IsJobPending(m_impl->runtime) && jobs++ < 64) {
        JSContext* context = nullptr;
        m_impl->BeginExecution();
        const int result = JS_ExecutePendingJob(m_impl->runtime, &context);
        m_impl->EndExecution();
        if (result < 0) {
            m_impl->Error("promise-job", m_impl->Exception());
            break;
        }
    }
}

bool JavaScriptHost::HasPendingCommand() const { return false; }
bool JavaScriptHost::HasPendingAction() const { return false; }
PendingCommandsState JavaScriptHost::CapturePending() const { return {}; }
PendingActionsState JavaScriptHost::CapturePendingActions() const { return {}; }

Status JavaScriptHost::RestorePending(const PendingCommandsState& state) {
    if (state.empty()) return Status::Ok();
    return Status::Fail(ScriptDiagnostic(
        "PXJS7501", "JavaScript command checkpoint is unsupported by this host revision"));
}

Status JavaScriptHost::RestorePendingActions(const PendingActionsState& state) {
    if (state.empty()) return Status::Ok();
    return Status::Fail(ScriptDiagnostic(
        "PXJS7502", "JavaScript Action checkpoint is unsupported by this host revision"));
}

void JavaScriptHost::CancelPending() {}

std::vector<DebugBreakpoint> JavaScriptHost::SetDebugBreakpoints(
    std::vector<DebugBreakpoint>) {
    return {};
}

bool JavaScriptHost::DebugPause() { return false; }
bool JavaScriptHost::DebugContinue() { return false; }
bool JavaScriptHost::DebugStep() { return false; }

std::optional<DebugVariable> JavaScriptHost::EvaluateDebugWatch(
    std::string_view) const {
    return std::nullopt;
}

const DebugSnapshot& JavaScriptHost::CaptureDebugState() const {
    return m_impl->debug;
}

}  // namespace px::script
