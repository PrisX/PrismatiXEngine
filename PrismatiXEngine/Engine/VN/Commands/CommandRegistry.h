#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/VN/Commands/Command.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::vn {

enum class CommandWaitPolicy : std::uint8_t { Immediate, Input, Timer, Async };
enum class RollbackPolicy : std::uint8_t { Reversible, Boundary, Transient };
enum class CommandEditorWidget : std::uint8_t {
    Default,
    Multiline,
    Resource,
    Character,
    Expression,
    Target,
    Preset,
    Hidden,
    Enum,
    Color,
    Route,
    Node,
    Animation,
    Token,
};

struct CommandParameterDescriptor {
    std::string name;
    std::string label;
    std::string description;
    VariantType type = VariantType::String;
    Variant defaultValue;
    bool hasDefault = false;
    bool required = false;
    CommandEditorWidget widget = CommandEditorWidget::Default;
    std::string resourceType;
    std::vector<std::string> options;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

struct CommandExecutionContext {
    void* runtimeSession = nullptr;
};

enum class CommandExecutionState : std::uint8_t { Complete, Suspended, Error };
struct CommandExecutionResult {
    CommandExecutionState state = CommandExecutionState::Complete;
    Status status = Status::Ok();
};

using CommandExecutor =
    std::function<CommandExecutionResult(CommandExecutionContext&, const Command&)>;

struct CommandDescriptor {
    std::string id;
    int version = 1;
    std::string displayName;
    std::string description;
    std::string category;
    std::vector<CommandParameterDescriptor> parameters;
    CommandWaitPolicy waitPolicy = CommandWaitPolicy::Immediate;
    RollbackPolicy rollbackPolicy = RollbackPolicy::Reversible;
    bool previewSafe = true;
    bool deterministic = true;
    bool seekSafe = true;
    bool rollbackSafe = true;
    bool allowAdditionalParameters = false;
    CommandExecutor executor;
};

class CommandRegistry {
public:
    Status Register(CommandDescriptor descriptor);
    [[nodiscard]] const CommandDescriptor* Find(std::string_view id) const;
    [[nodiscard]] const std::vector<CommandDescriptor>& Descriptors() const {
        return m_descriptors;
    }

    [[nodiscard]] Status Validate(const Command& command, const std::string& sourcePath = {}) const;
    [[nodiscard]] Status ValidateParameters(std::string_view commandId,
                                            const VariantObject& parameters,
                                            const std::string& sourcePath = {},
                                            const Uuid& nodeId = {}) const;

    [[nodiscard]] static const CommandRegistry& Builtins();
    [[nodiscard]] static CommandRegistry& Global();

private:
    std::vector<CommandDescriptor> m_descriptors;
    std::unordered_map<std::string, std::size_t> m_byId;
};

}  // namespace px::vn
