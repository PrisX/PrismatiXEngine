#pragma once

#include <imgui.h>
#include <imgui_node_editor.h>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Editor/Project/ProjectTypes.h"
#include "Editor/Tools/Lua/CustomCommand.h"
#include "Editor/Workspace/DocumentRegistry.h"
#include "Editor/Workspace/EditHistory.h"
#include "Engine/Resources/ResourceRef.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"

namespace px::editor {

class NodeGraphEditor : public IEditableDocument {
public:
    enum class GraphKind {
        Scenario,
    };

    using SelectedResourceCallback = std::function<std::string()>;
    using ResourceResolver = std::function<std::optional<ResourceRefValue>(const std::string&)>;
    using IdentityRegistrar =
        std::function<Status(const std::vector<std::filesystem::path>&)>;
    using FieldOptionsCallback =
        std::function<std::vector<std::string>(std::string_view nodeType,
                                               std::string_view parameterKey)>;

    NodeGraphEditor(GraphKind kind, LogSink log = {});
    ~NodeGraphEditor();

    NodeGraphEditor(const NodeGraphEditor&) = delete;
    NodeGraphEditor& operator=(const NodeGraphEditor&) = delete;

    void SetProject(const ProjectContext* context);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);
    void SetResourceResolver(ResourceResolver callback) { m_resourceResolver = std::move(callback); }
    void SetIdentityRegistrar(IdentityRegistrar callback) {
        m_identityRegistrar = std::move(callback);
    }
    void SetFieldOptionsCallback(FieldOptionsCallback callback) {
        m_fieldOptions = std::move(callback);
        m_fieldOptionsCache.clear();
    }
    void SetCustomCommands(std::vector<CustomCommandDef> commands);
    void SetHeaderTexture(ImTextureID texture, int width, int height);
    // Debugger hooks use stable Scenario statement lines.
    void SetBreakpointHooks(std::function<const std::set<int>*()> lines,
                            std::function<void(int line)> toggle) {
        m_breakpointLines = std::move(lines);
        m_toggleBreakpoint = std::move(toggle);
    }
    // Line of the node's first command in the compiled output (0 = unknown).
    [[nodiscard]] int CommandLineForNode(int nodeId) const {
        auto it = m_nodeCommandLine.find(nodeId);
        return it != m_nodeCommandLine.end() ? it->second : 0;
    }
    void Render();
    void RenderInspector();
    [[nodiscard]] bool Save();
    void Reload();
    // Reloads the graph if `script` (filename or runtime path) is the open document
    // and it has no unsaved edits. Used when the Flow editor rewrites the file.
    void ReloadIfOpen(const std::string& script);
    void RelocateIfOpen(const std::string& oldRuntimePath,
                        const std::string& newRuntimePath);
    bool OpenDocument(const std::string& runtimePath);
    bool NewDocument(const std::string& runtimePath);
    void ApplyAssetToSelection(const std::string& runtimePath);
    void CreateNodeForAsset(const std::string& runtimePath);
    void FrameSelection();
    // Re-imports the strict Scenario text projection.
    bool ImportScenarioText(const std::string& text);

    void Undo();
    void Redo();
    [[nodiscard]] bool CanUndo() const { return m_editHistory.CanUndo(); }
    [[nodiscard]] bool CanRedo() const { return m_editHistory.CanRedo(); }

