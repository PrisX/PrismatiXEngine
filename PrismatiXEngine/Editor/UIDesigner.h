#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace PrismatiX::Editor {

class UIDesigner {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using SelectedResourceCallback = std::function<std::string()>;

    struct RuntimeCanvasResult {
        bool rendered = false;
        void* texture = nullptr;
        std::string status;
    };

    using RuntimeCanvasRenderer = std::function<RuntimeCanvasResult(const ImRect&, float, int, int, bool, bool)>;

    struct GeneratedDocument {
        std::string label;
        std::filesystem::path relativePath;
        std::string content;
    };

    explicit UIDesigner(LogCallback logCallback = {});

    void Render(float deltaSeconds, const RuntimeCanvasRenderer& runtimeRenderer = {});
    void RenderInspector();
    void RenderPreview(const RuntimeCanvasRenderer& runtimeRenderer = {});
    void ApplyAssetToSelection(const std::string& assetPath);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);
    void SetProjectRoot(const std::filesystem::path& projectRoot);
    void ResetToDefaults();

    [[nodiscard]] bool HasSelection() const;
    [[nodiscard]] std::string GetSelectionSummary() const;
    [[nodiscard]] std::string GenerateLua() const;
    [[nodiscard]] std::vector<std::string> BuildPreviewLines() const;
    [[nodiscard]] std::vector<GeneratedDocument> BuildGeneratedDocuments() const;
    [[nodiscard]] std::string CurrentDocumentRuntimePath() const;
    [[nodiscard]] std::string GeneratedSceneScriptPath() const;
    [[nodiscard]] int SceneWidth() const { return 1280; }
    [[nodiscard]] int SceneHeight() const { return 720; }
    [[nodiscard]] int Revision() const { return m_revision; }

