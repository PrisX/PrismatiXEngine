#pragma once

#include "EditorServices.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace PrismatiX::Editor {

class UIDesigner {
public:
    struct GeneratedDocument {
        std::string label;
        std::filesystem::path relativePath;
        std::string content;
    };

    using SelectedResourceCallback = std::function<std::string()>;
    using TextureResolver = std::function<ImTextureID(const std::string& runtimePath, int* width, int* height)>;
    using RuntimePreviewCallback = std::function<ImTextureID(const ImVec2& screenPos, const ImVec2& size, int* width, int* height)>;

    explicit UIDesigner(LogSink log = {});

    void SetProject(const ProjectContext* context);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);
    void SetTextureResolver(TextureResolver resolver);
    void SetRuntimePreviewCallback(RuntimePreviewCallback callback);

    void Render();
    void RenderLayerPanel();
    void RenderInspector();
    void RenderActionsPanel();
    void RenderAnimationPanel();

    void Save();
    void Reload();
    void ResetToDefaults();
    bool OpenDocument(const std::string& runtimePath);
    bool NewDocument(const std::string& runtimePath);
    void ApplyAssetToSelection(const std::string& runtimePath);
    void FrameSelection();

    [[nodiscard]] bool Dirty() const { return m_dirty; }
    [[nodiscard]] int Revision() const { return m_revision; }
    [[nodiscard]] std::filesystem::path DocumentPath() const;
    [[nodiscard]] std::string CurrentDocumentRuntimePath() const;
    [[nodiscard]] std::string GeneratedSceneScriptPath() const;
    [[nodiscard]] std::string SelectionSummary() const;
    [[nodiscard]] std::string GenerateLua() const;
    [[nodiscard]] std::vector<GeneratedDocument> BuildGeneratedDocuments() const;
    [[nodiscard]] std::vector<ExportArtifact> BuildArtifacts() const;

