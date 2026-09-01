#include "Engine/VN/Commands/CommandRegistry.h"

#include "Engine/Core/NumberParsing.h"
#include "Engine/VN/Expression/Expression.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace px::vn {
namespace {

diag::Diagnostic SchemaError(std::string code, std::string message, const std::string& path,
                             int line = 0, std::string property = {}, const Uuid& nodeId = {}) {
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),
                                .category="VN.CommandSchema",.message=std::move(message)};
    diagnostic.source.path=path;diagnostic.source.line=line;
    diagnostic.source.property=std::move(property);
    if(!nodeId.Empty())diagnostic.source.nodeId=nodeId.ToString();
    return diagnostic;
}

bool StringMatchesType(std::string_view value, const CommandParameterDescriptor& parameter) {
    if (value.empty()) {
        return !parameter.required &&
               (parameter.type == VariantType::String ||
                parameter.type == VariantType::ResourceRef ||
                parameter.type == VariantType::Null);
    }
    if (!parameter.options.empty()) {
        bool found=false;for(const auto& option:parameter.options)if(option==value){found=true;break;}
        if(!found)return false;
    }
    if (parameter.type == VariantType::Integer) {
        std::int64_t parsed=0;const auto result=std::from_chars(value.data(),value.data()+value.size(),parsed);
        if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size())return false;
        const double numeric=static_cast<double>(parsed);
        return (!parameter.minimum||numeric>=*parameter.minimum)&&
               (!parameter.maximum||numeric<=*parameter.maximum);
    }
    if (parameter.type == VariantType::Number) {
        double parsed=0;if(!ParseFiniteDouble(value,parsed))return false;
        return (!parameter.minimum||parsed>=*parameter.minimum)&&
               (!parameter.maximum||parsed<=*parameter.maximum);
    }
    if (parameter.type == VariantType::Bool)
        return value=="true"||value=="false"||value=="1"||value=="0"||value=="yes"||value=="no";
    return true;
}

bool VariantMatchesType(const Variant& value, VariantType expected) {
    if (expected == VariantType::Null) return true;
    if (value.Type() == expected) return true;
    if (expected == VariantType::Number && value.Type() == VariantType::Integer) return true;
    return false;
}

