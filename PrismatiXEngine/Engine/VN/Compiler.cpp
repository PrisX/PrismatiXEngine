#include "Engine/VN/Compiler.h"

namespace px::vn {

namespace {
struct IfFrame {
    int ifIndex = -1;
    int elseIndex = -1;
};
}

Program Compile(const ParsedScript& parsed) {
    Program program;
    program.code = parsed.commands;
    program.branch.assign(program.code.size(), -1);

    for (int i = 0; i < static_cast<int>(program.code.size()); ++i) {
        const Command& cmd = program.code[i];
        if (cmd.type == "label") {
            const std::string name = cmd.Get("name");
            if (!name.empty()) {
                program.labels[name] = i;
            }
        }
    }

    std::vector<IfFrame> stack;
    for (int i = 0; i < static_cast<int>(program.code.size()); ++i) {
        const Command& cmd = program.code[i];

        if (cmd.type == "if") {
            stack.push_back(IfFrame{ i, -1 });
        } else if (cmd.type == "else") {
            if (stack.empty()) {
                program.errors.push_back("`else` without `if` at line " + std::to_string(cmd.line));
            } else {
                stack.back().elseIndex = i;
            }
        } else if (cmd.type == "endif") {
            if (stack.empty()) {
                program.errors.push_back("`endif` without `if` at line " +
                                         std::to_string(cmd.line));
            } else {
                const IfFrame frame = stack.back();
                stack.pop_back();
                if (frame.elseIndex >= 0) {
                    program.branch[frame.ifIndex] = frame.elseIndex + 1;
                    program.branch[frame.elseIndex] = i;
                } else {
                    program.branch[frame.ifIndex] = i;
                }
            }
        } else if (cmd.type == "jump" || cmd.type == "call") {
            const std::string target = cmd.Get("target", cmd.Get("name"));
            if (!target.empty() && (target[0] == '*' || program.LabelIndex(target) >= 0)) {
                const int idx = program.LabelIndex(target);
                if (idx < 0) {
                    program.errors.push_back("unknown label '" + target + "' at line " +
                                             std::to_string(cmd.line));
                }
                program.branch[i] = idx;
            }
        }
    }

    for (const IfFrame& frame : stack) {
        program.errors.push_back("`if` without matching `endif` at line " +
                                 std::to_string(program.code[frame.ifIndex].line));
    }

    return program;
}

}
