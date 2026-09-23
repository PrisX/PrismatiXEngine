#include "Engine/UI/Binding.h"
#include "Engine/Core/IndexedPropertyPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace px::ui {
namespace {
diag::Diagnostic MakeFailure(std::string code, std::string message, std::string path = {}) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error, .code = std::move(code),
                                .category = "UI.Binding", .message = std::move(message)};
    diagnostic.source.property = std::move(path);
    diag::Emit(diagnostic);
    return diagnostic;
}
Status Failure(std::string code, std::string message, std::string path = {}) {
    return Status::Fail(MakeFailure(std::move(code), std::move(message), std::move(path)));
}
template <typename T>
Result<T> ResultFailure(std::string code, std::string message, std::string path = {}) {
    return Result<T>::Failure(MakeFailure(std::move(code), std::move(message), std::move(path)));
}

Result<Variant> FormatBoolVisibility(const Variant& value) {
    if (const auto* boolean = value.TryGet<bool>()) return Result<Variant>::Success(Variant(*boolean ? "Visible" : "Collapsed"));
    return ResultFailure<Variant>("PXUI2101", "BoolToVisibility received a non-boolean value");
}

template <typename Value>
Value* NestedValue(Value* value,
                   const std::vector<IndexedPropertyPathSegment>& segments,
                   const std::size_t start) {
    for (std::size_t index = start; index < segments.size(); ++index) {
        const auto& segment = segments[index];
        if (segment.kind == IndexedPropertyPathSegment::Kind::Field) {
            auto* object = value->AsObject();
            if (!object) return nullptr;
            const auto found = object->find(std::string(segment.field));
            if (found == object->end()) return nullptr;
            value = &found->second;
        } else {
            auto* array = value->AsArray();
            if (!array || segment.index >= array->size()) return nullptr;
            value = &(*array)[segment.index];
        }
    }
    return value;
}

bool IsDescendantPath(const std::string_view path, const std::string_view root) {
    return path.size() > root.size() && path.substr(0, root.size()) == root &&
           (path[root.size()] == '.' || path[root.size()] == '[');
}
}

bool IsValidPropertyPath(std::string_view path) {
    if (path.empty()) return false;
    bool segmentStart = true;
    for (char c : path) {
        if (c == '.') { if (segmentStart) return false; segmentStart = true; continue; }
        const unsigned char ch = static_cast<unsigned char>(c);
        if (segmentStart) {
            if (!(std::isalpha(ch) || c == '_')) return false;
            segmentStart = false;
        } else if (!(std::isalnum(ch) || c == '_')) return false;
    }
    return !segmentStart;
}

Status ObservableViewModel::Define(std::string path, Variant initialValue, bool writable) {
    if (!IsValidPropertyPath(path)) return Failure("PXUI2102", "Invalid typed property path", path);
    if (m_entries.contains(path)) return Failure("PXUI2103", "Property path is already defined", path);
    m_entries.emplace(std::move(path), Entry{.type = initialValue.Type(), .value = std::move(initialValue), .writable = writable});
    return Status::Ok();
}

Status ObservableViewModel::DefineComputed(std::string path, VariantType type,
                                           std::vector<std::string> dependencies, Compute compute) {
    if (!compute) return Failure("PXUI2104", "Computed property has no compute function", path);
    for (const auto& dependency : dependencies) if (!m_entries.contains(dependency))
        return Failure("PXUI2105", "Computed property dependency does not exist: " + dependency, path);
    auto initial = compute(*this);
    if (!initial) return Status::Fail(initial.Diagnostics());
    if (initial.Value().Type() != type) return Failure("PXUI2106", "Computed property returned the wrong type", path);
    if (!IsValidPropertyPath(path) || m_entries.contains(path)) return Failure("PXUI2107", "Invalid or duplicate computed property", path);
    m_entries.emplace(std::move(path), Entry{.type = type, .value = std::move(initial.Value()), .writable = false,
                                             .dependencies = std::move(dependencies), .compute = std::move(compute)});
    return Status::Ok();
}

std::optional<ObservableViewModel::ResolvedPath> ObservableViewModel::ResolvePath(
    const std::string_view path) const {
    if (const auto it = m_entries.find(std::string(path)); it != m_entries.end())
        return ResolvedPath{it->first, &it->second, &it->second.value, 0};
    const auto segments = ParseIndexedPropertyPath(path);
    if (!segments) return std::nullopt;
    std::string prefix;
    std::optional<ResolvedPath> result;
    for (std::size_t index = 0; index < segments->size(); ++index) {
        const auto& segment = (*segments)[index];
        if (segment.kind != IndexedPropertyPathSegment::Kind::Field) break;
        if (!prefix.empty()) prefix += '.';
        prefix += segment.field;
        const auto found = m_entries.find(prefix);
        if (found == m_entries.end()) continue;
        if (const auto* value = NestedValue(&found->second.value, *segments, index + 1))
            result = ResolvedPath{found->first, &found->second, value, index + 1};
    }
    return result;
}