    [[nodiscard]] Uuid DocumentId() const override { return m_editDocumentId; }
    [[nodiscard]] Result<Variant> ReadProperty(const Uuid& target,const std::string& property) const override;
    Status WriteProperty(const Uuid& target,const std::string& property,const Variant& value) override;
    [[nodiscard]] Result<VariantObject> CaptureSubtree(const Uuid& target) const override;
    Status InsertSubtree(const Uuid& parent,std::size_t index,const VariantObject& subtree) override;
    [[nodiscard]] Result<VariantObject> RemoveSubtree(const Uuid& target) override;
    Status Reparent(const Uuid& target,const Uuid& parent,std::size_t index) override;
    Status MoveChild(const Uuid& parent,const Uuid& target,std::size_t index) override;

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
        Uuid stableId;
        std::vector<Uuid> dialogueLineIds;
    };

    struct Link {
        int id = 0;
        int startPinId = 0;
        int endPinId = 0;
        Uuid stableId;
    };

    void EnsureContext();
    void BuildLibrary();
    void LoadOrCreate();
    void LoadGraph(const Json& json);
    [[nodiscard]] Json SaveGraph() const;
    void RestoreGraph(const Json& snapshot);
    void SeedDefaultGraph();
    bool ImportScenario(const std::filesystem::path& path);
    bool ImportScenarioDocument(const vn::scenario::ScenarioDocument& document);
    void UpdateNodePositions();
    void MarkDirty();

    Node* AddNode(const std::string& type, ImVec2 position);
    void RemoveNode(int id);
    void AddLink(int startPinId, int endPinId);
    void RemoveLink(int id);
    void DeleteSelection();
    void CreateDefaultLinkChain();
    void CopySelection();
    void PasteClipboard(ImVec2 canvasPosition);
    void DuplicateSelection();
    [[nodiscard]] Json ParamsToJson(const Node& node) const;
    void ApplyParamsJson(Node& node, const Json& params);

    // Visual group/comment boxes (ed::Group); not part of the compiled flow.
    struct GroupNode {
        int id = 0;
        std::string title = "Group";
        ImVec2 position = ImVec2(0, 0);
        ImVec2 size = ImVec2(320, 220);
        ImVec2 decoration = ImVec2(0, 0);  // node size minus group area (per render)
        bool positionInitialized = true;
    };
    GroupNode* FindGroup(int id);
    void AddGroup(ImVec2 position);
    void RenderGroupNode(GroupNode& group);

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
    void RenderMiniMap() const;
    void AlignSelection(bool horizontal);
    void RenderPinSummary(const Node& node) const;
    void RenderParameter(Parameter& parameter, bool compact, int nodeId = 0);
    [[nodiscard]] const std::vector<std::string>& ContextOptions(
        std::string_view nodeType, std::string_view parameterKey) const;
    void InNodeOptionButton(int nodeId, Parameter& parameter, float width);
    void InNodeColorButton(const char* id, int nodeId, Parameter& parameter);
    void RenderInNodePopups();
    void RenderUnsavedChangesModal();
    // Drag-drop targets inside node-editor nodes never receive ImGui payloads
    // (the canvas swallows the hover), so each asset field records its screen
    // rect here and the drop is hit-tested after ed::End().
    void RecordDropTarget(int nodeId, const std::string& paramKey, int lineIndex = -1);
    void HandleAssetDrop(const ImVec2& graphMin, const ImVec2& graphMax);
    void ApplyAssetToField(int nodeId, const std::string& paramKey, int lineIndex,
                           const std::string& asset);

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

    [[nodiscard]] std::string CompileScenarioV4() const;
    [[nodiscard]] static std::string QuoteLua(const std::string& value);
    [[nodiscard]] static std::string Trim(std::string_view value);
    [[nodiscard]] static std::string Lower(std::string value);
    [[nodiscard]] static std::string NormalizeScenarioRuntimePath(std::string value);

    void Log(const std::string& message) const;

    GraphKind m_kind = GraphKind::Scenario;
    LogSink m_log;
    SelectedResourceCallback m_selectedResource;
    ResourceResolver m_resourceResolver;
    IdentityRegistrar m_identityRegistrar;
    FieldOptionsCallback m_fieldOptions;
    mutable std::unordered_map<std::string, std::vector<std::string>> m_fieldOptionsCache;
    const ProjectContext* m_project = nullptr;
    ax::NodeEditor::EditorContext* m_context = nullptr;
    std::vector<NodeTemplate> m_library;
    std::vector<CustomCommandDef> m_customCommands;
    std::vector<Node> m_nodes;
    std::vector<Link> m_links;
    std::vector<GroupNode> m_groups;
    Json m_clipboard;  // serialized node selection for copy/paste
    std::string m_groupRenameBuf;
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
        int lineIndex = -1;  // for per-line editors (dialogue voices)
    };
    InNodePopup m_inNodePopup;

    struct AssetDropTarget {
        int nodeId = 0;
        std::string paramKey;
        int lineIndex = -1;  // dialogue per-line voice fields
        ImVec2 rectMin = ImVec2(0, 0);
        ImVec2 rectMax = ImVec2(0, 0);
    };
    std::vector<AssetDropTarget> m_dropTargets;

    // Nodes/links from saved graphs whose templates no longer exist (e.g. a
    // removed custom command). Kept verbatim and re-emitted on save so they
    // survive round-trips instead of silently disappearing.
    std::vector<Json> m_unknownNodes;
    std::vector<Json> m_unknownLinks;

    // Snapshot undo: the baseline is captured when idle; the first MarkDirty of
    // an edit gesture pushes it, so drags/typing coalesce into one entry.
    Json m_undoBaseline;
    bool m_undoArmed = false;
    bool m_undoGestureDirty = false;
    Uuid m_editDocumentId;
    EditHistory m_editHistory;

    int m_pendingDocAction = 0;  // 1 = open, 2 = new (unsaved-changes prompt)
    std::string m_pendingDocPath;

    std::function<const std::set<int>*()> m_breakpointLines;
    std::function<void(int)> m_toggleBreakpoint;
    mutable std::unordered_map<int, int> m_nodeCommandLine;
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
