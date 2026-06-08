#pragma once

#include <imgui.h>
#include <imgui_node_editor.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Editor/Project/ProjectTypes.h"
#include "Editor/Tools/Lua/CustomCommand.h"
#include "Editor/Workspace/DocumentRegistry.h"

namespace px::editor {

class NodeGraphEditor {
public:
    enum class GraphKind {
        PDSDialogue,
        Gameloop,
    };

    using SelectedResourceCallback = std::function<std::string()>;

    NodeGraphEditor(GraphKind kind, LogSink log = {});
    ~NodeGraphEditor();

    NodeGraphEditor(const NodeGraphEditor&) = delete;
    NodeGraphEditor& operator=(const NodeGraphEditor&) = delete;

    void SetProject(const ProjectContext* context);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);
    void SetCustomCommands(std::vector<CustomCommandDef> commands);
    void SetHeaderTexture(ImTextureID texture, int width, int height);
    void Render();
    void RenderInspector();
    void Save();
    void Reload();
    bool OpenDocument(const std::string& runtimePath);
    bool NewDocument(const std::string& runtimePath);
    void ApplyAssetToSelection(const std::string& runtimePath);
    void FrameSelection();

    [[nodiscard]] std::string Title() const;
    [[nodiscard]] std::filesystem::path DocumentPath() const;
    [[nodiscard]] bool Dirty() const { return m_dirty; }
    [[nodiscard]] std::vector<ExportArtifact> BuildArtifacts() const;
    [[nodiscard]] std::string Compile() const;
    [[nodiscard]] std::string CurrentRuntimePath() const;
    [[nodiscard]] std::string SelectionSummary() const;

private:
    enum class PinType {
        Flow,
        Bool,
        Int,
        Float,
        String,
        Asset,
        Object,
    };

    enum class ParamType {
        Bool,
        Int,
        Float,
        String,
        Asset,
        Option,
        Color,
    };

    struct Parameter {
        std::string key;
        std::string label;
        ParamType type = ParamType::String;
        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        std::string stringValue;
        ImVec4 colorValue = ImVec4(1, 1, 1, 1);
        std::vector<std::string> options;
        bool multiline = false;
    };

    struct PinTemplate {
        std::string label;
        PinType type = PinType::Flow;
        bool input = true;
        std::string parameterKey;
    };

    struct NodeTemplate {
        std::string type;
        std::string title;
        std::string category;
        std::string description;
        ImColor accent;
        std::vector<PinTemplate> inputs;
        std::vector<PinTemplate> outputs;
        std::vector<Parameter> defaults;
    };

    struct Pin {
        int id = 0;
        int nodeId = 0;
        std::string label;
        PinType type = PinType::Flow;
        bool input = true;
        std::string parameterKey;
    };

    struct Node {
        int id = 0;
        std::string type;
        ImVec2 position = ImVec2(0, 0);
        bool positionInitialized = false;
        std::vector<Pin> inputs;
        std::vector<Pin> outputs;
        std::vector<Parameter> parameters;
    };

    struct Link {
        int id = 0;
        int startPinId = 0;
        int endPinId = 0;
    };

    void EnsureContext();
    void BuildLibrary();
    void LoadOrCreate();
    void LoadGraph(const Json& json);
    [[nodiscard]] Json SaveGraph() const;
    void SeedDefaultGraph();
    bool ImportPDSScript(const std::filesystem::path& path);
    void UpdateNodePositions();
    void MarkDirty();

    Node* AddNode(const std::string& type, ImVec2 position);
    void RemoveNode(int id);
    void AddLink(int startPinId, int endPinId);
    void RemoveLink(int id);
    void DeleteSelection();
    void CreateDefaultLinkChain();