std::optional<PropertyPathInfo> ObservableViewModel::Describe(std::string_view path) const {
    const auto resolved = ResolvePath(path);
    if (!resolved) return std::nullopt;
    return PropertyPathInfo{std::string(path), resolved->value->Type(),
                            resolved->root->writable};
}

std::vector<PropertyPathInfo> ObservableViewModel::EnumerateProperties() const {
    std::vector<PropertyPathInfo> result;
    result.reserve(m_entries.size());
    for (const auto& [path, entry] : m_entries) {
        result.push_back({path, entry.type, entry.writable});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) { return left.path < right.path; });
    return result;
}

Result<Variant> ObservableViewModel::Read(std::string_view path) const {
    if (const auto resolved = ResolvePath(path))
        return Result<Variant>::Success(*resolved->value);
    return ResultFailure<Variant>("PXUI2108", "Unknown ViewModel property path", std::string(path));
}

Status ObservableViewModel::Write(std::string_view path, const Variant& value) {
    const auto resolved = ResolvePath(path);
    if (!resolved) return Failure("PXUI2108", "Unknown ViewModel property path", std::string(path));
    if (!resolved->root->writable) return Failure("PXUI2109", "ViewModel property is read-only", std::string(path));
    if (resolved->value->Type() != value.Type()) return Failure("PXUI2110", "ViewModel property type mismatch", std::string(path));
    if (*resolved->value == value) return Status::Ok();
    auto root = m_entries.find(resolved->rootPath);
    if (resolved->firstNested == 0) {
        const auto rootSegments = ParseIndexedPropertyPath(path);
        for (const auto& subscription : m_subscriptions) {
            if (!IsDescendantPath(subscription.path, path)) continue;
            const auto segments = ParseIndexedPropertyPath(subscription.path);
            const auto* replacement = rootSegments && segments
                ? NestedValue(&value, *segments, rootSegments->size()) : nullptr;
            if (!replacement)
                return Failure("PXUI2124", "ViewModel replacement would invalidate an active binding",
                               subscription.path);
            if (const auto previous = Read(subscription.path);
                previous && previous.Value().Type() != replacement->Type())
                return Failure("PXUI2110", "ViewModel replacement would change a bound property type",
                               subscription.path);
        }
        root->second.value = value;
    }
    else {
        const auto segments = ParseIndexedPropertyPath(path);
        if (!segments) return Failure("PXUI2108", "Unknown ViewModel property path", std::string(path));
        Variant updated = root->second.value.Clone();
        auto* nested = NestedValue(&updated, *segments, resolved->firstNested);
        if (!nested) return Failure("PXUI2108", "Unknown ViewModel property path", std::string(path));
        *nested = value.Clone();
        root->second.value = std::move(updated);
    }
    Notify(root->first, root->second.value);
    return RecomputeDependents(root->first);
}

Status ObservableViewModel::RecomputeDependents(std::string_view changedPath) {
    std::vector<std::string> pending{std::string(changedPath)};
    std::size_t steps = 0;
    while (!pending.empty()) {
        const std::string dependency = std::move(pending.back()); pending.pop_back();
        if (++steps > m_entries.size() * m_entries.size() + 1)
            return Failure("PXUI2111", "Computed property dependency cycle detected", dependency);
        for (auto& [path, entry] : m_entries) {
            if (!entry.compute || std::find(entry.dependencies.begin(), entry.dependencies.end(), dependency) == entry.dependencies.end()) continue;
            auto result = entry.compute(*this);
            if (!result) return Status::Fail(result.Diagnostics());
            if (result.Value().Type() != entry.type) return Failure("PXUI2106", "Computed property returned the wrong type", path);
            if (result.Value() == entry.value) continue;
            entry.value = std::move(result.Value()); Notify(path, entry.value); pending.push_back(path);
        }
    }
    return Status::Ok();
}

void ObservableViewModel::Notify(const std::string& path, const Variant& value) {
    const auto copy = m_subscriptions;
    for (const auto& subscription : copy) {
        if (!subscription.callback) continue;
        if (subscription.path == path) subscription.callback(value);
        else if (IsDescendantPath(subscription.path, path)) {
            if (auto nested = Read(subscription.path); nested)
                subscription.callback(nested.Value());
        }
    }
}

std::uint64_t ObservableViewModel::Subscribe(std::string path, Changed changed) {
    const auto token = m_nextToken++;
    m_subscriptions.push_back({token, std::move(path), std::move(changed)});
    return token;
}
void ObservableViewModel::Unsubscribe(std::uint64_t token) {
    std::erase_if(m_subscriptions, [token](const Subscription& value) { return value.token == token; });
}

