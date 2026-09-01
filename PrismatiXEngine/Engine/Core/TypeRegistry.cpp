#include "Engine/Core/TypeRegistry.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace px {

namespace {
diag::Diagnostic RegistryError(std::string code, std::string message, std::string details = {}) {
    return diag::Diagnostic{
        .severity = diag::Severity::Error,
        .code = std::move(code),
        .category = "type-registry",
        .message = std::move(message),
        .details = std::move(details),
        .source = {},
        .operationId = {},
        .quickFix = {},
    };
}

Status ValidateTypeShape(const TypeInfo& type) {
    if (type.name.empty() || !type.factory)
        return Status::Fail(RegistryError(
            "PXTYPE-E1001", "A registered type needs a name and factory."));
    std::unordered_set<std::string> propertyNames;
    for (const PropertyInfo& property : type.properties)
        if (property.name.empty() ||
            !propertyNames.insert(property.name).second)
            return Status::Fail(RegistryError(
                "PXTYPE-E1003", "Duplicate or empty property name.",
                type.name + "." + property.name));
    std::unordered_set<std::string> signalNames;
    for (const SignalInfo& signal : type.signals) {
        if (signal.name.empty() || !signalNames.insert(signal.name).second)
            return Status::Fail(RegistryError(
                "PXTYPE-E1004", "Duplicate or empty signal name.",
                type.name + "." + signal.name));
        std::unordered_set<std::string> argumentNames;
        for (const SignalArgumentInfo& argument : signal.arguments)
            if (argument.name.empty() || argument.type == VariantType::Null ||
                !argumentNames.insert(argument.name).second)
                return Status::Fail(RegistryError(
                    "PXTYPE-E1005", "Invalid signal argument.",
                    type.name + "." + signal.name + "." + argument.name));
    }
    return Status::Ok();
}
}

Status TypeRegistry::Register(TypeInfo type) {
    if (const Status valid=ValidateTypeShape(type); !valid) return valid;
    if (type.sourceId.empty()) type.sourceId="PrismatiX.Legacy";
    std::lock_guard lock(m_mutex);
    if (m_types.contains(type.name)) {
        return Status::Fail(RegistryError("PXTYPE-E1002", "A type is already registered.", type.name));
    }
    if (!type.base.empty() && !m_types.contains(type.base))
        return Status::Fail(RegistryError(
            "PXTYPE-E1006", "A type references an unregistered base.",
            type.name + " -> " + type.base));
    m_types.emplace(type.name, std::move(type));
    return Status::Ok();
}