std::optional<double> NumericValue(const Variant& value) {
    if (const auto* number = value.TryGet<double>()) return *number;
    if (const auto* integer = value.TryGet<std::int64_t>()) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

bool VariantMatchesConstraints(const Variant& value,
                               const CommandParameterDescriptor& parameter) {
    if (!parameter.options.empty()) {
        const auto* text = value.TryGet<std::string>();
        if (!text || std::ranges::find(parameter.options, *text) == parameter.options.end()) {
            return false;
        }
    }
    if (parameter.minimum || parameter.maximum) {
        const auto numeric = NumericValue(value);
        if (!numeric || (parameter.minimum && *numeric < *parameter.minimum) ||
            (parameter.maximum && *numeric > *parameter.maximum)) {
            return false;
        }
    }
    return true;
}

bool EditorWidgetMatchesType(const CommandEditorWidget widget,
                             const VariantType type,
                             const bool hasOptions) {
    switch (widget) {
        case CommandEditorWidget::Default:
        case CommandEditorWidget::Hidden:
            return true;
        case CommandEditorWidget::Multiline:
        case CommandEditorWidget::Route:
        case CommandEditorWidget::Character:
        case CommandEditorWidget::Target:
            return type == VariantType::String;
        case CommandEditorWidget::Enum:
            return type == VariantType::String && hasOptions;
        case CommandEditorWidget::Color:
            return type == VariantType::Color;
        case CommandEditorWidget::Resource:
            return type == VariantType::ResourceRef;
        case CommandEditorWidget::Node:
            return type == VariantType::Uuid;
        case CommandEditorWidget::Animation:
        case CommandEditorWidget::Preset:
            return type == VariantType::ResourceRef || type == VariantType::String;
        case CommandEditorWidget::Token:
            return type == VariantType::TokenRef;
        case CommandEditorWidget::Expression:
            return type == VariantType::Object;
    }
    return false;
}

CommandParameterDescriptor Param(std::string name, VariantType type, bool required=false,
                                 Variant fallback={}, CommandEditorWidget widget=CommandEditorWidget::Default,
                                 std::vector<std::string> options={}) {
    CommandParameterDescriptor value;value.name=std::move(name);value.label=value.name;
    value.type=type;value.required=required;value.defaultValue=std::move(fallback);
    value.hasDefault=value.defaultValue.Type()!=VariantType::Null;
    value.widget=widget;value.options=std::move(options);return value;
}

CommandDescriptor Descriptor(std::string id, std::string category,
                             std::vector<CommandParameterDescriptor> parameters={},
                             CommandWaitPolicy wait=CommandWaitPolicy::Immediate,
                             RollbackPolicy rollback=RollbackPolicy::Reversible,
                             bool additional=true) {
    CommandDescriptor value;value.id=std::move(id);value.displayName=value.id;
    value.category=std::move(category);value.parameters=std::move(parameters);
    value.waitPolicy=wait;value.rollbackPolicy=rollback;
    value.previewSafe=rollback!=RollbackPolicy::Boundary;
    value.deterministic=true;
    value.seekSafe=rollback==RollbackPolicy::Reversible&&wait!=CommandWaitPolicy::Async;
    value.rollbackSafe=rollback==RollbackPolicy::Reversible;
    value.allowAdditionalParameters=additional;return value;
}

CommandRegistry MakeBuiltins() {
    CommandRegistry registry;
    const auto add=[&](CommandDescriptor descriptor){(void)registry.Register(std::move(descriptor));};
    const auto textParams=[] { return std::vector<CommandParameterDescriptor>{
        Param("textId",VariantType::String,true),Param("speaker",VariantType::String),Param("value",VariantType::String,false,{},CommandEditorWidget::Multiline),
        Param("voice",VariantType::ResourceRef,false,{},CommandEditorWidget::Resource),
        Param("speed",VariantType::Integer),Param("color",VariantType::String),
        Param("outline",VariantType::String),Param("effect",VariantType::String)};};
    add(Descriptor("say","Dialogue",textParams(),CommandWaitPolicy::Input));
    add(Descriptor("text","Dialogue",textParams(),CommandWaitPolicy::Input));
    add(Descriptor("name","Dialogue",{Param("speaker",VariantType::String)}));
    add(Descriptor("speaker","Dialogue",{Param("name",VariantType::String)}));
    add(Descriptor("chapter","Flow",{Param("title",VariantType::String,true)},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,false));
    add(Descriptor("choice","Flow",{Param("textId",VariantType::String,true),Param("text",VariantType::String,true,{},CommandEditorWidget::Multiline),Param("target",VariantType::String,false,{},CommandEditorWidget::Target)},CommandWaitPolicy::Input,RollbackPolicy::Reversible,true));
    add(Descriptor("label","Flow",{Param("name",VariantType::String,true,{},CommandEditorWidget::Target)},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,false));
    add(Descriptor("jump","Flow",{Param("target",VariantType::String,false,{},CommandEditorWidget::Target)},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,true));
    add(Descriptor("call","Flow",{Param("target",VariantType::String,false,{},CommandEditorWidget::Target)},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,true));
    add(Descriptor("return","Flow",{},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,false));
    add(Descriptor("branch","Logic",{Param("expression",VariantType::Object,true,{},CommandEditorWidget::Expression),Param("trueTarget",VariantType::String,false,{},CommandEditorWidget::Target),Param("falseTarget",VariantType::String,false,{},CommandEditorWidget::Target)},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,false));
    add(Descriptor("var","Logic",{Param("name",VariantType::String,true),Param("value",VariantType::Null),Param("add",VariantType::Number),Param("scope",VariantType::String,false,Variant("session"),CommandEditorWidget::Default,{"session","profile"})},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,false));
    add(Descriptor("bg","Stage",{Param("file",VariantType::ResourceRef,true,{},CommandEditorWidget::Resource),Param("rule",VariantType::ResourceRef,false,{},CommandEditorWidget::Resource),Param("time",VariantType::Integer),Param("vague",VariantType::Integer)}));
    add(Descriptor("layer","Stage",{Param("name",VariantType::String,true),Param("file",VariantType::ResourceRef,false,{},CommandEditorWidget::Resource),Param("x",VariantType::Number),Param("y",VariantType::Number),Param("scale",VariantType::Number),Param("alpha",VariantType::Integer),Param("z",VariantType::Integer)}));
    add(Descriptor("layer_clear","Stage",{Param("name",VariantType::String,true)}));
    add(Descriptor("char","Stage",{Param("id",VariantType::String,true,{},CommandEditorWidget::Character),Param("file",VariantType::ResourceRef,false,{},CommandEditorWidget::Resource),Param("expression",VariantType::String),Param("pos",VariantType::Integer),Param("x",VariantType::Number),Param("y",VariantType::Number),Param("scale",VariantType::Number)}));
    add(Descriptor("char_clear","Stage",{Param("id",VariantType::String,true,{},CommandEditorWidget::Character)}));
    add(Descriptor("char_move","Stage",{Param("id",VariantType::String,true,{},CommandEditorWidget::Character),Param("pos",VariantType::Integer)}));
    add(Descriptor("move","Stage",{Param("id",VariantType::String,true,{},CommandEditorWidget::Character),Param("pos",VariantType::Integer)}));
    add(Descriptor("anim","Animation",{Param("target",VariantType::String,true),Param("x",VariantType::Number),Param("y",VariantType::Number),Param("scale",VariantType::Number),Param("alpha",VariantType::Number),Param("duration",VariantType::Integer),Param("ease",VariantType::String),Param("wait",VariantType::Bool)},CommandWaitPolicy::Async));
    add(Descriptor("tween","Animation",registry.Find("anim")->parameters,CommandWaitPolicy::Async));
    add(Descriptor("shake","Animation",{Param("ms",VariantType::Integer),Param("amp",VariantType::Number)}));
    add(Descriptor("quake","Animation",{Param("time",VariantType::Integer),Param("power",VariantType::Number)}));
    add(Descriptor("wait","Flow",{Param("ms",VariantType::Integer,true)},CommandWaitPolicy::Timer));
    for(const char* id:{"bgm","se","voice","ambience"})add(Descriptor(id,"Audio",{Param("file",VariantType::ResourceRef,true,{},CommandEditorWidget::Resource),Param("fade",VariantType::Integer)}));
    add(Descriptor("stopbgm","Audio",{Param("fade",VariantType::Integer)}));
    add(Descriptor("stopambience","Audio",{Param("fade",VariantType::Integer)}));
    add(Descriptor("video","Video",{Param("file",VariantType::ResourceRef,true,{},CommandEditorWidget::Resource),Param("skippable",VariantType::Bool)},CommandWaitPolicy::Async,RollbackPolicy::Boundary));
    add(Descriptor("movie","Video",registry.Find("video")->parameters,CommandWaitPolicy::Async,RollbackPolicy::Boundary));
    add(Descriptor("route","UI",{Param("route",VariantType::String,true),Param("operation",VariantType::String,false,Variant("replace"),CommandEditorWidget::Default, {"push","replace","modal","back"})},CommandWaitPolicy::Immediate,RollbackPolicy::Reversible,false));
    add(Descriptor("animation","Animation",{Param("clip",VariantType::ResourceRef,true,{},CommandEditorWidget::Preset),Param("await",VariantType::Bool),Param("speed",VariantType::Number)},CommandWaitPolicy::Async));
    add(Descriptor("screen_effect","Animation",{Param("preset",VariantType::String,true,{},CommandEditorWidget::Preset),Param("duration",VariantType::Integer),Param("await",VariantType::Bool)},CommandWaitPolicy::Async));
    add(Descriptor("cg","Stage",{Param("id",VariantType::String),Param("image",VariantType::ResourceRef,true,{},CommandEditorWidget::Resource)}));
    add(Descriptor("unlock","Progression",{Param("kind",VariantType::String),Param("id",VariantType::String,true)},CommandWaitPolicy::Immediate,RollbackPolicy::Boundary));
    add(Descriptor("speed","Dialogue",{Param("value",VariantType::Integer,true)}));
    for(const char* id:{"nvl","adv","er","nvl_clear"})add(Descriptor(id,"Dialogue"));
    return registry;
}

}  // namespace

