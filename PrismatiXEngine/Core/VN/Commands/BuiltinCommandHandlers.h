#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "Core/VN/Commands/ICommandHandler.h"

namespace PrismatiX::VN {
namespace Commands {

std::unordered_map<std::string, std::unique_ptr<ICommandHandler>> CreateBuiltinHandlers();

}  // namespace Commands
} // namespace PrismatiX::VN