Status TypeRegistry::ReplaceSource(std::string sourceId,
                                   std::vector<TypeInfo> types) {
    if (sourceId.empty())
        return Status::Fail(RegistryError(
            "PXTYPE-E1007", "A registration source needs a stable id."));
    std::unordered_set<std::string> stagedNames;
    for (auto& type : types) {
        if (const Status valid=ValidateTypeShape(type); !valid) return valid;
        if (!stagedNames.insert(type.name).second)
            return Status::Fail(RegistryError(
                "PXTYPE-E1002", "A source contains duplicate type ids.",
                type.name));
        type.sourceId=sourceId;
    }

    std::lock_guard lock(m_mutex);
    for (const auto& type : types) {
        const auto current=m_types.find(type.name);
        if(current!=m_types.end() && current->second.sourceId!=sourceId)
            return Status::Fail(RegistryError(
                "PXTYPE-E1008", "A type id belongs to another source.",
                type.name + " owned by " + current->second.sourceId));
    }

    std::unordered_map<std::string,const TypeInfo*> staged;
    staged.reserve(types.size());
    for(const auto& type:types)staged.emplace(type.name,&type);
    const auto resolve=[&](const std::string& name)->const TypeInfo*{
        if(const auto found=staged.find(name);found!=staged.end())
            return found->second;
        const auto found=m_types.find(name);
        return found!=m_types.end() && found->second.sourceId!=sourceId
            ?&found->second:nullptr;
    };
    for(const auto& type:types)
        if(!type.base.empty()&&!resolve(type.base))
            return Status::Fail(RegistryError(
                "PXTYPE-E1006", "A type references an unavailable base.",
                type.name + " -> " + type.base));

    // Validate every staged inheritance chain, including cross-source bases.
    for(const auto& type:types){
        std::unordered_set<std::string> visiting;
        const TypeInfo* cursor=&type;
        while(cursor&&!cursor->base.empty()){
            if(!visiting.insert(cursor->name).second)
                return Status::Fail(RegistryError(
                    "PXTYPE-E1009", "Type inheritance contains a cycle.",
                    type.name));
            cursor=resolve(cursor->base);
            if(!cursor)
                return Status::Fail(RegistryError(
                    "PXTYPE-E1006", "A type inheritance chain is incomplete.",
                    type.name));
        }
    }

    // A source cannot disappear while a foreign source still derives from one
    // of its removed types. Keeping the old registrations is safer than
    // publishing a broken registry graph.
    for(const auto& [name,type]:m_types){
        if(type.sourceId==sourceId)continue;
        const TypeInfo* cursor=&type;
        std::unordered_set<std::string> visited;
        while(cursor&&!cursor->base.empty()&&visited.insert(cursor->name).second){
            const auto oldBase=m_types.find(cursor->base);
            if(oldBase!=m_types.end()&&oldBase->second.sourceId==sourceId&&
               !staged.contains(cursor->base))
                return Status::Fail(RegistryError(
                    "PXTYPE-E1010",
                    "A foreign type still depends on a type being removed.",
                    name + " -> " + cursor->base));
            cursor=resolve(cursor->base);
        }
    }

    // Allocate every candidate node and any required buckets before changing
    // the live registry. Moving unordered-map node handles is allocation-free.
    std::unordered_map<std::string,TypeInfo> prepared;
    prepared.reserve(types.size());
    for(auto& type:types)prepared.emplace(type.name,std::move(type));
    std::vector<decltype(m_types)::node_type> retired;
    retired.reserve(std::ranges::count_if(m_types,[&](const auto& entry){
        return entry.second.sourceId==sourceId;
    }));
    m_types.reserve(m_types.size()-retired.capacity()+prepared.size());
    for(auto it=m_types.begin();it!=m_types.end();)
        if(it->second.sourceId==sourceId){
            auto current=it++;
            retired.push_back(m_types.extract(current));
        }else ++it;
    while(!prepared.empty()){
        auto node=prepared.extract(prepared.begin());
        const auto inserted=m_types.insert(std::move(node));
        if(!inserted.inserted){
            // This should be unreachable after conflict validation, but retain
            // the strong transactional guarantee if invariants change later.
            for(auto current=m_types.begin();current!=m_types.end();)
                if(current->second.sourceId==sourceId)current=m_types.erase(current);
                else ++current;
            for(auto& old:retired)m_types.insert(std::move(old));
            return Status::Fail(RegistryError(
                "PXTYPE-E1011", "Atomic type source replacement failed.",
                inserted.position->first));
        }
    }
    return Status::Ok();
}

Status TypeRegistry::RemoveSource(const std::string_view sourceId) {
    return ReplaceSource(std::string(sourceId),{});
}

std::vector<std::string> TypeRegistry::TypesForSource(
    const std::string_view sourceId) const {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> result;
    for(const auto& [name,type]:m_types)
        if(type.sourceId==sourceId)result.push_back(name);
    std::ranges::sort(result);
    return result;
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

std::vector<const PropertyInfo*> TypeRegistry::PropertiesForType(
    const std::string& type) const {
    std::lock_guard lock(m_mutex);
    std::vector<const PropertyInfo*> result;
    std::unordered_set<std::string> seen;
    auto it = m_types.find(type);
    while (it != m_types.end()) {
        for (const PropertyInfo& property : it->second.properties)
            if (seen.insert(property.name).second) result.push_back(&property);
        if (it->second.base.empty()) break;
        it = m_types.find(it->second.base);
    }
    return result;
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