Status CommandRegistry::NormalizeAndValidateDescriptor(CommandDescriptor& descriptor) {
    if(descriptor.id.empty())return Status::Fail(SchemaError("PXSCHEMA7001","Command id cannot be empty",{}));
    std::unordered_set<std::string> names;
    for(const auto& parameter:descriptor.parameters){
        if(parameter.name.empty()||!names.insert(parameter.name).second)
            return Status::Fail(SchemaError("PXSCHEMA7003","Invalid or duplicate parameter in command "+descriptor.id,{},0,parameter.name));
        if((parameter.minimum||parameter.maximum)&&parameter.type!=VariantType::Integer&&parameter.type!=VariantType::Number)
            return Status::Fail(SchemaError("PXSCHEMA7011","Numeric range requires an integer or number parameter in command "+descriptor.id,{},0,parameter.name));
        if((parameter.minimum&&!std::isfinite(*parameter.minimum))||
           (parameter.maximum&&!std::isfinite(*parameter.maximum)))
            return Status::Fail(SchemaError("PXSCHEMA7015","Numeric range must use finite bounds in command "+descriptor.id,{},0,parameter.name));
        if(parameter.minimum&&parameter.maximum&&*parameter.minimum>*parameter.maximum)
            return Status::Fail(SchemaError("PXSCHEMA7012","Parameter minimum exceeds maximum in command "+descriptor.id,{},0,parameter.name));
        if(!parameter.options.empty()&&parameter.type!=VariantType::String)
            return Status::Fail(SchemaError("PXSCHEMA7013","Parameter options require string type in command "+descriptor.id,{},0,parameter.name));
        std::unordered_set<std::string> options;
        for(const auto& option:parameter.options)if(option.empty()||!options.insert(option).second)
            return Status::Fail(SchemaError("PXSCHEMA7016","Parameter options must be non-empty and unique in command "+descriptor.id,{},0,parameter.name));
        if(!parameter.resourceType.empty()&&parameter.type!=VariantType::ResourceRef)
            return Status::Fail(SchemaError("PXSCHEMA7017","Resource filter requires a resource parameter in command "+descriptor.id,{},0,parameter.name));
        if(!EditorWidgetMatchesType(parameter.widget,parameter.type,!parameter.options.empty()))
            return Status::Fail(SchemaError("PXSCHEMA7018","Editor hint does not match parameter type in command "+descriptor.id,{},0,parameter.name));
        if(parameter.hasDefault&&(!VariantMatchesType(parameter.defaultValue,parameter.type)||
                                  !VariantMatchesConstraints(parameter.defaultValue,parameter)))
            return Status::Fail(SchemaError("PXSCHEMA7014","Parameter default does not satisfy its schema in command "+descriptor.id,{},0,parameter.name));
    }
    return Status::Ok();
}

