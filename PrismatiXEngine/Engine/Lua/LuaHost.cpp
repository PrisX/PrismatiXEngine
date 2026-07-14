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
#include <optional>
#include <stdexcept>
#include <tuple>

namespace px::lua {
namespace {
std::string Lower(std::string value){std::transform(value.begin(),value.end(),value.begin(),[](const unsigned char c){return static_cast<char>(std::tolower(c));});return value;}
std::optional<VariantType> ManifestType(const std::string& raw){const auto type=Lower(raw);if(type=="null")return VariantType::Null;if(type=="bool"||type=="boolean")return VariantType::Bool;if(type=="int"||type=="integer")return VariantType::Integer;if(type=="number"||type=="float")return VariantType::Number;if(type=="string")return VariantType::String;if(type=="vec2"||type=="vector2")return VariantType::Vec2;if(type=="rect")return VariantType::Rect;if(type=="color")return VariantType::Color;if(type=="uuid"||type=="node")return VariantType::Uuid;if(type=="list"||type=="array")return VariantType::Array;if(type=="map"||type=="object"||type=="expression")return VariantType::Object;if(type=="resource")return VariantType::ResourceRef;if(type=="token")return VariantType::TokenRef;return std::nullopt;}
std::optional<ui::ActionEditorHint> ManifestEditorHint(const std::string& raw){const auto hint=Lower(raw);if(hint.empty()||hint=="default")return ui::ActionEditorHint::Default;if(hint=="multiline")return ui::ActionEditorHint::Multiline;if(hint=="enum")return ui::ActionEditorHint::Enum;if(hint=="color")return ui::ActionEditorHint::Color;if(hint=="resource")return ui::ActionEditorHint::Resource;if(hint=="route")return ui::ActionEditorHint::Route;if(hint=="node")return ui::ActionEditorHint::Node;if(hint=="animation")return ui::ActionEditorHint::Animation;if(hint=="token")return ui::ActionEditorHint::Token;return std::nullopt;}
bool SupportedCapability(const std::string_view capability){return capability=="runtime"||capability=="animation"||capability=="ui"||capability=="audio";}
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
sol::object ToLua(sol::state_view state,const Variant& value){switch(value.Type()){case VariantType::Null:return sol::make_object(state,sol::nil);case VariantType::Bool:return sol::make_object(state,*value.TryGet<bool>());case VariantType::Integer:return sol::make_object(state,*value.TryGet<std::int64_t>());case VariantType::Number:return sol::make_object(state,*value.TryGet<double>());case VariantType::String:return sol::make_object(state,*value.TryGet<std::string>());case VariantType::Uuid:return sol::make_object(state,value.TryGet<Uuid>()->ToString());case VariantType::TokenRef:return sol::make_object(state,value.TryGet<TokenRefValue>()->name);case VariantType::ResourceRef:{const auto& reference=*value.TryGet<ResourceRefValue>();sol::table table=state.create_table();table["id"]=reference.id.ToString();table["path"]=reference.lastKnownPath;return sol::make_object(state,table);}case VariantType::Array:{sol::table table=state.create_table();std::size_t index=1;for(const auto& item:*value.AsArray())table[index++]=ToLua(state,item);return sol::make_object(state,table);}case VariantType::Object:{sol::table table=state.create_table();for(const auto& [name,item]:*value.AsObject())table[name]=ToLua(state,item);return sol::make_object(state,table);}case VariantType::Vec2:{const auto& vector=*value.TryGet<Vec2>();sol::table table=state.create_table();table["x"]=vector.x;table["y"]=vector.y;return sol::make_object(state,table);}case VariantType::Rect:{const auto& rectangle=*value.TryGet<Rect>();sol::table table=state.create_table();table["x"]=rectangle.x;table["y"]=rectangle.y;table["w"]=rectangle.w;table["h"]=rectangle.h;return sol::make_object(state,table);}case VariantType::Color:{const auto& color=*value.TryGet<Color>();sol::table table=state.create_table();table["r"]=color.r;table["g"]=color.g;table["b"]=color.b;table["a"]=color.a;return sol::make_object(state,table);}}return sol::make_object(state,sol::nil);}
class LuaActionProvider final : public ui::IActionProvider {
public:
    explicit LuaActionProvider(LuaHost& host):m_host(host){}
    std::string_view ProviderId()const override{return "lua";}
    ui::ActionOrigin Origin()const override{return ui::ActionOrigin::LuaExtension;}
    bool CanInvoke(std::string_view action)const override{return m_host.HasAction(action);}
    Status Invoke(const ui::ActionInvocation& invocation)override{return m_host.InvokeAction(invocation);}
    ui::ProviderActionStart Start(const ui::ActionInvocation& invocation)override{return m_host.StartAction(invocation);}
    ui::ActionExecutionState Poll(std::uint64_t handle)const override{return m_host.ActionState(handle);}
    void Cancel(std::uint64_t handle)override{m_host.CancelAction(handle);}
private:LuaHost& m_host;
};
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
LuaHost::~LuaHost(){for(const auto& source:m_loadedActionSources)(void)ui::ActionCatalog::Global().RemoveSource(ui::ActionOrigin::LuaExtension,source);}

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
        std::vector<ui::ActionDescriptor> actionDescriptors;
        for (const auto& action : json.value("actions", nlohmann::json::array())) {
            if (!action.is_object()) { HandleError(manifestPath,"actions must be typed descriptor objects"); return false; }
            ui::ActionDescriptor descriptor;
            descriptor.id=action.at("id").get<std::string>();
            descriptor.label=action.value("displayName",descriptor.id);descriptor.displayName=descriptor.label;
            descriptor.description=action.value("description",std::string{});
            descriptor.category=action.value("category",std::string("Extension"));
            descriptor.origin=ui::ActionOrigin::LuaExtension;descriptor.sourceId=manifest.id;descriptor.providerId="lua";
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
                if(parameter.contains("default")){const auto parsed=ManifestValue(parameter["default"],value.type);if(!parsed){HandleError(manifestPath,"Action parameter default has the wrong type: "+descriptor.id+"."+value.name);return false;}value.defaultValue=parsed->Clone();}
                const auto enumJson=parameter.contains("enum")?parameter["enum"]:nlohmann::json::array();
                if(!enumJson.is_array()){HandleError(manifestPath,"Action parameter enum must be an array");return false;}
                if(!enumJson.empty()&&value.type!=VariantType::String){HandleError(manifestPath,"Action parameter enum requires string type");return false;}
                for(const auto& option:enumJson){if(!option.is_string()){HandleError(manifestPath,"Action enum values must be strings");return false;}value.enumValues.push_back(option.get<std::string>());}
                if(parameter.contains("range")){const auto& range=parameter["range"];if(!range.is_object()){HandleError(manifestPath,"Action parameter range must be an object");return false;}if(range.contains("minimum"))value.minimum=range["minimum"].get<double>();if(range.contains("maximum"))value.maximum=range["maximum"].get<double>();}
                if(parameter.contains("minimum"))value.minimum=parameter["minimum"].get<double>();if(parameter.contains("maximum"))value.maximum=parameter["maximum"].get<double>();
                if((value.minimum||value.maximum)&&value.type!=VariantType::Integer&&value.type!=VariantType::Number){HandleError(manifestPath,"Action parameter range requires numeric type");return false;}
                value.resourceType=parameter.value("resourceFilter",parameter.value("resourceType",std::string{}));
                const auto hint=ManifestEditorHint(parameter.value("editorHint",std::string{}));if(!hint){HandleError(manifestPath,"Action parameter editorHint is unknown");return false;}value.editorHint=*hint;
                if(!value.enumValues.empty()&&value.editorHint==ui::ActionEditorHint::Default)value.editorHint=ui::ActionEditorHint::Enum;
                if(value.type==VariantType::ResourceRef&&value.editorHint==ui::ActionEditorHint::Default)value.editorHint=ui::ActionEditorHint::Resource;
                descriptor.arguments.push_back(std::move(value));
            }
            manifest.actions.insert(descriptor.id);actionDescriptors.push_back(std::move(descriptor));
        }
        const Status actionRegistration=ui::ActionCatalog::Global().ReplaceSource(
            ui::ActionOrigin::LuaExtension,manifest.id,std::move(actionDescriptors));
        if(!actionRegistration){HandleError(manifestPath,diag::Describe(actionRegistration.Diagnostics().front()));return false;}
        m_loadedActionSources.insert(manifest.id);
        m_activeExtension = manifest.id;
        m_declaredCommands.insert(manifest.commands.begin(), manifest.commands.end());
        m_declaredActions.insert(manifest.actions.begin(), manifest.actions.end());
        const bool loaded = RunFile("Content/Extensions/" + manifest.entry);
        m_activeExtension.clear();
        if(!loaded)return false;
        for(const auto& action:manifest.actions)if(!m_actions.contains(action)){
            HandleError(manifestPath,"action '"+action+"' has no Engine.RegisterAction callback");return false;
        }
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
    sol::state_view runnerState=m_runner.state();sol::coroutine coroutine(runnerState,function->second);
    sol::protected_function_result result=coroutine(args,context);
    if(!result.valid()){const sol::error error=result;HandleError("action:"+invocation.action,error.what());return {.status=Status::Fail(diag::Diagnostic{
        .severity=diag::Severity::Error,.code="PXLUA7421",.category="Lua.Action",
        .message="Lua action failed",.details=error.what()})};}
    if(result.status()==sol::call_status::yielded){const std::uint64_t handle=m_nextActionHandle++;PendingActionCoroutine pending{std::move(coroutine)};pending.id=handle;pending.action=invocation.action;pending.invocation=invocation;pending.yieldIndex=1;
        if(result.return_count()>=1)pending.waitKind=result.get<std::string>(0);
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
    if(found!=m_pendingActions.end())m_pendingActions.erase(found);
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

void LuaHost::Update(const float deltaSeconds){for(auto iterator=m_pending.begin();iterator!=m_pending.end();){bool ready=false;if(iterator->waitKind=="animation")ready=!m_services.timeline||!m_services.timeline->Playing(iterator->handle);else if(iterator->waitKind=="timer"){iterator->remainingSeconds-=std::max(0.0f,deltaSeconds);ready=iterator->remainingSeconds<=0.0f;}else ready=true;if(!ready){++iterator;continue;}sol::protected_function_result result=iterator->coroutine();if(!result.valid()){const sol::error error=result;HandleError("await",error.what());iterator=m_pending.erase(iterator);continue;}if(result.status()==sol::call_status::yielded){++iterator->yieldIndex;iterator->waitKind=result.return_count()>=1?result.get<std::string>(0):std::string{};iterator->handle=iterator->waitKind=="animation"&&result.return_count()>=2?result.get<std::uint64_t>(1):0;iterator->remainingSeconds=iterator->waitKind=="timer"&&result.return_count()>=2?result.get<float>(1):0.0f;++iterator;}else iterator=m_pending.erase(iterator);}
    for(auto iterator=m_pendingActions.begin();iterator!=m_pendingActions.end();){bool ready=false;if(iterator->waitKind=="animation")ready=!m_services.timeline||!m_services.timeline->Playing(iterator->handle);else if(iterator->waitKind=="timer"){iterator->remainingSeconds-=std::max(0.0f,deltaSeconds);ready=iterator->remainingSeconds<=0.0f;}else ready=true;if(!ready){++iterator;continue;}sol::protected_function_result result=iterator->coroutine();if(!result.valid()){const sol::error error=result;HandleError("action:"+iterator->action,error.what());m_actionTerminalStates[iterator->id]=ui::ActionExecutionState::Failed;iterator=m_pendingActions.erase(iterator);continue;}if(result.status()==sol::call_status::yielded){++iterator->yieldIndex;iterator->waitKind=result.return_count()>=1?result.get<std::string>(0):std::string{};iterator->handle=iterator->waitKind=="animation"&&result.return_count()>=2?result.get<std::uint64_t>(1):0;iterator->remainingSeconds=iterator->waitKind=="timer"&&result.return_count()>=2?result.get<float>(1):0.0f;++iterator;}else{m_actionTerminalStates[iterator->id]=ui::ActionExecutionState::Completed;iterator=m_pendingActions.erase(iterator);}}}

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
        sol::state_view runnerState=m_runner.state();
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
        PendingActionCoroutine pending{std::move(coroutine)};
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
    m_runner = sol::thread::create(m_lua);
}

}
