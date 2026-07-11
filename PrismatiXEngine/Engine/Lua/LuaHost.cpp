#include "Engine/Lua/LuaHost.h"

#include "Engine/IO/VFS.h"
#include "Engine/Animation/Timeline.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <tuple>

namespace px::lua {
namespace {
std::optional<VariantType> ManifestType(const std::string& type){if(type=="null")return VariantType::Null;if(type=="bool")return VariantType::Bool;if(type=="int")return VariantType::Integer;if(type=="number")return VariantType::Number;if(type=="string")return VariantType::String;if(type=="list")return VariantType::Array;if(type=="map"||type=="expression")return VariantType::Object;if(type=="resource")return VariantType::ResourceRef;return std::nullopt;}
sol::object ToLua(sol::state_view state,const Variant& value){switch(value.Type()){case VariantType::Null:return sol::make_object(state,sol::nil);case VariantType::Bool:return sol::make_object(state,*value.TryGet<bool>());case VariantType::Integer:return sol::make_object(state,*value.TryGet<std::int64_t>());case VariantType::Number:return sol::make_object(state,*value.TryGet<double>());case VariantType::String:return sol::make_object(state,*value.TryGet<std::string>());case VariantType::Uuid:return sol::make_object(state,value.TryGet<Uuid>()->ToString());case VariantType::ResourceRef:{const auto& reference=*value.TryGet<ResourceRefValue>();sol::table table=state.create_table();table["id"]=reference.id.ToString();table["path"]=reference.lastKnownPath;return sol::make_object(state,table);}case VariantType::Array:{sol::table table=state.create_table();std::size_t index=1;for(const auto& item:*value.AsArray())table[index++]=ToLua(state,item);return sol::make_object(state,table);}case VariantType::Object:{sol::table table=state.create_table();for(const auto& [name,item]:*value.AsObject())table[name]=ToLua(state,item);return sol::make_object(state,table);}case VariantType::Vec2:{const auto& vector=*value.TryGet<Vec2>();sol::table table=state.create_table();table["x"]=vector.x;table["y"]=vector.y;return sol::make_object(state,table);}case VariantType::Rect:case VariantType::Color:return sol::make_object(state,sol::nil);}return sol::make_object(state,sol::nil);}
}

LuaHost::LuaHost(const LuaServices& services) : m_runner(sol::thread::create(m_lua)), m_services(services) {
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                         sol::lib::coroutine, sol::lib::utf8);
    // Base Lua exposes host-filesystem helpers even when io/os are not opened.
    // Extensions receive only VFS-aware require unless a future platform
    // capability provider explicitly grants a broader API.
    m_lua["dofile"] = sol::nil;
    m_lua["loadfile"] = sol::nil;
    BindVfsRequire();
    BindApi();
}

