#pragma once

#include <imgui.h>
#include <imgui_node_editor.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace PrismatiX::Editor {

enum class BlueprintFlavor {
    Entrypoint,
    SceneScript,
};

class BlueprintEditor {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using SelectedResourceCallback = std::function<std::string()>;

    BlueprintEditor(BlueprintFlavor flavor, LogCallback logCallback = {});
    ~BlueprintEditor();

    BlueprintEditor(const BlueprintEditor&) = delete;
    BlueprintEditor& operator=(const BlueprintEditor&) = delete;

    void Render();
    void RenderInspector();
    void NavigateToContent();
    void ApplyAssetToSelection(const std::string& assetPath);
    void SetHeaderTexture(ImTextureID texture, int width, int height);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);

    [[nodiscard]] bool HasSelection() const;
    [[nodiscard]] std::string GetSelectionSummary() const;
    [[nodiscard]] std::string GenerateLua() const;
    [[nodiscard]] std::vector<std::string> BuildPreviewLines() const;

private:
    enum class PinDataType {
        Flow,
        Bool,
        Int,
        Float,
        String,
        Asset,
    };

    enum class ParameterKind {
        Boolean,
        Integer,
        Float,
        String,
        Asset,
        Option,
    };

    struct NodeParameter {
        std::string key;
        std::string label;
        ParameterKind kind = ParameterKind::String;
        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        float speed = 0.1f;
        std::string stringValue;
        std::vector<std::string> options;
    };

    struct PinTemplate {
        std::string name;
        PinDataType type = PinDataType::Flow;
        ax::NodeEditor::PinKind kind = ax::NodeEditor::PinKind::Input;
        std::string parameterKey;
    };

    struct NodeTemplate {
        std::string typeId;
        std::string title;
        std::string category;
        std::string description;
        ImColor accent;
        std::vector<PinTemplate> inputs;
        std::vector<PinTemplate> outputs;
        std::vector<NodeParameter> defaults;
    };

    struct PinInstance {
        ax::NodeEditor::PinId id = 0;
        int ownerId = 0;
        std::string name;
        PinDataType type = PinDataType::Flow;
        ax::NodeEditor::PinKind kind = ax::NodeEditor::PinKind::Input;
        std::string parameterKey;
    };

    struct NodeInstance {
        ax::NodeEditor::NodeId id = 0;
        const NodeTemplate* templateRef = nullptr;
        std::vector<PinInstance> inputs;
        std::vector<PinInstance> outputs;
        std::vector<NodeParameter> parameters;
        ImVec2 initialPosition = ImVec2(0.0f, 0.0f);
        bool positionInitialized = false;
    };

    struct LinkInstance {
        ax::NodeEditor::LinkId id = 0;
        ax::NodeEditor::PinId startPinId = 0;
        ax::NodeEditor::PinId endPinId = 0;
        ImColor color;
    };

    void BuildLibrary();
    void SeedDefaults();
    void SeedEntrypointGraph();
    void SeedSceneGraph();
    void EnsureEditorContext();

    NodeInstance* AddNode(const std::string& typeId, const ImVec2& position);
    void AddLink(ax::NodeEditor::PinId from, ax::NodeEditor::PinId to);
    void AddLinkByNodeIndex(size_t fromNodeIndex, size_t fromOutputIndex, size_t toNodeIndex, size_t toInputIndex);

    void RenderToolbar();
    void RenderGraph();
    void RenderNode(NodeInstance& node);
    void RenderCreatePopup();
    void RenderNodeContextMenu();
    void RenderLinkContextMenu();
    void HandleCreateDeleteInteractions();
    void RefreshSelectionState();

    void RenderPin(const NodeInstance& node, const PinInstance& pin, bool inputSide);
    void RenderParameterEditor(NodeParameter& parameter, std::string_view labelSuffix, bool compact);
    void RenderAssetParameterEditor(NodeParameter& parameter, const std::string& controlId, bool compact);

    [[nodiscard]] const NodeTemplate* FindTemplate(const std::string& typeId) const;
    [[nodiscard]] NodeInstance* FindNode(ax::NodeEditor::NodeId id);
    [[nodiscard]] const NodeInstance* FindNode(ax::NodeEditor::NodeId id) const;
    [[nodiscard]] PinInstance* FindPin(ax::NodeEditor::PinId id);
    [[nodiscard]] const PinInstance* FindPin(ax::NodeEditor::PinId id) const;
    [[nodiscard]] LinkInstance* FindLink(ax::NodeEditor::LinkId id);
    [[nodiscard]] const LinkInstance* FindLink(ax::NodeEditor::LinkId id) const;
    [[nodiscard]] NodeParameter* FindParameter(NodeInstance& node, std::string_view key);
    [[nodiscard]] const NodeParameter* FindParameter(const NodeInstance& node, std::string_view key) const;
    [[nodiscard]] bool IsPinLinked(ax::NodeEditor::PinId id) const;
    [[nodiscard]] bool CanCreateLink(const PinInstance* a, const PinInstance* b) const;
    [[nodiscard]] const NodeInstance* ResolveSourceNode(const PinInstance& inputPin) const;
    [[nodiscard]] const PinInstance* ResolveSourcePin(const PinInstance& inputPin) const;
    [[nodiscard]] std::string ResolveString(const NodeInstance& node, std::string_view key) const;
    [[nodiscard]] float ResolveFloat(const NodeInstance& node, std::string_view key) const;
    [[nodiscard]] int ResolveInt(const NodeInstance& node, std::string_view key) const;
    [[nodiscard]] bool ResolveBool(const NodeInstance& node, std::string_view key) const;
    [[nodiscard]] const NodeInstance* FindFlowStart() const;
    [[nodiscard]] const NodeInstance* FindFlowNext(const NodeInstance& node) const;
    [[nodiscard]] std::vector<const NodeInstance*> BuildLinearFlow() const;
    [[nodiscard]] ImColor GetPinColor(PinDataType type) const;
    [[nodiscard]] std::string EmitEntrypointLua() const;
    [[nodiscard]] std::string EmitScenePds() const;
    [[nodiscard]] std::string NormalizeLuaString(const std::string& value) const;

    void Log(const std::string& message) const;

    BlueprintFlavor m_flavor = BlueprintFlavor::Entrypoint;
    LogCallback m_logCallback;
    SelectedResourceCallback m_selectedResourceCallback;
    ax::NodeEditor::EditorContext* m_context = nullptr;
    ImTextureID m_headerTexture = ImTextureID{};
    int m_headerTextureWidth = 0;
    int m_headerTextureHeight = 0;
    int m_nextId = 1;
    int m_navigateCountdown = 3;
    std::vector<NodeTemplate> m_library;
    std::vector<NodeInstance> m_nodes;
    std::vector<LinkInstance> m_links;
    ax::NodeEditor::NodeId m_selectedNodeId = 0;
    ax::NodeEditor::LinkId m_selectedLinkId = 0;
    ax::NodeEditor::NodeId m_contextNodeId = 0;
    ax::NodeEditor::LinkId m_contextLinkId = 0;
    ax::NodeEditor::PinId m_pendingLinkPinId = 0;
    ImVec2 m_createPopupPosition = ImVec2(0.0f, 0.0f);
    bool m_createPopupOpen = false;
};

}  // namespace PrismatiX::Editor
