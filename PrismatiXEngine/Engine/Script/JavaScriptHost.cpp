#include "Engine/Script/JavaScriptHost.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Core/SemanticVersion.h"
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
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>
#include <iterator>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

std::optional<vn::CommandEditorWidget> ManifestCommandEditorHint(
    const std::string& raw) {
    const auto hint = Lower(raw);
    if (hint.empty() || hint == "default") return vn::CommandEditorWidget::Default;
    if (hint == "multiline") return vn::CommandEditorWidget::Multiline;
    if (hint == "resource") return vn::CommandEditorWidget::Resource;
    if (hint == "character") return vn::CommandEditorWidget::Character;
    if (hint == "expression") return vn::CommandEditorWidget::Expression;
    if (hint == "target") return vn::CommandEditorWidget::Target;
    if (hint == "preset") return vn::CommandEditorWidget::Preset;
    if (hint == "hidden") return vn::CommandEditorWidget::Hidden;
    if (hint == "enum") return vn::CommandEditorWidget::Enum;
    if (hint == "color") return vn::CommandEditorWidget::Color;
    if (hint == "route") return vn::CommandEditorWidget::Route;
    if (hint == "node") return vn::CommandEditorWidget::Node;
    if (hint == "animation") return vn::CommandEditorWidget::Animation;
    if (hint == "token") return vn::CommandEditorWidget::Token;
    return std::nullopt;
}

std::optional<ui::ActionEditorHint> ManifestActionEditorHint(
    const std::string& raw) {
    const auto hint = Lower(raw);
    if (hint.empty() || hint == "default") return ui::ActionEditorHint::Default;
    if (hint == "multiline") return ui::ActionEditorHint::Multiline;
    if (hint == "enum") return ui::ActionEditorHint::Enum;
    if (hint == "color") return ui::ActionEditorHint::Color;
    if (hint == "resource") return ui::ActionEditorHint::Resource;
    if (hint == "route") return ui::ActionEditorHint::Route;
    if (hint == "node") return ui::ActionEditorHint::Node;
    if (hint == "animation") return ui::ActionEditorHint::Animation;
    if (hint == "token") return ui::ActionEditorHint::Token;
    return std::nullopt;
}

bool ActionHintMatchesType(const ui::ActionEditorHint hint,
                           const VariantType type, const bool hasEnum) {
    switch (hint) {
        case ui::ActionEditorHint::Default: return true;
        case ui::ActionEditorHint::Multiline:
        case ui::ActionEditorHint::Route: return type == VariantType::String;
        case ui::ActionEditorHint::Enum:
            return type == VariantType::String && hasEnum;
        case ui::ActionEditorHint::Color: return type == VariantType::Color;
        case ui::ActionEditorHint::Resource:
            return type == VariantType::ResourceRef;
        case ui::ActionEditorHint::Node: return type == VariantType::Uuid;
        case ui::ActionEditorHint::Animation:
            return type == VariantType::ResourceRef || type == VariantType::String;
        case ui::ActionEditorHint::Token: return type == VariantType::TokenRef;
    }
    return false;
}

bool SupportedCapability(const std::string_view capability) {
    return capability == "runtime" || capability == "animation" ||
           capability == "ui" || capability == "audio" ||
           capability == "video" || capability == "persistence" ||
           capability == "input";
}

bool IsJsonValue(const Variant& value, const int depth = 0) {
    if (depth > 32) return false;
    switch (value.Type()) {
        case VariantType::Null:
        case VariantType::Bool:
        case VariantType::Integer:
        case VariantType::Number:
        case VariantType::String: return true;
        case VariantType::Array:
            return std::ranges::all_of(*value.AsArray(), [&](const Variant& item) {
                return IsJsonValue(item, depth + 1);
            });
        case VariantType::Object:
            return std::ranges::all_of(*value.AsObject(), [&](const auto& item) {
                return IsJsonValue(item.second, depth + 1);
            });
        default: return false;
    }
}

std::string_view CapabilityForOperation(const std::string_view operation) {
    if (operation.starts_with("route.")) return "ui";
    if (operation.starts_with("audio.")) return "audio";
    if (operation.starts_with("animation.")) return "animation";
    if (operation.starts_with("video.") || operation == "await.video" ||
        operation == "wait.video")
        return "video";
    if (operation.starts_with("save.")) return "persistence";
    // Existing raw pointer access remains part of the legacy runtime
    // capability. Only the new device-neutral action surface requires input.
    if (operation == "input.actionPressed" || operation == "input.actionDown")
        return "input";
    return "runtime";
}

std::optional<InputAction> ParseInputAction(const std::string_view name) {
    if (name == "navigate-up") return InputAction::NavigateUp;
    if (name == "navigate-down") return InputAction::NavigateDown;
    if (name == "navigate-left") return InputAction::NavigateLeft;
    if (name == "navigate-right") return InputAction::NavigateRight;
    if (name == "accept") return InputAction::Accept;
    if (name == "cancel") return InputAction::Cancel;
    if (name == "focus-next") return InputAction::FocusNext;
    if (name == "focus-previous") return InputAction::FocusPrevious;
    if (name == "advance") return InputAction::Advance;
    if (name == "menu") return InputAction::Menu;
    if (name == "backlog") return InputAction::Backlog;
    if (name == "toggle-auto") return InputAction::ToggleAuto;
    if (name == "toggle-skip") return InputAction::ToggleSkip;
    if (name == "toggle-ui") return InputAction::ToggleUi;
    if (name == "quick-save") return InputAction::QuickSave;
    if (name == "quick-load") return InputAction::QuickLoad;
    if (name == "rollback") return InputAction::Rollback;
    if (name == "pause") return InputAction::Pause;
    return std::nullopt;
}

struct ManifestSafety {
    bool previewSafe = false;
    bool deterministic = false;
    bool seekSafe = false;
    bool rollbackSafe = false;
};

ManifestSafety ReadManifestSafety(const nlohmann::json& descriptor,
                                  const nlohmann::json& manifest) {
    const nlohmann::json* safety = nullptr;
    if (const auto local = descriptor.find("safety");
        local != descriptor.end()) {
        safety = &*local;
    } else if (const auto inherited = manifest.find("safety");
               inherited != manifest.end()) {
        safety = &*inherited;
    }
    if (!safety || !safety->is_object())
        throw std::invalid_argument(
            "extension commands and actions require an explicit safety contract");
    for (const char* field : {"previewSafe", "deterministic", "seekSafe",
                              "rollbackSafe"}) {
        if (!safety->contains(field) || !(*safety)[field].is_boolean())
            throw std::invalid_argument(
                std::string("extension safety flag is missing or invalid: ") +
                field);
    }
    return {safety->at("previewSafe").get<bool>(),
            safety->at("deterministic").get<bool>(),
            safety->at("seekSafe").get<bool>(),
            safety->at("rollbackSafe").get<bool>()};
}

bool SafeRelativePath(const std::string_view path) {
    const auto normalized = io::VFS::NormalizeVirtualPath(path);
    return normalized && *normalized == path;
}

std::optional<std::string> ManifestRelativePath(
    const std::string_view manifestPath, const std::string_view relativePath) {
    if (!SafeRelativePath(relativePath)) return std::nullopt;
    const auto separator = manifestPath.find_last_of('/');
    const std::string joined = separator == std::string_view::npos
                                   ? std::string(relativePath)
                                   : std::string(manifestPath.substr(0, separator + 1)) +
                                         std::string(relativePath);
    return io::VFS::NormalizeVirtualPath(joined);
}

std::vector<std::string> PathSegments(const std::string_view path) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto separator = path.find('/', start);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        result.emplace_back(path.substr(start, end - start));
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return result;
}