FormatterRegistry::FormatterRegistry() {
    Register({"BoolToVisibility", VariantType::Bool, VariantType::String, FormatBoolVisibility});
    Register({"Not", VariantType::Bool, VariantType::Bool, [](const Variant& value) -> Result<Variant> {
        if (const auto* boolean = value.TryGet<bool>()) return Result<Variant>::Success(Variant(!*boolean));
        return ResultFailure<Variant>("PXUI2112", "Not formatter received a non-boolean value");
    }});
    Register({"Percent", VariantType::Number, VariantType::String, [](const Variant& value) -> Result<Variant> {
        if (const auto* number = value.TryGet<double>()) return Result<Variant>::Success(Variant(std::to_string(static_cast<int>(std::round(*number * 100.0))) + "%"));
        return ResultFailure<Variant>("PXUI2113", "Percent formatter received a non-number value");
    }});
}

Status FormatterRegistry::Register(Formatter formatter) {
    if (formatter.name.empty() || !formatter.format) return Failure("PXUI2114", "Invalid formatter registration");
    if (m_formatters.contains(formatter.name)) return Failure("PXUI2115", "Formatter is already registered: " + formatter.name);
    m_formatters.emplace(formatter.name, std::move(formatter)); return Status::Ok();
}
const Formatter* FormatterRegistry::Find(std::string_view name) const {
    const auto it = m_formatters.find(std::string(name)); return it == m_formatters.end() ? nullptr : &it->second;
}

std::vector<const Formatter*> FormatterRegistry::Descriptors() const {
    std::vector<const Formatter*> result;
    result.reserve(m_formatters.size());
    for (const auto& [name, formatter] : m_formatters) {
        (void)name;
        result.push_back(&formatter);
    }
    return result;
}

Binding::~Binding() { Disconnect(); }
Binding::Binding(Binding&& other) noexcept { *this = std::move(other); }
Binding& Binding::operator=(Binding&& other) noexcept {
    if (this == &other) return *this;
    Disconnect(); m_model = other.m_model; m_path = std::move(other.m_path); m_target = std::move(other.m_target);
    m_formatter = other.m_formatter; m_subscription = other.m_subscription;
    other.m_model = nullptr; other.m_subscription = 0; return *this;
}

Result<Binding> Binding::Create(IViewModel& model, std::string path, BindingTarget target,
                                const FormatterRegistry& formatters, std::string formatterName) {
    const auto info = model.Describe(path);
    if (!info) return ResultFailure<Binding>("PXUI2116", "Binding path does not exist on the ViewModel", path);
    const Formatter* formatter = nullptr;
    VariantType output = info->type;
    if (!formatterName.empty()) {
        formatter = formatters.Find(formatterName);
        if (!formatter) return ResultFailure<Binding>("PXUI2117", "Unknown formatter: " + formatterName, path);
        if (formatter->input != info->type) return ResultFailure<Binding>("PXUI2118", "Formatter input type does not match binding path", path);
        output = formatter->output;
    }
    if (!target.set || target.type != output) return ResultFailure<Binding>("PXUI2119", "Binding output type does not match target " + target.description, path);
    Binding binding; binding.m_model = &model; binding.m_path = std::move(path); binding.m_target = std::move(target); binding.m_formatter = formatter;
    const Status status = binding.Refresh();
    if (!status) return Result<Binding>::Failure(status.Diagnostics());
    const std::string subscribedPath = binding.m_path;
    const BindingTarget subscribedTarget = binding.m_target;
    binding.m_subscription = model.Subscribe(binding.m_path,
        [&model, subscribedPath, subscribedTarget, formatter](const Variant&) {
            auto value = model.Read(subscribedPath);
            if (!value) return;
            if (formatter) {
                auto formatted = formatter->format(value.Value());
                if (formatted) subscribedTarget.set(formatted.Value());
            } else {
                subscribedTarget.set(value.Value());
            }
        });
    return Result<Binding>::Success(std::move(binding));
}

void Binding::Disconnect() {
    if (m_model && m_subscription) m_model->Unsubscribe(m_subscription);
    m_model = nullptr; m_subscription = 0;
}

Status Binding::Refresh() {
    if (!m_model) return Failure("PXUI2120", "Binding is disconnected", m_path);
    auto value = m_model->Read(m_path); if (!value) return Status::Fail(value.Diagnostics());
    if (m_formatter) { auto formatted = m_formatter->format(value.Value()); if (!formatted) return Status::Fail(formatted.Diagnostics()); return m_target.set(formatted.Value()); }
    return m_target.set(value.Value());
}

Status CommandRegistry::Register(std::string name, Command command) {
    if (name.empty() || !command) return Failure("PXUI2121", "Invalid command registration");
    if (m_commands.contains(name)) return Failure("PXUI2122", "Command is already registered: " + name);
    m_commands.emplace(std::move(name), std::move(command)); return Status::Ok();
}
Status CommandRegistry::Execute(std::string_view name, Variant parameter) const {
    const auto it = m_commands.find(std::string(name));
    if (it == m_commands.end()) return Failure("PXUI2123", "UI command is not registered: " + std::string(name));
    return it->second(parameter);
}
bool CommandRegistry::Contains(std::string_view name) const { return m_commands.contains(std::string(name)); }

}  // namespace px::ui