private:
    struct Layer {
        std::string id;
        std::string name;
        bool visible = true;
        bool locked = false;
        bool expanded = true;
    };

    struct AnimationKey {
        float time = 0.0f;
        std::string property = "opacity";
        float value = 255.0f;
    };

    struct AnimationClip {
        std::string name = "Appear";
        float duration = 0.45f;
        float delay = 0.0f;
        std::string easing = "outCubic";
        bool loop = false;
        std::string trigger = "scene.enter";
        std::vector<AnimationKey> keys;
    };

    struct Node {
        std::string id;
        std::string type = "button";
        std::string name;
        std::string layerId = "content";
        int layer = 1;
        int order = 0;
        bool visible = true;
        bool locked = false;
        float x = 0.0f;
        float y = 0.0f;
        float w = 220.0f;
        float h = 56.0f;
        float opacity = 255.0f;
        float radius = 8.0f;
        std::string text;
        std::string image;
        std::string hoverImage;
        std::string component;
        std::string font = "NotoSansTC-Bold.ttf";
        int fontSize = 32;
        ImVec4 bgColor = ImVec4(0.10f, 0.15f, 0.20f, 0.86f);
        ImVec4 hoverColor = ImVec4(0.16f, 0.28f, 0.34f, 0.94f);
        ImVec4 borderColor = ImVec4(0.46f, 0.56f, 0.72f, 0.47f);
        ImVec4 hoverBorderColor = ImVec4(1.00f, 0.84f, 0.56f, 0.86f);
        ImVec4 textColor = ImVec4(0.93f, 0.97f, 1.00f, 1.00f);
        float borderTopHeight = 0.0f;
        bool textShadow = false;
        std::string state = "normal";
        std::string actionType = "scene.switch";
        std::string actionTarget = "Scripts/scenes/play_scene.lua";
        std::string actionArgument;
        std::vector<AnimationClip> animations;
    };

    struct GuideLine {
        bool vertical = true;
        float pos = 0.0f;
    };

    void LoadOrCreate();
    void LoadDocument(const Json& json);
    [[nodiscard]] Json SaveDocument() const;
    void SeedDefaultScene();
    void SeedBlankDocument();
    void SeedTemplateDocument(const std::string& runtimePath);
    void MarkDirty();

    void RenderDocumentPanel();
    void RenderCanvasToolbar();
    void RenderCanvas();
    void RenderNodeOnCanvas(Node& node, const ImVec2& origin, float scale, ImDrawList* drawList, bool selected);
    void RenderGrid(const ImVec2& origin, float scale, ImDrawList* drawList, const ImRect& clipRect) const;
    void HandleCanvasInput(const ImVec2& origin, float scale, const ImRect& canvasRect);
    void DrawSelectionAndGuides(const ImVec2& origin, float scale, ImDrawList* drawList);
    void DrawResizeHandles(const ImVec2& origin, float scale, ImDrawList* drawList, Node& node);
    void AddNodeAt(const std::string& type, const ImVec2& canvasPoint, const std::string& runtimeAsset = {});
    void DeleteSelected();
    void SelectNode(const std::string& id, bool append);
    void ClearSelection();
    void RecalculateLayerOrders();
    void MoveLayer(int from, int to);
    void MoveNodeOrder(Node& node, int direction);
    void SortNodesForRuntime(std::vector<Node*>& nodes);

    [[nodiscard]] Node* FindNode(const std::string& id);
    [[nodiscard]] const Node* FindNode(const std::string& id) const;
    [[nodiscard]] Layer* FindLayer(const std::string& id);
    [[nodiscard]] const Layer* FindLayer(const std::string& id) const;
    [[nodiscard]] bool IsSelected(const std::string& id) const;
    [[nodiscard]] Node* PrimarySelection();
    [[nodiscard]] const Node* PrimarySelection() const;
    [[nodiscard]] ImRect NodeRect(const Node& node) const;
    [[nodiscard]] ImVec2 ScreenToCanvas(const ImVec2& point, const ImVec2& origin, float scale) const;
    [[nodiscard]] ImVec2 CanvasToScreen(const ImVec2& point, const ImVec2& origin, float scale) const;
    [[nodiscard]] std::vector<GuideLine> BuildGuides(const Node& moving, float& snapDx, float& snapDy) const;
    [[nodiscard]] std::vector<std::string> EnumerateUiDocuments() const;
    [[nodiscard]] static Json ColorToJson(const ImVec4& color);
    [[nodiscard]] static ImVec4 ColorFromJson(const Json& json, ImVec4 fallback);
    [[nodiscard]] static std::string QuoteLua(const std::string& value);
    [[nodiscard]] static std::string SanitizeId(std::string value);
    [[nodiscard]] static std::string NormalizeUiRuntimePath(std::string value);
    [[nodiscard]] static std::string GeneratedScriptPathForRuntimePath(std::string runtimePath);
    [[nodiscard]] static std::string GenerateLuaFromDocument(const Json& document, const std::string& runtimePath);

    void RenderTextRow(const char* label, std::string& value);
    void RenderNodeTypeCombo(Node& node);
    void RenderActionTypeCombo(Node& node);
    void Log(const std::string& message) const;

    LogSink m_log;
    SelectedResourceCallback m_selectedResource;
    TextureResolver m_textureResolver;
    RuntimePreviewCallback m_runtimePreview;
    const ProjectContext* m_project = nullptr;

    std::vector<Layer> m_layers;
    std::vector<Node> m_nodes;
    std::vector<std::string> m_selectedIds;
    std::string m_layerSearch;
    std::string m_documentSearch;
    std::string m_documentRuntimePath = "UI/title_menu.pxui";
    std::string m_documentPathInput = "UI/title_menu.pxui";

    int m_canvasW = 1280;
    int m_canvasH = 720;
    ImVec4 m_backgroundColor = ImVec4(0.03f, 0.05f, 0.08f, 1.0f);
    std::string m_backgroundImage;
    float m_zoom = 0.72f;
    ImVec2 m_pan = ImVec2(0.0f, 0.0f);
    float m_snapGrid = 8.0f;
    float m_snapDistance = 7.0f;
    bool m_snapEnabled = true;
    bool m_gridVisible = true;
    bool m_showGuides = true;
    bool m_draggingNode = false;
    bool m_draggingCanvas = false;
    bool m_resizingNode = false;
    bool m_boxSelecting = false;
    ImVec2 m_dragStartMouse = ImVec2(0, 0);
    ImVec2 m_dragStartCanvas = ImVec2(0, 0);
    ImVec2 m_dragStartPan = ImVec2(0, 0);
    ImVec2 m_selectionBoxStart = ImVec2(0, 0);
    std::vector<Node> m_dragStartNodes;
    std::vector<GuideLine> m_activeGuides;

    bool m_dirty = false;
    int m_revision = 0;
    int m_nextNodeIndex = 1;
};

}
