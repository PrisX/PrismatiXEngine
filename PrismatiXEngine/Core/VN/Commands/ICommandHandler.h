#pragma once

namespace PrismatiX::Models {
struct VNCommand;
}
namespace PrismatiX::VN {
namespace Commands {
struct VNContext;
}
} // namespace PrismatiX::VN

namespace PrismatiX::VN {
namespace Commands {

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    virtual void Execute(const PrismatiX::Models::VNCommand& cmd, VNContext& ctx) = 0;
};

}  // namespace Commands
} // namespace PrismatiX::VN