Status CommandRegistry::Register(CommandDescriptor descriptor) {
    const Status valid = NormalizeAndValidateDescriptor(descriptor);
    if (!valid) return valid;
    if(m_byId.contains(descriptor.id))return Status::Fail(SchemaError("PXSCHEMA7002","Duplicate command id: "+descriptor.id,{}));
    m_byId[descriptor.id]=m_descriptors.size();m_descriptors.push_back(std::move(descriptor));return Status::Ok();
}

Status CommandRegistry::ReplaceSource(const std::string_view sourceId,
                                      std::vector<CommandDescriptor> descriptors) {
    if (sourceId.empty())
        return Status::Fail(SchemaError("PXSCHEMA7020", "Command source id cannot be empty", {}));
    std::unordered_set<std::string> incoming;
    for (auto& descriptor : descriptors) {
        descriptor.sourceId = std::string(sourceId);
        const Status valid = NormalizeAndValidateDescriptor(descriptor);
        if (!valid) return valid;
        if (!incoming.insert(descriptor.id).second)
            return Status::Fail(SchemaError("PXSCHEMA7002", "Duplicate command id: " + descriptor.id, {}));
        if (const auto* current = Find(descriptor.id);
            current && current->sourceId != sourceId)
            return Status::Fail(SchemaError("PXSCHEMA7002", "Duplicate command id: " + descriptor.id, {}));
    }

    std::vector<CommandDescriptor> next;
    next.reserve(m_descriptors.size() + descriptors.size());
    for (auto& descriptor : m_descriptors)
        if (descriptor.sourceId != sourceId) next.push_back(std::move(descriptor));
    for (auto& descriptor : descriptors) next.push_back(std::move(descriptor));
    m_descriptors = std::move(next);
    Reindex();
    return Status::Ok();
}

Status CommandRegistry::RemoveSource(const std::string_view sourceId) {
    if (sourceId.empty()) return Status::Ok();
    std::erase_if(m_descriptors, [&](const CommandDescriptor& descriptor) {
        return descriptor.sourceId == sourceId;
    });
    Reindex();
    return Status::Ok();
}

void CommandRegistry::Reindex() {
    m_byId.clear();
    for (std::size_t index = 0; index < m_descriptors.size(); ++index)
        m_byId[m_descriptors[index].id] = index;
}

const CommandDescriptor* CommandRegistry::Find(std::string_view id) const {
    const auto found=m_byId.find(std::string(id));return found==m_byId.end()?nullptr:&m_descriptors[found->second];
}

