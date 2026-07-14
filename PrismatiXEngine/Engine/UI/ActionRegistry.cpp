#include "Engine/UI/ActionRegistry.h"
#include "Engine/UI/Actions/ActionCatalog.h"

#include "Engine/Diagnostics/Diagnostic.h"

namespace px::ui {
namespace {
Status Failure(std::string code,std::string message){diag::Diagnostic value{.severity=diag::Severity::Error,.code=std::move(code),.category="UI.Action",.message=std::move(message)};return Status::Fail(std::move(value));}
}
Status ActionRegistry::Register(ActionDescriptor descriptor){if(descriptor.id.empty())return Failure("PXUIACTION2001","Action id is empty");if(m_byId.contains(descriptor.id))return Failure("PXUIACTION2002","Duplicate action: "+descriptor.id);m_byId[descriptor.id]=m_descriptors.size();m_descriptors.push_back(std::move(descriptor));return Status::Ok();}
const ActionDescriptor* ActionRegistry::Find(std::string_view id)const{const auto found=m_byId.find(std::string(id));return found==m_byId.end()?nullptr:&m_descriptors[found->second];}
const ActionRegistry& ActionRegistry::Builtins(){
    static const ActionRegistry registry=[] {
        ActionRegistry value;
        for (const auto& descriptor : ActionCatalog::Global().Descriptors())
            if (descriptor.origin == ActionOrigin::BuiltIn) (void)value.Register(descriptor);
        return value;
    }();
    return registry;
}
}  // namespace px::ui
