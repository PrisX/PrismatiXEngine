#include "Engine/VN/Runtime/Program.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/VN/Commands/CommandRegistry.h"

namespace px::vn {

Program CompileProgram(std::vector<Command> commands) {
    Program program;
    program.code = std::move(commands);
    program.branch.assign(program.code.size(), -1);
    const auto& registry = CommandRegistry::Builtins();
    for (const auto& command : program.code) {
        if (!registry.Find(command.type)) continue;
        const Status validation = registry.Validate(command);
        for (const auto& diagnostic : validation.Diagnostics())
            program.errors.push_back(diag::Describe(diagnostic) + " at statement " + std::to_string(command.line));
    }
    for (int i = 0; i < static_cast<int>(program.code.size()); ++i) {
        const auto& command = program.code[static_cast<std::size_t>(i)];
        if (command.type != "label") continue;
        const std::string name = command.Get("name");
        if (name.empty()) continue;
        if (program.labels.contains(name)) program.errors.push_back("duplicate label '" + name + "'");
        else program.labels[name] = i;
    }
    for (int i = 0; i < static_cast<int>(program.code.size()); ++i) {
        const auto& command = program.code[static_cast<std::size_t>(i)];
        if (command.type == "jump" || command.type == "call") {
            const std::string target = command.Get("target");
            if (!target.empty() && target.front() == '@') {
                const int destination = program.LabelIndex(target);
                if (destination < 0) program.errors.push_back("unknown statement target '" + target + "'");
                program.branch[i] = destination;
            }
        }
    }
    return program;
}

}  // namespace px::vn
