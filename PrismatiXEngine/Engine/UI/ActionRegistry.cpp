#include "Engine/UI/ActionRegistry.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::ui {
namespace {
Status Failure(std::string code,std::string message){diag::Diagnostic value{.severity=diag::Severity::Error,.code=std::move(code),.category="UI.Action",.message=std::move(message)};return Status::Fail(std::move(value));}
ActionRegistry MakeBuiltins(){ActionRegistry registry;const auto add=[&](const char* id,const char* label,const char* category,ActionArgumentType argument=ActionArgumentType::None){(void)registry.Register({id,label,category,argument});};
    add("game.start","Start Game","Game");add("app.quit","Quit","Application");add("overlay.close","Close / Back","Navigation");
    for(const auto& value:std::vector<std::pair<const char*,const char*>>{{"load.open","Open Load"},{"save.open","Open Save"},{"gallery.open","Open Gallery"},{"settings.open","Open Settings"},{"backlog.open","Open Backlog"}})add(value.first,value.second,"Navigation");
    add("choice.select","Select Choice","Scenario",ActionArgumentType::Integer);add("load.slot","Load Slot","Save",ActionArgumentType::Integer);add("save.slot","Save Slot","Save",ActionArgumentType::Integer);add("cg.view","View CG","Gallery",ActionArgumentType::String);add("backlog.voice","Replay Voice","Backlog",ActionArgumentType::Integer);add("backlog.rollback","Rollback","Backlog",ActionArgumentType::Integer);
    add("mode.auto","Toggle Auto","Game");add("mode.skip","Toggle Skip","Game");add("set.skipread.toggle","Toggle Skip Read","Settings");add("set.fullscreen.toggle","Toggle Fullscreen","Settings");
    add("hud.toolbar.pin","Pin / unpin HUD toolbar","HUD");
    for(const char* bus:{"bgm","se","voice","speed"}){add((std::string("set.")+bus+".up").c_str(),(std::string("Increase ")+bus).c_str(),"Settings");add((std::string("set.")+bus+".down").c_str(),(std::string("Decrease ")+bus).c_str(),"Settings");add((std::string("set.")+bus+".value").c_str(),(std::string("Set ")+bus).c_str(),"Settings",ActionArgumentType::Integer);}add("set.skipread.value","Set Skip Read","Settings",ActionArgumentType::Boolean);add("set.fullscreen.value","Set Fullscreen","Settings",ActionArgumentType::Boolean);add("set.textscale.value","Set Text Scale","Accessibility",ActionArgumentType::Integer);add("set.highcontrast.value","Set High Contrast","Accessibility",ActionArgumentType::Boolean);add("set.reducedmotion.value","Set Reduced Motion","Accessibility",ActionArgumentType::Boolean);add("set.selfvoicing.value","Set Self Voicing","Accessibility",ActionArgumentType::Boolean);return registry;}
}
Status ActionRegistry::Register(ActionDescriptor descriptor){if(descriptor.id.empty())return Failure("PXUIACTION2001","Action id is empty");if(m_byId.contains(descriptor.id))return Failure("PXUIACTION2002","Duplicate action: "+descriptor.id);m_byId[descriptor.id]=m_descriptors.size();m_descriptors.push_back(std::move(descriptor));return Status::Ok();}
const ActionDescriptor* ActionRegistry::Find(std::string_view id)const{const auto found=m_byId.find(std::string(id));return found==m_byId.end()?nullptr:&m_descriptors[found->second];}
const ActionRegistry& ActionRegistry::Builtins(){static const ActionRegistry registry=MakeBuiltins();return registry;}
}  // namespace px::ui
