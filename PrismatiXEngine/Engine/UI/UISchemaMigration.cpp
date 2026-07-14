#include "Engine/UI/UISchemaMigration.h"

#include "Engine/IO/AtomicFile.h"
#include "Engine/UI/Actions/TriggerBinding.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace px::ui {
namespace {

diag::Diagnostic MigrationError(std::string code,std::string message,const std::string& path={}){
    diag::Diagnostic diagnostic{.severity=diag::Severity::Error,.code=std::move(code),
        .category="UI.Migration",.message=std::move(message)};diagnostic.source.path=path;return diagnostic;
}

Status ConvertBinding(Variant& value,const std::string& path){
    auto* object=value.AsObject();if(!object)return Status::Fail(MigrationError("PXUIMIG5001","Legacy event binding must be an Object",path));const auto modeIt=object->find("mode");const auto* mode=modeIt==object->end()?nullptr:modeIt->second.TryGet<std::string>();if(!mode||(*mode!="action"&&*mode!="graph"))return Status::Fail(MigrationError("PXUIMIG5002","Legacy event binding mode must be action or graph",path));(*object)["kind"]=std::string(*mode=="graph"?"flow":"action");object->erase("mode");return Status::Ok();
}

Status ConvertBindingMap(Variant& value,const std::string& path){auto* bindings=value.AsObject();if(!bindings)return Status::Fail(MigrationError("PXUIMIG5003","Legacy event collection must be an Object",path));for(auto& [name,binding]:*bindings){const Status status=ConvertBinding(binding,path+"."+name);if(!status)return status;}return Status::Ok();}

bool Number(const Variant& value,double& output){if(const auto* number=value.TryGet<double>()){output=*number;return true;}if(const auto* integer=value.TryGet<std::int64_t>()){output=static_cast<double>(*integer);return true;}return false;}

Result<AnimationClip> LegacyClip(const resource::TypedDocument& document,const std::string& path){
    AnimationClip clip;clip.id=Uuid::FromName(document.id.ToString()+"/animations/Default");clip.name="Default";
    if(const auto found=document.properties.find("animation.duration");found!=document.properties.end()){double value=0;if(!Number(found->second,value))return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5010","animation.duration must be Number",path));clip.duration=static_cast<float>(value);}
    if(const auto found=document.properties.find("animation.loop");found!=document.properties.end()){const auto* value=found->second.TryGet<bool>();if(!value)return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5011","animation.loop must be Bool",path));clip.loop=*value;}
    const auto tracksIt=document.properties.find("animation.tracks");const auto* tracks=tracksIt==document.properties.end()?nullptr:tracksIt->second.AsArray();if(!tracks)return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5012","animation.tracks must be an Array",path));
    for(const auto& trackValue:*tracks){const auto* object=trackValue.AsObject();if(!object)return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5013","Legacy animation track must be an Object",path));AnimationTrack track;const auto nodeIt=object->find("node"),propertyIt=object->find("property"),keysIt=object->find("keys");const auto* node=nodeIt==object->end()?nullptr:nodeIt->second.TryGet<Uuid>();const auto* property=propertyIt==object->end()?nullptr:propertyIt->second.TryGet<std::string>();const auto* keys=keysIt==object->end()?nullptr:keysIt->second.AsArray();if(!node||!property||!keys)return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5014","Legacy animation track fields are invalid",path));track.node=*node;track.property=*property;
        for(const auto& keyValue:*keys){const auto* key=keyValue.AsObject();if(!key)return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5015","Legacy animation key must be an Object",path));const auto timeIt=key->find("time"),valueIt=key->find("value");double time=0;if(timeIt==key->end()||!Number(timeIt->second,time)||valueIt==key->end())return Result<AnimationClip>::Failure(MigrationError("PXUIMIG5016","Legacy animation key fields are invalid",path));Ease ease=Ease::Linear;KeyInterpolation interpolation=KeyInterpolation::Linear;if(const auto found=key->find("ease");found!=key->end())if(const auto* text=found->second.TryGet<std::string>()){if(*text=="EaseIn")ease=Ease::EaseIn;else if(*text=="EaseOut")ease=Ease::EaseOut;else if(*text=="EaseInOut")ease=Ease::EaseInOut;else if(*text=="Step")ease=Ease::Step;}if(const auto found=key->find("interpolation");found!=key->end())if(const auto* text=found->second.TryGet<std::string>())if(*text=="Discrete"||*text=="Step")interpolation=KeyInterpolation::Discrete;track.keys.push_back({static_cast<float>(time),valueIt->second.Clone(),ease,interpolation});}clip.tracks.push_back(std::move(track));}
    const Status valid=clip.Validate();return valid?Result<AnimationClip>::Success(std::move(clip)):Result<AnimationClip>::Failure(valid.Diagnostics());
}

Result<std::string> ReadText(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);if(!stream)return Result<std::string>::Failure(MigrationError("PXUIMIG5020","Cannot read UI document",path.generic_string()));std::ostringstream text;text<<stream.rdbuf();return Result<std::string>::Success(text.str());}

}  // namespace

Result<resource::TypedDocument> MigrateUIDocumentV4(const resource::TypedDocument& input,const std::string& sourcePath){
    if(input.kind!=resource::DocumentKind::Scene||(input.type!="UIScene"&&input.type!="UIComponent"))return Result<resource::TypedDocument>::Failure(MigrationError("PXUIMIG5030","Document is not a UI Scene or Component",sourcePath));const auto schemaIt=input.properties.find("uiSchemaVersion");const auto* schema=schemaIt==input.properties.end()?nullptr:schemaIt->second.TryGet<std::int64_t>();if(!schema||*schema!=4)return Result<resource::TypedDocument>::Failure(MigrationError("PXUIMIG5031","Migration input must use uiSchemaVersion 4",sourcePath));resource::TypedDocument document=input;
    if(document.properties.contains("interactionGraph")||document.properties.contains("animations"))return Result<resource::TypedDocument>::Failure(MigrationError("PXUIMIG5032","v4 document already contains v5-only fields",sourcePath));
    if(const auto found=document.properties.find("behaviorGraph");found!=document.properties.end()){document.properties["interactionGraph"]=found->second.Clone();document.properties.erase("behaviorGraph");}
    for(auto& node:document.nodes){if(const auto found=node.properties.find("events");found!=node.properties.end()){Variant converted=found->second.Clone();const Status status=ConvertBindingMap(converted,sourcePath+"#"+node.name+".events");if(!status)return Result<resource::TypedDocument>::Failure(status.Diagnostics());node.properties["triggers"]=std::move(converted);node.properties.erase("events");}if(const auto found=node.properties.find("componentEvents");found!=node.properties.end()){Variant converted=found->second.Clone();const Status status=ConvertBindingMap(converted,sourcePath+"#"+node.name+".componentEvents");if(!status)return Result<resource::TypedDocument>::Failure(status.Diagnostics());node.properties["componentEvents"]=std::move(converted);}}
    if(document.properties.contains("animation.tracks")){auto clip=LegacyClip(document,sourcePath);if(!clip)return Result<resource::TypedDocument>::Failure(clip.Diagnostics());const Uuid stateId=Uuid::FromName(document.id.ToString()+"/animations/DefaultState");UIAnimationLibrary library;library.clips.push_back(clip.TakeValue());library.machine.entry=stateId;library.machine.states.push_back({stateId,"Default",library.clips.front().id,{80,80}});document.properties["animations"]=WriteUIAnimationLibrary(library);document.properties.erase("animation.duration");document.properties.erase("animation.loop");document.properties.erase("animation.tracks");}
    document.properties["uiSchemaVersion"]=std::int64_t{5};
    if(const auto graph=document.properties.find("interactionGraph");graph!=document.properties.end()){auto parsed=ParseBehaviorGraph(graph->second,sourcePath);if(!parsed)return Result<resource::TypedDocument>::Failure(parsed.Diagnostics());}
    if(const auto animations=document.properties.find("animations");animations!=document.properties.end()){auto parsed=ParseUIAnimationLibrary(animations->second,sourcePath);if(!parsed)return Result<resource::TypedDocument>::Failure(parsed.Diagnostics());}
    for(const auto& node:document.nodes)if(const auto found=node.properties.find("triggers");found!=node.properties.end()){const auto* triggers=found->second.AsObject();if(!triggers)return Result<resource::TypedDocument>::Failure(MigrationError("PXUIMIG5033","Migrated triggers must be an Object",sourcePath));for(const auto& [signal,value]:*triggers){auto parsed=ParseTriggerBinding(node.id,signal,value,sourcePath);if(!parsed)return Result<resource::TypedDocument>::Failure(parsed.Diagnostics());}}
    return Result<resource::TypedDocument>::Success(std::move(document));
}

Result<UIMigrationReport> MigrateUIProjectV5(const std::filesystem::path& projectRoot,const bool write){
    UIMigrationReport report;std::error_code error;if(!std::filesystem::is_directory(projectRoot,error))return Result<UIMigrationReport>::Failure(MigrationError("PXUIMIG5040","Project root does not exist",projectRoot.generic_string()));
    static const std::vector<std::string> ignoredDirectories{
        ".git", ".prismatix", "build", "out", "Export", "Save", "logs"};
    for(std::filesystem::recursive_directory_iterator iterator(projectRoot,error),end;iterator!=end&&!error;iterator.increment(error)){
        if(iterator->is_directory()){
            const auto name=iterator->path().filename().string();
            if(std::find(ignoredDirectories.begin(),ignoredDirectories.end(),name)!=ignoredDirectories.end())
                iterator.disable_recursion_pending();
            continue;
        }
        if(!iterator->is_regular_file())continue;
        const auto extension=iterator->path().extension().string();if(extension!=".pxscene"&&extension!=".pxcomponent")continue;++report.scanned;auto text=ReadText(iterator->path());if(!text)return Result<UIMigrationReport>::Failure(text.Diagnostics());auto parsed=resource::ParseTypedDocument(text.Value(),iterator->path().generic_string());if(!parsed)return Result<UIMigrationReport>::Failure(parsed.Diagnostics());const auto schemaIt=parsed.Value().properties.find("uiSchemaVersion");const auto* schema=schemaIt==parsed.Value().properties.end()?nullptr:schemaIt->second.TryGet<std::int64_t>();if(schema&&*schema==5){++report.alreadyCurrent;continue;}auto migrated=MigrateUIDocumentV4(parsed.Value(),iterator->path().generic_string());if(!migrated)return Result<UIMigrationReport>::Failure(migrated.Diagnostics());report.changed.push_back({iterator->path(),text.TakeValue(),resource::WriteTypedDocument(migrated.Value())});}
    if(error)return Result<UIMigrationReport>::Failure(MigrationError("PXUIMIG5041","Failed while scanning the project",projectRoot.generic_string()));if(!write)return Result<UIMigrationReport>::Success(std::move(report));std::vector<std::size_t> committed;for(std::size_t index=0;index<report.changed.size();++index){const Status status=io::AtomicFile::WriteText(report.changed[index].path,report.changed[index].after);if(!status){Status rollback;for(auto iterator=committed.rbegin();iterator!=committed.rend();++iterator){const Status restored=io::AtomicFile::WriteText(report.changed[*iterator].path,report.changed[*iterator].before);for(const auto& diagnostic:restored.Diagnostics())rollback.Add(diagnostic);}std::vector<diag::Diagnostic> diagnostics=status.Diagnostics();for(const auto& diagnostic:rollback.Diagnostics())diagnostics.push_back(diagnostic);return Result<UIMigrationReport>::Failure(std::move(diagnostics));}committed.push_back(index);}return Result<UIMigrationReport>::Success(std::move(report));
}

}  // namespace px::ui
