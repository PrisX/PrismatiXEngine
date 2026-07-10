#include "Engine/UI/Binding.h"

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

std::optional<PropertyPathInfo> ObservableViewModel::Describe(std::string_view path) const {
    if (const auto it = m_entries.find(std::string(path)); it != m_entries.end())
        return PropertyPathInfo{it->first, it->second.type, it->second.writable};
    return std::nullopt;
}

Result<Variant> ObservableViewModel::Read(std::string_view path) const {
    if (const auto it = m_entries.find(std::string(path)); it != m_entries.end()) return Result<Variant>::Success(it->second.value);
    return ResultFailure<Variant>("PXUI2108", "Unknown ViewModel property path", std::string(path));
}

Status ObservableViewModel::Write(std::string_view path, const Variant& value) {
    const auto it = m_entries.find(std::string(path));
    if (it == m_entries.end()) return Failure("PXUI2108", "Unknown ViewModel property path", std::string(path));
    if (!it->second.writable) return Failure("PXUI2109", "ViewModel property is read-only", std::string(path));
    if (it->second.type != value.Type()) return Failure("PXUI2110", "ViewModel property type mismatch", std::string(path));
    if (it->second.value == value) return Status::Ok();
    it->second.value = value;
    Notify(it->first, it->second.value);
    return RecomputeDependents(it->first);
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
    for (const auto& subscription : copy) if (subscription.path == path && subscription.callback) subscription.callback(value);
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