    void RenderToolbar();
    void RenderGraph();
    void RenderNode(Node& node);
    void RenderPin(Node& node, const Pin& pin);
    void RenderDialogueTextEditor(Node& node);
    void RenderUnboundParameters(Node& node);
    void RenderCreatePopup();
    void RenderNodeContextMenu();
    void HandleInteractions();
    void RefreshSelection();
    void RenderGraphOverview();
    void RenderPinSummary(const Node& node) const;
    void RenderParameter(Parameter& parameter, bool compact, int nodeId = 0);
    void InNodeOptionButton(int nodeId, Parameter& parameter, float width);
    void InNodeColorButton(const char* id, int nodeId, Parameter& parameter);
    void RenderInNodePopups();

    [[nodiscard]] const NodeTemplate* FindTemplate(std::string_view type) const;
    [[nodiscard]] const CustomCommandDef* FindCustomCommand(std::string_view type) const;
    [[nodiscard]] Node* FindNode(int id);
    [[nodiscard]] const Node* FindNode(int id) const;
    [[nodiscard]] Pin* FindPin(int id);
    [[nodiscard]] const Pin* FindPin(int id) const;
    [[nodiscard]] Link* FindLink(int id);
    [[nodiscard]] const Link* FindLink(int id) const;
    [[nodiscard]] Parameter* FindParameter(Node& node, std::string_view key);
    [[nodiscard]] const Parameter* FindParameter(const Node& node, std::string_view key) const;
    [[nodiscard]] bool CanLink(const Pin* start, const Pin* end) const;
    [[nodiscard]] bool IsPinLinked(int id) const;
    [[nodiscard]] const Node* FindFlowStart() const;
    [[nodiscard]] const Node* FindFlowNext(const Node& node, int outputIndex = 0) const;
    [[nodiscard]] std::vector<const Node*> LinearFlow() const;
    [[nodiscard]] std::string ResolveString(const Node& node, std::string_view key) const;
    [[nodiscard]] int ResolveInt(const Node& node, std::string_view key) const;
    [[nodiscard]] float ResolveFloat(const Node& node, std::string_view key) const;
    [[nodiscard]] bool ResolveBool(const Node& node, std::string_view key) const;
    [[nodiscard]] ImColor PinColor(PinType type) const;
    [[nodiscard]] static const char* PinTypeName(PinType type);

    [[nodiscard]] std::string CompilePDS() const;
    [[nodiscard]] std::string CompileGameloop() const;
    [[nodiscard]] static std::string QuoteLua(const std::string& value);
    [[nodiscard]] static std::string Trim(std::string_view value);
    [[nodiscard]] static std::string Lower(std::string value);
    [[nodiscard]] static std::string NormalizePDSRuntimePath(std::string value);

    void Log(const std::string& message) const;

    GraphKind m_kind = GraphKind::PDSDialogue;
    LogSink m_log;
    SelectedResourceCallback m_selectedResource;
    const ProjectContext* m_project = nullptr;
    ax::NodeEditor::EditorContext* m_context = nullptr;
    std::vector<NodeTemplate> m_library;
    std::vector<CustomCommandDef> m_customCommands;
    std::vector<Node> m_nodes;
    std::vector<Link> m_links;
    int m_nextId = 1;
    int m_selectedNodeId = 0;
    int m_selectedLinkId = 0;
    int m_contextNodeId = 0;
    int m_contextLinkId = 0;
    int m_pendingPinId = 0;
    ImVec2 m_createPosition = ImVec2(0, 0);
    bool m_createPopup = false;

    struct InNodePopup {
        int nodeId = 0;
        std::string paramKey;
        bool isColor = false;
        bool requestOpen = false;
    };
    InNodePopup m_inNodePopup;
    bool m_dirty = false;
    bool m_loaded = false;
    int m_navigateCountdown = 0;
    std::string m_search;
    std::string m_documentRuntimePath;
    std::string m_documentPathInput;
    ImTextureID m_headerTexture = ImTextureID{};
    int m_headerTextureWidth = 0;
    int m_headerTextureHeight = 0;
};

}  // namespace px::editor
