#pragma once

#include <map>
#include <memory>
#include <string>

#include "Core/VN/Commands/ICommandHandler.h"

namespace PrismatiX {
namespace VN {
namespace Commands {

std::map<std::string, std::unique_ptr<ICommandHandler>> CreateBuiltinHandlers();

}  // namespace Commands
}  // namespace VN
}  // namespace PrismatiX