private:
    enum class DocumentKind {
        Scene,
        Component,
    };

    enum class ElementType {
        Button,
        Label,
        Panel,
    };

    enum class PreviewInteraction {
        None,
        Move,
        Resize,
    };

    struct ValueSpan {
        size_t start = 0;
        size_t end = 0;
        bool valid = false;
    };

    struct ParsedElement {
        int id = 0;
        ElementType type = ElementType::Button;
        std::string name;
        size_t callStart = 0;
        size_t callEnd = 0;
        size_t blockStart = 0;
        size_t blockEnd = 0;

        std::string text;
        std::string imageAsset;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        int fontSize = 24;
        ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImVec4 accentColor = ImVec4(0.16f, 0.16f, 0.16f, 0.9f);
        ImVec4 accentColor2 = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);

        ValueSpan textSpan;
        ValueSpan imageAssetSpan;
        ValueSpan xSpan;
        ValueSpan ySpan;
        ValueSpan wSpan;
        ValueSpan hSpan;
        ValueSpan fontSizeSpan;
        ValueSpan colorSpan;
        ValueSpan accentColorSpan;
        ValueSpan accentColor2Span;
    };

    struct ScriptDocument {
        int id = 0;
        DocumentKind kind = DocumentKind::Scene;
        std::filesystem::path path;
        std::string runtimePath;
        std::string displayName;
        std::string source;
        std::string parseWarning;
        std::string backgroundAsset;
        ValueSpan backgroundSpan;
        bool dirty = false;
        std::vector<std::string> linkedComponents;
        std::vector<std::string> linkedScenes;
        std::vector<ParsedElement> elements;
    };

    struct PreviewCommand {
        enum class Type {
            Rect,
            Text,
        };

        Type type = Type::Rect;
        ImVec2 position = ImVec2(0.0f, 0.0f);
        ImVec2 size = ImVec2(0.0f, 0.0f);
        ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImVec4 outlineColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        std::string text;
        float fontSize = 24.0f;
        int outlineSize = 0;
    };

    struct PreviewHitRegion {
        DocumentKind kind = DocumentKind::Component;
        ElementType type = ElementType::Button;
        int documentId = 0;
        int elementId = 0;
        ImVec2 position = ImVec2(0.0f, 0.0f);
        ImVec2 size = ImVec2(0.0f, 0.0f);
    };

    struct PreviewResult {
        std::string targetLabel;
        std::string error;
        std::vector<PreviewCommand> commands;
        std::vector<PreviewHitRegion> hitRegions;
    };

    void Touch();
    void EnsureDocuments();
    void RefreshDocuments(bool force);
    void RenderToolbar();
    void RenderDocumentNavigator();
    void RenderCanvasWorkspace(const RuntimeCanvasRenderer& runtimeRenderer);
    void RenderCodeEditor();
    void RenderDocumentSummary(const ScriptDocument& document);
    void RenderElementInspector(ScriptDocument& document, ParsedElement& element);
    void RenderPreviewContents(const RuntimeCanvasRenderer& runtimeRenderer, bool embedded);
    void DrawPreviewCommands(const ImRect& rect, const PreviewResult& preview) const;
    void AppendDocumentPreviewCommands(const ScriptDocument& document, PreviewResult& preview, bool includeBackground) const;

    [[nodiscard]] PreviewResult BuildStaticPreview(const ScriptDocument& document) const;
    [[nodiscard]] std::filesystem::path LoadDataRoot() const;
    [[nodiscard]] std::vector<std::filesystem::path> EnumerateDocuments(DocumentKind kind) const;
    [[nodiscard]] std::string DocumentKindLabel(DocumentKind kind) const;
    [[nodiscard]] std::string ElementTypeLabel(ElementType type) const;
    [[nodiscard]] std::string ResolvePreviewScenePath() const;
    [[nodiscard]] ScriptDocument* ActiveDocument();
    [[nodiscard]] const ScriptDocument* ActiveDocument() const;
    [[nodiscard]] ParsedElement* ActiveElement();
    [[nodiscard]] const ParsedElement* ActiveElement() const;
    [[nodiscard]] ScriptDocument* FindDocumentById(DocumentKind kind, int documentId);
    [[nodiscard]] const ScriptDocument* FindDocumentById(DocumentKind kind, int documentId) const;
    [[nodiscard]] ScriptDocument* FindDocument(DocumentKind kind, std::string_view runtimePath);
    [[nodiscard]] const ScriptDocument* FindDocument(DocumentKind kind, std::string_view runtimePath) const;
    void ParseDocument(ScriptDocument& document);
    void RebuildSceneLinks();
    void SelectDocument(DocumentKind kind, int documentId);
    void SetSelection(DocumentKind kind, int documentId, int elementId);
    void SaveActiveDocument();
    void ReplaceValue(ScriptDocument& document, const ValueSpan& span, std::string_view replacement);
    void SetStringValue(ScriptDocument& document, ValueSpan& span, const std::string& value);
    void SetIntValue(ScriptDocument& document, ValueSpan& span, int value);
    void SetColorValue(ScriptDocument& document, ValueSpan& span, const ImVec4& value);
    void ApplyElementFrame(ScriptDocument& document, const ParsedElement& element, float x, float y, float w, float h);
    bool UpsertStringProperty(ScriptDocument& document, const ParsedElement& element, const ValueSpan& span, std::string_view key, const std::string& value);
    bool InsertElementAfterReference(ScriptDocument& document, const ParsedElement& reference, std::string_view expression);
    void AddElement(ElementType type, bool imageButton = false);
    bool ReplaceBackgroundAsset(ScriptDocument& document, const std::string& assetPath);
    [[nodiscard]] static std::string NormalizePath(std::string value);
    [[nodiscard]] static std::string EscapeLuaString(const std::string& value);
    [[nodiscard]] static std::string UnescapeLuaString(std::string_view value);
    [[nodiscard]] static std::string Trim(std::string_view value);
    [[nodiscard]] static ValueSpan FindQuotedValue(std::string_view source, std::string_view key, size_t absoluteOffset);
    [[nodiscard]] static ValueSpan FindExpressionValue(std::string_view source, std::string_view key, size_t absoluteOffset);
    [[nodiscard]] static ValueSpan FindTableValue(std::string_view source, std::string_view key, size_t absoluteOffset);
    [[nodiscard]] static std::vector<float> ParseColor(ValueSpan span, const std::string& source);
    [[nodiscard]] static std::string ReadSpan(const std::string& source, const ValueSpan& span);
    [[nodiscard]] static bool IsWordBoundary(char ch);
    [[nodiscard]] float EstimateTextWidth(const std::string& text, float fontSize) const;
    void Log(const std::string& message) const;

    LogCallback m_logCallback;
    SelectedResourceCallback m_selectedResourceCallback;
    std::filesystem::path m_projectRoot;
    std::vector<ScriptDocument> m_sceneDocuments;
    std::vector<ScriptDocument> m_componentDocuments;
    DocumentKind m_activeKind = DocumentKind::Scene;
    int m_activeDocumentId = 0;
    int m_activeElementId = 0;
    int m_nextDocumentId = 1;
    int m_revision = 1;
    bool m_showScenes = true;
    bool m_showComponents = true;
    bool m_showCodeEditor = false;
    bool m_snapToGrid = true;
    bool m_forceRefresh = true;
    PreviewInteraction m_previewInteraction = PreviewInteraction::None;
    DocumentKind m_interactionKind = DocumentKind::Component;
    int m_interactionDocumentId = 0;
    int m_interactionElementId = 0;
    ImVec2 m_interactionStartMouse = ImVec2(0.0f, 0.0f);
    ImVec2 m_interactionStartPosition = ImVec2(0.0f, 0.0f);
    ImVec2 m_interactionStartSize = ImVec2(0.0f, 0.0f);
    PreviewResult m_lastPreview;
};

}  // namespace PrismatiX::Editor
