#pragma once

namespace PrismatiX {
namespace Models {
struct VNCommand;
}
namespace VN {
namespace Commands {
struct VNContext;
}
}  // namespace VN
}  // namespace PrismatiX

namespace PrismatiX {
namespace VN {
namespace Commands {

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    virtual void Execute(const Models::VNCommand& cmd, VNContext& ctx) = 0;
};

}  // namespace Commands
}  // namespace VN
}  // namespace PrismatiX
