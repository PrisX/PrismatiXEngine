#include "Engine/Script/JavaScriptHost.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Support/Logger.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"

#include <nlohmann/json.hpp>
#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
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

std::string NormalizeDebugSource(std::string source) {
    if (!source.empty() && source.front() == '@') source.erase(0, 1);
    std::ranges::replace(source, '\\', '/');
    return source;
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
    ui::ProviderActionStart Start(const ui::ActionInvocation& invocation) override {
        return m_host.StartAction(invocation);
    }
    [[nodiscard]] ui::ActionExecutionState Poll(
        const std::uint64_t handle) const override {
        return m_host.ActionState(handle);
    }
    void Cancel(const std::uint64_t handle) override {
        m_host.CancelAction(handle);
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
            for (auto& pending : pendingCommands) FreeContinuation(pending);
            for (auto& pending : pendingActions) FreeContinuation(pending);
            DiscardCreatedPromiseWait();
            JS_FreeValue(context, debugLocals);
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

    struct WaitToken {
        std::string kind;
        std::uint64_t handle = 0;
        float remainingSeconds = 0.0f;
    };

    enum class ContinuationFlavor { Generator, Promise };

    struct PendingCommandContinuation {
        ContinuationFlavor flavor = ContinuationFlavor::Generator;
        JSValue continuation = JS_UNDEFINED;
        JSValue resume = JS_UNDEFINED;
        vn::Command command;
        std::uint32_t yieldIndex = 0;
        WaitToken wait;
    };

    struct PendingActionContinuation {
        ContinuationFlavor flavor = ContinuationFlavor::Generator;
        JSValue continuation = JS_UNDEFINED;
        JSValue resume = JS_UNDEFINED;
        std::uint64_t id = 0;
        ui::ActionInvocation invocation;
        std::uint32_t yieldIndex = 0;
        WaitToken wait;
    };

    struct PromiseWaitCapability {
        WaitToken wait;
        JSValue resolve = JS_UNDEFINED;
    };

    enum class StepResult { Yielded, Finished, Failed };

    template <typename Pending>
    void FreeContinuation(Pending& pending) {
        JS_FreeValue(context, pending.continuation);
        JS_FreeValue(context, pending.resume);
        pending.continuation = JS_UNDEFINED;
        pending.resume = JS_UNDEFINED;
    }

    void DiscardCreatedPromiseWait() {
        if (!createdPromiseWait) return;
        JS_FreeValue(context, createdPromiseWait->resolve);
        createdPromiseWait.reset();
    }

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

    [[nodiscard]] bool StringArgument(const int count, JSValueConst* values,
                                      const int index, std::string& result) const {
        if (index >= count || !JS_IsString(values[index])) return false;
        const auto converted = String(values[index]);
        if (!converted) return false;
        result = *converted;
        return true;
    }

    [[nodiscard]] bool NumberArgument(const int count, JSValueConst* values,
                                      const int index, double& result) const {
        return index < count && JS_IsNumber(values[index]) &&
               JS_ToFloat64(context, &result, values[index]) == 0 &&
               std::isfinite(result);
    }

    [[nodiscard]] bool IntegerArgument(const int count, JSValueConst* values,
                                       const int index, std::int64_t& result) const {
        return index < count && JS_IsNumber(values[index]) &&
               JS_ToInt64(context, &result, values[index]) == 0;
    }

    [[nodiscard]] bool BoolArgument(const int count, JSValueConst* values,
                                    const int index, bool& result) const {
        if (index >= count || !JS_IsBool(values[index])) return false;
        const int converted = JS_ToBool(context, values[index]);
        if (converted < 0) return false;
        result = converted != 0;
        return true;
    }

    [[nodiscard]] std::optional<Variant> FromJavaScript(
        JSValueConst value, const int depth = 0) const {
        if (depth > 32) return std::nullopt;
        if (JS_IsNull(value) || JS_IsUndefined(value)) return Variant{};
        if (JS_IsBool(value)) {
            const int converted = JS_ToBool(context, value);
            return converted < 0 ? std::nullopt
                                 : std::optional<Variant>(Variant(converted != 0));
        }
        if (JS_IsNumber(value)) {
            double number = 0.0;
            if (JS_ToFloat64(context, &number, value) < 0 || !std::isfinite(number))
                return std::nullopt;
            if (std::trunc(number) == number &&
                number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                number <= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                return Variant(static_cast<std::int64_t>(number));
            return Variant(number);
        }
        if (JS_IsString(value)) {
            const auto text = String(value);
            return text ? std::optional<Variant>(Variant(*text)) : std::nullopt;
        }
        if (JS_IsArray(value)) {
            JSValue lengthValue = JS_GetPropertyStr(context, value, "length");
            std::uint32_t length = 0;
            const bool validLength = JS_ToUint32(context, &length, lengthValue) == 0 &&
                                     length <= 1'000'000;
            JS_FreeValue(context, lengthValue);
            if (!validLength) return std::nullopt;
            VariantArray array;
            array.reserve(length);
            for (std::uint32_t index = 0; index < length; ++index) {
                JSValue item = JS_GetPropertyUint32(context, value, index);
                auto converted = FromJavaScript(item, depth + 1);
                JS_FreeValue(context, item);
                if (!converted) return std::nullopt;
                array.push_back(std::move(*converted));
            }
            return Variant(std::move(array));
        }
        if (JS_IsObject(value)) {
            JSPropertyEnum* properties = nullptr;
            std::uint32_t count = 0;
            if (JS_GetOwnPropertyNames(context, &properties, &count, value,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0 ||
                count > 1'000'000) {
                if (properties) js_free(context, properties);
                return std::nullopt;
            }
            VariantObject object;
            bool valid = true;
            for (std::uint32_t index = 0; index < count; ++index) {
                const char* key = JS_AtomToCString(context, properties[index].atom);
                if (!key) {
                    valid = false;
                    break;
                }
                JSValue item = JS_GetProperty(context, value, properties[index].atom);
                auto converted = FromJavaScript(item, depth + 1);
                JS_FreeValue(context, item);
                if (converted) object.emplace(key, std::move(*converted));
                else valid = false;
                JS_FreeCString(context, key);
                if (!valid) break;
            }
            js_free(context, properties);
            if (!valid) return std::nullopt;
            return Variant(std::move(object));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> NumberProperty(JSValueConst object,
                                                        const char* name) const {
        JSValue value = JS_GetPropertyStr(context, object, name);
        if (JS_IsUndefined(value) || JS_IsNull(value)) {
            JS_FreeValue(context, value);
            return std::nullopt;
        }
        double result = 0.0;
        const bool valid = JS_IsNumber(value) &&
                           JS_ToFloat64(context, &result, value) == 0 &&
                           std::isfinite(result);
        JS_FreeValue(context, value);
        return valid ? std::optional<double>(result) : std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> StringProperty(
        JSValueConst object, const char* name) const {
        JSValue value = JS_GetPropertyStr(context, object, name);
        if (JS_IsUndefined(value) || JS_IsNull(value)) {
            JS_FreeValue(context, value);
            return std::nullopt;
        }
        const auto result = JS_IsString(value) ? String(value) : std::nullopt;
        JS_FreeValue(context, value);
        return result;
    }

    JSValue CreatePromiseWait(WaitToken wait) {
        if (!acceptingPromiseWait)
            return JS_ThrowTypeError(
                context,
                "Wait operations are only valid inside an invoked async command or Action");
        if (createdPromiseWait)
            return JS_ThrowTypeError(
                context,
                "Only one unresolved engine wait may be created at an async boundary");
        JSValue resolvingFunctions[2] = {JS_UNDEFINED, JS_UNDEFINED};
        JSValue promise = JS_NewPromiseCapability(context, resolvingFunctions);
        if (JS_IsException(promise)) return promise;
        JS_FreeValue(context, resolvingFunctions[1]);
        createdPromiseWait = PromiseWaitCapability{
            .wait = std::move(wait), .resolve = resolvingFunctions[0]};
        return promise;
    }

    [[nodiscard]] std::string DebugValue(JSValueConst value) const {
        if (JS_IsUndefined(value)) return "undefined";
        if (JS_IsNull(value)) return "null";
        if (JS_IsBool(value)) return JS_ToBool(context, value) ? "true" : "false";
        if (JS_IsNumber(value) || JS_IsString(value)) {
            auto text = String(value).value_or(std::string{});
            constexpr std::size_t limit = 160;
            const bool truncated = text.size() > limit;
            if (text.size() > limit) text.resize(limit);
            return text + (truncated ? "…" : "");
        }
        if (JS_IsFunction(context, value)) return "<function>";
        if (JS_IsArray(value)) return "<array>";
        if (JS_IsObject(value)) return "<object>";
        return "<value>";
    }

    [[nodiscard]] bool BreakpointMatches(const std::string& source,
                                         const int line) const {
        return std::ranges::any_of(
            debugBreakpoints, [&](const auto& configured) {
                const auto& [configuredSource, lines] = configured;
                const bool sourceMatches =
                    configuredSource.empty() || source == configuredSource ||
                    (source.size() > configuredSource.size() &&
                     source.ends_with(configuredSource));
                return sourceMatches && lines.contains(line);
            });
    }

    [[nodiscard]] bool ShouldPauseAt(const std::string& source, const int line,
                                     std::string& reason) {
        const bool breakpoint = BreakpointMatches(source, line);
        if (!breakpoint && !debugPauseRequested && !debugStepRequested)
            return false;
        reason = breakpoint ? "breakpoint"
                            : debugStepRequested ? "step" : "pause";
        debugPauseRequested = false;
        debugStepRequested = false;
        return true;
    }

    void CaptureDebugPoint(const std::string& source, const int line,
                           JSValueConst locals, std::string function,
                           std::string reason) {
        debug = {};
        debug.paused = true;
        debug.reason = std::move(reason);
        JS_FreeValue(context, debugLocals);
        debugLocals = JS_IsObject(locals) ? JS_DupValue(context, locals)
                                          : JS_NewObject(context);

        DebugFrame frame;
        frame.source = source;
        frame.function = function.empty() ? "<anonymous>" : std::move(function);
        frame.line = line;
        JSPropertyEnum* properties = nullptr;
        std::uint32_t count = 0;
        if (JS_GetOwnPropertyNames(context, &properties, &count, debugLocals,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
            count = std::min<std::uint32_t>(count, 128);
            for (std::uint32_t index = 0; index < count; ++index) {
                const char* name =
                    JS_AtomToCString(context, properties[index].atom);
                if (!name) continue;
                JSValue value =
                    JS_GetProperty(context, debugLocals, properties[index].atom);
                frame.locals.push_back({name, DebugValue(value)});
                JS_FreeValue(context, value);
                JS_FreeCString(context, name);
            }
            js_free(context, properties);
        }
        debug.frames.push_back(std::move(frame));
    }

    JSValue RuntimeOperation(const std::string_view operation, const int count,
                             JSValueConst* values) {
        std::string first;
        std::string second;
        std::int64_t integer = 0;
        bool boolean = false;

        if (operation == "debug.point") {
            if (!StringArgument(count, values, 0, first) ||
                !IntegerArgument(count, values, 1, integer) || integer <= 0 ||
                integer > std::numeric_limits<int>::max())
                return JS_ThrowTypeError(
                    context, "DebugPoint requires a source path and positive line");
            const std::string source = NormalizeDebugSource(std::move(first));
            std::string reason;
            if (!ShouldPauseAt(source, static_cast<int>(integer), reason))
                return JS_NewSettledPromise(context, false, JS_UNDEFINED);
            if (!acceptingPromiseWait)
                return JS_ThrowTypeError(
                    context,
                    "DebugPoint can only pause inside an invoked async command or Action");
            std::string function;
            if (count >= 4 && !JS_IsUndefined(values[3]) &&
                !StringArgument(count, values, 3, function))
                return JS_ThrowTypeError(context,
                                          "DebugPoint function must be a string");
            const JSValueConst locals = count >= 3 ? values[2] : JS_UNDEFINED;
            if (!JS_IsUndefined(locals) && !JS_IsObject(locals))
                return JS_ThrowTypeError(context,
                                          "DebugPoint locals must be an object");
            CaptureDebugPoint(source, static_cast<int>(integer), locals,
                              std::move(function), std::move(reason));
            return CreatePromiseWait({.kind = "debug"});
        }
        if (operation == "await.timer") {
            double seconds = 0.0;
            if (!NumberArgument(count, values, 0, seconds) || seconds < 0.0 ||
                seconds > std::numeric_limits<float>::max())
                return JS_ThrowRangeError(context,
                                          "AwaitSeconds requires non-negative seconds");
            JSValue token = JS_NewObject(context);
            JS_SetPropertyStr(context, token, "kind",
                              JS_NewString(context, "timer"));
            JS_SetPropertyStr(context, token, "seconds",
                              JS_NewFloat64(context, seconds));
            return token;
        }
        if (operation == "await.animation") {
            if (!IntegerArgument(count, values, 0, integer) || integer < 0)
                return JS_ThrowRangeError(context,
                                          "AwaitAnimation requires a valid handle");
            JSValue token = JS_NewObject(context);
            JS_SetPropertyStr(context, token, "kind",
                              JS_NewString(context, "animation"));
            JS_SetPropertyStr(context, token, "handle",
                              JS_NewInt64(context, integer));
            return token;
        }
        if (operation == "wait.timer") {
            double seconds = 0.0;
            if (!NumberArgument(count, values, 0, seconds) || seconds < 0.0 ||
                seconds > std::numeric_limits<float>::max())
                return JS_ThrowRangeError(context,
                                          "WaitSeconds requires non-negative seconds");
            return CreatePromiseWait(
                {.kind = "timer",
                 .remainingSeconds = static_cast<float>(seconds)});
        }
        if (operation == "wait.animation") {
            if (!IntegerArgument(count, values, 0, integer) || integer < 0)
                return JS_ThrowRangeError(context,
                                          "WaitAnimation requires a valid handle");
            return CreatePromiseWait(
                {.kind = "animation",
                 .handle = static_cast<std::uint64_t>(integer)});
        }
        if (operation == "variables.get") {
            if (!services.variables)
                return JS_ThrowInternalError(context, "VariableStore is unavailable");
            if (!StringArgument(count, values, 0, first))
                return JS_ThrowTypeError(context, "GetVariable requires a name");
            const auto* value = services.variables->GetValue(first);
            return value ? ToJavaScript(*value) : JS_UNDEFINED;
        }
        if (operation == "variables.set") {
            if (!services.variables)
                return JS_ThrowInternalError(context, "VariableStore is unavailable");
            if (!StringArgument(count, values, 0, first) || count < 2)
                return JS_ThrowTypeError(context, "SetVariable requires a name and value");
            auto value = FromJavaScript(values[1]);
            if (!value)
                return JS_ThrowTypeError(context, "SetVariable received an unsupported value");
            std::string scope = "save";
            if (count >= 3 && !JS_IsUndefined(values[2]) &&
                !StringArgument(count, values, 2, scope))
                return JS_ThrowTypeError(context, "SetVariable scope must be a string");
            vn::VariableScope selected = vn::VariableScope::SaveLocal;
            if (scope == "persistent") selected = vn::VariableScope::Persistent;
            else if (scope == "temporary") selected = vn::VariableScope::Temporary;
            else if (scope != "save")
                return JS_ThrowRangeError(context, "unknown variable scope: %s", scope.c_str());
            services.variables->SetValue(first, std::move(*value), selected);
            return JS_UNDEFINED;
        }
        if (operation == "resource.exists") {
            if (!services.vfs) return JS_ThrowInternalError(context, "VFS is unavailable");
            if (!StringArgument(count, values, 0, first))
                return JS_ThrowTypeError(context, "ResourceExists requires a path");
            return JS_NewBool(context, services.vfs->Exists(first));
        }
        if (operation == "resource.readText") {
            if (!services.vfs) return JS_ThrowInternalError(context, "VFS is unavailable");
            if (!StringArgument(count, values, 0, first))
                return JS_ThrowTypeError(context, "ReadResourceText requires a path");
            const auto text = services.vfs->ReadText(first);
            if (!text) return JS_ThrowReferenceError(context, "resource was not found: %s", first.c_str());
            return JS_NewStringLen(context, text->data(), text->size());
        }
        if (operation.starts_with("route.")) {
            if (!services.routes)
                return JS_ThrowInternalError(context, "UIRouter is unavailable");
            if (operation == "route.back")
                return JS_NewBool(context, static_cast<bool>(services.routes->Back()));
            if (operation == "route.closeModal")
                return JS_NewBool(context, static_cast<bool>(services.routes->CloseModal()));
            if (!StringArgument(count, values, 0, first))
                return JS_ThrowTypeError(context, "route operation requires a route id");
            Status status;
            if (operation == "route.push") status = services.routes->Push(first);
            else if (operation == "route.replace") status = services.routes->Replace(first);
            else if (operation == "route.showModal") status = services.routes->ShowModal(first);
            else return JS_ThrowRangeError(context, "unknown route operation");
            if (!status)
                return JS_ThrowInternalError(context, "%s",
                    diag::Describe(status.Diagnostics().front()).c_str());
            return JS_TRUE;
        }
        if (operation.starts_with("profile.")) {
            if (!services.profile)
                return JS_ThrowInternalError(context, "GlobalProfile is unavailable");
            if (operation == "profile.clearCount")
                return JS_NewInt32(context, services.profile->ClearCount());
            if (!StringArgument(count, values, 0, first))
                return JS_ThrowTypeError(context, "profile operation requires an id");
            if (operation == "profile.hasSeen")
                return JS_NewBool(context, services.profile->HasSeen(first));
            if (operation == "profile.markSeen") {
                services.profile->MarkSeen(first);
                return JS_UNDEFINED;
            }
            if (operation == "profile.cgUnlocked")
                return JS_NewBool(context, services.profile->CGUnlocked(first));
            if (operation == "profile.sceneUnlocked")
                return JS_NewBool(context, services.profile->SceneUnlocked(first));
            if (operation == "profile.unlockCG") {
                services.profile->UnlockCG(first);
                return JS_UNDEFINED;
            }
            if (operation == "profile.unlockScene") {
                services.profile->UnlockScene(first);
                return JS_UNDEFINED;
            }
            if (operation == "profile.persistentVar")
                return JS_NewInt32(context, services.profile->PersistentVar(first));
            return JS_ThrowRangeError(context, "unknown profile operation");
        }
        if (operation.starts_with("audio.")) {
            if (!services.audio)
                return JS_ThrowInternalError(context, "AudioEngine is unavailable");
            if (operation == "audio.playSE") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "PlaySE requires a path");
                services.audio->PlaySE(first);
                return JS_UNDEFINED;
            }
            if (operation == "audio.playBGM" || operation == "audio.playAmbience") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "audio play requires a path");
                bool loop = true;
                std::int64_t fade = 0;
                if (count >= 2 && !JS_IsUndefined(values[1]) &&
                    !BoolArgument(count, values, 1, loop))
                    return JS_ThrowTypeError(context, "audio loop must be boolean");
                if (count >= 3 && !JS_IsUndefined(values[2]) &&
                    !IntegerArgument(count, values, 2, fade))
                    return JS_ThrowTypeError(context, "audio fade must be integer milliseconds");
                if (operation == "audio.playBGM")
                    services.audio->PlayBGM(first, loop, static_cast<int>(fade));
                else services.audio->PlayAmbience(first, loop, static_cast<int>(fade));
                return JS_UNDEFINED;
            }
            if (operation == "audio.stopBGM" || operation == "audio.stopAmbience") {
                std::int64_t fade = 0;
                if (count >= 1 && !JS_IsUndefined(values[0]) &&
                    !IntegerArgument(count, values, 0, fade))
                    return JS_ThrowTypeError(context, "audio fade must be integer milliseconds");
                if (operation == "audio.stopBGM") services.audio->StopBGM(static_cast<int>(fade));
                else services.audio->StopAmbience(static_cast<int>(fade));
                return JS_UNDEFINED;
            }
            if (!IntegerArgument(count, values, 0, integer))
                return JS_ThrowTypeError(context, "audio volume requires an integer");
            if (operation == "audio.bgmVolume") services.audio->SetBGMVolume(static_cast<int>(integer));
            else if (operation == "audio.seVolume") services.audio->SetSEVolume(static_cast<int>(integer));
            else if (operation == "audio.voiceVolume") services.audio->SetVoiceVolume(static_cast<int>(integer));
            else if (operation == "audio.ambienceVolume") services.audio->SetAmbienceVolume(static_cast<int>(integer));
            else return JS_ThrowRangeError(context, "unknown audio operation");
            return JS_UNDEFINED;
        }
        if (operation.starts_with("stage.")) {
            if (!services.stage)
                return JS_ThrowInternalError(context, "Stage is unavailable");
            if (operation == "stage.background") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "SetBackground requires a path");
                boolean = true;
                if (count >= 2 && !JS_IsUndefined(values[1]) &&
                    !BoolArgument(count, values, 1, boolean))
                    return JS_ThrowTypeError(context, "transition must be boolean");
                services.stage->SetBackground(first, boolean);
                return JS_UNDEFINED;
            }
            if (operation == "stage.character") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(context, "SetCharacter requires a name and image");
                std::int64_t slot = 2;
                bool transition = true;
                double x = 0.0, y = 0.0, scale = 1.0;
                if (count >= 3 && !JS_IsUndefined(values[2]) && !IntegerArgument(count, values, 2, slot))
                    return JS_ThrowTypeError(context, "character slot must be integer");
                if (count >= 4 && !JS_IsUndefined(values[3]) && !BoolArgument(count, values, 3, transition))
                    return JS_ThrowTypeError(context, "character transition must be boolean");
                if (count >= 5 && !JS_IsUndefined(values[4]) && !NumberArgument(count, values, 4, x))
                    return JS_ThrowTypeError(context, "character x must be numeric");
                if (count >= 6 && !JS_IsUndefined(values[5]) && !NumberArgument(count, values, 5, y))
                    return JS_ThrowTypeError(context, "character y must be numeric");
                if (count >= 7 && !JS_IsUndefined(values[6]) && !NumberArgument(count, values, 6, scale))
                    return JS_ThrowTypeError(context, "character scale must be numeric");
                services.stage->SetCharacter(first, second, static_cast<int>(slot), transition,
                                             static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(scale));
                return JS_UNDEFINED;
            }
            if (operation == "stage.clearCharacter") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "ClearCharacter requires a name");
                boolean = true;
                if (count >= 2 && !JS_IsUndefined(values[1]) && !BoolArgument(count, values, 1, boolean))
                    return JS_ThrowTypeError(context, "transition must be boolean");
                services.stage->ClearCharacter(first, boolean);
                return JS_UNDEFINED;
            }
            if (operation == "stage.moveCharacter") {
                if (!StringArgument(count, values, 0, first) ||
                    !IntegerArgument(count, values, 1, integer))
                    return JS_ThrowTypeError(context, "MoveCharacter requires a name and slot");
                services.stage->MoveCharacter(first, static_cast<int>(integer));
                return JS_UNDEFINED;
            }
            if (operation == "stage.layer") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(context, "SetLayer requires a name and image");
                double x = 0.0, y = 0.0, scale = 1.0;
                std::int64_t alpha = 255, z = 0;
                if (count >= 3 && !JS_IsUndefined(values[2]) && !NumberArgument(count, values, 2, x)) return JS_ThrowTypeError(context, "layer x must be numeric");
                if (count >= 4 && !JS_IsUndefined(values[3]) && !NumberArgument(count, values, 3, y)) return JS_ThrowTypeError(context, "layer y must be numeric");
                if (count >= 5 && !JS_IsUndefined(values[4]) && !NumberArgument(count, values, 4, scale)) return JS_ThrowTypeError(context, "layer scale must be numeric");
                if (count >= 6 && !JS_IsUndefined(values[5]) && !IntegerArgument(count, values, 5, alpha)) return JS_ThrowTypeError(context, "layer alpha must be integer");
                if (count >= 7 && !JS_IsUndefined(values[6]) && !IntegerArgument(count, values, 6, z)) return JS_ThrowTypeError(context, "layer z must be integer");
                services.stage->SetLayer(first, second, static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(scale),
                                         static_cast<std::uint8_t>(std::clamp<std::int64_t>(alpha, 0, 255)),
                                         static_cast<int>(z));
                return JS_UNDEFINED;
            }
            if (operation == "stage.clearLayer") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "ClearLayer requires a name");
                services.stage->ClearLayer(first);
                return JS_UNDEFINED;
            }
            if (operation == "stage.shake") {
                std::int64_t milliseconds = 400;
                double amplitude = 12.0;
                if (count >= 1 && !JS_IsUndefined(values[0]) && !IntegerArgument(count, values, 0, milliseconds)) return JS_ThrowTypeError(context, "shake duration must be integer");
                if (count >= 2 && !JS_IsUndefined(values[1]) && !NumberArgument(count, values, 1, amplitude)) return JS_ThrowTypeError(context, "shake amplitude must be numeric");
                services.stage->Shake(static_cast<int>(milliseconds), static_cast<float>(amplitude));
                return JS_UNDEFINED;
            }
            if (operation == "stage.animate") {
                if (!StringArgument(count, values, 0, first) || count < 2 ||
                    !JS_IsObject(values[1]))
                    return JS_ThrowTypeError(context, "Animate requires a target and property object");
                vn::Stage::TweenSpec spec;
                if (const auto value = NumberProperty(values[1], "x")) { spec.hasX = true; spec.x = static_cast<float>(*value); }
                if (const auto value = NumberProperty(values[1], "y")) { spec.hasY = true; spec.y = static_cast<float>(*value); }
                if (const auto value = NumberProperty(values[1], "scale")) { spec.hasScale = true; spec.scale = static_cast<float>(*value); }
                if (const auto value = NumberProperty(values[1], "alpha")) { spec.hasAlpha = true; spec.alpha = static_cast<float>(*value); }
                if (const auto value = NumberProperty(values[1], "duration")) spec.durationMs = static_cast<int>(*value);
                if (const auto value = StringProperty(values[1], "ease")) spec.ease = *value;
                return JS_NewBool(context, services.stage->Animate(first, spec));
            }
            return JS_ThrowRangeError(context, "unknown stage operation");
        }
        if (operation.starts_with("animation.")) {
            if (!services.timeline)
                return JS_ThrowInternalError(context, "TimelinePlayer is unavailable");
            if (operation == "animation.load") {
                if (!services.vfs) return JS_ThrowInternalError(context, "VFS is unavailable");
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "LoadAnimation requires a path");
                const auto text = services.vfs->ReadText(first);
                if (!text) return JS_ThrowReferenceError(context, "animation was not found: %s", first.c_str());
                auto clip = animation::ParseAnimationClip(*text, first);
                if (!clip) return JS_ThrowTypeError(context, "%s",
                    diag::Describe(clip.Diagnostics().front()).c_str());
                const auto id = clip.Value().id;
                const Status status = services.timeline->Register(clip.TakeValue());
                if (!status) return JS_ThrowInternalError(context, "%s",
                    diag::Describe(status.Diagnostics().front()).c_str());
                return JS_NewString(context, id.ToString().c_str());
            }
            if (operation == "animation.play") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "PlayAnimation requires a ResourceId");
                const auto id = Uuid::Parse(first);
                if (!id) return JS_ThrowTypeError(context, "animation ResourceId is invalid");
                bool await = false;
                double speed = 1.0;
                if (count >= 2 && !JS_IsUndefined(values[1]) && !BoolArgument(count, values, 1, await)) return JS_ThrowTypeError(context, "animation await must be boolean");
                if (count >= 3 && !JS_IsUndefined(values[2]) && !NumberArgument(count, values, 2, speed)) return JS_ThrowTypeError(context, "animation speed must be numeric");
                return JS_NewInt64(context, static_cast<std::int64_t>(
                    services.timeline->Play(*id, await, static_cast<float>(speed))));
            }
            if (operation == "animation.cancel") {
                if (!IntegerArgument(count, values, 0, integer))
                    return JS_ThrowTypeError(context, "CancelAnimation requires a handle");
                return JS_NewBool(context, static_cast<bool>(services.timeline->Cancel(
                    static_cast<animation::PlaybackHandle>(integer))));
            }
            return JS_ThrowRangeError(context, "unknown animation operation");
        }
        if (operation.starts_with("input.")) {
            if (!services.input) return JS_ThrowInternalError(context, "Input is unavailable");
            if (operation == "input.mouseX") return JS_NewFloat64(context, services.input->MouseX());
            if (operation == "input.mouseY") return JS_NewFloat64(context, services.input->MouseY());
            if (operation == "input.leftClick") return JS_NewBool(context, services.input->LeftClick());
            if (operation == "input.rightClick") return JS_NewBool(context, services.input->RightClick());
            return JS_ThrowRangeError(context, "unknown input operation");
        }
        if (operation.starts_with("renderer.")) {
            if (!services.renderer)
                return JS_ThrowInternalError(context, "Renderer2D is unavailable");
            if (operation == "renderer.logicalSize") {
                int width = 0, height = 0;
                services.renderer->GetLogicalSize(width, height);
                JSValue result = JS_NewObject(context);
                JS_SetPropertyStr(context, result, "w", JS_NewInt32(context, width));
                JS_SetPropertyStr(context, result, "h", JS_NewInt32(context, height));
                return result;
            }
            if (operation == "renderer.drawImage") {
                if (!StringArgument(count, values, 0, first)) return JS_ThrowTypeError(context, "DrawImage requires a path");
                double x=0,y=0,width=0,height=0;std::int64_t alpha=255;
                if (!NumberArgument(count, values, 1, x) || !NumberArgument(count, values, 2, y) || !NumberArgument(count, values, 3, width) || !NumberArgument(count, values, 4, height)) return JS_ThrowTypeError(context, "DrawImage requires numeric bounds");
                if (count >= 6 && !JS_IsUndefined(values[5]) && !IntegerArgument(count, values, 5, alpha)) return JS_ThrowTypeError(context, "DrawImage alpha must be integer");
                services.renderer->DrawImage(first, Rect{static_cast<float>(x),static_cast<float>(y),static_cast<float>(width),static_cast<float>(height)}, static_cast<std::uint8_t>(std::clamp<std::int64_t>(alpha,0,255)));
                return JS_UNDEFINED;
            }
            if (operation == "renderer.drawAuto") {
                if (!StringArgument(count, values, 0, first) || !IntegerArgument(count, values, 1, integer)) return JS_ThrowTypeError(context, "DrawAuto requires a path and display mode");
                std::int64_t alpha=255;if(count>=3&&!JS_IsUndefined(values[2])&&!IntegerArgument(count,values,2,alpha))return JS_ThrowTypeError(context,"DrawAuto alpha must be integer");
                (void)services.renderer->DrawImageAuto(first, static_cast<graphics::DisplayMode>(integer), static_cast<std::uint8_t>(std::clamp<std::int64_t>(alpha,0,255)));
                return JS_UNDEFINED;
            }
            if (operation == "renderer.drawRect" || operation == "renderer.drawRoundedRect") {
                double x=0,y=0,width=0,height=0,radius=0;int offset=4;
                if(!NumberArgument(count,values,0,x)||!NumberArgument(count,values,1,y)||!NumberArgument(count,values,2,width)||!NumberArgument(count,values,3,height))return JS_ThrowTypeError(context,"rectangle bounds must be numeric");
                if(operation=="renderer.drawRoundedRect"){if(!NumberArgument(count,values,4,radius))return JS_ThrowTypeError(context,"rounded radius must be numeric");offset=5;}
                std::int64_t red=0,green=0,blue=0,alpha=0;
                if(!IntegerArgument(count,values,offset,red)||!IntegerArgument(count,values,offset+1,green)||!IntegerArgument(count,values,offset+2,blue)||!IntegerArgument(count,values,offset+3,alpha))return JS_ThrowTypeError(context,"rectangle color channels must be integers");
                const Color color{static_cast<std::uint8_t>(std::clamp<std::int64_t>(red,0,255)),static_cast<std::uint8_t>(std::clamp<std::int64_t>(green,0,255)),static_cast<std::uint8_t>(std::clamp<std::int64_t>(blue,0,255)),static_cast<std::uint8_t>(std::clamp<std::int64_t>(alpha,0,255))};
                const Rect rect{static_cast<float>(x),static_cast<float>(y),static_cast<float>(width),static_cast<float>(height)};
                if(operation=="renderer.drawRect")services.renderer->DrawRect(rect,color);else services.renderer->DrawRoundedRect(rect,static_cast<float>(radius),color);
                return JS_UNDEFINED;
            }
            if (operation == "renderer.drawText") {
                if(!StringArgument(count,values,0,first))return JS_ThrowTypeError(context,"DrawText requires text");
                double x=0,y=0;std::string font;std::int64_t size=0,red=0,green=0,blue=0,alpha=255;
                if(!NumberArgument(count,values,1,x)||!NumberArgument(count,values,2,y)||!StringArgument(count,values,3,font)||!IntegerArgument(count,values,4,size)||!IntegerArgument(count,values,5,red)||!IntegerArgument(count,values,6,green)||!IntegerArgument(count,values,7,blue))return JS_ThrowTypeError(context,"DrawText arguments are invalid");
                if(count>=9&&!JS_IsUndefined(values[8])&&!IntegerArgument(count,values,8,alpha))return JS_ThrowTypeError(context,"DrawText alpha must be integer");
                services.renderer->DrawText(first,static_cast<float>(x),static_cast<float>(y),font,static_cast<int>(size),Color{static_cast<std::uint8_t>(std::clamp<std::int64_t>(red,0,255)),static_cast<std::uint8_t>(std::clamp<std::int64_t>(green,0,255)),static_cast<std::uint8_t>(std::clamp<std::int64_t>(blue,0,255)),static_cast<std::uint8_t>(std::clamp<std::int64_t>(alpha,0,255))});
                return JS_UNDEFINED;
            }
            if (operation == "renderer.measureText") {
                if(!StringArgument(count,values,0,first)||!StringArgument(count,values,1,second)||!IntegerArgument(count,values,2,integer))return JS_ThrowTypeError(context,"MeasureText requires text, font, and size");
                const Vec2 measured=services.renderer->MeasureText(first,second,static_cast<int>(integer));JSValue result=JS_NewObject(context);JS_SetPropertyStr(context,result,"w",JS_NewFloat64(context,measured.x));JS_SetPropertyStr(context,result,"h",JS_NewFloat64(context,measured.y));return result;
            }
            return JS_ThrowRangeError(context, "unknown renderer operation");
        }
        return JS_ThrowRangeError(context, "unknown runtime operation: %.*s",
                                  static_cast<int>(operation.size()), operation.data());
    }

    static JSValue RuntimeCall(JSContext* context, JSValueConst, const int count,
                               JSValueConst* values) {
        auto* self = From(context);
        if (count < 1 || !JS_IsString(values[0]))
            return JS_ThrowTypeError(context, "runtime operation id must be a string");
        const auto operation = self->String(values[0]);
        if (!operation) return JS_EXCEPTION;
        return self->RuntimeOperation(*operation, count - 1, values + 1);
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
        JS_SetPropertyStr(context, engine, "__runtimeCall",
                          JS_NewCFunction(context, &RuntimeCall, "__runtimeCall", 1));
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
            const runtimeCall = Engine.__runtimeCall;
            const bindRuntime = (operation) => (...args) => runtimeCall(operation, ...args);
            Object.defineProperties(Engine, {
                GetVariable: { value: bindRuntime("variables.get") },
                SetVariable: { value: bindRuntime("variables.set") },
                ResourceExists: { value: bindRuntime("resource.exists") },
                ReadResourceText: { value: bindRuntime("resource.readText") },
                PushRoute: { value: bindRuntime("route.push") },
                ReplaceRoute: { value: bindRuntime("route.replace") },
                BackRoute: { value: bindRuntime("route.back") },
                ShowModal: { value: bindRuntime("route.showModal") },
                CloseModal: { value: bindRuntime("route.closeModal") },
                HasSeen: { value: bindRuntime("profile.hasSeen") },
                MarkSeen: { value: bindRuntime("profile.markSeen") },
                ClearCount: { value: bindRuntime("profile.clearCount") },
                CGUnlocked: { value: bindRuntime("profile.cgUnlocked") },
                SceneUnlocked: { value: bindRuntime("profile.sceneUnlocked") },
                UnlockCG: { value: bindRuntime("profile.unlockCG") },
                UnlockScene: { value: bindRuntime("profile.unlockScene") },
                PersistentVar: { value: bindRuntime("profile.persistentVar") },
                PlaySE: { value: bindRuntime("audio.playSE") },
                PlayBGM: { value: bindRuntime("audio.playBGM") },
                StopBGM: { value: bindRuntime("audio.stopBGM") },
                SetBGMVolume: { value: bindRuntime("audio.bgmVolume") },
                SetSEVolume: { value: bindRuntime("audio.seVolume") },
                SetVoiceVolume: { value: bindRuntime("audio.voiceVolume") },
                SetAmbienceVolume: { value: bindRuntime("audio.ambienceVolume") },
                PlayAmbience: { value: bindRuntime("audio.playAmbience") },
                StopAmbience: { value: bindRuntime("audio.stopAmbience") },
                SetBackground: { value: bindRuntime("stage.background") },
                SetCharacter: { value: bindRuntime("stage.character") },
                ClearCharacter: { value: bindRuntime("stage.clearCharacter") },
                MoveCharacter: { value: bindRuntime("stage.moveCharacter") },
                SetLayer: { value: bindRuntime("stage.layer") },
                ClearLayer: { value: bindRuntime("stage.clearLayer") },
                Shake: { value: bindRuntime("stage.shake") },
                Animate: { value: bindRuntime("stage.animate") },
                LoadAnimation: { value: bindRuntime("animation.load") },
                PlayAnimation: { value: bindRuntime("animation.play") },
                CancelAnimation: { value: bindRuntime("animation.cancel") },
                AwaitSeconds: { value: bindRuntime("await.timer") },
                AwaitAnimation: { value: bindRuntime("await.animation") },
                WaitSeconds: { value: bindRuntime("wait.timer") },
                WaitAnimation: { value: bindRuntime("wait.animation") },
                DebugPoint: { value: bindRuntime("debug.point") },
                GetMouseX: { value: bindRuntime("input.mouseX") },
                GetMouseY: { value: bindRuntime("input.mouseY") },
                GetLeftClick: { value: bindRuntime("input.leftClick") },
                GetRightClick: { value: bindRuntime("input.rightClick") },
                GetLogicalSize: { value: bindRuntime("renderer.logicalSize") },
                DrawImage: { value: bindRuntime("renderer.drawImage") },
                DrawAuto: { value: bindRuntime("renderer.drawAuto") },
                DrawRect: { value: bindRuntime("renderer.drawRect") },
                DrawRoundedRect: { value: bindRuntime("renderer.drawRoundedRect") },
                DrawText: { value: bindRuntime("renderer.drawText") },
                MeasureText: { value: bindRuntime("renderer.measureText") }
            });
            delete Engine.__runtimeCall;
            globalThis.px = Engine;
            globalThis.DisplayMode = Object.freeze({
                TopLeft: 0, TopRight: 1, BottomLeft: 2, BottomRight: 3,
                Top: 4, Bottom: 5, Left: 6, Right: 7, Center: 8,
                FitWidthBottom: 9, Fit: 10, Fill: 11
            });
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

    [[nodiscard]] bool IsGenerator(JSValueConst value) const {
        if (!JS_IsObject(value)) return false;
        JSValue next = JS_GetPropertyStr(context, value, "next");
        const bool generator = JS_IsFunction(context, next);
        JS_FreeValue(context, next);
        return generator;
    }

    void BeginPromiseCapture() {
        DiscardCreatedPromiseWait();
        acceptingPromiseWait = true;
    }

    void EndPromiseCapture() { acceptingPromiseWait = false; }

    [[nodiscard]] std::string PromiseFailure(JSValueConst promise) {
        JSValue reason = JS_PromiseResult(context, promise);
        std::string message = String(reason).value_or("JavaScript Promise rejected");
        if (JS_IsObject(reason)) {
            JSValue stack = JS_GetPropertyStr(context, reason, "stack");
            if (!JS_IsUndefined(stack)) {
                if (const auto trace = String(stack); trace && *trace != message)
                    message.append("\n").append(*trace);
            }
            JS_FreeValue(context, stack);
        }
        JS_FreeValue(context, reason);
        return message;
    }

    StepResult ObservePromise(JSValueConst promise, JSValue& resume,
                              WaitToken& wait, const std::string& source) {
        int jobs = 0;
        while (JS_IsJobPending(runtime) && jobs++ < 64) {
            JSContext* jobContext = nullptr;
            BeginExecution();
            const int result = JS_ExecutePendingJob(runtime, &jobContext);
            EndExecution();
            if (result < 0) {
                EndPromiseCapture();
                DiscardCreatedPromiseWait();
                Error(source, Exception());
                return StepResult::Failed;
            }
        }
        if (JS_IsJobPending(runtime)) {
            EndPromiseCapture();
            DiscardCreatedPromiseWait();
            Error(source, "JavaScript Promise exceeded the pending-job budget");
            return StepResult::Failed;
        }
        EndPromiseCapture();

        const auto state = JS_PromiseState(context, promise);
        if (state == JS_PROMISE_REJECTED) {
            DiscardCreatedPromiseWait();
            Error(source, PromiseFailure(promise));
            return StepResult::Failed;
        }
        if (state == JS_PROMISE_FULFILLED) {
            if (createdPromiseWait) {
                DiscardCreatedPromiseWait();
                Error(source, "async callback created an engine wait without awaiting it");
                return StepResult::Failed;
            }
            wait = {};
            return StepResult::Finished;
        }
        if (state != JS_PROMISE_PENDING || !createdPromiseWait) {
            DiscardCreatedPromiseWait();
            Error(source,
                  "async callback suspended outside a persistent Engine wait operation");
            return StepResult::Failed;
        }

        wait = std::move(createdPromiseWait->wait);
        resume = createdPromiseWait->resolve;
        createdPromiseWait.reset();
        return StepResult::Yielded;
    }

    StepResult StepPromise(JSValueConst promise, JSValue& resume,
                           WaitToken& wait, const std::string& source) {
        if (JS_IsUndefined(resume)) {
            Error(source, "JavaScript Promise continuation has no engine resolver");
            return StepResult::Failed;
        }
        BeginPromiseCapture();
        JSValue argument = JS_UNDEFINED;
        BeginExecution();
        JSValue result = JS_Call(context, resume, JS_UNDEFINED, 1, &argument);
        EndExecution();
        JS_FreeValue(context, resume);
        resume = JS_UNDEFINED;
        if (JS_IsException(result)) {
            EndPromiseCapture();
            DiscardCreatedPromiseWait();
            Error(source, Exception());
            JS_FreeValue(context, result);
            return StepResult::Failed;
        }
        JS_FreeValue(context, result);
        return ObservePromise(promise, resume, wait, source);
    }

    StepResult StepGenerator(JSValueConst generator, WaitToken& wait,
                             const std::string& source) {
        JSValue next = JS_GetPropertyStr(context, generator, "next");
        if (!JS_IsFunction(context, next)) {
            JS_FreeValue(context, next);
            Error(source, "JavaScript continuation no longer has a next() function");
            return StepResult::Failed;
        }
        BeginExecution();
        JSValue result = JS_Call(context, next, generator, 0, nullptr);
        EndExecution();
        JS_FreeValue(context, next);
        if (JS_IsException(result)) {
            Error(source, Exception());
            JS_FreeValue(context, result);
            return StepResult::Failed;
        }
        if (!JS_IsObject(result)) {
            JS_FreeValue(context, result);
            Error(source, "JavaScript generator returned a malformed iterator result");
            return StepResult::Failed;
        }
        JSValue doneValue = JS_GetPropertyStr(context, result, "done");
        const int done = JS_ToBool(context, doneValue);
        JS_FreeValue(context, doneValue);
        if (done < 0) {
            JS_FreeValue(context, result);
            Error(source, Exception());
            return StepResult::Failed;
        }
        if (done != 0) {
            JS_FreeValue(context, result);
            wait = {};
            return StepResult::Finished;
        }

        JSValue yielded = JS_GetPropertyStr(context, result, "value");
        JS_FreeValue(context, result);
        if (!JS_IsObject(yielded)) {
            JS_FreeValue(context, yielded);
            Error(source,
                  "JavaScript generators must yield an Engine await token");
            return StepResult::Failed;
        }
        JSValue kindValue = JS_GetPropertyStr(context, yielded, "kind");
        const auto kind = JS_IsString(kindValue) ? String(kindValue) : std::nullopt;
        JS_FreeValue(context, kindValue);
        if (!kind || (*kind != "timer" && *kind != "animation")) {
            JS_FreeValue(context, yielded);
            Error(source, "JavaScript generator yielded an unknown wait token");
            return StepResult::Failed;
        }
        WaitToken parsed;
        parsed.kind = *kind;
        if (parsed.kind == "timer") {
            JSValue secondsValue = JS_GetPropertyStr(context, yielded, "seconds");
            double seconds = 0.0;
            const bool valid = JS_IsNumber(secondsValue) &&
                               JS_ToFloat64(context, &seconds, secondsValue) == 0 &&
                               std::isfinite(seconds) && seconds >= 0.0;
            JS_FreeValue(context, secondsValue);
            if (!valid) {
                JS_FreeValue(context, yielded);
                Error(source, "JavaScript timer wait token is invalid");
                return StepResult::Failed;
            }
            parsed.remainingSeconds = static_cast<float>(seconds);
        } else {
            JSValue handleValue = JS_GetPropertyStr(context, yielded, "handle");
            std::int64_t handle = 0;
            const bool valid = JS_IsNumber(handleValue) &&
                               JS_ToInt64(context, &handle, handleValue) == 0 &&
                               handle >= 0;
            JS_FreeValue(context, handleValue);
            if (!valid) {
                JS_FreeValue(context, yielded);
                Error(source, "JavaScript animation wait token is invalid");
                return StepResult::Failed;
            }
            parsed.handle = static_cast<std::uint64_t>(handle);
        }
        JS_FreeValue(context, yielded);
        wait = std::move(parsed);
        return StepResult::Yielded;
    }

    [[nodiscard]] JSValue CommandArguments(const vn::Command& command) {
        JSValue arguments = JS_NewObject(context);
        for (const auto& argument : command.args)
            JS_SetPropertyStr(context, arguments, argument.key.c_str(),
                              JS_NewString(context, argument.value.c_str()));
        for (const auto& [name, value] : command.typedArgs)
            JS_SetPropertyStr(context, arguments, name.c_str(),
                              ToJavaScript(value));
        return arguments;
    }

    [[nodiscard]] std::pair<JSValue, JSValue> ActionArguments(
        const ui::ActionInvocation& invocation) {
        JSValue arguments = JS_NewObject(context);
        for (const auto& [name, value] : invocation.arguments)
            JS_SetPropertyStr(context, arguments, name.c_str(),
                              ToJavaScript(value));
        JSValue actionContext = JS_NewObject(context);
        JS_SetPropertyStr(context, actionContext, "scene",
                          JS_NewString(context,
                                       invocation.context.sourceScene.c_str()));
        JS_SetPropertyStr(context, actionContext, "node",
                          JS_NewString(context,
                              invocation.context.sourceNode.ToString().c_str()));
        JS_SetPropertyStr(context, actionContext, "signal",
                          JS_NewString(context,
                                       invocation.context.signal.c_str()));
        JS_SetPropertyStr(context, actionContext, "route",
                          JS_NewString(context,
                                       invocation.context.currentRoute.c_str()));
        JS_SetPropertyStr(context, actionContext, "preview",
                          JS_NewBool(context, invocation.context.preview));
        return {arguments, actionContext};
    }

    struct ReplayedContinuation {
        ContinuationFlavor flavor = ContinuationFlavor::Generator;
        JSValue continuation = JS_UNDEFINED;
        JSValue resume = JS_UNDEFINED;
        WaitToken wait;
    };

    [[nodiscard]] std::optional<ReplayedContinuation> ReplayToBoundary(
        JSValueConst callback, const int argumentCount, JSValueConst* arguments,
        const std::uint32_t yieldIndex, const std::string& source) {
        BeginPromiseCapture();
        BeginExecution();
        JSValue continuation = JS_Call(context, callback, JS_UNDEFINED,
                                       argumentCount, arguments);
        EndExecution();
        if (JS_IsException(continuation)) {
            EndPromiseCapture();
            DiscardCreatedPromiseWait();
            Error(source, Exception());
            JS_FreeValue(context, continuation);
            return std::nullopt;
        }

        ReplayedContinuation replayed;
        replayed.continuation = continuation;
        StepResult result = StepResult::Failed;
        if (IsGenerator(continuation)) {
            EndPromiseCapture();
            DiscardCreatedPromiseWait();
            for (std::uint32_t index = 0; index < yieldIndex; ++index) {
                result = StepGenerator(continuation, replayed.wait, source);
                if (result != StepResult::Yielded) break;
            }
        } else if (JS_IsPromise(continuation)) {
            replayed.flavor = ContinuationFlavor::Promise;
            result = ObservePromise(continuation, replayed.resume,
                                    replayed.wait, source);
            for (std::uint32_t index = 1;
                 result == StepResult::Yielded && index < yieldIndex; ++index) {
                result = StepPromise(continuation, replayed.resume,
                                     replayed.wait, source);
            }
        } else {
            EndPromiseCapture();
            DiscardCreatedPromiseWait();
            Error(source,
                  "checkpoint callback no longer returns a persistent continuation");
        }

        if (result == StepResult::Yielded) return replayed;
        JS_FreeValue(context, replayed.continuation);
        JS_FreeValue(context, replayed.resume);
        return std::nullopt;
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
    std::vector<PendingCommandContinuation> pendingCommands;
    std::vector<PendingActionContinuation> pendingActions;
    std::unordered_map<std::uint64_t, ui::ActionExecutionState>
        actionTerminalStates;
    std::optional<PromiseWaitCapability> createdPromiseWait;
    bool acceptingPromiseWait = false;
    std::uint64_t nextActionHandle = 1;
    DebugSnapshot debug;
    JSValue debugLocals = JS_UNDEFINED;
    std::unordered_map<std::string, std::set<int>> debugBreakpoints;
    bool debugPauseRequested = false;
    bool debugStepRequested = false;
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
            descriptor.waitPolicy = encoded.value("await", false)
                                        ? vn::CommandWaitPolicy::Async
                                        : vn::CommandWaitPolicy::Immediate;
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
    JSValue arguments = m_impl->CommandArguments(command);
    m_impl->BeginPromiseCapture();
    m_impl->BeginExecution();
    JSValue result = JS_Call(m_impl->context, found->second, JS_UNDEFINED, 1, &arguments);
    m_impl->EndExecution();
    JS_FreeValue(m_impl->context, arguments);
    if (JS_IsException(result)) {
        m_impl->EndPromiseCapture();
        m_impl->DiscardCreatedPromiseWait();
        m_impl->Error("command:" + command.type, m_impl->Exception());
        JS_FreeValue(m_impl->context, result);
        return true;
    }
    if (m_impl->IsGenerator(result)) {
        m_impl->EndPromiseCapture();
        m_impl->DiscardCreatedPromiseWait();
        Impl::WaitToken wait;
        const auto step = m_impl->StepGenerator(
            result, wait, "command:" + command.type);
        if (step == Impl::StepResult::Yielded) {
            Impl::PendingCommandContinuation pending;
            pending.continuation = result;
            pending.command = command;
            pending.yieldIndex = 1;
            pending.wait = std::move(wait);
            m_impl->pendingCommands.push_back(std::move(pending));
            return true;
        }
        JS_FreeValue(m_impl->context, result);
        return true;
    }
    if (JS_IsPromise(result)) {
        JSValue resume = JS_UNDEFINED;
        Impl::WaitToken wait;
        const auto step = m_impl->ObservePromise(
            result, resume, wait, "command:" + command.type);
        if (step == Impl::StepResult::Yielded) {
            Impl::PendingCommandContinuation pending;
            pending.flavor = Impl::ContinuationFlavor::Promise;
            pending.continuation = result;
            pending.resume = resume;
            pending.command = command;
            pending.yieldIndex = 1;
            pending.wait = std::move(wait);
            m_impl->pendingCommands.push_back(std::move(pending));
            return true;
        }
        JS_FreeValue(m_impl->context, resume);
        JS_FreeValue(m_impl->context, result);
        return true;
    }
    const bool orphanedWait = m_impl->createdPromiseWait.has_value();
    m_impl->EndPromiseCapture();
    m_impl->DiscardCreatedPromiseWait();
    if (orphanedWait)
        m_impl->Error("command:" + command.type,
                      "command created an engine wait without returning an async Promise");
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
    return StartAction(invocation).status;
}

ui::ProviderActionStart JavaScriptHost::StartAction(
    const ui::ActionInvocation& invocation) {
    const auto found = m_impl->actions.find(invocation.action);
    if (found == m_impl->actions.end())
        return {.status = Status::Fail(ScriptDiagnostic(
                    "PXJS7420", "JavaScript action callback is missing",
                    invocation.action))};
    auto [arguments, actionContext] = m_impl->ActionArguments(invocation);
    JSValue parameters[] = {arguments, actionContext};
    m_impl->BeginPromiseCapture();
    m_impl->BeginExecution();
    JSValue result = JS_Call(m_impl->context, found->second, JS_UNDEFINED, 2, parameters);
    m_impl->EndExecution();
    JS_FreeValue(m_impl->context, arguments);
    JS_FreeValue(m_impl->context, actionContext);
    if (JS_IsException(result)) {
        m_impl->EndPromiseCapture();
        m_impl->DiscardCreatedPromiseWait();
        const auto error = m_impl->Exception();
        m_impl->Error("action:" + invocation.action, error);
        JS_FreeValue(m_impl->context, result);
        return {.status = Status::Fail(ScriptDiagnostic(
                    "PXJS7421", "JavaScript action failed", error))};
    }
    if (m_impl->IsGenerator(result)) {
        m_impl->EndPromiseCapture();
        m_impl->DiscardCreatedPromiseWait();
        Impl::WaitToken wait;
        const auto step = m_impl->StepGenerator(
            result, wait, "action:" + invocation.action);
        if (step == Impl::StepResult::Yielded) {
            const std::uint64_t handle = m_impl->nextActionHandle++;
            Impl::PendingActionContinuation pending;
            pending.continuation = result;
            pending.id = handle;
            pending.invocation = invocation;
            pending.yieldIndex = 1;
            pending.wait = std::move(wait);
            m_impl->pendingActions.push_back(std::move(pending));
            return {.status = Status::Ok(), .handle = handle, .pending = true};
        }
        JS_FreeValue(m_impl->context, result);
        if (step == Impl::StepResult::Failed)
            return {.status = Status::Fail(ScriptDiagnostic(
                        "PXJS7422", "JavaScript Action continuation failed",
                        invocation.action))};
        return {.status = Status::Ok()};
    }
    if (JS_IsPromise(result)) {
        JSValue resume = JS_UNDEFINED;
        Impl::WaitToken wait;
        const auto step = m_impl->ObservePromise(
            result, resume, wait, "action:" + invocation.action);
        if (step == Impl::StepResult::Yielded) {
            const std::uint64_t handle = m_impl->nextActionHandle++;
            Impl::PendingActionContinuation pending;
            pending.flavor = Impl::ContinuationFlavor::Promise;
            pending.continuation = result;
            pending.resume = resume;
            pending.id = handle;
            pending.invocation = invocation;
            pending.yieldIndex = 1;
            pending.wait = std::move(wait);
            m_impl->pendingActions.push_back(std::move(pending));
            return {.status = Status::Ok(), .handle = handle, .pending = true};
        }
        JS_FreeValue(m_impl->context, resume);
        JS_FreeValue(m_impl->context, result);
        if (step == Impl::StepResult::Failed)
            return {.status = Status::Fail(ScriptDiagnostic(
                        "PXJS7423", "JavaScript async Action failed",
                        invocation.action))};
        return {.status = Status::Ok()};
    }
    const bool orphanedWait = m_impl->createdPromiseWait.has_value();
    m_impl->EndPromiseCapture();
    m_impl->DiscardCreatedPromiseWait();
    JS_FreeValue(m_impl->context, result);
    if (orphanedWait)
        return {.status = Status::Fail(ScriptDiagnostic(
                    "PXJS7424",
                    "JavaScript Action created an engine wait without returning an async Promise",
                    invocation.action))};
    return {.status = Status::Ok()};
}

ui::ActionExecutionState JavaScriptHost::ActionState(
    const std::uint64_t handle) const {
    if (std::ranges::any_of(m_impl->pendingActions,
                           [handle](const auto& pending) {
                               return pending.id == handle;
                           }))
        return ui::ActionExecutionState::Running;
    if (const auto found = m_impl->actionTerminalStates.find(handle);
        found != m_impl->actionTerminalStates.end())
        return found->second;
    return ui::ActionExecutionState::Unknown;
}

void JavaScriptHost::CancelAction(const std::uint64_t handle) {
    const auto found = std::ranges::find_if(
        m_impl->pendingActions,
        [handle](const auto& pending) { return pending.id == handle; });
    if (found != m_impl->pendingActions.end()) {
        m_impl->FreeContinuation(*found);
        m_impl->pendingActions.erase(found);
    }
    m_impl->actionTerminalStates[handle] =
        ui::ActionExecutionState::Cancelled;
}

void JavaScriptHost::Update(const float deltaSeconds) {
    if (!m_impl->runtime) return;

    const auto ready = [this, deltaSeconds](Impl::WaitToken& wait) {
        if (wait.kind == "debug") return false;
        if (wait.kind == "debug-resume") return true;
        if (wait.kind == "animation")
            return !m_impl->services.timeline ||
                   !m_impl->services.timeline->Playing(wait.handle);
        if (wait.kind == "timer") {
            wait.remainingSeconds -= std::max(0.0f, deltaSeconds);
            return wait.remainingSeconds <= 0.0f;
        }
        return true;
    };

    for (auto iterator = m_impl->pendingCommands.begin();
         iterator != m_impl->pendingCommands.end();) {
        if (!ready(iterator->wait)) {
            ++iterator;
            continue;
        }
        const auto source = "command:" + iterator->command.type;
        const auto result = iterator->flavor == Impl::ContinuationFlavor::Promise
                                ? m_impl->StepPromise(
                                      iterator->continuation, iterator->resume,
                                      iterator->wait, source)
                                : m_impl->StepGenerator(
                                      iterator->continuation, iterator->wait,
                                      source);
        if (result == Impl::StepResult::Yielded) {
            ++iterator->yieldIndex;
            ++iterator;
            continue;
        }
        m_impl->FreeContinuation(*iterator);
        iterator = m_impl->pendingCommands.erase(iterator);
    }

    for (auto iterator = m_impl->pendingActions.begin();
         iterator != m_impl->pendingActions.end();) {
        if (!ready(iterator->wait)) {
            ++iterator;
            continue;
        }
        const auto source = "action:" + iterator->invocation.action;
        const auto result = iterator->flavor == Impl::ContinuationFlavor::Promise
                                ? m_impl->StepPromise(
                                      iterator->continuation, iterator->resume,
                                      iterator->wait, source)
                                : m_impl->StepGenerator(
                                      iterator->continuation, iterator->wait,
                                      source);
        if (result == Impl::StepResult::Yielded) {
            ++iterator->yieldIndex;
            ++iterator;
            continue;
        }
        m_impl->actionTerminalStates[iterator->id] =
            result == Impl::StepResult::Finished
                ? ui::ActionExecutionState::Completed
                : ui::ActionExecutionState::Failed;
        m_impl->FreeContinuation(*iterator);
        iterator = m_impl->pendingActions.erase(iterator);
    }

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

bool JavaScriptHost::HasPendingCommand() const {
    return !m_impl->pendingCommands.empty();
}

bool JavaScriptHost::HasPendingAction() const {
    return !m_impl->pendingActions.empty();
}

PendingCommandsState JavaScriptHost::CapturePending() const {
    PendingCommandsState state;
    state.reserve(m_impl->pendingCommands.size());
    for (const auto& pending : m_impl->pendingCommands) {
        state.push_back({.command = pending.command,
                         .yieldIndex = pending.yieldIndex,
                         .waitKind = pending.wait.kind,
                         .handle = pending.wait.handle,
                         .remainingSeconds = pending.wait.remainingSeconds});
    }
    return state;
}

PendingActionsState JavaScriptHost::CapturePendingActions() const {
    PendingActionsState state;
    state.reserve(m_impl->pendingActions.size());
    for (const auto& pending : m_impl->pendingActions) {
        state.push_back({.id = pending.id,
                         .invocation = pending.invocation,
                         .yieldIndex = pending.yieldIndex,
                         .waitKind = pending.wait.kind,
                         .handle = pending.wait.handle,
                         .remainingSeconds = pending.wait.remainingSeconds});
    }
    return state;
}

Status JavaScriptHost::RestorePending(const PendingCommandsState& state) {
    for (auto& pending : m_impl->pendingCommands)
        m_impl->FreeContinuation(pending);
    for (auto& pending : m_impl->pendingActions)
        m_impl->FreeContinuation(pending);
    m_impl->pendingCommands.clear();
    m_impl->pendingActions.clear();
    m_impl->actionTerminalStates.clear();

    const auto fail = [this](std::string code, std::string message,
                             std::string details) {
        for (auto& pending : m_impl->pendingCommands)
            m_impl->FreeContinuation(pending);
        m_impl->pendingCommands.clear();
        return Status::Fail(ScriptDiagnostic(
            std::move(code), std::move(message), std::move(details)));
    };

    for (const auto& saved : state) {
        const bool validTimer = saved.waitKind != "timer" ||
                                (std::isfinite(saved.remainingSeconds) &&
                                 saved.remainingSeconds >= 0.0f);
        if (saved.yieldIndex == 0 || !validTimer ||
            (saved.waitKind != "timer" && saved.waitKind != "animation")) {
            return fail("PXJS7501", "Saved JavaScript await checkpoint is invalid",
                        saved.command.type);
        }
        const auto callback = m_impl->commands.find(saved.command.type);
        if (callback == m_impl->commands.end()) {
            return fail("PXJS7502", "Saved JavaScript command is no longer registered",
                        saved.command.type);
        }

        JSValue arguments = m_impl->CommandArguments(saved.command);
        auto replayed = m_impl->ReplayToBoundary(
            callback->second, 1, &arguments, saved.yieldIndex,
            "restore-command:" + saved.command.type);
        JS_FreeValue(m_impl->context, arguments);
        if (!replayed) {
            return fail("PXJS7503", "JavaScript command checkpoint replay failed",
                        saved.command.type);
        }
        if (replayed->wait.kind != saved.waitKind) {
            JS_FreeValue(m_impl->context, replayed->continuation);
            JS_FreeValue(m_impl->context, replayed->resume);
            return fail("PXJS7504", "JavaScript command await structure changed",
                        saved.command.type);
        }

        Impl::PendingCommandContinuation pending;
        pending.flavor = replayed->flavor;
        pending.continuation = replayed->continuation;
        pending.resume = replayed->resume;
        pending.command = saved.command;
        pending.yieldIndex = saved.yieldIndex;
        pending.wait = {.kind = saved.waitKind,
                        .handle = saved.handle,
                        .remainingSeconds = saved.remainingSeconds};
        m_impl->pendingCommands.push_back(std::move(pending));
    }
    return Status::Ok();
}

Status JavaScriptHost::RestorePendingActions(const PendingActionsState& state) {
    for (auto& pending : m_impl->pendingActions)
        m_impl->FreeContinuation(pending);
    m_impl->pendingActions.clear();
    m_impl->actionTerminalStates.clear();

    const auto fail = [this](std::string code, std::string message,
                             std::string details) {
        for (auto& pending : m_impl->pendingActions)
            m_impl->FreeContinuation(pending);
        m_impl->pendingActions.clear();
        return Status::Fail(ScriptDiagnostic(
            std::move(code), std::move(message), std::move(details)));
    };

    std::unordered_set<std::uint64_t> ids;
    for (const auto& saved : state) {
        const bool validTimer = saved.waitKind != "timer" ||
                                (std::isfinite(saved.remainingSeconds) &&
                                 saved.remainingSeconds >= 0.0f);
        if (saved.id == 0 || !ids.insert(saved.id).second ||
            saved.yieldIndex == 0 || !validTimer ||
            (saved.waitKind != "timer" && saved.waitKind != "animation")) {
            return fail("PXJS7510", "Saved JavaScript Action checkpoint is invalid",
                        saved.invocation.action);
        }
        const auto callback = m_impl->actions.find(saved.invocation.action);
        if (callback == m_impl->actions.end()) {
            return fail("PXJS7511", "Saved JavaScript Action is no longer registered",
                        saved.invocation.action);
        }

        auto [arguments, actionContext] =
            m_impl->ActionArguments(saved.invocation);
        JSValue parameters[] = {arguments, actionContext};
        auto replayed = m_impl->ReplayToBoundary(
            callback->second, 2, parameters, saved.yieldIndex,
            "restore-action:" + saved.invocation.action);
        JS_FreeValue(m_impl->context, arguments);
        JS_FreeValue(m_impl->context, actionContext);
        if (!replayed) {
            return fail("PXJS7512", "JavaScript Action checkpoint replay failed",
                        saved.invocation.action);
        }
        if (replayed->wait.kind != saved.waitKind) {
            JS_FreeValue(m_impl->context, replayed->continuation);
            JS_FreeValue(m_impl->context, replayed->resume);
            return fail("PXJS7513", "JavaScript Action await structure changed",
                        saved.invocation.action);
        }

        Impl::PendingActionContinuation pending;
        pending.flavor = replayed->flavor;
        pending.continuation = replayed->continuation;
        pending.resume = replayed->resume;
        pending.id = saved.id;
        pending.invocation = saved.invocation;
        pending.yieldIndex = saved.yieldIndex;
        pending.wait = {.kind = saved.waitKind,
                        .handle = saved.handle,
                        .remainingSeconds = saved.remainingSeconds};
        m_impl->pendingActions.push_back(std::move(pending));
        m_impl->nextActionHandle =
            std::max(m_impl->nextActionHandle, saved.id + 1);
    }
    return Status::Ok();
}

void JavaScriptHost::CancelPending() {
    for (auto& pending : m_impl->pendingCommands)
        m_impl->FreeContinuation(pending);
    for (auto& pending : m_impl->pendingActions)
        m_impl->FreeContinuation(pending);
    m_impl->pendingCommands.clear();
    m_impl->pendingActions.clear();
    m_impl->actionTerminalStates.clear();
    m_impl->debug = {};
    JS_FreeValue(m_impl->context, m_impl->debugLocals);
    m_impl->debugLocals = JS_UNDEFINED;
    m_impl->debugPauseRequested = false;
    m_impl->debugStepRequested = false;
}

std::vector<DebugBreakpoint> JavaScriptHost::SetDebugBreakpoints(
    std::vector<DebugBreakpoint> breakpoints) {
    m_impl->debugBreakpoints.clear();
    std::vector<DebugBreakpoint> accepted;
    for (auto& breakpoint : breakpoints) {
        breakpoint.source = NormalizeDebugSource(std::move(breakpoint.source));
        if (breakpoint.line <= 0) continue;
        if (m_impl->debugBreakpoints[breakpoint.source]
                .insert(breakpoint.line)
                .second)
            accepted.push_back(std::move(breakpoint));
    }
    return accepted;
}

bool JavaScriptHost::DebugPause() {
    if (m_impl->debug.paused) return false;
    m_impl->debugPauseRequested = true;
    return true;
}

bool JavaScriptHost::DebugContinue() {
    if (!m_impl->debug.paused) return false;
    m_impl->debugPauseRequested = false;
    m_impl->debugStepRequested = false;
    m_impl->debug = {};
    JS_FreeValue(m_impl->context, m_impl->debugLocals);
    m_impl->debugLocals = JS_UNDEFINED;
    for (auto& pending : m_impl->pendingCommands)
        if (pending.wait.kind == "debug") pending.wait.kind = "debug-resume";
    for (auto& pending : m_impl->pendingActions)
        if (pending.wait.kind == "debug") pending.wait.kind = "debug-resume";
    return true;
}

bool JavaScriptHost::DebugStep() {
    if (!m_impl->debug.paused) return false;
    m_impl->debugPauseRequested = false;
    m_impl->debugStepRequested = true;
    m_impl->debug = {};
    JS_FreeValue(m_impl->context, m_impl->debugLocals);
    m_impl->debugLocals = JS_UNDEFINED;
    for (auto& pending : m_impl->pendingCommands)
        if (pending.wait.kind == "debug") pending.wait.kind = "debug-resume";
    for (auto& pending : m_impl->pendingActions)
        if (pending.wait.kind == "debug") pending.wait.kind = "debug-resume";
    return true;
}

std::optional<DebugVariable> JavaScriptHost::EvaluateDebugWatch(
    const std::string_view expression) const {
    if (!m_impl->debug.paused || JS_IsUndefined(m_impl->debugLocals) ||
        expression.empty())
        return std::nullopt;

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
            !std::ranges::all_of(
                part.substr(1), [](const unsigned char character) {
                    return std::isalnum(character) || character == '_';
                }))
            return std::nullopt;
        path.push_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    JSValue current = JS_DupValue(m_impl->context, m_impl->debugLocals);
    for (const auto& part : path) {
        if (!JS_IsObject(current)) {
            JS_FreeValue(m_impl->context, current);
            return std::nullopt;
        }
        const JSAtom atom = JS_NewAtomLen(m_impl->context, part.data(), part.size());
        JSPropertyDescriptor descriptor{};
        const int present = JS_GetOwnProperty(
            m_impl->context, &descriptor, current, atom);
        JS_FreeAtom(m_impl->context, atom);
        if (present <= 0) {
            if (present < 0) {
                JSValue exception = JS_GetException(m_impl->context);
                JS_FreeValue(m_impl->context, exception);
            }
            JS_FreeValue(m_impl->context, current);
            return std::nullopt;
        }
        const bool accessor = !JS_IsUndefined(descriptor.getter) ||
                              !JS_IsUndefined(descriptor.setter);
        JSValue next = descriptor.value;
        JS_FreeValue(m_impl->context, descriptor.getter);
        JS_FreeValue(m_impl->context, descriptor.setter);
        JS_FreeValue(m_impl->context, current);
        if (accessor || JS_IsException(next)) {
            JS_FreeValue(m_impl->context, next);
            return std::nullopt;
        }
        current = next;
    }
    DebugVariable value{std::string(expression), m_impl->DebugValue(current)};
    JS_FreeValue(m_impl->context, current);
    return value;
}

const DebugSnapshot& JavaScriptHost::CaptureDebugState() const {
    return m_impl->debug;
}

}  // namespace px::script
