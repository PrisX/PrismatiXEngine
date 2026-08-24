#include "Engine/Script/ScriptHost.h"

#include "Engine/Script/JavaScriptHost.h"

namespace px::script {

std::unique_ptr<ScriptHost> CreateScriptHost(const ScriptServices& services) {
    return std::make_unique<JavaScriptHost>(services);
}

}  // namespace px::script