std::optional<std::string> ResolveExtensionImport(
    const std::string_view packageRoot, const std::string_view baseModule,
    const std::string_view specifier) {
    if (!(specifier.starts_with("./") || specifier.starts_with("../")) ||
        specifier.find('\0') != std::string_view::npos ||
        specifier.find('\\') != std::string_view::npos ||
        specifier.find(':') != std::string_view::npos ||
        specifier.ends_with('/')) {
        return std::nullopt;
    }

    auto segments = PathSegments(baseModule);
    if (segments.empty()) return std::nullopt;
    segments.pop_back();
    const std::size_t rootDepth = packageRoot.empty()
                                      ? 0
                                      : PathSegments(packageRoot).size();
    if (segments.size() < rootDepth) return std::nullopt;

    for (const auto& segment : PathSegments(specifier)) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (segments.size() <= rootDepth) return std::nullopt;
            segments.pop_back();
            continue;
        }
        segments.push_back(segment);
    }
    if (segments.empty()) return std::nullopt;
    std::string resolved;
    for (const auto& segment : segments) {
        if (!resolved.empty()) resolved.push_back('/');
        resolved.append(segment);
    }
    const auto normalized = io::VFS::NormalizeVirtualPath(resolved);
    if (!normalized || !normalized->ends_with(".js")) return std::nullopt;
    if (!packageRoot.empty() && *normalized != packageRoot &&
        !normalized->starts_with(std::string(packageRoot) + "/")) {
        return std::nullopt;
    }
    return normalized;
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
        JS_SetModuleLoaderFunc(runtime, &NormalizeModule, &LoadModule, this);
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
            for (auto& [_, provider] : stateProviders) {
                JS_FreeValue(context, provider.capture);
                JS_FreeValue(context, provider.restore);
                JS_FreeValue(context, provider.migrate);
            }
            if (ownsRegistrations) {
                for (const auto& source : loadedActionSources)
                    (void)ui::ActionCatalog::Global().RemoveSource(
                        ui::ActionOrigin::ScriptExtension, source);
                for (const auto& source : loadedCommandSources)
                    (void)vn::CommandRegistry::Global().RemoveSource(source);
            }
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
        EngineOperationJournal journal;
    };

    struct PendingActionContinuation {
        ContinuationFlavor flavor = ContinuationFlavor::Generator;
        JSValue continuation = JS_UNDEFINED;
        JSValue resume = JS_UNDEFINED;
        std::uint64_t id = 0;
        ui::ActionInvocation invocation;
        std::uint32_t yieldIndex = 0;
        WaitToken wait;
        EngineOperationJournal journal;
    };

    struct PromiseWaitCapability {
        WaitToken wait;
        JSValue resolve = JS_UNDEFINED;
    };

    enum class StepResult { Yielded, Finished, Failed };

    struct StateProvider {
        std::uint32_t version = 0;
        JSValue capture = JS_UNDEFINED;
        JSValue restore = JS_UNDEFINED;
        JSValue migrate = JS_UNDEFINED;
    };

    class JournalScope {
    public:
        JournalScope(Impl& owner, EngineOperationJournal& recording)
            : owner(owner), previousRecording(owner.recordingJournal),
              previousReplay(owner.replayJournal),
              previousCursor(owner.replayJournalCursor),
              previousPersistent(owner.insidePersistentCallback) {
            owner.recordingJournal = &recording;
            owner.replayJournal = nullptr;
            owner.replayJournalCursor = 0;
            owner.insidePersistentCallback = true;
        }
        JournalScope(Impl& owner, const EngineOperationJournal& replay)
            : owner(owner), previousRecording(owner.recordingJournal),
              previousReplay(owner.replayJournal),
              previousCursor(owner.replayJournalCursor),
              previousPersistent(owner.insidePersistentCallback) {
            owner.recordingJournal = nullptr;
            owner.replayJournal = &replay;
            owner.replayJournalCursor = 0;
            owner.insidePersistentCallback = true;
        }
        ~JournalScope() {
            owner.recordingJournal = previousRecording;
            owner.replayJournal = previousReplay;
            owner.replayJournalCursor = previousCursor;
            owner.insidePersistentCallback = previousPersistent;
        }

    private:
        Impl& owner;
        EngineOperationJournal* previousRecording = nullptr;
        const EngineOperationJournal* previousReplay = nullptr;
        std::size_t previousCursor = 0;
        bool previousPersistent = false;
    };

    class StateCallbackScope {
    public:
        explicit StateCallbackScope(Impl& owner)
            : owner(owner), previous(owner.insideStateProviderCallback),
              previousPersistent(owner.insidePersistentCallback) {
            owner.insideStateProviderCallback = true;
            owner.insidePersistentCallback = true;
        }
        ~StateCallbackScope() {
            owner.insideStateProviderCallback = previous;
            owner.insidePersistentCallback = previousPersistent;
        }

    private:
        Impl& owner;
        bool previous = false;
        bool previousPersistent = false;
    };

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

    static char* NormalizeModule(JSContext* context, const char* baseName,
                                 const char* moduleName, void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (!self || !self->isExtensionRealm || !baseName || !moduleName) {
            JS_ThrowReferenceError(context,
                                   "module imports are only available to isolated extensions");
            return nullptr;
        }
        const auto resolved = ResolveExtensionImport(
            self->moduleRoot, baseName, moduleName);
        if (!resolved || !self->moduleSources.contains(*resolved)) {
            JS_ThrowReferenceError(
                context,
                "extension import '%s' is outside its package or not declared in modules",
                moduleName);
            return nullptr;
        }
        return js_strdup(context, resolved->c_str());
    }

    static JSModuleDef* LoadModule(JSContext* context, const char* moduleName,
                                   void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (!self || !moduleName) {
            JS_ThrowReferenceError(context, "invalid extension module request");
            return nullptr;
        }
        const auto source = self->moduleSources.find(moduleName);
        if (source == self->moduleSources.end()) {
            JS_ThrowReferenceError(context,
                                   "extension module '%s' is not declared",
                                   moduleName);
            return nullptr;
        }
        JSValue compiled = JS_Eval(
            context, source->second.data(), source->second.size(), moduleName,
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) return nullptr;
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JS_FreeValue(context, compiled);
        return module;
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

    [[nodiscard]] std::pair<std::string, int> CurrentLocation() {
        JSValue error = JS_NewError(context);
        JSValue stackValue = JS_GetPropertyStr(context, error, "stack");
        const auto stack = String(stackValue);
        JS_FreeValue(context, stackValue);
        JS_FreeValue(context, error);
        if (!stack) return {};

        std::size_t offset = 0;
        while (offset < stack->size()) {
            const std::size_t end = stack->find('\n', offset);
            const std::string_view frame(
                stack->data() + offset,
                (end == std::string::npos ? stack->size() : end) - offset);
            offset = end == std::string::npos ? stack->size() : end + 1;
            if (frame.find("native") != std::string_view::npos ||
                frame.find("<prismatix-sandbox>") != std::string_view::npos)
                continue;

            std::size_t locationStart = frame.rfind('(');
            std::size_t locationEnd = frame.rfind(')');
            if (locationStart == std::string_view::npos ||
                locationEnd == std::string_view::npos ||
                locationStart >= locationEnd) {
                locationStart = frame.find("at ");
                if (locationStart == std::string_view::npos) continue;
                locationStart += 2;
                locationEnd = frame.size();
            } else {
                ++locationStart;
            }
            const auto location =
                frame.substr(locationStart, locationEnd - locationStart);
            const std::size_t columnSeparator = location.rfind(':');
            if (columnSeparator == std::string_view::npos) continue;
            const std::size_t lineSeparator =
                location.rfind(':', columnSeparator - 1);
            if (lineSeparator == std::string_view::npos) continue;
            int line = 0;
            const auto encodedLine = location.substr(
                lineSeparator + 1, columnSeparator - lineSeparator - 1);
            const auto parsed = std::from_chars(
                encodedLine.data(), encodedLine.data() + encodedLine.size(), line);
            if (parsed.ec != std::errc{} || line <= 0) continue;
            std::string source(location.substr(0, lineSeparator));
            if (source.empty() || source.front() == '<') continue;
            return {NormalizeDebugSource(std::move(source)), line};
        }
        return {};
    }

    void Error(const std::string& source, std::string message) const {
        Console(ConsoleLevel::Error, message, source);
        diag::Emit(ScriptDiagnostic("PXJS7001", "JavaScript execution failed",
                                    std::move(message), source));
    }

    static Impl* From(JSContext* context) {
        return static_cast<Impl*>(JS_GetContextOpaque(context));
    }

    [[nodiscard]] bool HasCapability(const std::string_view capability) const {
        return !isExtensionRealm || capabilities.contains(std::string(capability));
    }

    JSValue PermissionDenied(const std::string_view capability,
                             const std::string_view operation) const {
        JSValue error = JS_NewError(context);
        JS_SetPropertyStr(context, error, "name",
                          JS_NewString(context, "PermissionError"));
        JS_SetPropertyStr(context, error, "code",
                          JS_NewString(context, "PXJS7101"));
        JS_SetPropertyStr(context, error, "capability",
                          JS_NewStringLen(context, capability.data(), capability.size()));
        JS_SetPropertyStr(context, error, "operation",
                          JS_NewStringLen(context, operation.data(), operation.size()));
        JS_SetPropertyStr(context, error, "extensionId",
                          JS_NewString(context, extensionId.c_str()));
        JS_SetPropertyStr(
            context, error, "message",
            JS_NewString(context,
                ("Extension '" + extensionId + "' lacks capability '" +
                 std::string(capability) + "' for " + std::string(operation)).c_str()));
        return JS_Throw(context, error);
    }

    JSValue SettledPromise(const bool rejected, JSValueConst value) {
        JSValue resolving[2] = {JS_UNDEFINED, JS_UNDEFINED};
        JSValue promise = JS_NewPromiseCapability(context, resolving);
        if (JS_IsException(promise)) return promise;
        JSValue argument = JS_DupValue(context, value);
        JSValue result = JS_Call(context, resolving[rejected ? 1 : 0],
                                 JS_UNDEFINED, 1, &argument);
        JS_FreeValue(context, argument);
        JS_FreeValue(context, result);
        JS_FreeValue(context, resolving[0]);
        JS_FreeValue(context, resolving[1]);
        return promise;
    }

    static JSValue Log(JSContext* context, JSValueConst, const int count,
                       JSValueConst* values) {
        auto* self = From(context);
        std::string text;
        for (int index = 0; index < count; ++index) {
            if (index) text.push_back(' ');
            text.append(self->String(values[index]).value_or("<unprintable>"));
        }
        auto [source, line] = self->CurrentLocation();
        self->Console(ConsoleLevel::Info, std::move(text), std::move(source), line);
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
        auto [source, line] = self->CurrentLocation();
        self->Console(ConsoleLevel::Warning, std::move(text), std::move(source), line);
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
        auto [source, line] = self->CurrentLocation();
        self->Console(ConsoleLevel::Error, std::move(text), std::move(source), line);
        return JS_UNDEFINED;
    }

    static JSValue RegisterCommand(JSContext* context, JSValueConst, const int count,
                                   JSValueConst* values) {
        auto* self = From(context);
        if (self->insidePersistentCallback)
            return JS_ThrowTypeError(
                context, "registrations are forbidden inside a persistent callback");
        if (!self->HasCapability("runtime"))
            return self->PermissionDenied("runtime", "Engine.RegisterCommand");
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
        if (self->insidePersistentCallback)
            return JS_ThrowTypeError(
                context, "registrations are forbidden inside a persistent callback");
        if (!self->HasCapability("ui"))
            return self->PermissionDenied("ui", "Engine.RegisterAction");
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

    static JSValue RegisterStateProvider(JSContext* context, JSValueConst,
                                         const int count,
                                         JSValueConst* values) {
        auto* self = From(context);
        if (self->insidePersistentCallback ||
            self->insideStateProviderCallback)
            return JS_ThrowTypeError(
                context,
                "state provider registration is forbidden inside a persistent callback");
        if (!self->HasCapability("persistence"))
            return self->PermissionDenied(
                "persistence", "Engine.RegisterStateProvider");
        std::int64_t version = 0;
        if (count != 3 || !JS_IsString(values[0]) ||
            JS_ToInt64(context, &version, values[1]) != 0 || version <= 0 ||
            version > (std::numeric_limits<std::uint32_t>::max)() ||
            !JS_IsObject(values[2]))
            return JS_ThrowTypeError(
                context,
                "RegisterStateProvider requires an id, positive version, and provider object");
        const auto id = self->String(values[0]);
        const auto validId = [](const std::string_view value) {
            return !value.empty() && value.size() <= 128 &&
                   std::ranges::all_of(value, [](const unsigned char character) {
                       return std::isalnum(character) || character == '.' ||
                              character == '_' || character == '-';
                   });
        };
        if (!id || !validId(*id))
            return JS_ThrowRangeError(
                context, "state provider id must be a portable identifier");
        if (self->stateProviders.size() >= 256 ||
            self->stateProviders.contains(*id))
            return JS_ThrowRangeError(
                context, "state provider id is duplicated or limit was exceeded");

        JSValue capture = JS_GetPropertyStr(context, values[2], "capture");
        JSValue restore = JS_GetPropertyStr(context, values[2], "restore");
        JSValue migrate = JS_GetPropertyStr(context, values[2], "migrate");
        const bool valid = JS_IsFunction(context, capture) &&
                           JS_IsFunction(context, restore) &&
                           (JS_IsUndefined(migrate) ||
                            JS_IsFunction(context, migrate));
        if (!valid) {
            JS_FreeValue(context, capture);
            JS_FreeValue(context, restore);
            JS_FreeValue(context, migrate);
            return JS_ThrowTypeError(
                context,
                "state provider requires capture/restore functions and optional migrate function");
        }
        self->stateProviders.emplace(
            *id, StateProvider{static_cast<std::uint32_t>(version), capture,
                               restore, migrate});
        return JS_UNDEFINED;
    }

    static JSValue On(JSContext* context, JSValueConst, const int count,
                      JSValueConst* values) {
        auto* self = From(context);
        if (self->insidePersistentCallback)
            return JS_ThrowTypeError(
                context, "registrations are forbidden inside a persistent callback");
        if (!self->HasCapability("runtime"))
            return self->PermissionDenied("runtime", "Engine.On");
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
        if (self->insideStateProviderCallback)
            return JS_ThrowTypeError(
                context,
                "event emission is forbidden inside a state provider callback");
        if (!self->HasCapability("runtime"))
            return self->PermissionDenied("runtime", "Engine.Emit");
        if (count < 1 || !JS_IsString(values[0]))
            return JS_ThrowTypeError(context, "Engine.Emit requires an event id");
        const auto event = self->String(values[0]);
        if (!event || event->empty()) return JS_ThrowTypeError(context, "event id is empty");
        EventPayload payload;
        if (count >= 2 && !JS_IsUndefined(values[1])) {
            const auto converted = self->FromJavaScript(values[1]);
            if (!converted || !IsJsonValue(*converted))
                return JS_ThrowTypeError(
                    context, "Engine.Emit payload must be a recursive JSON value");
            payload = converted->Clone();
        }
        const Status status =
            (self->eventRoot ? self->eventRoot : &self->host)->Emit(*event, payload);
        if (status) return self->SettledPromise(false, JS_UNDEFINED);
        JSValue error = JS_NewError(context);
        JS_SetPropertyStr(context, error, "name",
                          JS_NewString(context, "EventDispatchError"));
        JS_SetPropertyStr(context, error, "code",
                          JS_NewString(context, "PXJS7201"));
        const auto details = status.Diagnostics().empty()
                                 ? std::string("event dispatch failed")
                                 : diag::Describe(status.Diagnostics().front());
        JS_SetPropertyStr(context, error, "message",
                          JS_NewString(context, details.c_str()));
        JSValue promise = self->SettledPromise(true, error);
        JS_FreeValue(context, error);
        return promise;
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

    JSValue RuntimeOperationLive(const std::string_view operation, const int count,
                                 JSValueConst* values) {
        const auto capability = CapabilityForOperation(operation);
        if (!HasCapability(capability))
            return PermissionDenied(capability, operation);
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
        if (operation == "await.screenEffect") {
            if (!IntegerArgument(count, values, 0, integer) || integer < 0)
                return JS_ThrowRangeError(
                    context, "AwaitScreenEffect requires a valid handle");
            JSValue token = JS_NewObject(context);
            JS_SetPropertyStr(context, token, "kind",
                              JS_NewString(context, "screen-effect"));
            JS_SetPropertyStr(context, token, "handle",
                              JS_NewInt64(context, integer));
            return token;
        }
        if (operation == "await.video") {
            if (!IntegerArgument(count, values, 0, integer) || integer <= 0)
                return JS_ThrowRangeError(context,
                                          "AwaitVideo requires a valid handle");
            JSValue token = JS_NewObject(context);
            JS_SetPropertyStr(context, token, "kind",
                              JS_NewString(context, "video"));
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
        if (operation == "wait.screenEffect") {
            if (!IntegerArgument(count, values, 0, integer) || integer < 0)
                return JS_ThrowRangeError(
                    context, "WaitScreenEffect requires a valid handle");
            return CreatePromiseWait(
                {.kind = "screen-effect",
                 .handle = static_cast<std::uint64_t>(integer)});
        }
        if (operation == "wait.video") {
            if (!IntegerArgument(count, values, 0, integer) || integer <= 0)
                return JS_ThrowRangeError(context,
                                          "WaitVideo requires a valid handle");
            return CreatePromiseWait(
                {.kind = "video",
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
            std::string scope = "session";
            if (count >= 3 && !JS_IsUndefined(values[2]) &&
                !StringArgument(count, values, 2, scope))
                return JS_ThrowTypeError(context, "SetVariable scope must be a string");
            vn::VariableScope selected = vn::VariableScope::Session;
            if (scope == "profile") selected = vn::VariableScope::Profile;
            else if (scope != "session")
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
            const auto parseTransition = [&](const int index)
                -> std::optional<ui::RouteTransition> {
                ui::RouteTransition transition;
                if (index >= count || JS_IsUndefined(values[index]) ||
                    JS_IsNull(values[index]))
                    return transition;
                if (!JS_IsObject(values[index])) return std::nullopt;
                if (const auto preset = StringProperty(values[index], "preset"))
                    transition.preset = *preset;
                if (const auto duration =
                        NumberProperty(values[index], "durationSeconds"))
                    transition.durationSeconds = static_cast<float>(*duration);
                else if (const auto legacyDuration =
                             NumberProperty(values[index], "duration"))
                    transition.durationSeconds = static_cast<float>(*legacyDuration);
                return transition;
            };
            if (operation == "route.defaultTransition") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(
                        context,
                        "SetRouteTransition requires outgoing and incoming routes");
                const auto transition = parseTransition(2);
                if (!transition || transition->preset.empty())
                    return JS_ThrowTypeError(
                        context,
                        "SetRouteTransition requires a preset transition object");
                const Status status = services.routes->SetDefaultTransition(
                    first, second, *transition);
                if (!status)
                    return JS_ThrowTypeError(
                        context, "%s",
                        diag::Describe(status.Diagnostics().front()).c_str());
                return JS_TRUE;
            }
            if (operation == "route.back" || operation == "route.closeModal") {
                const auto transition = parseTransition(0);
                if (!transition)
                    return JS_ThrowTypeError(
                        context, "route transition must be an object");
                const Status status = operation == "route.back"
                                          ? services.routes->Back(*transition)
                                          : services.routes->CloseModal(*transition);
                if (!status)
                    return JS_ThrowInternalError(
                        context, "%s",
                        diag::Describe(status.Diagnostics().front()).c_str());
                return JS_TRUE;
            }
            if (!StringArgument(count, values, 0, first))
                return JS_ThrowTypeError(context, "route operation requires a route id");
            const auto transition = parseTransition(1);
            if (!transition)
                return JS_ThrowTypeError(context,
                                         "route transition must be an object");
            Status status;
            if (operation == "route.push")
                status = services.routes->Push(first, *transition);
            else if (operation == "route.replace")
                status = services.routes->Replace(first, *transition);
            else if (operation == "route.showModal")
                status = services.routes->ShowModal(first, *transition);
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
            if (operation == "audio.playVoice") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context,
                                             "PlayVoice requires a path");
                services.audio->PlayVoice(first);
                return JS_UNDEFINED;
            }
            if (operation == "audio.stopVoice") {
                services.audio->StopVoice();
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
        if (operation.starts_with("save.")) {
            if (insidePersistentCallback &&
                operation != "save.query" && operation != "save.list")
                return JS_ThrowTypeError(
                    context,
                    "save mutations are forbidden inside a persistent callback");
            if (operation == "save.autosave") {
                if (!services.requestSave)
                    return JS_ThrowInternalError(context,
                                                 "save service is unavailable");
                return JS_NewBool(context, services.requestSave(-1));
            }
            if (operation == "save.list") {
                std::int64_t countValue = 20;
                if (count >= 1 && !JS_IsUndefined(values[0]) &&
                    (!IntegerArgument(count, values, 0, countValue) ||
                     countValue < 0 || countValue > 1000))
                    return JS_ThrowRangeError(
                        context, "save list count must be between 0 and 1000");
                if (!services.listSaves)
                    return JS_ThrowInternalError(context,
                                                 "save query service is unavailable");
                return ToJavaScript(services.listSaves(
                    static_cast<int>(countValue)));
            }
            if (!IntegerArgument(count, values, 0, integer) || integer < 0 ||
                integer > 999)
                return JS_ThrowRangeError(context,
                                          "save slot must be between 0 and 999");
            if (operation == "save.write") {
                if (!services.requestSave)
                    return JS_ThrowInternalError(context,
                                                 "save service is unavailable");
                return JS_NewBool(
                    context, services.requestSave(static_cast<int>(integer)));
            }
            if (operation == "save.load") {
                if (!services.requestLoad)
                    return JS_ThrowInternalError(context,
                                                 "load service is unavailable");
                return JS_NewBool(
                    context, services.requestLoad(static_cast<int>(integer)));
            }
            if (operation == "save.delete") {
                if (!services.deleteSave)
                    return JS_ThrowInternalError(context,
                                                 "save service is unavailable");
                return JS_NewBool(
                    context, services.deleteSave(static_cast<int>(integer)));
            }
            if (operation == "save.query") {
                if (!services.querySave)
                    return JS_ThrowInternalError(context,
                                                 "save query service is unavailable");
                return ToJavaScript(
                    services.querySave(static_cast<int>(integer)));
            }
            return JS_ThrowRangeError(context, "unknown save operation");
        }
        if (operation.starts_with("video.")) {
            if (operation == "video.play") {
                if (!services.playVideo ||
                    !StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context,
                                             "PlayVideo requires a path");
                double volume = 1.0;
                bool skippable = true;
                if (count >= 2 && !JS_IsUndefined(values[1]) &&
                    (!NumberArgument(count, values, 1, volume) || volume < 0.0 ||
                     volume > 1.0))
                    return JS_ThrowRangeError(
                        context, "video volume must be between 0 and 1");
                if (count >= 3 && !JS_IsUndefined(values[2]) &&
                    !BoolArgument(count, values, 2, skippable))
                    return JS_ThrowTypeError(context,
                                             "video skippable must be boolean");
                const auto handle = services.playVideo(
                    first, static_cast<float>(volume), skippable);
                if (handle == 0)
                    return JS_ThrowInternalError(context,
                                                 "video could not be opened");
                return JS_NewInt64(context,
                                   static_cast<std::int64_t>(handle));
            }
            if (!IntegerArgument(count, values, 0, integer) || integer <= 0)
                return JS_ThrowRangeError(context,
                                          "video handle is invalid");
            const auto handle = static_cast<std::uint64_t>(integer);
            if (operation == "video.pause")
                return JS_NewBool(
                    context, services.pauseVideo && services.pauseVideo(handle));
            if (operation == "video.resume")
                return JS_NewBool(
                    context, services.resumeVideo && services.resumeVideo(handle));
            if (operation == "video.stop")
                return JS_NewBool(context,
                                  services.stopVideo && services.stopVideo(handle));
            if (operation == "video.skip")
                return JS_NewBool(context,
                                  services.skipVideo && services.skipVideo(handle));
            if (operation == "video.status")
                return JS_NewString(
                    context,
                    services.videoStatus
                        ? services.videoStatus(handle).c_str()
                        : "unavailable");
            if (operation == "video.error")
                return JS_NewString(
                    context,
                    services.videoError ? services.videoError(handle).c_str()
                                        : "video service is unavailable");
            return JS_ThrowRangeError(context, "unknown video operation");
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
            if (operation == "stage.backgroundRule") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(
                        context,
                        "SetBackgroundRule requires background and rule paths");
                std::int64_t duration = 600;
                std::int64_t vague = 64;
                if (count >= 3 && !JS_IsUndefined(values[2]) &&
                    !IntegerArgument(count, values, 2, duration))
                    return JS_ThrowTypeError(
                        context, "background rule duration must be milliseconds");
                if (count >= 4 && !JS_IsUndefined(values[3]) &&
                    !IntegerArgument(count, values, 3, vague))
                    return JS_ThrowTypeError(
                        context, "background rule vague must be an integer");
                services.stage->SetBackgroundRule(
                    first, second, static_cast<int>(std::max<std::int64_t>(0, duration)),
                    static_cast<int>(std::max<std::int64_t>(1, vague)));
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
            if (operation == "stage.layerTransform") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(
                        context,
                        "SetLayerTransform requires a name and image");
                double x = 0.0, y = 0.0, scaleX = 1.0, scaleY = 1.0,
                       rotation = 0.0;
                std::int64_t alpha = 255, z = 0;
                if (count >= 3 && !JS_IsUndefined(values[2]) &&
                    !NumberArgument(count, values, 2, x))
                    return JS_ThrowTypeError(context, "layer x must be numeric");
                if (count >= 4 && !JS_IsUndefined(values[3]) &&
                    !NumberArgument(count, values, 3, y))
                    return JS_ThrowTypeError(context, "layer y must be numeric");
                if (count >= 5 && !JS_IsUndefined(values[4]) &&
                    !NumberArgument(count, values, 4, scaleX))
                    return JS_ThrowTypeError(context,
                                             "layer scaleX must be numeric");
                if (count >= 6 && !JS_IsUndefined(values[5]) &&
                    !NumberArgument(count, values, 5, scaleY))
                    return JS_ThrowTypeError(context,
                                             "layer scaleY must be numeric");
                if (count >= 7 && !JS_IsUndefined(values[6]) &&
                    !NumberArgument(count, values, 6, rotation))
                    return JS_ThrowTypeError(context,
                                             "layer rotation must be numeric");
                if (count >= 8 && !JS_IsUndefined(values[7]) &&
                    !IntegerArgument(count, values, 7, alpha))
                    return JS_ThrowTypeError(context,
                                             "layer alpha must be integer");
                if (count >= 9 && !JS_IsUndefined(values[8]) &&
                    !IntegerArgument(count, values, 8, z))
                    return JS_ThrowTypeError(context, "layer z must be integer");
                services.stage->SetLayerTransform(
                    first, second, static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(scaleX), static_cast<float>(scaleY),
                    static_cast<float>(rotation),
                    static_cast<std::uint8_t>(
                        std::clamp<std::int64_t>(alpha, 0, 255)),
                    static_cast<int>(z));
                return JS_UNDEFINED;
            }
            if (operation == "stage.clearLayer") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context, "ClearLayer requires a name");
                services.stage->ClearLayer(first);
                return JS_UNDEFINED;
            }
            if (operation == "stage.group") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context,
                                             "SetStageGroup requires a name");
                if (count >= 2 && !JS_IsUndefined(values[1]) &&
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(context,
                                             "stage group parent must be a string");
                return JS_NewBool(context,
                                  services.stage->SetGroupNode(first, second));
            }
            if (operation == "stage.nodeParent") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(
                        context, "SetStageNodeParent requires a name and parent");
                return JS_NewBool(context,
                                  services.stage->SetNodeParent(first, second));
            }
            if (operation == "stage.nodeTransform") {
                if (!StringArgument(count, values, 0, first) || count < 2 ||
                    !JS_IsObject(values[1]))
                    return JS_ThrowTypeError(
                        context, "SetStageNodeTransform requires a name and options");
                vn::Stage::NodeTransform transform;
                if (const auto value = NumberProperty(values[1], "x"))
                    transform.x = static_cast<float>(*value);
                if (const auto value = NumberProperty(values[1], "y"))
                    transform.y = static_cast<float>(*value);
                if (const auto value = NumberProperty(values[1], "scaleX"))
                    transform.scaleX = static_cast<float>(*value);
                if (const auto value = NumberProperty(values[1], "scaleY"))
                    transform.scaleY = static_cast<float>(*value);
                if (const auto value = NumberProperty(values[1], "rotation"))
                    transform.rotation = static_cast<float>(*value);
                if (const auto value = NumberProperty(values[1], "opacity"))
                    transform.opacity = static_cast<float>(*value);
                return JS_NewBool(
                    context, services.stage->SetNodeTransform(first, transform));
            }
            if (operation == "stage.nodeOrder") {
                std::int64_t order = 0;
                if (!StringArgument(count, values, 0, first) ||
                    !IntegerArgument(count, values, 1, integer) ||
                    !IntegerArgument(count, values, 2, order) ||
                    integer < std::numeric_limits<int>::min() ||
                    integer > std::numeric_limits<int>::max() ||
                    order < std::numeric_limits<int>::min() ||
                    order > std::numeric_limits<int>::max())
                    return JS_ThrowTypeError(
                        context, "SetStageNodeOrder requires name, z, and order");
                return JS_NewBool(context, services.stage->SetNodeOrder(
                                               first, static_cast<int>(integer),
                                               static_cast<int>(order)));
            }
            if (operation == "stage.nodeVisibility") {
                if (!StringArgument(count, values, 0, first) ||
                    !BoolArgument(count, values, 1, boolean))
                    return JS_ThrowTypeError(
                        context, "SetStageNodeVisibility requires name and boolean");
                return JS_NewBool(context,
                                  services.stage->SetNodeVisibility(first, boolean));
            }
            if (operation == "stage.removeNode") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context,
                                             "RemoveStageNode requires a name");
                services.stage->RemoveNode(first);
                return JS_UNDEFINED;
            }
            if (operation == "stage.particles") {
                if (!StringArgument(count, values, 0, first) ||
                    !StringArgument(count, values, 1, second))
                    return JS_ThrowTypeError(
                        context, "SetParticleEmitter requires a name and preset");
                const auto preset = vn::ParticlePresetFromName(second);
                if (!preset)
                    return JS_ThrowRangeError(context,
                                              "particle preset is unsupported");
                vn::ParticleEmitterSpec spec;
                spec.preset = *preset;
                if (count >= 3 && !JS_IsUndefined(values[2])) {
                    if (!JS_IsObject(values[2]))
                        return JS_ThrowTypeError(
                            context, "particle options must be an object");
                    if (const auto value = NumberProperty(values[2], "seed")) {
                        if (*value < 1.0 ||
                            *value > std::numeric_limits<std::uint32_t>::max() ||
                            std::floor(*value) != *value)
                            return JS_ThrowRangeError(
                                context, "particle seed must be a positive uint32");
                        spec.seed = static_cast<std::uint32_t>(*value);
                    }
                    if (const auto value = NumberProperty(values[2], "rate"))
                        spec.rate = static_cast<float>(*value);
                    if (const auto value = NumberProperty(values[2], "maxParticles")) {
                        if (*value < 1.0 || *value > 4096.0 ||
                            std::floor(*value) != *value)
                            return JS_ThrowRangeError(
                                context, "particle maxParticles must be 1..4096");
                        spec.maxParticles = static_cast<std::uint32_t>(*value);
                    }
                    if (const auto value = NumberProperty(values[2], "z")) {
                        if (*value < std::numeric_limits<int>::min() ||
                            *value > std::numeric_limits<int>::max() ||
                            std::floor(*value) != *value)
                            return JS_ThrowRangeError(
                                context, "particle z must be an integer");
                        spec.z = static_cast<int>(*value);
                    }
                    if (const auto value = NumberProperty(values[2], "opacity"))
                        spec.opacity = static_cast<float>(*value);
                    if (const auto value = NumberProperty(values[2], "wind"))
                        spec.wind = static_cast<float>(*value);
                    if (const auto value = NumberProperty(values[2], "speed"))
                        spec.speed = static_cast<float>(*value);
                    if (const auto value = NumberProperty(values[2], "size"))
                        spec.size = static_cast<float>(*value);
                }
                return JS_NewBool(
                    context, services.stage->SetParticleEmitter(first, spec));
            }
            if (operation == "stage.clearParticles") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(
                        context, "ClearParticleEmitter requires a name");
                services.stage->ClearParticleEmitter(first);
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
            if (operation == "stage.camera") {
                double x = 0.0, y = 0.0, zoom = 1.0;
                if (!NumberArgument(count, values, 0, x) ||
                    !NumberArgument(count, values, 1, y) ||
                    !NumberArgument(count, values, 2, zoom) ||
                    !services.stage->SetCamera(static_cast<float>(x),
                                               static_cast<float>(y),
                                               static_cast<float>(zoom)))
                    return JS_ThrowRangeError(
                        context, "SetCamera requires finite x/y and positive zoom");
                return JS_UNDEFINED;
            }
            if (operation == "stage.screenEffect") {
                double amount = 0.0;
                if (!StringArgument(count, values, 0, first) ||
                    !NumberArgument(count, values, 1, amount))
                    return JS_ThrowTypeError(
                        context, "SetScreenEffect requires an id and amount");
                if (!services.stage->SetScreenEffect(first,
                                                     static_cast<float>(amount)))
                    return JS_ThrowRangeError(
                        context, "screen effect is unknown or unavailable");
                return JS_UNDEFINED;
            }
            if (operation == "stage.clearScreenEffect") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(
                        context, "ClearScreenEffect requires an id");
                services.stage->ClearScreenEffect(first);
                return JS_UNDEFINED;
            }
            if (operation == "stage.customEffect") {
                double progress = 0.0;
                if (!StringArgument(count, values, 0, first) ||
                    !NumberArgument(count, values, 1, progress))
                    return JS_ThrowTypeError(
                        context, "SetCustomEffect requires an id and progress");
                std::array<std::array<float, 4>, 8> parameters{};
                const std::array<std::array<float, 4>, 8>* parameterPointer = nullptr;
                graphics::CustomEffectNamedParameters namedParameters;
                bool hasNamedParameters = false;
                if (count >= 3 && !JS_IsUndefined(values[2])) {
                    const auto converted = FromJavaScript(values[2]);
                    const auto* slots = converted ? converted->AsArray() : nullptr;
                    const auto* named = converted ? converted->AsObject() : nullptr;
                    const auto numeric = [](const Variant& value,
                                            float& output) {
                        if (const auto* number = value.TryGet<double>()) {
                            output = static_cast<float>(*number);
                            return std::isfinite(output);
                        }
                        if (const auto* integer =
                                value.TryGet<std::int64_t>()) {
                            output = static_cast<float>(*integer);
                            return std::isfinite(output);
                        }
                        return false;
                    };
                    if (slots) {
                        if (slots->size() > parameters.size())
                            return JS_ThrowTypeError(
                                context, "custom effect parameters must contain at most 8 vec4 slots");
                        for (std::size_t slot = 0; slot < slots->size(); ++slot) {
                            const auto* components = slots->at(slot).AsArray();
                            if (!components || components->size() > 4)
                                return JS_ThrowTypeError(
                                    context, "custom effect parameter slots must be vec4 arrays");
                            for (std::size_t component = 0;
                                 component < components->size(); ++component) {
                                if (!numeric(components->at(component),
                                             parameters[slot][component]))
                                    return JS_ThrowTypeError(
                                        context, "custom effect parameters must be finite numbers");
                            }
                        }
                        parameterPointer = &parameters;
                    } else if (named) {
                        if (named->size() > parameters.size())
                            return JS_ThrowTypeError(
                                context, "custom effect parameters must contain at most 8 named values");
                        for (const auto& [name, value] : *named) {
                            std::vector<float> components;
                            if (const auto* vectorValues = value.AsArray()) {
                                if (vectorValues->empty() ||
                                    vectorValues->size() > 4)
                                    return JS_ThrowTypeError(
                                        context, "named custom effect vectors must contain 1 to 4 numbers");
                                components.resize(vectorValues->size());
                                for (std::size_t component = 0;
                                     component < vectorValues->size(); ++component)
                                    if (!numeric(vectorValues->at(component),
                                                 components[component]))
                                        return JS_ThrowTypeError(
                                            context, "custom effect parameters must be finite numbers");
                            } else {
                                components.resize(1);
                                if (!numeric(value, components.front()))
                                    return JS_ThrowTypeError(
                                        context, "named custom effect parameters must be numbers or numeric arrays");
                            }
                            namedParameters.emplace(name, std::move(components));
                        }
                        hasNamedParameters = true;
                    } else {
                        return JS_ThrowTypeError(
                            context, "custom effect parameters must be named values or at most 8 vec4 slots");
                    }
                }
                const bool applied = hasNamedParameters
                    ? services.stage->SetCustomEffect(
                          first, static_cast<float>(progress), namedParameters)
                    : services.stage->SetCustomEffect(
                          first, static_cast<float>(progress), parameterPointer);
                if (!applied)
                    return JS_ThrowRangeError(
                        context, "custom stage effect or its named parameters are invalid or unavailable");
                return JS_UNDEFINED;
            }
            if (operation == "stage.clearCustomEffect") {
                services.stage->ClearCustomEffect();
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
        if (operation.starts_with("effect.")) {
            if (!services.renderer)
                return JS_ThrowInternalError(context, "Renderer2D is unavailable");
            if (operation == "effect.register") {
                if (!StringArgument(count, values, 0, first) || count < 2 ||
                    !JS_IsObject(values[1]))
                    return JS_ThrowTypeError(
                        context, "RegisterScreenEffect requires an id and render plan");
                graphics::ScreenEffectDefinition definition;
                definition.id = first;
                definition.operation =
                    StringProperty(values[1], "operator")
                        .value_or(StringProperty(values[1], "operation")
                                      .value_or(std::string{}));
                if (const auto columns = NumberProperty(values[1], "columns"))
                    definition.columns = static_cast<int>(*columns);
                if (const auto rows = NumberProperty(values[1], "rows"))
                    definition.rows = static_cast<int>(*rows);
                if (const auto stagger = NumberProperty(values[1], "stagger"))
                    definition.stagger = static_cast<float>(*stagger);
                if (const auto order = StringProperty(values[1], "order"))
                    definition.order = *order;
                const Status status =
                    services.renderer->RegisterScreenEffect(std::move(definition));
                if (!status)
                    return JS_ThrowTypeError(
                        context, "%s",
                        diag::Describe(status.Diagnostics().front()).c_str());
                return JS_TRUE;
            }
            if (operation == "effect.play") {
                double duration = 0.5;
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(
                        context, "PlayScreenEffect requires an id");
                if (count >= 2 && !JS_IsUndefined(values[1]) &&
                    !NumberArgument(count, values, 1, duration))
                    return JS_ThrowTypeError(
                        context, "screen effect duration must be seconds");
                return JS_NewInt64(
                    context,
                    static_cast<std::int64_t>(services.renderer->PlayScreenEffect(
                        first, static_cast<float>(duration))));
            }
            if (!IntegerArgument(count, values, 0, integer) || integer < 0)
                return JS_ThrowTypeError(context,
                                         "screen effect handle is invalid");
            const auto handle =
                static_cast<graphics::ScreenEffectHandle>(integer);
            if (operation == "effect.stop")
                return JS_NewBool(
                    context, services.renderer->StopScreenEffect(handle));
            if (operation == "effect.cancel")
                return JS_NewBool(
                    context, services.renderer->CancelScreenEffect(handle));
            if (operation == "effect.playing")
                return JS_NewBool(
                    context, services.renderer->ScreenEffectPlaying(handle));
            if (operation == "effect.status") {
                const auto status = services.renderer->ScreenEffectState(handle);
                const char* name = "unknown";
                if (status == graphics::ScreenEffectStatus::Playing) name = "playing";
                else if (status == graphics::ScreenEffectStatus::Completed) name = "completed";
                else if (status == graphics::ScreenEffectStatus::Stopped) name = "stopped";
                else if (status == graphics::ScreenEffectStatus::Cancelled) name = "cancelled";
                return JS_NewString(context, name);
            }
            return JS_ThrowRangeError(context, "unknown screen effect operation");
        }
        if (operation.starts_with("input.")) {
            if (!services.input) return JS_ThrowInternalError(context, "Input is unavailable");
            if (operation == "input.mouseX") return JS_NewFloat64(context, services.input->MouseX());
            if (operation == "input.mouseY") return JS_NewFloat64(context, services.input->MouseY());
            if (operation == "input.leftClick") return JS_NewBool(context, services.input->LeftClick());
            if (operation == "input.rightClick") return JS_NewBool(context, services.input->RightClick());
            if (operation == "input.actionPressed" ||
                operation == "input.actionDown") {
                if (!StringArgument(count, values, 0, first))
                    return JS_ThrowTypeError(context,
                                             "input action requires a name");
                const auto action = ParseInputAction(first);
                if (!action)
                    return JS_ThrowRangeError(context,
                                              "unknown logical input action");
                return JS_NewBool(
                    context, operation == "input.actionPressed"
                                 ? services.input->ActionPressed(*action)
                                 : services.input->ActionDown(*action));
            }
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

    [[nodiscard]] bool JournaledOperation(
        const std::string_view operation) const {
        return operation != "debug.point" &&
               !operation.starts_with("await.") &&
               !operation.starts_with("wait.") &&
               !operation.starts_with("video.");
    }

    JSValue RuntimeOperation(const std::string_view operation, const int count,
                             JSValueConst* values) {
        if (insideStateProviderCallback)
            return JS_ThrowTypeError(
                context,
                "Engine operations are forbidden inside a state provider callback");
        const auto capability = CapabilityForOperation(operation);
        if (!HasCapability(capability))
            return PermissionDenied(capability, operation);
        if ((!recordingJournal && !replayJournal) ||
            !JournaledOperation(operation))
            return RuntimeOperationLive(operation, count, values);

        VariantArray arguments;
        arguments.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int index = 0; index < count; ++index) {
            auto converted = FromJavaScript(values[index]);
            if (!converted)
                return JS_ThrowTypeError(
                    context, "engine operation arguments are not journalable");
            arguments.push_back(converted->Clone());
        }

        if (replayJournal) {
            if (replayJournalCursor >= replayJournal->size())
                return JS_ThrowInternalError(
                    context, "engine operation journal ended before replay");
            const auto& expected = (*replayJournal)[replayJournalCursor++];
            if (expected.operation != operation ||
                expected.arguments != arguments)
                return JS_ThrowInternalError(
                    context, "engine operation journal diverged at operation %s",
                    std::string(operation).c_str());
            return expected.resultUndefined ? JS_UNDEFINED
                                            : ToJavaScript(expected.result);
        }

        JSValue result = RuntimeOperationLive(operation, count, values);
        if (JS_IsException(result)) return result;
        const bool undefined = JS_IsUndefined(result);
        auto converted = FromJavaScript(result);
        if (!converted) {
            JS_FreeValue(context, result);
            return JS_ThrowInternalError(
                context, "engine operation returned a non-journalable value");
        }
        recordingJournal->push_back({.operation = std::string(operation),
                                     .arguments = std::move(arguments),
                                     .result = converted->Clone(),
                                     .resultUndefined = undefined});
        return result;
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
        JS_SetPropertyStr(
            context, engine, "RegisterStateProvider",
            JS_NewCFunction(context, &RegisterStateProvider,
                            "RegisterStateProvider", 3));
        JS_SetPropertyStr(context, engine, "On",
                          JS_NewCFunction(context, &On, "On", 2));
        JS_SetPropertyStr(context, engine, "Emit",
                          JS_NewCFunction(context, &EmitEvent, "Emit", 2));
        JS_SetPropertyStr(context, engine, "on",
                          JS_NewCFunction(context, &On, "on", 2));
        JS_SetPropertyStr(context, engine, "emit",
                          JS_NewCFunction(context, &EmitEvent, "emit", 2));
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
                SetRouteTransition: { value: bindRuntime("route.defaultTransition") },
                HasSeen: { value: bindRuntime("profile.hasSeen") },
                MarkSeen: { value: bindRuntime("profile.markSeen") },
                ClearCount: { value: bindRuntime("profile.clearCount") },
                CGUnlocked: { value: bindRuntime("profile.cgUnlocked") },
                SceneUnlocked: { value: bindRuntime("profile.sceneUnlocked") },
                UnlockCG: { value: bindRuntime("profile.unlockCG") },
                UnlockScene: { value: bindRuntime("profile.unlockScene") },
                PlaySE: { value: bindRuntime("audio.playSE") },
                PlayVoice: { value: bindRuntime("audio.playVoice") },
                StopVoice: { value: bindRuntime("audio.stopVoice") },
                PlayBGM: { value: bindRuntime("audio.playBGM") },
                StopBGM: { value: bindRuntime("audio.stopBGM") },
                SetBGMVolume: { value: bindRuntime("audio.bgmVolume") },
                SetSEVolume: { value: bindRuntime("audio.seVolume") },
                SetVoiceVolume: { value: bindRuntime("audio.voiceVolume") },
                SetAmbienceVolume: { value: bindRuntime("audio.ambienceVolume") },
                PlayAmbience: { value: bindRuntime("audio.playAmbience") },
                StopAmbience: { value: bindRuntime("audio.stopAmbience") },
                Save: { value: bindRuntime("save.write") },
                Load: { value: bindRuntime("save.load") },
                Autosave: { value: bindRuntime("save.autosave") },
                DeleteSave: { value: bindRuntime("save.delete") },
                QuerySave: { value: bindRuntime("save.query") },
                ListSaves: { value: bindRuntime("save.list") },
                PlayVideo: { value: bindRuntime("video.play") },
                PauseVideo: { value: bindRuntime("video.pause") },
                ResumeVideo: { value: bindRuntime("video.resume") },
                StopVideo: { value: bindRuntime("video.stop") },
                SkipVideo: { value: bindRuntime("video.skip") },
                GetVideoStatus: { value: bindRuntime("video.status") },
                GetVideoError: { value: bindRuntime("video.error") },
                SetBackground: { value: bindRuntime("stage.background") },
                SetBackgroundRule: { value: bindRuntime("stage.backgroundRule") },
                SetCharacter: { value: bindRuntime("stage.character") },
                ClearCharacter: { value: bindRuntime("stage.clearCharacter") },
                MoveCharacter: { value: bindRuntime("stage.moveCharacter") },
                SetLayer: { value: bindRuntime("stage.layer") },
                SetLayerTransform: { value: bindRuntime("stage.layerTransform") },
                ClearLayer: { value: bindRuntime("stage.clearLayer") },
                SetStageGroup: { value: bindRuntime("stage.group") },
                SetStageNodeParent: { value: bindRuntime("stage.nodeParent") },
                SetStageNodeTransform: { value: bindRuntime("stage.nodeTransform") },
                SetStageNodeOrder: { value: bindRuntime("stage.nodeOrder") },
                SetStageNodeVisibility: { value: bindRuntime("stage.nodeVisibility") },
                RemoveStageNode: { value: bindRuntime("stage.removeNode") },
                SetParticleEmitter: { value: bindRuntime("stage.particles") },
                ClearParticleEmitter: { value: bindRuntime("stage.clearParticles") },
                Shake: { value: bindRuntime("stage.shake") },
                SetCamera: { value: bindRuntime("stage.camera") },
                SetScreenEffect: { value: bindRuntime("stage.screenEffect") },
                ClearScreenEffect: { value: bindRuntime("stage.clearScreenEffect") },
                SetCustomEffect: { value: bindRuntime("stage.customEffect") },
                ClearCustomEffect: { value: bindRuntime("stage.clearCustomEffect") },
                Animate: { value: bindRuntime("stage.animate") },
                LoadAnimation: { value: bindRuntime("animation.load") },
                PlayAnimation: { value: bindRuntime("animation.play") },
                CancelAnimation: { value: bindRuntime("animation.cancel") },
                RegisterScreenEffect: { value: bindRuntime("effect.register") },
                PlayScreenEffect: { value: bindRuntime("effect.play") },
                StopScreenEffect: { value: bindRuntime("effect.stop") },
                CancelScreenEffect: { value: bindRuntime("effect.cancel") },
                IsScreenEffectPlaying: { value: bindRuntime("effect.playing") },
                GetScreenEffectStatus: { value: bindRuntime("effect.status") },
                AwaitSeconds: { value: bindRuntime("await.timer") },
                AwaitAnimation: { value: bindRuntime("await.animation") },
                AwaitScreenEffect: { value: bindRuntime("await.screenEffect") },
                AwaitVideo: { value: bindRuntime("await.video") },
                WaitSeconds: { value: bindRuntime("wait.timer") },
                WaitAnimation: { value: bindRuntime("wait.animation") },
                WaitScreenEffect: { value: bindRuntime("wait.screenEffect") },
                WaitVideo: { value: bindRuntime("wait.video") },
                DebugPoint: { value: bindRuntime("debug.point") },
                GetMouseX: { value: bindRuntime("input.mouseX") },
                GetMouseY: { value: bindRuntime("input.mouseY") },
                GetLeftClick: { value: bindRuntime("input.leftClick") },
                GetRightClick: { value: bindRuntime("input.rightClick") },
                IsInputActionPressed: { value: bindRuntime("input.actionPressed") },
                IsInputActionDown: { value: bindRuntime("input.actionDown") },
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

    [[nodiscard]] Result<ExtensionStates> CaptureStateProviders() {
        std::vector<std::string> ids;
        ids.reserve(stateProviders.size());
        for (const auto& [id, _] : stateProviders) ids.push_back(id);
        std::ranges::sort(ids);

        ExtensionStates snapshots;
        snapshots.reserve(ids.size());
        for (const auto& id : ids) {
            auto& provider = stateProviders.at(id);
            StateCallbackScope scope(*this);
            BeginExecution();
            JSValue result =
                JS_Call(context, provider.capture, JS_UNDEFINED, 0, nullptr);
            EndExecution();
            if (JS_IsException(result)) {
                const std::string details = Exception();
                JS_FreeValue(context, result);
                return Result<ExtensionStates>::Failure(ScriptDiagnostic(
                    "PXJS7601", "Extension state capture failed",
                    id + ": " + details, extensionId));
            }
            if (JS_IsPromise(result)) {
                JS_FreeValue(context, result);
                return Result<ExtensionStates>::Failure(ScriptDiagnostic(
                    "PXJS7602",
                    "Extension state capture must be synchronous", id,
                    extensionId));
            }
            auto converted = FromJavaScript(result);
            JS_FreeValue(context, result);
            if (!converted || !IsJsonValue(*converted))
                return Result<ExtensionStates>::Failure(ScriptDiagnostic(
                    "PXJS7603",
                    "Extension state capture returned a non-JSON value", id,
                    extensionId));
            snapshots.push_back({extensionId, id, provider.version,
                                 converted->Clone()});
        }
        return Result<ExtensionStates>::Success(std::move(snapshots));
    }

    Status RestoreStateProviders(const ExtensionStates& snapshots) {
        std::unordered_map<std::string, const ExtensionStateSnapshot*> saved;
        for (const auto& snapshot : snapshots) {
            if (snapshot.sourceId != extensionId || snapshot.providerId.empty() ||
                !saved.emplace(snapshot.providerId, &snapshot).second)
                return Status::Fail(ScriptDiagnostic(
                    "PXJS7610",
                    "Extension state checkpoint contains an invalid provider identity",
                    snapshot.providerId, extensionId));
        }
        for (const auto& [id, _] : saved) {
            if (!stateProviders.contains(id))
                return Status::Fail(ScriptDiagnostic(
                    "PXJS7611",
                    "Saved extension state provider is no longer registered", id,
                    extensionId));
        }

        std::vector<std::string> ids;
        ids.reserve(stateProviders.size());
        for (const auto& [id, _] : stateProviders) ids.push_back(id);
        std::ranges::sort(ids);
        for (const auto& id : ids) {
            auto& provider = stateProviders.at(id);
            const auto found = saved.find(id);
            Variant value;
            std::uint32_t version = 0;
            if (found != saved.end()) {
                value = found->second->state.Clone();
                version = found->second->version;
            }
            if (!IsJsonValue(value) || version > provider.version)
                return Status::Fail(ScriptDiagnostic(
                    "PXJS7612",
                    "Saved extension state version or value is incompatible", id,
                    extensionId));

            StateCallbackScope scope(*this);
            if (version != provider.version) {
                if (JS_IsUndefined(provider.migrate))
                    return Status::Fail(ScriptDiagnostic(
                        "PXJS7613",
                        "Extension state requires an explicit migration", id,
                        extensionId));
                JSValue arguments[] = {
                    ToJavaScript(value), JS_NewUint32(context, version),
                    JS_NewUint32(context, provider.version)};
                BeginExecution();
                JSValue migrated = JS_Call(context, provider.migrate,
                                           JS_UNDEFINED, 3, arguments);
                EndExecution();
                for (auto& argument : arguments)
                    JS_FreeValue(context, argument);
                if (JS_IsException(migrated)) {
                    const std::string details = Exception();
                    JS_FreeValue(context, migrated);
                    return Status::Fail(ScriptDiagnostic(
                        "PXJS7614", "Extension state migration failed",
                        id + ": " + details, extensionId));
                }
                if (JS_IsPromise(migrated)) {
                    JS_FreeValue(context, migrated);
                    return Status::Fail(ScriptDiagnostic(
                        "PXJS7615",
                        "Extension state migration must be synchronous", id,
                        extensionId));
                }
                auto converted = FromJavaScript(migrated);
                JS_FreeValue(context, migrated);
                if (!converted || !IsJsonValue(*converted))
                    return Status::Fail(ScriptDiagnostic(
                        "PXJS7616",
                        "Extension state migration returned a non-JSON value",
                        id, extensionId));
                value = converted->Clone();
            }

            JSValue argument = ToJavaScript(value);
            BeginExecution();
            JSValue restored = JS_Call(context, provider.restore, JS_UNDEFINED,
                                       1, &argument);
            EndExecution();
            JS_FreeValue(context, argument);
            if (JS_IsException(restored)) {
                const std::string details = Exception();
                JS_FreeValue(context, restored);
                return Status::Fail(ScriptDiagnostic(
                    "PXJS7617", "Extension state restore failed",
                    id + ": " + details, extensionId));
            }
            if (JS_IsPromise(restored)) {
                JS_FreeValue(context, restored);
                return Status::Fail(ScriptDiagnostic(
                    "PXJS7618",
                    "Extension state restore must be synchronous", id,
                    extensionId));
            }
            JS_FreeValue(context, restored);
        }
        return Status::Ok();
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
        if (!kind || (*kind != "timer" && *kind != "animation" &&
                      *kind != "screen-effect" && *kind != "video")) {
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
        const std::uint32_t yieldIndex, const std::string& source,
        const EngineOperationJournal& journal) {
        JournalScope journalScope(*this, journal);
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

        if (result == StepResult::Yielded &&
            replayJournalCursor == journal.size()) return replayed;
        if (result == StepResult::Yielded)
            Error(source,
                  "engine operation journal contains calls beyond the restored checkpoint");
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
    std::unordered_set<std::string> loadedCommandSources;
    std::unordered_map<std::string, StateProvider> stateProviders;
    std::string activeExtension;
    std::string extensionId;
    std::string moduleRoot;
    std::unordered_map<std::string, std::string> moduleSources;
    std::unordered_set<std::string> capabilities;
    bool isExtensionRealm = false;
    bool ownsRegistrations = true;
    JavaScriptHost* eventRoot = nullptr;
    std::vector<std::unique_ptr<JavaScriptHost>> extensionRealms;
    struct QueuedEvent {
        std::string name;
        EventPayload payload;
        std::uint32_t depth = 0;
    };
    std::deque<QueuedEvent> eventQueue;
    bool dispatchingEvents = false;
    std::uint32_t currentEventDepth = 0;
    std::vector<PendingCommandContinuation> pendingCommands;
    std::vector<PendingActionContinuation> pendingActions;
    std::unordered_map<std::uint64_t, ui::ActionExecutionState>
        actionTerminalStates;
    std::optional<PromiseWaitCapability> createdPromiseWait;
    EngineOperationJournal* recordingJournal = nullptr;
    const EngineOperationJournal* replayJournal = nullptr;
    std::size_t replayJournalCursor = 0;
    bool insidePersistentCallback = false;
    bool insideStateProviderCallback = false;
    bool acceptingPromiseWait = false;
    std::uint64_t nextActionHandle = 1;
    std::uint64_t nextRealmNamespace = 1;
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

bool JavaScriptHost::RunModuleFile(const std::string& vfsPath) {
    if (!m_impl->Ready()) return false;
    const auto source = m_impl->moduleSources.find(vfsPath);
    if (source == m_impl->moduleSources.end()) {
        m_impl->Error(vfsPath, "JavaScript module was not declared by its extension");
        return false;
    }
    m_impl->BeginExecution();
    JSValue result = JS_Eval(m_impl->context, source->second.data(),
                             source->second.size(), vfsPath.c_str(),
                             JS_EVAL_TYPE_MODULE);
    m_impl->EndExecution();
    if (JS_IsException(result)) {
        m_impl->Error(vfsPath, m_impl->Exception());
        JS_FreeValue(m_impl->context, result);
        return false;
    }

    int jobs = 0;
    while (JS_IsJobPending(m_impl->runtime) && jobs++ < 64) {
        JSContext* jobContext = nullptr;
        m_impl->BeginExecution();
        const int executed = JS_ExecutePendingJob(m_impl->runtime, &jobContext);
        m_impl->EndExecution();
        if (executed < 0) {
            m_impl->Error(vfsPath, m_impl->Exception());
            JS_FreeValue(m_impl->context, result);
            return false;
        }
    }
    if (JS_IsJobPending(m_impl->runtime)) {
        m_impl->Error(vfsPath,
                      "extension module exceeded the pending-job budget during initialization");
        JS_FreeValue(m_impl->context, result);
        return false;
    }
    if (JS_IsPromise(result) &&
        JS_PromiseState(m_impl->context, result) != JS_PROMISE_FULFILLED) {
        const auto details = JS_PromiseState(m_impl->context, result) ==
                                     JS_PROMISE_REJECTED
                                 ? m_impl->PromiseFailure(result)
                                 : std::string("extension module initialization did not settle");
        m_impl->Error(vfsPath, details);
        JS_FreeValue(m_impl->context, result);
        return false;
    }
    JS_FreeValue(m_impl->context, result);
    return true;
}

bool JavaScriptHost::LoadExtensionManifest(const std::string& manifestPath) {
    if (m_impl->isExtensionRealm)
        return LoadExtensionManifestIntoCurrentRealm(manifestPath);

    auto candidate = std::make_unique<JavaScriptHost>(m_impl->services);
    candidate->m_impl->isExtensionRealm = true;
    candidate->m_impl->eventRoot = this;
    if (m_impl->nextRealmNamespace > 0xffffU) {
        m_impl->Error(manifestPath, "extension realm namespace limit was exceeded");
        return false;
    }
    candidate->m_impl->nextActionHandle =
        (m_impl->nextRealmNamespace++ << 48U) | 1U;
    if (!candidate->LoadExtensionManifestIntoCurrentRealm(manifestPath))
        return false;

    const std::string id = candidate->m_impl->extensionId;
    const auto current = std::ranges::find_if(
        m_impl->extensionRealms, [&](const auto& realm) {
            return realm->m_impl->extensionId == id;
        });
    if (current == m_impl->extensionRealms.end()) {
        m_impl->extensionRealms.push_back(std::move(candidate));
    } else {
        // The candidate has already atomically replaced both descriptor
        // sources. Prevent the retired realm from removing the new source.
        (*current)->m_impl->ownsRegistrations = false;
        *current = std::move(candidate);
    }
    return true;
}

bool JavaScriptHost::LoadExtensionManifestIntoCurrentRealm(
    const std::string& manifestPath) {
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
    bool stagedCommandSource = false;
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
        if (stagedCommandSource && !stagedExtensionId.empty()) {
            (void)vn::CommandRegistry::Global().RemoveSource(stagedExtensionId);
            m_impl->loadedCommandSources.erase(stagedExtensionId);
        }
    };
    try {
        if (json.value("format", std::string{}) != "PrismatiXExtension" ||
            json.value("schemaRevision", 0) != 2 ||
            Lower(json.value("language", std::string{})) != "javascript") {
            m_impl->Error(manifestPath,
                          "extension manifest must be PrismatiXExtension schemaRevision 2 with language javascript");
            return false;
        }
        const std::string id = json.at("id").get<std::string>();
        const std::string version = json.at("version").get<std::string>();
        const std::string requiredEngineVersion =
            json.at("requiredEngineVersion").get<std::string>();
        const std::string entry = json.at("entry").get<std::string>();
        if (id.empty() || !semver::Parse(version) ||
            !SafeRelativePath(entry) || !entry.ends_with(".js")) {
            m_impl->Error(manifestPath, "extension id or JavaScript entry is unsafe");
            return false;
        }
        const auto engineCompatible = semver::Satisfies(
            semver::Version{0, 2, 0}, requiredEngineVersion);
        if (!engineCompatible)
            throw std::invalid_argument("requiredEngineVersion is not a supported SemVer range");
        if (!*engineCompatible)
            throw std::invalid_argument(
                "extension requires engine '" + requiredEngineVersion +
                "' but this Player is 0.2.0");
        stagedExtensionId = id;
        m_impl->extensionId = id;

        const auto manifestSeparator = manifestPath.find_last_of('/');
        m_impl->moduleRoot = manifestSeparator == std::string::npos
                                 ? std::string{}
                                 : manifestPath.substr(0, manifestSeparator);
        const auto resolvedEntry = ManifestRelativePath(manifestPath, entry);
        if (!resolvedEntry)
            throw std::invalid_argument("extension entry cannot be resolved inside its package");
        std::vector<std::string> declaredModules{entry};
        const auto modules = json.find("modules");
        if (modules != json.end()) {
            if (!modules->is_array() || modules->size() > 1024)
                throw std::invalid_argument("extension modules must be a bounded array");
            for (const auto& module : *modules) {
                if (!module.is_string())
                    throw std::invalid_argument("extension module path must be a string");
                declaredModules.push_back(module.get<std::string>());
            }
        }
        std::unordered_set<std::string> uniqueModulePaths;
        std::size_t moduleBytes = 0;
        for (const auto& module : declaredModules) {
            if (!SafeRelativePath(module) || !module.ends_with(".js"))
                throw std::invalid_argument("extension module path is unsafe: " + module);
            const auto resolved = ManifestRelativePath(manifestPath, module);
            if (!resolved || !uniqueModulePaths.insert(*resolved).second) {
                if (resolved && *resolved == *resolvedEntry) continue;
                throw std::invalid_argument("extension module path is duplicated or cannot be resolved: " + module);
            }
            const auto moduleSource = m_impl->services.vfs->ReadText(*resolved);
            if (!moduleSource)
                throw std::invalid_argument("extension module is missing: " + module);
            if (moduleSource->size() > 4U * 1024U * 1024U ||
                moduleBytes > 16U * 1024U * 1024U - moduleSource->size())
                throw std::invalid_argument("extension module source budget was exceeded");
            moduleBytes += moduleSource->size();
            m_impl->moduleSources.emplace(*resolved, *moduleSource);
        }
        std::unordered_set<std::string> capabilities;
        for (const auto& capability : json.value("capabilities", nlohmann::json::array())) {
            if (!capability.is_string()) throw std::invalid_argument("capability must be a string");
            const auto name = capability.get<std::string>();
            if (!SupportedCapability(name) || !capabilities.insert(name).second)
                throw std::invalid_argument("unsupported or duplicate capability: " + name);
        }
        m_impl->capabilities = capabilities;

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
            const ManifestSafety safety = ReadManifestSafety(encoded, json);
            descriptor.previewSafe = safety.previewSafe;
            descriptor.deterministic = safety.deterministic;
            descriptor.seekSafe = safety.seekSafe;
            descriptor.rollbackSafe = safety.rollbackSafe;
            std::unordered_set<std::string> commandCapabilities;
            for (const auto& capability :
                 encoded.value("capabilities", nlohmann::json::array())) {
                if (!capability.is_string())
                    throw std::invalid_argument(
                        "command capability must be a string");
                const auto name = capability.get<std::string>();
                if (!capabilities.contains(name) ||
                    !commandCapabilities.insert(name).second)
                    throw std::invalid_argument(
                        "command capability is undeclared or duplicated: " + name);
            }
            if (descriptor.id.empty() || !commandIds.insert(descriptor.id).second ||
                (vn::CommandRegistry::Global().Find(descriptor.id) &&
                 vn::CommandRegistry::Global().Find(descriptor.id)->sourceId != id))
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
                if (encodedParameter.contains("default")) {
                    if (encodedParameter["default"].is_null()) {
                        if (*type == VariantType::Null) parameter.hasDefault = true;
                    } else {
                        const auto value = JsonVariant(encodedParameter["default"], *type);
                        if (!value) throw std::invalid_argument("command parameter default type mismatch");
                        parameter.defaultValue = value->Clone();
                        parameter.hasDefault = true;
                    }
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
                const auto hint = ManifestCommandEditorHint(
                    encodedParameter.value("editorHint", std::string{}));
                if (!hint) throw std::invalid_argument("unknown command editor hint");
                parameter.widget = *hint;
                if (!parameter.options.empty() &&
                    parameter.widget == vn::CommandEditorWidget::Default)
                    parameter.widget = vn::CommandEditorWidget::Enum;
                if (parameter.type == VariantType::ResourceRef &&
                    parameter.widget == vn::CommandEditorWidget::Default)
                    parameter.widget = vn::CommandEditorWidget::Resource;
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
            const ManifestSafety safety = ReadManifestSafety(encoded, json);
            descriptor.previewSafe = safety.previewSafe;
            descriptor.deterministic = safety.deterministic;
            descriptor.seekSafe = safety.seekSafe;
            descriptor.rollbackSafe = safety.rollbackSafe;
            descriptor.destructiveInPreview = !safety.previewSafe;
            descriptor.allowAdditionalArguments = false;
            const auto reentry = ui::ParseActionReentryPolicy(
                encoded.value("reentry", std::string("allow")));
            if (descriptor.id.empty() || !reentry || !actionIds.insert(descriptor.id).second)
                throw std::invalid_argument("empty, duplicate, or invalid action id: " + descriptor.id);
            descriptor.reentryPolicy = *reentry;
            std::unordered_set<std::string> actionCapabilities;
            for (const auto& capability :
                 encoded.value("capabilities", nlohmann::json::array())) {
                if (!capability.is_string())
                    throw std::invalid_argument("action capability must be a string");
                const auto name = capability.get<std::string>();
                if (!capabilities.contains(name) ||
                    !actionCapabilities.insert(name).second)
                    throw std::invalid_argument(
                        "action capability is undeclared or duplicated: " + name);
                descriptor.capabilities.push_back(name);
            }
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
                const auto hint = ManifestActionEditorHint(
                    encodedParameter.value("editorHint", std::string{}));
                if (!hint) throw std::invalid_argument("unknown action editor hint");
                parameter.editorHint = *hint;
                if (!parameter.enumValues.empty() &&
                    parameter.editorHint == ui::ActionEditorHint::Default)
                    parameter.editorHint = ui::ActionEditorHint::Enum;
                if (parameter.type == VariantType::ResourceRef &&
                    parameter.editorHint == ui::ActionEditorHint::Default)
                    parameter.editorHint = ui::ActionEditorHint::Resource;
                if (!ActionHintMatchesType(parameter.editorHint, parameter.type,
                                           !parameter.enumValues.empty()))
                    throw std::invalid_argument(
                        "action editor hint does not match parameter type");
                if (!parameter.resourceType.empty() &&
                    parameter.type != VariantType::ResourceRef)
                    throw std::invalid_argument(
                        "action resource filter requires resource parameter type");
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
        for (const auto& descriptor : actions) {
            if (const auto* current = ui::ActionCatalog::Global().Find(descriptor.id);
                current && !(current->origin == ui::ActionOrigin::ScriptExtension &&
                             current->sourceId == id))
                throw std::invalid_argument("action id conflicts with another source: " + descriptor.id);
        }

        stagedCommandIds = commandIds;
        stagedActionIds = actionIds;
        m_impl->declaredCommands.insert(commandIds.begin(), commandIds.end());
        m_impl->declaredActions.insert(actionIds.begin(), actionIds.end());
        m_impl->activeExtension = id;
        const std::string scriptPath = *resolvedEntry;
        const bool loaded = RunModuleFile(scriptPath);
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
        const Status commandStatus =
            vn::CommandRegistry::Global().ReplaceSource(id, std::move(commands));
        if (!commandStatus)
            throw std::invalid_argument("command registry rejected extension descriptors");
        stagedCommandSource = true;
        m_impl->loadedCommandSources.insert(id);
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

Status JavaScriptHost::DispatchEventInCurrentRealm(
    const std::string& event, const EventPayload& payloadValue) {
    if (!m_impl->Ready())
        return Status::Fail(ScriptDiagnostic(
            "PXJS7200", "JavaScript event realm is unavailable", event));
    const auto found = m_impl->events.find(event);
    if (found != m_impl->events.end()) {
        JSValue payload = m_impl->ToJavaScript(payloadValue);
        for (const auto& callback : found->second) {
            m_impl->BeginExecution();
            JSValue result = JS_Call(m_impl->context, callback, JS_UNDEFINED, 1, &payload);
            m_impl->EndExecution();
            if (JS_IsException(result)) {
                const auto details = m_impl->Exception();
                m_impl->Error("event:" + event, details);
                JS_FreeValue(m_impl->context, result);
                JS_FreeValue(m_impl->context, payload);
                return Status::Fail(ScriptDiagnostic(
                    "PXJS7202", "JavaScript event handler failed", details,
                    m_impl->extensionId));
            }
            if (JS_IsPromise(result)) {
                int jobs = 0;
                while (JS_IsJobPending(m_impl->runtime) && jobs++ < 64) {
                    JSContext* jobContext = nullptr;
                    m_impl->BeginExecution();
                    const int executed =
                        JS_ExecutePendingJob(m_impl->runtime, &jobContext);
                    m_impl->EndExecution();
                    if (executed < 0) break;
                }
                const auto promiseState = JS_PromiseState(m_impl->context, result);
                if (promiseState != JS_PROMISE_FULFILLED) {
                    const auto details = promiseState == JS_PROMISE_REJECTED
                                             ? m_impl->PromiseFailure(result)
                                             : std::string(
                                                   "event Promise exceeded the pending-job budget or awaited a non-event operation");
                    m_impl->Error("event:" + event, details);
                    JS_FreeValue(m_impl->context, result);
                    JS_FreeValue(m_impl->context, payload);
                    return Status::Fail(ScriptDiagnostic(
                        "PXJS7203", "JavaScript event Promise failed", details,
                        m_impl->extensionId));
                }
            }
            JS_FreeValue(m_impl->context, result);
        }
        JS_FreeValue(m_impl->context, payload);
    }
    return Status::Ok();
}

Status JavaScriptHost::Emit(const std::string& event,
                            const EventPayload& payload) {
    if (m_impl->eventRoot && m_impl->eventRoot != this)
        return m_impl->eventRoot->Emit(event, payload);
    if (event.empty())
        return Status::Fail(ScriptDiagnostic(
            "PXJS7204", "Event id cannot be empty"));
    if (!IsJsonValue(payload))
        return Status::Fail(ScriptDiagnostic(
            "PXJS7205", "Event payload is not a recursive JSON value", event));

    constexpr std::uint32_t kMaxEventDepth = 32;
    constexpr std::size_t kEventDispatchBudget = 256;
    const std::uint32_t depth =
        m_impl->dispatchingEvents ? m_impl->currentEventDepth + 1 : 0;
    if (depth > kMaxEventDepth)
        return Status::Fail(ScriptDiagnostic(
            "PXJS7206", "Event recursion depth exceeded", event));
    m_impl->eventQueue.push_back(
        {.name = event, .payload = payload.Clone(), .depth = depth});
    if (m_impl->dispatchingEvents) return Status::Ok();

    m_impl->dispatchingEvents = true;
    Status status;
    const auto merge = [&](const Status& next) {
        for (const auto& diagnostic : next.Diagnostics()) status.Add(diagnostic);
    };
    std::size_t dispatched = 0;
    while (!m_impl->eventQueue.empty() && dispatched++ < kEventDispatchBudget) {
        auto current = std::move(m_impl->eventQueue.front());
        m_impl->eventQueue.pop_front();
        m_impl->currentEventDepth = current.depth;
        merge(DispatchEventInCurrentRealm(current.name, current.payload));
        if (!status) break;
        for (const auto& realm : m_impl->extensionRealms) {
            merge(realm->DispatchEventInCurrentRealm(
                current.name, current.payload));
            if (!status) break;
        }
        if (!status) break;
    }
    if (status && !m_impl->eventQueue.empty())
        status.Add(ScriptDiagnostic(
            "PXJS7207", "Event dispatch budget exceeded",
            m_impl->eventQueue.front().name));
    if (!status) m_impl->eventQueue.clear();
    m_impl->currentEventDepth = 0;
    m_impl->dispatchingEvents = false;
    return status;
}

bool JavaScriptHost::InvokeCommand(const vn::Command& command) {
    const auto found = m_impl->commands.find(command.type);
    if (found == m_impl->commands.end()) {
        for (const auto& realm : m_impl->extensionRealms)
            if (realm->m_impl->commands.contains(command.type))
                return realm->InvokeCommand(command);
        return false;
    }
    JSValue arguments = m_impl->CommandArguments(command);
    EngineOperationJournal journal;
    Impl::JournalScope journalScope(*m_impl, journal);
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
            pending.journal = std::move(journal);
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
            pending.journal = std::move(journal);
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
    if (m_impl->actions.contains(std::string(action))) return true;
    return std::ranges::any_of(m_impl->extensionRealms, [&](const auto& realm) {
        return realm->HasAction(action);
    });
}

Status JavaScriptHost::InvokeAction(const ui::ActionInvocation& invocation) {
    return StartAction(invocation).status;
}

ui::ProviderActionStart JavaScriptHost::StartAction(
    const ui::ActionInvocation& invocation) {
    const auto found = m_impl->actions.find(invocation.action);
    if (found == m_impl->actions.end()) {
        for (const auto& realm : m_impl->extensionRealms)
            if (realm->HasAction(invocation.action))
                return realm->StartAction(invocation);
        return {.status = Status::Fail(ScriptDiagnostic(
                    "PXJS7420", "JavaScript action callback is missing",
                    invocation.action))};
    }
    auto [arguments, actionContext] = m_impl->ActionArguments(invocation);
    JSValue parameters[] = {arguments, actionContext};
    EngineOperationJournal journal;
    Impl::JournalScope journalScope(*m_impl, journal);
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
            pending.journal = std::move(journal);
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
            pending.journal = std::move(journal);
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
    for (const auto& realm : m_impl->extensionRealms) {
        const auto state = realm->ActionState(handle);
        if (state != ui::ActionExecutionState::Unknown) return state;
    }
    return ui::ActionExecutionState::Unknown;
}

void JavaScriptHost::CancelAction(const std::uint64_t handle) {
    const auto found = std::ranges::find_if(
        m_impl->pendingActions,
        [handle](const auto& pending) { return pending.id == handle; });
    const bool local = found != m_impl->pendingActions.end();
    if (local) {
        m_impl->FreeContinuation(*found);
        m_impl->pendingActions.erase(found);
        m_impl->actionTerminalStates[handle] =
            ui::ActionExecutionState::Cancelled;
        return;
    }
    for (const auto& realm : m_impl->extensionRealms)
        if (realm->ActionState(handle) != ui::ActionExecutionState::Unknown) {
            realm->CancelAction(handle);
            return;
        }
}

void JavaScriptHost::Update(const float deltaSeconds) {
    if (!m_impl->runtime) return;

    for (const auto& realm : m_impl->extensionRealms)
        realm->Update(deltaSeconds);

    const auto ready = [this, deltaSeconds](Impl::WaitToken& wait) {
        if (wait.kind == "debug") return false;
        if (wait.kind == "debug-resume") return true;
        if (wait.kind == "animation")
            return !m_impl->services.timeline ||
                   !m_impl->services.timeline->Playing(wait.handle);
        if (wait.kind == "screen-effect")
            return !m_impl->services.renderer ||
                   !m_impl->services.renderer->ScreenEffectPlaying(wait.handle);
        if (wait.kind == "video") {
            if (!m_impl->services.videoStatus) return true;
            const std::string state = m_impl->services.videoStatus(wait.handle);
            return state != "playing" && state != "paused";
        }
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
        Impl::JournalScope journalScope(*m_impl, iterator->journal);
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
        Impl::JournalScope journalScope(*m_impl, iterator->journal);
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
    return !m_impl->pendingCommands.empty() ||
           std::ranges::any_of(m_impl->extensionRealms, [](const auto& realm) {
               return realm->HasPendingCommand();
           });
}

bool JavaScriptHost::HasPendingAction() const {
    return !m_impl->pendingActions.empty() ||
           std::ranges::any_of(m_impl->extensionRealms, [](const auto& realm) {
               return realm->HasPendingAction();
           });
}

PendingCommandsState JavaScriptHost::CapturePending() const {
    PendingCommandsState state;
    state.reserve(m_impl->pendingCommands.size());
    for (const auto& pending : m_impl->pendingCommands) {
        state.push_back({.sourceId = m_impl->extensionId,
                         .command = pending.command,
                         .yieldIndex = pending.yieldIndex,
                         .waitKind = pending.wait.kind,
                         .handle = pending.wait.handle,
                         .remainingSeconds = pending.wait.remainingSeconds,
                         .journal = pending.journal});
    }
    for (const auto& realm : m_impl->extensionRealms) {
        auto child = realm->CapturePending();
        state.insert(state.end(), std::make_move_iterator(child.begin()),
                     std::make_move_iterator(child.end()));
    }
    return state;
}

PendingActionsState JavaScriptHost::CapturePendingActions() const {
    PendingActionsState state;
    state.reserve(m_impl->pendingActions.size());
    for (const auto& pending : m_impl->pendingActions) {
        state.push_back({.sourceId = m_impl->extensionId,
                         .id = pending.id,
                         .invocation = pending.invocation,
                         .yieldIndex = pending.yieldIndex,
                         .waitKind = pending.wait.kind,
                         .handle = pending.wait.handle,
                         .remainingSeconds = pending.wait.remainingSeconds,
                         .journal = pending.journal});
    }
    for (const auto& realm : m_impl->extensionRealms) {
        auto child = realm->CapturePendingActions();
        state.insert(state.end(), std::make_move_iterator(child.begin()),
                     std::make_move_iterator(child.end()));
    }
    return state;
}

Status JavaScriptHost::RestorePending(const PendingCommandsState& state) {
    if (m_impl->isExtensionRealm)
        return RestorePendingInCurrentRealm(state);

    PendingCommandsState local;
    std::unordered_map<JavaScriptHost*, PendingCommandsState> grouped;
    for (const auto& saved : state) {
        if (saved.sourceId.empty()) {
            local.push_back(saved);
            continue;
        }
        const auto realm = std::ranges::find_if(
            m_impl->extensionRealms, [&](const auto& candidate) {
                return candidate->m_impl->extensionId == saved.sourceId;
            });
        if (realm == m_impl->extensionRealms.end())
            return Status::Fail(ScriptDiagnostic(
                "PXJS7505", "Saved JavaScript extension realm is unavailable",
                saved.sourceId));
        grouped[realm->get()].push_back(saved);
    }
    Status status = RestorePendingInCurrentRealm(local);
    if (!status) return status;
    for (const auto& realm : m_impl->extensionRealms) {
        status = realm->RestorePendingInCurrentRealm(grouped[realm.get()]);
        if (!status) return status;
    }
    return Status::Ok();
}

Status JavaScriptHost::RestorePendingInCurrentRealm(
    const PendingCommandsState& state) {
    std::vector<Impl::PendingCommandContinuation> candidate;
    candidate.reserve(state.size());

    const auto fail = [this, &candidate](std::string code, std::string message,
                             std::string details) {
        for (auto& pending : candidate)
            m_impl->FreeContinuation(pending);
        candidate.clear();
        return Status::Fail(ScriptDiagnostic(
            std::move(code), std::move(message), std::move(details)));
    };

    for (const auto& saved : state) {
        const bool validTimer = saved.waitKind != "timer" ||
                                (std::isfinite(saved.remainingSeconds) &&
                                 saved.remainingSeconds >= 0.0f);
        if (saved.yieldIndex == 0 || !validTimer ||
            (saved.waitKind != "timer" && saved.waitKind != "animation" &&
             saved.waitKind != "screen-effect" && saved.waitKind != "video")) {
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
            "restore-command:" + saved.command.type, saved.journal);
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
                        .handle = saved.waitKind == "video"
                                      ? replayed->wait.handle
                                      : saved.handle,
                        .remainingSeconds = saved.remainingSeconds};
        pending.journal = saved.journal;
        candidate.push_back(std::move(pending));
    }
    for (auto& pending : m_impl->pendingCommands)
        m_impl->FreeContinuation(pending);
    m_impl->pendingCommands = std::move(candidate);
    return Status::Ok();
}

Status JavaScriptHost::RestorePendingActions(const PendingActionsState& state) {
    if (m_impl->isExtensionRealm)
        return RestorePendingActionsInCurrentRealm(state);

    PendingActionsState local;
    std::unordered_map<JavaScriptHost*, PendingActionsState> grouped;
    for (const auto& saved : state) {
        if (saved.sourceId.empty()) {
            local.push_back(saved);
            continue;
        }
        const auto realm = std::ranges::find_if(
            m_impl->extensionRealms, [&](const auto& candidate) {
                return candidate->m_impl->extensionId == saved.sourceId;
            });
        if (realm == m_impl->extensionRealms.end())
            return Status::Fail(ScriptDiagnostic(
                "PXJS7514", "Saved JavaScript Action realm is unavailable",
                saved.sourceId));
        grouped[realm->get()].push_back(saved);
    }
    Status status = RestorePendingActionsInCurrentRealm(local);
    if (!status) return status;
    for (const auto& realm : m_impl->extensionRealms) {
        status = realm->RestorePendingActionsInCurrentRealm(grouped[realm.get()]);
        if (!status) return status;
    }
    return Status::Ok();
}

Status JavaScriptHost::RestorePendingActionsInCurrentRealm(
    const PendingActionsState& state) {
    std::vector<Impl::PendingActionContinuation> candidate;
    candidate.reserve(state.size());

    const auto fail = [this, &candidate](std::string code, std::string message,
                             std::string details) {
        for (auto& pending : candidate)
            m_impl->FreeContinuation(pending);
        candidate.clear();
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
            (saved.waitKind != "timer" && saved.waitKind != "animation" &&
             saved.waitKind != "screen-effect" && saved.waitKind != "video")) {
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
            "restore-action:" + saved.invocation.action, saved.journal);
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
                        .handle = saved.waitKind == "video"
                                      ? replayed->wait.handle
                                      : saved.handle,
                        .remainingSeconds = saved.remainingSeconds};
        pending.journal = saved.journal;
        candidate.push_back(std::move(pending));
    }
    for (auto& pending : m_impl->pendingActions)
        m_impl->FreeContinuation(pending);
    m_impl->pendingActions = std::move(candidate);
    m_impl->actionTerminalStates.clear();
    for (const auto& pending : m_impl->pendingActions)
        m_impl->nextActionHandle =
            std::max(m_impl->nextActionHandle, pending.id + 1);
    return Status::Ok();
}

Result<ExtensionStates> JavaScriptHost::CaptureExtensionStateInCurrentRealm() {
    return m_impl->CaptureStateProviders();
}

Result<ExtensionStates> JavaScriptHost::CaptureExtensionState() {
    ExtensionStates combined;
    auto append = [&combined](Result<ExtensionStates> captured)
        -> std::optional<std::vector<diag::Diagnostic>> {
        if (!captured) return captured.Diagnostics();
        auto values = captured.TakeValue();
        combined.insert(combined.end(),
                        std::make_move_iterator(values.begin()),
                        std::make_move_iterator(values.end()));
        return std::nullopt;
    };
    if (auto failure = append(CaptureExtensionStateInCurrentRealm()))
        return Result<ExtensionStates>::Failure(std::move(*failure));
    for (const auto& realm : m_impl->extensionRealms) {
        if (auto failure = append(realm->CaptureExtensionStateInCurrentRealm()))
            return Result<ExtensionStates>::Failure(std::move(*failure));
    }
    std::ranges::sort(combined, [](const auto& left, const auto& right) {
        return std::tie(left.sourceId, left.providerId) <
               std::tie(right.sourceId, right.providerId);
    });
    return Result<ExtensionStates>::Success(std::move(combined));
}

Status JavaScriptHost::RestoreExtensionStateInCurrentRealm(
    const ExtensionStates& state) {
    return m_impl->RestoreStateProviders(state);
}

Status JavaScriptHost::RestoreExtensionStateUnchecked(
    const ExtensionStates& state) {
    ExtensionStates local;
    std::unordered_map<JavaScriptHost*, ExtensionStates> grouped;
    std::set<std::pair<std::string, std::string>> identities;
    for (const auto& saved : state) {
        if (!identities.emplace(saved.sourceId, saved.providerId).second)
            return Status::Fail(ScriptDiagnostic(
                "PXJS7620",
                "Extension state checkpoint contains duplicate providers",
                saved.sourceId + ":" + saved.providerId));
        if (saved.sourceId.empty()) {
            local.push_back(saved);
            continue;
        }
        const auto realm = std::ranges::find_if(
            m_impl->extensionRealms, [&](const auto& candidate) {
                return candidate->m_impl->extensionId == saved.sourceId;
            });
        if (realm == m_impl->extensionRealms.end())
            return Status::Fail(ScriptDiagnostic(
                "PXJS7621", "Saved extension state realm is unavailable",
                saved.sourceId));
        grouped[realm->get()].push_back(saved);
    }
    if (Status restored = RestoreExtensionStateInCurrentRealm(local); !restored)
        return restored;
    for (const auto& realm : m_impl->extensionRealms) {
        if (Status restored = realm->RestoreExtensionStateInCurrentRealm(
                grouped[realm.get()]);
            !restored)
            return restored;
    }
    return Status::Ok();
}

Status JavaScriptHost::RestoreExtensionState(const ExtensionStates& state) {
    auto previous = CaptureExtensionState();
    if (!previous)
        return Status::Fail(previous.Diagnostics());
    const Status restored = RestoreExtensionStateUnchecked(state);
    if (restored) return restored;
    if (const Status rolledBack =
            RestoreExtensionStateUnchecked(previous.Value());
        !rolledBack) {
        diag::Emit(diag::Diagnostic{
            .severity = diag::Severity::Fatal,
            .code = "PXJS7622",
            .category = "Script.Restore",
            .message =
                "Extension state rollback could not recover the active realms"});
    }
    return restored;
}

void JavaScriptHost::CancelPending() {
    for (const auto& realm : m_impl->extensionRealms) realm->CancelPending();
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
    for (const auto& realm : m_impl->extensionRealms)
        (void)realm->SetDebugBreakpoints(accepted);
    return accepted;
}

bool JavaScriptHost::DebugPause() {
    bool armed = false;
    if (!m_impl->debug.paused) {
        m_impl->debugPauseRequested = true;
        armed = true;
    }
    for (const auto& realm : m_impl->extensionRealms)
        armed = realm->DebugPause() || armed;
    return armed;
}

bool JavaScriptHost::DebugContinue() {
    if (!m_impl->debug.paused) {
        for (const auto& realm : m_impl->extensionRealms)
            if (realm->CaptureDebugState().paused)
                return realm->DebugContinue();
        return false;
    }
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
    if (!m_impl->debug.paused) {
        for (const auto& realm : m_impl->extensionRealms)
            if (realm->CaptureDebugState().paused)
                return realm->DebugStep();
        return false;
    }
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
    if (!m_impl->debug.paused || JS_IsUndefined(m_impl->debugLocals)) {
        for (const auto& realm : m_impl->extensionRealms)
            if (auto result = realm->EvaluateDebugWatch(expression)) return result;
        return std::nullopt;
    }
    if (expression.empty()) return std::nullopt;

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
    for (const auto& realm : m_impl->extensionRealms)
        if (realm->CaptureDebugState().paused) return realm->CaptureDebugState();
    return m_impl->debug;
}

}  // namespace px::script
