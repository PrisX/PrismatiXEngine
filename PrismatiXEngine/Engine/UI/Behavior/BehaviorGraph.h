#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Actions/ActionDispatcher.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace px::ui {

enum class BehaviorNodeKind : std::uint8_t {
    SignalEntry,
    Action,
    Sequence,
    Branch,
    Delay,
    Constant,
    Compare,
    Boolean,
    GetVariable,
    SetVariable,
    GetProperty,
    SetProperty,
    PlayAnimation,
    SetAnimationParameter,
    TravelAnimationState,
};

struct BehaviorNode {
    Uuid id;
    BehaviorNodeKind kind = BehaviorNodeKind::Constant;
    Vec2 position{};
    VariantObject properties;
};

struct BehaviorLink {
    Uuid id;
    Uuid fromNode;
    std::string fromPin;
    Uuid toNode;
    std::string toPin;
};

struct BehaviorGroup {
    Uuid id;
    std::string title;
    Rect bounds{};
};

struct BehaviorGraph {
    static constexpr std::int64_t CurrentVersion = 1;
    std::int64_t version = CurrentVersion;
    std::vector<BehaviorNode> nodes;
    std::vector<BehaviorLink> links;
    std::vector<BehaviorGroup> groups;

    [[nodiscard]] const BehaviorNode* Find(const Uuid& id) const;
    [[nodiscard]] Status Validate(const std::string& sourcePath = {}) const;
};

[[nodiscard]] const char* BehaviorNodeKindName(BehaviorNodeKind kind);
[[nodiscard]] std::optional<BehaviorNodeKind> ParseBehaviorNodeKind(std::string_view value);
[[nodiscard]] Result<BehaviorGraph> ParseBehaviorGraph(const Variant& value,
                                                       const std::string& sourcePath = {});
[[nodiscard]] Variant WriteBehaviorGraph(const BehaviorGraph& graph);

using BehaviorFiberId = std::uint64_t;

struct BehaviorFiberState {
    BehaviorFiberId id = 0;
    Uuid entry;
    Uuid current;
    std::vector<Uuid> continuation;
    float delayRemaining = 0.0f;
    ActionExecutionId actionExecution = 0;
    std::uint64_t animationHandle = 0;
    VariantObject signalArguments;
};

struct BehaviorRuntimeState {
    std::vector<BehaviorFiberState> fibers;
    std::vector<ActionExecutionCheckpoint> actions;
};

struct BehaviorRuntimeServices {
    Control* root = nullptr;
    ActionDispatcher* actions = nullptr;
    std::function<std::optional<Variant>(std::string_view)> readVariable;
    std::function<Status(std::string_view, const Variant&)> writeVariable;
    std::function<Result<std::uint64_t>(std::string_view)> playAnimation;
    std::function<bool(std::uint64_t)> animationPlaying;
    std::function<Status(std::string_view,const Variant&)> setAnimationParameter;
    std::function<Status(std::string_view,float)> travelAnimationState;
};

class BehaviorGraphRunner {
public:
    explicit BehaviorGraphRunner(BehaviorRuntimeServices services = {})
        : m_services(std::move(services)) {}

    Status SetGraph(BehaviorGraph graph, std::string sourceScene = {});
    void SetServices(BehaviorRuntimeServices services) { m_services = std::move(services); }
    [[nodiscard]] Result<BehaviorFiberId> Start(
        const Uuid& entry, VariantObject signalArguments = {},
        ActionContext context = {},
        ActionReentryPolicy reentry = ActionReentryPolicy::Allow);
    void Update(float deltaSeconds);
    void CancelAll();
    [[nodiscard]] const std::vector<BehaviorFiberState>& ActiveFibers() const { return m_fibers; }
    [[nodiscard]] std::vector<BehaviorFiberState> CaptureState() const { return m_fibers; }
    [[nodiscard]] Status ValidateState(
        const std::vector<BehaviorFiberState>& state) const;
    Status RestoreState(std::vector<BehaviorFiberState> state);
    [[nodiscard]] const Status& LastFailure() const { return m_lastFailure; }

private:
    [[nodiscard]] const BehaviorLink* Incoming(const Uuid& node, std::string_view pin) const;
    [[nodiscard]] std::vector<const BehaviorLink*> Outgoing(const Uuid& node) const;
    [[nodiscard]] Result<Variant> Evaluate(const BehaviorFiberState& fiber, const Uuid& node,
                                           std::string_view output,
                                           std::unordered_set<Uuid, UuidHash>& visiting,
                                           std::size_t& budget) const;
    [[nodiscard]] Result<Variant> Input(const BehaviorFiberState& fiber,
                                        const BehaviorNode& node, std::string_view pin,
                                        const Variant* fallback,
                                        std::unordered_set<Uuid, UuidHash>& visiting,
                                        std::size_t& budget) const;
    Status Run(BehaviorFiberState& fiber, ActionContext context);
    void Advance(BehaviorFiberState& fiber, std::string_view pin = "out");
    void Fail(Status status);

    BehaviorGraph m_graph;
    BehaviorRuntimeServices m_services;
    std::string m_sourceScene;
    std::vector<BehaviorFiberState> m_fibers;
    BehaviorFiberId m_nextFiber = 1;
    Status m_lastFailure;
};

}  // namespace px::ui
