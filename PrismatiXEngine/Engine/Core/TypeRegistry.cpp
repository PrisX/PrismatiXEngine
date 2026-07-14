#include "Engine/Core/TypeRegistry.h"

#include <unordered_set>

namespace px {

namespace {
diag::Diagnostic RegistryError(std::string code, std::string message, std::string details = {}) {
    return diag::Diagnostic{ diag::Severity::Error, std::move(code), "type-registry",
                             std::move(message), std::move(details) };
}
}

Status TypeRegistry::Register(TypeInfo type) {
    if (type.name.empty() || !type.factory) {
        return Status::Fail(
            RegistryError("PXTYPE-E1001", "A registered type needs a name and factory."));
    }
    std::lock_guard lock(m_mutex);
    if (m_types.contains(type.name)) {
        return Status::Fail(RegistryError("PXTYPE-E1002", "A type is already registered.", type.name));
    }
    std::unordered_set<std::string> propertyNames;
    for (const PropertyInfo& p : type.properties) {
        if (p.name.empty() || !propertyNames.insert(p.name).second) {
            return Status::Fail(RegistryError("PXTYPE-E1003", "Duplicate or empty property name.",
                                              type.name + "." + p.name));
        }
    }
    std::unordered_set<std::string> signalNames;
    for (const SignalInfo& signal : type.signals) {
        if (signal.name.empty() || !signalNames.insert(signal.name).second)
            return Status::Fail(RegistryError("PXTYPE-E1004", "Duplicate or empty signal name.",
                                              type.name + "." + signal.name));
        std::unordered_set<std::string> argumentNames;
        for (const SignalArgumentInfo& argument : signal.arguments)
            if (argument.name.empty() || argument.type == VariantType::Null ||
                !argumentNames.insert(argument.name).second)
                return Status::Fail(RegistryError("PXTYPE-E1005", "Invalid signal argument.",
                    type.name + "." + signal.name + "." + argument.name));
    }
    m_types.emplace(type.name, std::move(type));
    return Status::Ok();
}

const SignalInfo* TypeRegistry::FindSignal(const std::string& type,
                                           const std::string& signal) const {
    std::lock_guard lock(m_mutex);
    auto it = m_types.find(type);
    while (it != m_types.end()) {
        for (const SignalInfo& candidate : it->second.signals)
            if (candidate.name == signal) return &candidate;
        if (it->second.base.empty()) break;
        it = m_types.find(it->second.base);
    }
    return nullptr;
}

std::vector<const SignalInfo*> TypeRegistry::SignalsForType(const std::string& type) const {
    std::lock_guard lock(m_mutex);
    std::vector<const SignalInfo*> result;
    std::unordered_set<std::string> seen;
    auto it = m_types.find(type);
    while (it != m_types.end()) {
        for (const SignalInfo& signal : it->second.signals)
            if (seen.insert(signal.name).second) result.push_back(&signal);
        if (it->second.base.empty()) break;
        it = m_types.find(it->second.base);
    }
    return result;
}

const TypeInfo* TypeRegistry::Find(const std::string& name) const {
    std::lock_guard lock(m_mutex);
    auto it = m_types.find(name);
    return it == m_types.end() ? nullptr : &it->second;
}

const PropertyInfo* TypeRegistry::FindProperty(const std::string& type,
                                               const std::string& property) const {
    std::lock_guard lock(m_mutex);
    auto it = m_types.find(type);
    while (it != m_types.end()) {
        for (const PropertyInfo& p : it->second.properties)
            if (p.name == property) return &p;
        if (it->second.base.empty()) break;
        it = m_types.find(it->second.base);
    }
    return nullptr;
}

std::unique_ptr<Object> TypeRegistry::Create(const std::string& type) const {
    std::lock_guard lock(m_mutex);
    auto it = m_types.find(type);
    return it == m_types.end() ? nullptr : it->second.factory();
}

std::vector<const TypeInfo*> TypeRegistry::TypesDerivedFrom(const std::string& base) const {
    std::lock_guard lock(m_mutex);
    std::vector<const TypeInfo*> result;
    for (const auto& [_, type] : m_types) {
        std::string current = type.name;
        while (!current.empty()) {
            if (current == base) {
                result.push_back(&type);
                break;
            }
            auto it = m_types.find(current);
            current = it == m_types.end() ? std::string{} : it->second.base;
        }
    }
    return result;
}

bool TypeRegistry::IsDerivedFrom(const std::string& type, const std::string& base) const {
    std::lock_guard lock(m_mutex);
    std::string current = type;
    while (!current.empty()) {
        if (current == base) return true;
        auto it = m_types.find(current);
        current = it == m_types.end() ? std::string{} : it->second.base;
    }
    return false;
}

TypeRegistry& TypeRegistry::Global() {
    static TypeRegistry registry;
    return registry;
}

}  // namespace px