void LuaHost::HandleError(const std::string& where, const std::string& message) {
    PX_LOG_ERROR("Lua error in {}: {}", where, message);
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code="PXLUA7401",
                                .category="Lua.Runtime",.message="Lua extension failed",
                                .details=message};
    diagnostic.source.path=where;diag::Emit(std::move(diagnostic));
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
        json.value("version", 0) != 3) {
        HandleError(manifestPath, "extension manifest must be strict PrismatiXExtension version 3");
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
        for (const auto& capability : json.value("capabilities", nlohmann::json::array())) {
            if (!capability.is_string()) {
                HandleError(manifestPath, "capability must be a string");
                return false;
            }
            manifest.capabilities.push_back(capability.get<std::string>());
        }
        // File/network/native access are deliberately not implemented by the
        // sandbox provider yet; declaring them produces a clear hard failure.
        for (const auto& capability : manifest.capabilities) {
            if (capability != "runtime" && capability != "animation" &&
                capability != "ui" && capability != "audio") {
                HandleError(manifestPath, "unsupported capability: " + capability);
                return false;
            }
        }
        for (const auto& command : json.value("commands", nlohmann::json::array())) {
            if (!command.is_object()) {
                HandleError(manifestPath, "commands must be typed descriptor objects");
                return false;
            }
            vn::CommandDescriptor descriptor;descriptor.id=command.at("id").get<std::string>();descriptor.displayName=command.value("displayName",descriptor.id);descriptor.category=command.value("category",std::string("Extension"));descriptor.waitPolicy=command.value("await",false)?vn::CommandWaitPolicy::Async:vn::CommandWaitPolicy::Immediate;descriptor.rollbackPolicy=command.value("rollback",std::string("boundary"))=="reversible"?vn::RollbackPolicy::Reversible:vn::RollbackPolicy::Boundary;
            if(descriptor.id.empty()){HandleError(manifestPath,"command id is empty");return false;}
            for(const auto& parameter:command.value("parameters",nlohmann::json::array())){if(!parameter.is_object()){HandleError(manifestPath,"command parameter must be an object");return false;}const auto type=ManifestType(parameter.value("type",std::string{}));if(!type){HandleError(manifestPath,"unsupported command parameter type");return false;}vn::CommandParameterDescriptor value;value.name=parameter.at("name").get<std::string>();value.label=parameter.value("label",value.name);value.type=*type;value.required=parameter.value("required",false);if(value.name.empty()){HandleError(manifestPath,"command parameter name is empty");return false;}descriptor.parameters.push_back(std::move(value));}
            manifest.commands.insert(descriptor.id);if(!vn::CommandRegistry::Global().Find(descriptor.id)){const Status registered=vn::CommandRegistry::Global().Register(std::move(descriptor));if(!registered){HandleError(manifestPath,diag::Describe(registered.Diagnostics().front()));return false;}}
        }
        m_activeExtension = manifest.id;
        m_declaredCommands.insert(manifest.commands.begin(), manifest.commands.end());
        const bool loaded = RunFile("Content/Extensions/" + manifest.entry);
        m_activeExtension.clear();
        return loaded;
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
    sol::state_view runnerState=m_runner.state();sol::coroutine coroutine(runnerState,it->second);
    sol::protected_function_result result = coroutine(args);
    if (!result.valid()) {
        const sol::error err = result;
        HandleError("command:" + cmd.type, err.what());
        return true;
    }
    if(result.status()==sol::call_status::yielded){PendingCoroutine pending{std::move(coroutine)};pending.command=cmd;pending.yieldIndex=1;if(result.return_count()>=1)pending.waitKind=result.get<std::string>(0);if(result.return_count()>=2){if(pending.waitKind=="animation")pending.handle=result.get<std::uint64_t>(1);else if(pending.waitKind=="timer")pending.remainingSeconds=result.get<float>(1);}m_pending.push_back(std::move(pending));}
    return true;
}

void LuaHost::Update(const float deltaSeconds){for(auto iterator=m_pending.begin();iterator!=m_pending.end();){bool ready=false;if(iterator->waitKind=="animation")ready=!m_services.timeline||!m_services.timeline->Playing(iterator->handle);else if(iterator->waitKind=="timer"){iterator->remainingSeconds-=std::max(0.0f,deltaSeconds);ready=iterator->remainingSeconds<=0.0f;}else ready=true;if(!ready){++iterator;continue;}sol::protected_function_result result=iterator->coroutine();if(!result.valid()){const sol::error error=result;HandleError("await",error.what());iterator=m_pending.erase(iterator);continue;}if(result.status()==sol::call_status::yielded){++iterator->yieldIndex;iterator->waitKind=result.return_count()>=1?result.get<std::string>(0):std::string{};iterator->handle=iterator->waitKind=="animation"&&result.return_count()>=2?result.get<std::uint64_t>(1):0;iterator->remainingSeconds=iterator->waitKind=="timer"&&result.return_count()>=2?result.get<float>(1):0.0f;++iterator;}else iterator=m_pending.erase(iterator);}}

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
    m_runner = sol::thread::create(m_lua);
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
        sol::state_view runnerState = m_runner.state();
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
        PendingCoroutine pending{std::move(coroutine)};
        pending.command = saved.command;
        pending.yieldIndex = saved.yieldIndex;
        pending.waitKind = saved.waitKind;
        pending.handle = saved.handle;
        pending.remainingSeconds = saved.remainingSeconds;
        m_pending.push_back(std::move(pending));
    }
    return Status::Ok();
}

void LuaHost::CancelPending() {
    m_pending.clear();
    m_runner = sol::thread::create(m_lua);
}

}
