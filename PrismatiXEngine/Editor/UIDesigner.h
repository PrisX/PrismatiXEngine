#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace PrismatiX::Editor {

class UIDesigner {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using SelectedResourceCallback = std::function<std::string()>;

    explicit UIDesigner(LogCallback logCallback = {});

    void Render(float deltaSeconds);
    void RenderInspector();
    void RenderPreview();
    void ApplyAssetToSelection(const std::string& assetPath);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);

    [[nodiscard]] bool HasSelection() const;
    [[nodiscard]] std::string GetSelectionSummary() const;
    [[nodiscard]] std::string GenerateLua() const;
    [[nodiscard]] std::vector<std::string> BuildPreviewLines() const;

private:
    enum class ComponentType {
        Panel,
        Label,
        Button,
        Image,
        ChoiceStrip,
    };

    struct MotionSpec {
        std::string kind = "fade";
        float duration = 0.35f;
        float delay = 0.0f;
        float amplitude = 40.0f;
    };

    struct UIComponent {
        int id = 0;
        ComponentType type = ComponentType::Panel;
        std::string name;
        std::string text;
        std::string assetPath;
        ImVec2 position = ImVec2(0.0f, 0.0f);
        ImVec2 size = ImVec2(240.0f, 80.0f);
        ImVec4 tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        int fontSize = 28;
        bool visible = true;
        std::string layout = "None";
        float spacing = 12.0f;
        MotionSpec motion;
    };

    void SeedDefaults();
    void RenderPalette();
    void RenderCanvas(float deltaSeconds);
    void RenderCanvasContents(const ImRect& rect, float deltaSeconds, bool interactive);
    void RenderComponent(const UIComponent& component, const ImRect& rect, float scale, float deltaSeconds, bool selected) const;
    void HandleCanvasInteractions(const ImRect& rect, float scale);
    void RenderCanvasContextMenu(const ImRect& rect, float scale);

    UIComponent* AddComponent(ComponentType type, const ImVec2& designPosition);
    UIComponent* FindComponent(int id);
    const UIComponent* FindComponent(int id) const;
    void DeleteSelectedComponent();
    void CenterSelectedComponent();

    [[nodiscard]] std::string ComponentLabel(ComponentType type) const;
    [[nodiscard]] ImU32 ComponentFill(const UIComponent& component, float alphaMultiplier) const;
    [[nodiscard]] ImRect ComponentRect(const UIComponent& component, const ImRect& canvasRect, float scale, float deltaSeconds) const;
    [[nodiscard]] ImVec2 ScreenToDesign(const ImRect& rect, float scale, const ImVec2& screenPoint) const;
    [[nodiscard]] ImVec2 DesignToScreen(const ImRect& rect, float scale, const ImVec2& designPoint) const;
    [[nodiscard]] std::string NormalizeLuaString(const std::string& value) const;
    void RenderAssetPathEditor(std::string& assetPath, std::string_view idSuffix);
    void Log(const std::string& message) const;

    LogCallback m_logCallback;
    SelectedResourceCallback m_selectedResourceCallback;
    int m_nextId = 1;
    int m_selectedId = 0;
    int m_draggingId = 0;
    ImVec2 m_dragOffset = ImVec2(0.0f, 0.0f);
    bool m_previewAnimation = true;
    bool m_showGrid = true;
    ImVec2 m_designSize = ImVec2(1280.0f, 720.0f);
    std::vector<UIComponent> m_components;
};

}  // namespace PrismatiX::Editor