Status CommandRegistry::Validate(const Command& command,const std::string& sourcePath) const {
    const auto* descriptor=Find(command.type);
    if(!descriptor)return Status::Fail(SchemaError("PXSCHEMA7004","Unknown command: "+command.type,sourcePath,command.line));
    Status status;
    std::unordered_set<std::string> accepted;
    for(const auto& parameter:descriptor->parameters){
        accepted.insert(parameter.name);
        const std::string* value=command.Find(parameter.name);
        const Variant* typed=command.FindTyped(parameter.name);
        if(!value&&!typed){if(parameter.required)status.Add(SchemaError("PXSCHEMA7005","Missing required parameter '"+parameter.name+"' for "+command.type,sourcePath,command.line,parameter.name));continue;}
        if(typed&&(!VariantMatchesType(*typed,parameter.type)||!VariantMatchesConstraints(*typed,parameter)))status.Add(SchemaError("PXSCHEMA7008","Wrong typed value for "+command.type+"."+parameter.name,sourcePath,command.line,parameter.name));
        else if(value&&!StringMatchesType(*value,parameter))status.Add(SchemaError("PXSCHEMA7006","Invalid value for "+command.type+"."+parameter.name,sourcePath,command.line,parameter.name));
    }
    if(!descriptor->allowAdditionalParameters)for(const auto& arg:command.args)if(!accepted.contains(arg.key))
        status.Add(SchemaError("PXSCHEMA7007","Unknown parameter '"+arg.key+"' for "+command.type,sourcePath,command.line,arg.key));
    if(!descriptor->allowAdditionalParameters)for(const auto& [name,_]:command.typedArgs)if(!accepted.contains(name))
        status.Add(SchemaError("PXSCHEMA7007","Unknown typed parameter '"+name+"' for "+command.type,sourcePath,command.line,name));
    if((command.type=="choice"||command.type=="jump"||command.type=="call")&&
       !command.Has("target")&&
       !(command.FindTyped("targetScenario")&&command.FindTyped("targetEntry")))
        status.Add(SchemaError("PXSCHEMA7005","Command requires an explicit target",sourcePath,command.line,"target"));
    return status;
}

Status CommandRegistry::ValidateParameters(std::string_view commandId,const VariantObject& parameters,
                                           const std::string& sourcePath,const Uuid& nodeId) const {
    const auto* descriptor=Find(commandId);
    if(!descriptor)return Status::Fail(SchemaError("PXSCHEMA7004","Unknown command: "+std::string(commandId),sourcePath,0,{},nodeId));
    Status status;std::unordered_set<std::string> accepted;
    for(const auto& parameter:descriptor->parameters){
        accepted.insert(parameter.name);
        auto found=parameters.find(parameter.name);
        if(found==parameters.end()){if(parameter.required)status.Add(SchemaError("PXSCHEMA7005","Missing required parameter '"+parameter.name+"' for "+std::string(commandId),sourcePath,0,parameter.name,nodeId));continue;}
        if(!VariantMatchesType(found->second,parameter.type)||!VariantMatchesConstraints(found->second,parameter))status.Add(SchemaError("PXSCHEMA7008","Wrong typed value for "+std::string(commandId)+"."+parameter.name,sourcePath,0,parameter.name,nodeId));
        else if(parameter.required && parameter.type==VariantType::String && found->second.TryGet<std::string>()->empty())
            status.Add(SchemaError("PXSCHEMA7009","Required string is empty for "+std::string(commandId)+"."+parameter.name,sourcePath,0,parameter.name,nodeId));
        else if(parameter.required && parameter.type==VariantType::ResourceRef && found->second.TryGet<ResourceRefValue>()->id.Empty())
            status.Add(SchemaError("PXSCHEMA7010","Required resource is not selected for "+std::string(commandId)+"."+parameter.name,sourcePath,0,parameter.name,nodeId));
    }
    if(!descriptor->allowAdditionalParameters)for(const auto& [name,_]:parameters)if(!accepted.contains(name))status.Add(SchemaError("PXSCHEMA7007","Unknown parameter '"+name+"' for "+std::string(commandId),sourcePath,0,name,nodeId));
    if(commandId=="branch"){
        const auto expression=parameters.find("expression");
        if(expression!=parameters.end()&&expression->second.Type()==VariantType::Object){
            const auto parsed=ExpressionFromValue(expression->second,sourcePath);
            if(!parsed)for(auto diagnostic:parsed.Diagnostics()){diagnostic.source.nodeId=nodeId.ToString();diagnostic.source.property="expression";status.Add(std::move(diagnostic));}
        }
    }
    if(commandId=="jump"||commandId=="call"){
        const bool local=parameters.contains("target");
        const bool story=parameters.contains("targetScenario")&&parameters.contains("targetEntry");
        if(!local&&!story)status.Add(SchemaError("PXSCHEMA7005","Node requires a local or Story Map target",sourcePath,0,"target",nodeId));
    }
    return status;
}

CommandRegistry& CommandRegistry::Global(){static CommandRegistry registry=MakeBuiltins();return registry;}
const CommandRegistry& CommandRegistry::Builtins(){return Global();}

}  // namespace px::vn
