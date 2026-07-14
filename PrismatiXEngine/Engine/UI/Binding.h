#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::ui {

struct PropertyPathInfo {
    std::string path;
    VariantType type = VariantType::Null;
    bool writable = false;
};

class IViewModel {
public:
    using Changed = std::function<void(const Variant&)>;
    virtual ~IViewModel() = default;
    [[nodiscard]] virtual std::optional<PropertyPathInfo> Describe(std::string_view path) const = 0;
    [[nodiscard]] virtual std::vector<PropertyPathInfo> EnumerateProperties() const { return {}; }
    [[nodiscard]] virtual Result<Variant> Read(std::string_view path) const = 0;
    virtual Status Write(std::string_view path, const Variant& value) = 0;
    virtual std::uint64_t Subscribe(std::string path, Changed changed) = 0;
    virtual void Unsubscribe(std::uint64_t token) = 0;
};

class ObservableViewModel final : public IViewModel {
public:
    using Compute = std::function<Result<Variant>(const ObservableViewModel&)>;

    Status Define(std::string path, Variant initialValue, bool writable = true);
    Status DefineComputed(std::string path, VariantType type, std::vector<std::string> dependencies,
                          Compute compute);
    std::optional<PropertyPathInfo> Describe(std::string_view path) const override;
    std::vector<PropertyPathInfo> EnumerateProperties() const override;
    Result<Variant> Read(std::string_view path) const override;
    Status Write(std::string_view path, const Variant& value) override;
    std::uint64_t Subscribe(std::string path, Changed changed) override;
    void Unsubscribe(std::uint64_t token) override;

private:
    struct Entry {
        VariantType type = VariantType::Null;
        Variant value;
        bool writable = false;
        std::vector<std::string> dependencies;
        Compute compute;
    };
    struct Subscription { std::uint64_t token; std::string path; Changed callback; };
    Status RecomputeDependents(std::string_view changedPath);
    void Notify(const std::string& path, const Variant& value);

    std::unordered_map<std::string, Entry> m_entries;
    std::vector<Subscription> m_subscriptions;
    std::uint64_t m_nextToken = 1;
};

struct Formatter {
    std::string name;
    VariantType input = VariantType::Null;
    VariantType output = VariantType::Null;
    std::function<Result<Variant>(const Variant&)> format;
};

class FormatterRegistry {
public:
    FormatterRegistry();
    Status Register(Formatter formatter);
    [[nodiscard]] const Formatter* Find(std::string_view name) const;
    [[nodiscard]] std::vector<const Formatter*> Descriptors() const;
private:
    std::map<std::string, Formatter> m_formatters;
};

struct BindingTarget {
    VariantType type = VariantType::Null;
    std::string description;
    std::function<Status(const Variant&)> set;
};

class Binding {
public:
    Binding() = default;
    ~Binding();
    Binding(Binding&& other) noexcept;
    Binding& operator=(Binding&& other) noexcept;
    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;

    static Result<Binding> Create(IViewModel& model, std::string path, BindingTarget target,
                                  const FormatterRegistry& formatters, std::string formatter = {});
    void Disconnect();
    Status Refresh();

private:
    IViewModel* m_model = nullptr;
    std::string m_path;
    BindingTarget m_target;
    const Formatter* m_formatter = nullptr;
    std::uint64_t m_subscription = 0;
};

class CommandRegistry {
public:
    using Command = std::function<Status(const Variant& parameter)>;
    Status Register(std::string name, Command command);
    Status Execute(std::string_view name, Variant parameter = {}) const;
    [[nodiscard]] bool Contains(std::string_view name) const;
private:
    std::unordered_map<std::string, Command> m_commands;
};

[[nodiscard]] bool IsValidPropertyPath(std::string_view path);

}  // namespace px::ui
