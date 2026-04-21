#define IMGUI_DEFINE_MATH_OPERATORS

#include "UIDesigner.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

namespace {

constexpr float kPreviewWidth = 1280.0f;
constexpr float kPreviewHeight = 720.0f;

struct UiCallBounds {
    size_t callStart = 0;
    size_t blockStart = 0;
    size_t blockEnd = 0;
    size_t callEnd = 0;
};

[[nodiscard]] std::string TrimView(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

struct QuotedLiteralSpan {
    size_t start = 0;
    size_t end = 0;
    bool valid = false;
};

[[nodiscard]] QuotedLiteralSpan FindFirstQuotedLiteral(std::string_view source, size_t absoluteOffset, size_t searchStart = 0) {
    size_t quote = source.find('"', searchStart);
    if (quote == std::string_view::npos) {
        return {};
    }

    size_t end = quote + 1;
    bool escaped = false;
    while (end < source.size()) {
        const char ch = source[end];
        if (!escaped && ch == '"') {
            return {absoluteOffset + quote, absoluteOffset + end + 1, true};
        }
        escaped = !escaped && ch == '\\';
        ++end;
    }
    return {};
}

[[nodiscard]] ImVec2 FitRect(ImVec2 available, float aspectWidth, float aspectHeight) {
    if (available.x <= 0.0f || available.y <= 0.0f) {
        return ImVec2(0.0f, 0.0f);
    }

    const float aspect = aspectWidth / aspectHeight;
    ImVec2 size = available;
    if (size.x / size.y > aspect) {
        size.x = size.y * aspect;
    } else {
        size.y = size.x / aspect;
    }
    return size;
}

[[nodiscard]] ImU32 ToColor32(const ImVec4& color) {
    return ImColor(color.x, color.y, color.z, color.w);
}

[[nodiscard]] std::string FormatColorValue(const ImVec4& value) {
    const auto to255 = [](float channel) {
        return static_cast<int>(std::round(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
    };

    std::ostringstream stream;
    stream << "{ "
           << to255(value.x) << ", "
           << to255(value.y) << ", "
           << to255(value.z) << ", "
           << to255(value.w) << " }";
    return stream.str();
}

[[nodiscard]] ImVec4 ColorFromNumbers(const std::vector<float>& values, const ImVec4& fallback) {
    if (values.empty()) {
        return fallback;
    }

    const float divisor = 255.0f;
    return ImVec4(
        values.size() > 0 ? std::clamp(values[0] / divisor, 0.0f, 1.0f) : fallback.x,
        values.size() > 1 ? std::clamp(values[1] / divisor, 0.0f, 1.0f) : fallback.y,
        values.size() > 2 ? std::clamp(values[2] / divisor, 0.0f, 1.0f) : fallback.z,
        values.size() > 3 ? std::clamp(values[3] / divisor, 0.0f, 1.0f) : fallback.w);
}

[[nodiscard]] std::string IndentForLine(const std::string& source, size_t position) {
    const size_t lineStartPos = source.rfind('\n', std::min(position, source.size()));
    const size_t lineStart = lineStartPos == std::string::npos ? 0 : lineStartPos + 1;
    size_t cursor = lineStart;
    while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t')) {
        ++cursor;
    }
    return source.substr(lineStart, cursor - lineStart);
}

[[nodiscard]] size_t ClosingBraceLineStart(const std::string& source, size_t closingBracePos) {
    const size_t lineStartPos = source.rfind('\n', std::min(closingBracePos, source.size()));
    return lineStartPos == std::string::npos ? 0 : lineStartPos + 1;
}

[[nodiscard]] float SnapValue(float value, float step) {
    return std::round(value / step) * step;
}

class NumericExpressionParser {
public:
    NumericExpressionParser(std::string_view expression, const std::unordered_map<std::string, float>& variables)
        : m_expression(expression), m_variables(variables) {}

    [[nodiscard]] bool Parse(float& value) {
        SkipWhitespace();
        if (!ParseExpression(value)) {
            return false;
        }
        SkipWhitespace();
        return m_cursor == m_expression.size();
    }

private:
    [[nodiscard]] bool ParseExpression(float& value) {
        if (!ParseTerm(value)) {
            return false;
        }

        while (true) {
            SkipWhitespace();
            if (Consume('+')) {
                float rhs = 0.0f;
                if (!ParseTerm(rhs)) {
                    return false;
                }
                value += rhs;
            } else if (Consume('-')) {
                float rhs = 0.0f;
                if (!ParseTerm(rhs)) {
                    return false;
                }
                value -= rhs;
            } else {
                return true;
            }
        }
    }

    [[nodiscard]] bool ParseTerm(float& value) {
        if (!ParseFactor(value)) {
            return false;
        }

        while (true) {
            SkipWhitespace();
            if (Consume('*')) {
                float rhs = 0.0f;
                if (!ParseFactor(rhs)) {
                    return false;
                }
                value *= rhs;
            } else if (Consume('/')) {
                float rhs = 0.0f;
                if (!ParseFactor(rhs) || std::fabs(rhs) <= 0.0001f) {
                    return false;
                }
                value /= rhs;
            } else {
                return true;
            }
        }
    }

    [[nodiscard]] bool ParseFactor(float& value) {
        SkipWhitespace();

        if (Consume('+')) {
            return ParseFactor(value);
        }
        if (Consume('-')) {
            if (!ParseFactor(value)) {
                return false;
            }
            value = -value;
            return true;
        }
        if (Consume('(')) {
            if (!ParseExpression(value)) {
                return false;
            }
            SkipWhitespace();
            return Consume(')');
        }
        if (ParseNumber(value)) {
            return true;
        }
        return ParseIdentifier(value);
    }

    [[nodiscard]] bool ParseNumber(float& value) {
        const size_t start = m_cursor;
        bool sawDigit = false;
        while (m_cursor < m_expression.size()) {
            const char ch = m_expression[m_cursor];
            if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
                sawDigit = true;
                ++m_cursor;
                continue;
            }
            if (ch == '.') {
                ++m_cursor;
                continue;
            }
            break;
        }

        if (!sawDigit) {
            m_cursor = start;
            return false;
        }

        try {
            value = std::stof(std::string(m_expression.substr(start, m_cursor - start)));
            return true;
        } catch (...) {
            m_cursor = start;
            return false;
        }
    }

    [[nodiscard]] bool ParseIdentifier(float& value) {
        const size_t start = m_cursor;
        while (m_cursor < m_expression.size()) {
            const char ch = m_expression[m_cursor];
            if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '.') {
                ++m_cursor;
                continue;
            }
            break;
        }

        if (start == m_cursor) {
            return false;
        }

        const std::string name = std::string(m_expression.substr(start, m_cursor - start));
        const auto it = m_variables.find(name);
        if (it == m_variables.end()) {
            m_cursor = start;
            return false;
        }

        value = it->second;
        return true;
    }

    void SkipWhitespace() {
        while (m_cursor < m_expression.size() && std::isspace(static_cast<unsigned char>(m_expression[m_cursor])) != 0) {
            ++m_cursor;
        }
    }

    [[nodiscard]] bool Consume(char ch) {
        if (m_cursor < m_expression.size() && m_expression[m_cursor] == ch) {
            ++m_cursor;
            return true;
        }
        return false;
    }

    std::string_view m_expression;
    const std::unordered_map<std::string, float>& m_variables;
    size_t m_cursor = 0;
};

[[nodiscard]] bool TryEvaluateNumericExpression(std::string_view expression, float& value) {
    static const std::unordered_map<std::string, float> kVariables{
        {"screenW", kPreviewWidth},
        {"winW", kPreviewWidth},
        {"self.screenW", kPreviewWidth},
        {"screenH", kPreviewHeight},
        {"winH", kPreviewHeight},
        {"self.screenH", kPreviewHeight},
        {"fontSize", 24.0f},
        {"self.fontSize", 24.0f},
        {"nameFontSize", 24.0f},
        {"self.nameFontSize", 24.0f},
    };

    const std::string trimmed = TrimView(expression);
    NumericExpressionParser parser(trimmed, kVariables);
    return parser.Parse(value);
}

[[nodiscard]] std::vector<UiCallBounds> FindUiCallBounds(const std::string& source, std::string_view marker) {
    std::vector<UiCallBounds> blocks;
    size_t searchFrom = 0;
    while ((searchFrom = source.find(marker, searchFrom)) != std::string::npos) {
        const size_t braceStart = source.find('{', searchFrom + marker.size() - 1);
        if (braceStart == std::string::npos) {
            break;
        }

        int depth = 0;
        for (size_t cursor = braceStart; cursor < source.size(); ++cursor) {
            if (source[cursor] == '{') {
                ++depth;
            } else if (source[cursor] == '}') {
                --depth;
                if (depth == 0) {
                    size_t callEnd = cursor + 1;
                    while (callEnd < source.size() && std::isspace(static_cast<unsigned char>(source[callEnd])) != 0) {
                        ++callEnd;
                    }
                    if (callEnd < source.size() && source[callEnd] == ')') {
                        ++callEnd;
                    }

                    blocks.push_back(UiCallBounds{searchFrom, braceStart, cursor + 1, callEnd});
                    searchFrom = cursor + 1;
                    break;
                }
            }
        }

        if (blocks.empty() || searchFrom <= braceStart) {
            break;
        }
    }
    return blocks;
}

}  // namespace

UIDesigner::UIDesigner(LogCallback logCallback)
    : m_logCallback(std::move(logCallback)) {}

void UIDesigner::Touch() {
    ++m_revision;
}

void UIDesigner::SetSelectedResourceCallback(SelectedResourceCallback callback) {
    m_selectedResourceCallback = std::move(callback);
}

void UIDesigner::SetProjectRoot(const fs::path& projectRoot) {
    m_projectRoot = projectRoot;
    m_forceRefresh = true;
}

void UIDesigner::ResetToDefaults() {
    m_sceneDocuments.clear();
    m_componentDocuments.clear();
    m_activeDocumentId = 0;
    m_activeElementId = 0;
    m_nextDocumentId = 1;
    m_forceRefresh = true;
    Touch();
}

void UIDesigner::Render(float deltaSeconds, const RuntimeCanvasRenderer& runtimeRenderer) {
    (void)deltaSeconds;
    EnsureDocuments();
    RenderToolbar();
    ImGui::Separator();

    if (ImGui::BeginTable("ui-editor-layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Documents", ImGuiTableColumnFlags_WidthFixed, 280.0f);
        ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        RenderDocumentNavigator();

        ImGui::TableSetColumnIndex(1);
        if (m_showCodeEditor) {
            RenderCodeEditor();
        } else {
            RenderCanvasWorkspace(runtimeRenderer);
        }

        ImGui::EndTable();
    }
}

void UIDesigner::RenderToolbar() {
    ImGui::BeginChild("ui-toolbar", ImVec2(0.0f, 72.0f), false);
    if (ImGui::Button("Refresh")) {
        RefreshDocuments(true);
    }

    ImGui::SameLine();
    const ScriptDocument* document = ActiveDocument();
    const bool disableSave = !document || !document->dirty;
    if (disableSave) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save")) {
        SaveActiveDocument();
    }
    if (disableSave) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    const bool disableAdd = document == nullptr;
    if (disableAdd) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Add Button")) {
        AddElement(ElementType::Button, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Text")) {
        AddElement(ElementType::Label, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Panel")) {
        AddElement(ElementType::Panel, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Image Button")) {
        AddElement(ElementType::Button, true);
    }
    if (disableAdd) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    ImGui::Checkbox("Code", &m_showCodeEditor);
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &m_snapToGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Scenes", &m_showScenes);
    ImGui::SameLine();
    ImGui::Checkbox("Components", &m_showComponents);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu scenes / %zu components", m_sceneDocuments.size(), m_componentDocuments.size());
    ImGui::EndChild();
}

void UIDesigner::RenderDocumentNavigator() {
    ImGui::BeginChild("ui-document-browser", ImVec2(0.0f, 0.0f), false);

    if (m_sceneDocuments.empty() && m_componentDocuments.empty()) {
        ImGui::TextDisabled("No scripts were found under Data/Scripts/scenes or Data/Scripts/components.");
        ImGui::EndChild();
        return;
    }

    const auto drawGroup = [&](DocumentKind kind, const char* label, const std::vector<ScriptDocument>& documents, bool visible) {
        if (!visible) {
            return;
        }

        if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const ScriptDocument& document : documents) {
                std::string itemLabel = document.displayName;
                if (document.dirty) {
                    itemLabel += " *";
                }
                const bool selected = document.id == m_activeDocumentId;
                if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                    SelectDocument(kind, document.id);
                }
            }
        }
    };

    drawGroup(DocumentKind::Scene, "Scenes", m_sceneDocuments, m_showScenes);
    drawGroup(DocumentKind::Component, "Components", m_componentDocuments, m_showComponents);
    ImGui::EndChild();
}

void UIDesigner::RenderCanvasWorkspace(const RuntimeCanvasRenderer& runtimeRenderer) {
    ImGui::BeginChild("ui-canvas-workspace", ImVec2(0.0f, 0.0f), false);
    if (const ScriptDocument* document = ActiveDocument()) {
        RenderDocumentSummary(*document);
        ImGui::Separator();
    }
    RenderPreviewContents(runtimeRenderer, true);
    ImGui::EndChild();
}

void UIDesigner::RenderCodeEditor() {
    ScriptDocument* document = ActiveDocument();
    if (!document) {
        ImGui::TextDisabled("Choose a scene or component to edit.");
        return;
    }

    ImGui::BeginChild("ui-code-editor", ImVec2(0.0f, 0.0f), false);
    RenderDocumentSummary(*document);
    ImGui::Separator();

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
    if (ImGui::InputTextMultiline("##ui-source-buffer", &document->source, ImVec2(-1.0f, -1.0f), flags)) {
        document->dirty = true;
        Touch();
        ParseDocument(*document);
    }
    ImGui::EndChild();
}

void UIDesigner::RenderDocumentSummary(const ScriptDocument& document) {
    ImGui::TextUnformatted(document.displayName.c_str());
    ImGui::TextDisabled("%s", document.runtimePath.c_str());
    if (!document.parseWarning.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.46f, 1.0f), "%s", document.parseWarning.c_str());
    } else {
        ImGui::TextDisabled("%zu parsed UI blocks", document.elements.size());
    }
}

void UIDesigner::RenderInspector() {
    EnsureDocuments();
    ScriptDocument* document = ActiveDocument();
    if (!document) {
        ImGui::TextDisabled("Select a scene or component.");
        return;
    }

    RenderDocumentSummary(*document);
    ImGui::Separator();

    if (document->kind == DocumentKind::Scene) {
        ImGui::SeparatorText("Scene");
        std::string background = document->backgroundAsset;
        if (ImGui::InputText("Background", &background)) {
            if (ReplaceBackgroundAsset(*document, background)) {
                ParseDocument(*document);
            }
        }

        const std::string selectedResource = m_selectedResourceCallback ? m_selectedResourceCallback() : std::string{};
        if (!selectedResource.empty()) {
            if (ImGui::Button("Use Explorer Asset As Background")) {
                if (ReplaceBackgroundAsset(*document, selectedResource)) {
                    ParseDocument(*document);
                }
            }
        }

        if (!document->linkedComponents.empty()) {
            ImGui::SeparatorText("Linked Components");
            for (const std::string& component : document->linkedComponents) {
                if (ImGui::Selectable(component.c_str(), false)) {
                    if (ScriptDocument* linked = FindDocument(DocumentKind::Component, component)) {
                        SelectDocument(DocumentKind::Component, linked->id);
                    }
                }
            }
        }
    } else if (!document->linkedScenes.empty()) {
        ImGui::SeparatorText("Previewed In");
        for (const std::string& scene : document->linkedScenes) {
            if (ImGui::Selectable(scene.c_str(), false)) {
                if (ScriptDocument* linked = FindDocument(DocumentKind::Scene, scene)) {
                    SelectDocument(DocumentKind::Scene, linked->id);
                }
            }
        }
    }

    if (!document->elements.empty()) {
        ImGui::SeparatorText("Elements");
        for (const ParsedElement& candidate : document->elements) {
            const bool selected = candidate.id == m_activeElementId;
            if (ImGui::Selectable((candidate.name + "##element").c_str(), selected)) {
                m_activeElementId = candidate.id;
            }
        }
    }

    if (ParsedElement* element = ActiveElement()) {
        ImGui::SeparatorText("Properties");
        RenderElementInspector(*document, *element);
    }
}

void UIDesigner::RenderElementInspector(ScriptDocument& document, ParsedElement& element) {
    ImGui::TextUnformatted(element.name.c_str());
    ImGui::TextDisabled("%s", ElementTypeLabel(element.type).c_str());

    std::string text = element.text;
    if (element.textSpan.valid && ImGui::InputText("Text", &text)) {
        SetStringValue(document, element.textSpan, text);
        ParseDocument(document);
        return;
    }

    int x = static_cast<int>(std::round(element.x));
    if (element.xSpan.valid && ImGui::DragInt("X", &x, 1.0f, -4096, 4096)) {
        ReplaceValue(document, element.xSpan, std::to_string(x));
        ParseDocument(document);
        return;
    }

    int y = static_cast<int>(std::round(element.y));
    if (element.ySpan.valid && ImGui::DragInt("Y", &y, 1.0f, -4096, 4096)) {
        ReplaceValue(document, element.ySpan, std::to_string(y));
        ParseDocument(document);
        return;
    }

    int width = static_cast<int>(std::round(element.w));
    if (element.wSpan.valid && ImGui::DragInt("Width", &width, 1.0f, 0, 4096)) {
        ReplaceValue(document, element.wSpan, std::to_string(std::max(0, width)));
        ParseDocument(document);
        return;
    }

    int height = static_cast<int>(std::round(element.h));
    if (element.hSpan.valid && ImGui::DragInt("Height", &height, 1.0f, 0, 4096)) {
        ReplaceValue(document, element.hSpan, std::to_string(std::max(0, height)));
        ParseDocument(document);
        return;
    }

    int fontSize = element.fontSize;
    if (element.fontSizeSpan.valid && ImGui::DragInt("Font Size", &fontSize, 1.0f, 8, 128)) {
        SetIntValue(document, element.fontSizeSpan, fontSize);
        ParseDocument(document);
        return;
    }

    if (element.type == ElementType::Button) {
        std::string imageAsset = element.imageAsset;
        if (ImGui::InputText("Image Asset", &imageAsset)) {
            UpsertStringProperty(document, element, element.imageAssetSpan, "imageAsset", imageAsset);
            ParseDocument(document);
            return;
        }

        const std::string selectedResource = m_selectedResourceCallback ? m_selectedResourceCallback() : std::string{};
        if (!selectedResource.empty() && ImGui::Button("Use Explorer Asset As Button Image")) {
            UpsertStringProperty(document, element, element.imageAssetSpan, "imageAsset", selectedResource);
            ParseDocument(document);
            return;
        }
    }

    if (element.colorSpan.valid) {
        ImVec4 color = element.color;
        if (ImGui::ColorEdit4("Primary Color", &color.x, ImGuiColorEditFlags_AlphaBar)) {
            SetColorValue(document, element.colorSpan, color);
            ParseDocument(document);
            return;
        }
    }

    if (element.accentColorSpan.valid) {
        ImVec4 color = element.accentColor;
        if (ImGui::ColorEdit4("Surface", &color.x, ImGuiColorEditFlags_AlphaBar)) {
            SetColorValue(document, element.accentColorSpan, color);
            ParseDocument(document);
            return;
        }
    }

    if (element.accentColor2Span.valid) {
        ImVec4 color = element.accentColor2;
        if (ImGui::ColorEdit4("Hover Surface", &color.x, ImGuiColorEditFlags_AlphaBar)) {
            SetColorValue(document, element.accentColor2Span, color);
            ParseDocument(document);
            return;
        }
    }
}

void UIDesigner::RenderPreview(const RuntimeCanvasRenderer& runtimeRenderer) {
    EnsureDocuments();
    RenderPreviewContents(runtimeRenderer, false);
}

void UIDesigner::RenderPreviewContents(const RuntimeCanvasRenderer& runtimeRenderer, bool embedded) {
    ScriptDocument* document = ActiveDocument();
    if (!document) {
        ImGui::TextDisabled("Select a scene or component.");
        return;
    }

    if (!embedded) {
        ImGui::TextDisabled("UI Editor now edits the real Data/Scripts scenes and components. It no longer depends on a generated ui_scene.lua.");
        ImGui::Separator();
    }

    const char* childId = embedded ? "ui-live-preview-embedded" : "ui-live-preview";
    ImGui::BeginChild(childId, ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("ui-preview-canvas", available);
    const ImRect outer(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const ImVec2 fitted = FitRect(available, kPreviewWidth, kPreviewHeight);
    const ImVec2 previewMin(
        outer.Min.x + (outer.GetWidth() - fitted.x) * 0.5f,
        outer.Min.y + (outer.GetHeight() - fitted.y) * 0.5f);
    const ImRect previewRect(previewMin, previewMin + fitted);

    m_lastPreview = BuildStaticPreview(*document);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(previewRect.Min, previewRect.Max, IM_COL32(8, 8, 8, 255), 18.0f);
    drawList->AddRect(previewRect.Min, previewRect.Max, IM_COL32(48, 48, 48, 255), 18.0f, ImDrawFlags_RoundCornersAll, 1.0f);

    RuntimeCanvasResult runtimeCanvas;
    if (runtimeRenderer) {
        const float scale = previewRect.GetWidth() / kPreviewWidth;
        int mouseX = -1000;
        int mouseY = -1000;
        if (previewRect.Contains(ImGui::GetMousePos())) {
            mouseX = static_cast<int>((ImGui::GetMousePos().x - previewRect.Min.x) / scale);
            mouseY = static_cast<int>((ImGui::GetMousePos().y - previewRect.Min.y) / scale);
        }
        runtimeCanvas = runtimeRenderer(previewRect, scale, mouseX, mouseY, false, false);
    }

    if (runtimeCanvas.rendered && runtimeCanvas.texture) {
        drawList->AddImage(runtimeCanvas.texture, previewRect.Min, previewRect.Max);
    } else {
        DrawPreviewCommands(previewRect, m_lastPreview);
    }

    const float scale = previewRect.GetWidth() / kPreviewWidth;
    const ImVec2 origin = previewRect.Min;
    const ImVec2 mouseScreen = ImGui::GetIO().MousePos;
    const bool hovered = previewRect.Contains(mouseScreen);
    const ImVec2 canvasMouse = hovered
        ? ImVec2((mouseScreen.x - origin.x) / scale, (mouseScreen.y - origin.y) / scale)
        : ImVec2(0.0f, 0.0f);

    const auto findHitRegion = [&](const ImVec2& canvasPoint) -> const PreviewHitRegion* {
        for (auto it = m_lastPreview.hitRegions.rbegin(); it != m_lastPreview.hitRegions.rend(); ++it) {
            const ImRect bounds(it->position, it->position + it->size);
            if (bounds.Contains(canvasPoint)) {
                return &(*it);
            }
        }
        return nullptr;
    };

    const auto findSelectionRegion = [&]() -> const PreviewHitRegion* {
        for (const PreviewHitRegion& region : m_lastPreview.hitRegions) {
            if (region.kind == m_activeKind && region.documentId == m_activeDocumentId && region.elementId == m_activeElementId) {
                return &region;
            }
        }
        return nullptr;
    };

    const PreviewHitRegion* selectionRegion = findSelectionRegion();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (const PreviewHitRegion* hit = findHitRegion(canvasMouse)) {
            SetSelection(hit->kind, hit->documentId, hit->elementId);
            m_interactionKind = hit->kind;
            m_interactionDocumentId = hit->documentId;
            m_interactionElementId = hit->elementId;
            m_interactionStartMouse = canvasMouse;
            m_interactionStartPosition = hit->position;
            m_interactionStartSize = hit->size;

            const ImVec2 selectionMax = origin + (hit->position + hit->size) * scale;
            const ImRect handleRect(selectionMax - ImVec2(14.0f, 14.0f), selectionMax);
            m_previewInteraction = handleRect.Contains(mouseScreen) ? PreviewInteraction::Resize : PreviewInteraction::Move;
        } else {
            m_activeElementId = 0;
            m_previewInteraction = PreviewInteraction::None;
        }
    }

    if (m_previewInteraction != PreviewInteraction::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (ScriptDocument* interactionDocument = FindDocumentById(m_interactionKind, m_interactionDocumentId)) {
            auto it = std::find_if(interactionDocument->elements.begin(), interactionDocument->elements.end(), [&](const ParsedElement& element) {
                return element.id == m_interactionElementId;
            });
            if (it != interactionDocument->elements.end()) {
                const ImVec2 delta = canvasMouse - m_interactionStartMouse;
                float x = m_interactionStartPosition.x;
                float y = m_interactionStartPosition.y;
                float w = m_interactionStartSize.x;
                float h = m_interactionStartSize.y;

                if (m_previewInteraction == PreviewInteraction::Move) {
                    x += delta.x;
                    y += delta.y;
                } else {
                    w = std::max(24.0f, w + delta.x);
                    h = std::max(24.0f, h + delta.y);
                }

                if (m_snapToGrid) {
                    x = SnapValue(x, 8.0f);
                    y = SnapValue(y, 8.0f);
                    w = SnapValue(w, 8.0f);
                    h = SnapValue(h, 8.0f);
                }

                const bool changed =
                    (it->xSpan.valid && std::fabs(it->x - x) > 0.1f) ||
                    (it->ySpan.valid && std::fabs(it->y - y) > 0.1f) ||
                    (it->wSpan.valid && std::fabs(it->w - w) > 0.1f) ||
                    (it->hSpan.valid && std::fabs(it->h - h) > 0.1f);

                if (changed) {
                    ApplyElementFrame(*interactionDocument, *it, x, y, w, h);
                    ParseDocument(*interactionDocument);
                    SetSelection(m_interactionKind, m_interactionDocumentId, m_interactionElementId);
                    Touch();
                }
            }
        }
    } else if (m_previewInteraction != PreviewInteraction::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_previewInteraction = PreviewInteraction::None;
    }

    selectionRegion = findSelectionRegion();
    if (selectionRegion) {
        const ImVec2 selectionMin = origin + selectionRegion->position * scale;
        const ImVec2 selectionMax = origin + (selectionRegion->position + selectionRegion->size) * scale;
        drawList->AddRect(selectionMin, selectionMax, IM_COL32(255, 255, 255, 235), 0.0f, 0, 2.0f);
        const ImRect handleRect(selectionMax - ImVec2(14.0f, 14.0f), selectionMax);
        drawList->AddRectFilled(handleRect.Min, handleRect.Max, IM_COL32(255, 255, 255, 240), 3.0f);
        drawList->AddRect(handleRect.Min, handleRect.Max, IM_COL32(18, 18, 18, 255), 3.0f, 0, 1.0f);
    }

    if (!runtimeCanvas.status.empty()) {
        drawList->AddText(previewRect.Min + ImVec2(16.0f, 16.0f), IM_COL32(220, 220, 220, 255), runtimeCanvas.status.c_str());
    } else if (!m_lastPreview.targetLabel.empty()) {
        drawList->AddText(previewRect.Min + ImVec2(16.0f, 16.0f), IM_COL32(220, 220, 220, 255), m_lastPreview.targetLabel.c_str());
    }

    ImGui::EndChild();
}

void UIDesigner::DrawPreviewCommands(const ImRect& rect, const PreviewResult& preview) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float scale = rect.GetWidth() / kPreviewWidth;
    const ImVec2 origin = rect.Min;

    if (!preview.error.empty()) {
        drawList->AddText(rect.Min + ImVec2(18.0f, 18.0f), IM_COL32(255, 210, 170, 255), preview.error.c_str());
        return;
    }

    for (const PreviewCommand& command : preview.commands) {
        const ImVec2 min = origin + command.position * scale;
        const ImVec2 max = origin + (command.position + command.size) * scale;
        if (command.type == PreviewCommand::Type::Rect) {
            drawList->AddRectFilled(min, max, ToColor32(command.color), 0.0f);
            continue;
        }

        const float fontSize = std::max(10.0f, command.fontSize * scale);
        if (command.outlineSize > 0) {
            const float outline = std::max(1.0f, command.outlineSize * scale);
            const std::array<ImVec2, 8> offsets{
                ImVec2(-outline, 0.0f), ImVec2(outline, 0.0f), ImVec2(0.0f, -outline), ImVec2(0.0f, outline),
                ImVec2(-outline, -outline), ImVec2(outline, -outline), ImVec2(-outline, outline), ImVec2(outline, outline)
            };
            for (const ImVec2& offset : offsets) {
                drawList->AddText(nullptr, fontSize, min + offset, ToColor32(command.outlineColor), command.text.c_str());
            }
        }
        drawList->AddText(nullptr, fontSize, min, ToColor32(command.color), command.text.c_str());
    }
}

void UIDesigner::ApplyAssetToSelection(const std::string& assetPath) {
    if (ScriptDocument* document = ActiveDocument()) {
        if (ParsedElement* element = ActiveElement()) {
            if (element->type == ElementType::Button) {
                UpsertStringProperty(*document, *element, element->imageAssetSpan, "imageAsset", assetPath);
                ParseDocument(*document);
                Touch();
                Log("Updated button image: " + assetPath);
                return;
            }
        }

        if (document->kind == DocumentKind::Scene && ReplaceBackgroundAsset(*document, assetPath)) {
            ParseDocument(*document);
            Touch();
            Log("Updated scene background: " + assetPath);
            return;
        }
    }
}

bool UIDesigner::HasSelection() const {
    return ActiveDocument() != nullptr;
}

std::string UIDesigner::GetSelectionSummary() const {
    if (const ParsedElement* element = ActiveElement()) {
        return element->name;
    }
    if (const ScriptDocument* document = ActiveDocument()) {
        return DocumentKindLabel(document->kind) + ": " + document->displayName;
    }
    return "Nothing selected";
}

std::string UIDesigner::GenerateLua() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        return document->source;
    }
    return {};
}

std::vector<std::string> UIDesigner::BuildPreviewLines() const {
    std::vector<std::string> lines;
    if (const ScriptDocument* document = ActiveDocument()) {
        lines.push_back(DocumentKindLabel(document->kind) + ": " + document->displayName);
        lines.push_back("Scene Preview: " + ResolvePreviewScenePath());
        lines.push_back("Parsed UI Blocks: " + std::to_string(document->elements.size()));
    }
    return lines;
}

std::vector<UIDesigner::GeneratedDocument> UIDesigner::BuildGeneratedDocuments() const {
    const fs::path dataRoot = LoadDataRoot();
    if (dataRoot.empty()) {
        return {};
    }

    std::vector<GeneratedDocument> documents;
    documents.reserve(m_sceneDocuments.size() + m_componentDocuments.size());

    const auto append = [&](const std::vector<ScriptDocument>& source) {
        for (const ScriptDocument& document : source) {
            GeneratedDocument generated;
            generated.label = DocumentKindLabel(document.kind) + ": " + document.displayName;
            generated.relativePath = fs::relative(document.path, dataRoot);
            generated.content = document.source;
            documents.push_back(std::move(generated));
        }
    };

    append(m_sceneDocuments);
    append(m_componentDocuments);
    return documents;
}

std::string UIDesigner::CurrentDocumentRuntimePath() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        return NormalizePath(document->runtimePath);
    }
    return {};
}

std::string UIDesigner::GeneratedSceneScriptPath() const {
    return ResolvePreviewScenePath();
}

void UIDesigner::EnsureDocuments() {
    if (m_forceRefresh) {
        RefreshDocuments(true);
    }
}

void UIDesigner::RefreshDocuments(bool force) {
    if (!force && !m_forceRefresh) {
        return;
    }

    m_sceneDocuments.clear();
    m_componentDocuments.clear();
    m_activeDocumentId = 0;
    m_activeElementId = 0;
    m_nextDocumentId = 1;

    for (const fs::path& path : EnumerateDocuments(DocumentKind::Scene)) {
        ScriptDocument document;
        document.id = m_nextDocumentId++;
        document.kind = DocumentKind::Scene;
        document.path = path;
        document.runtimePath = NormalizePath(fs::relative(path, LoadDataRoot()).generic_string());
        document.displayName = path.stem().string();

        std::ifstream in(path, std::ios::binary);
        if (in) {
            document.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        ParseDocument(document);
        m_sceneDocuments.push_back(std::move(document));
    }

    for (const fs::path& path : EnumerateDocuments(DocumentKind::Component)) {
        ScriptDocument document;
        document.id = m_nextDocumentId++;
        document.kind = DocumentKind::Component;
        document.path = path;
        document.runtimePath = NormalizePath(fs::relative(path, LoadDataRoot()).generic_string());
        document.displayName = path.stem().string();

        std::ifstream in(path, std::ios::binary);
        if (in) {
            document.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        ParseDocument(document);
        m_componentDocuments.push_back(std::move(document));
    }

    RebuildSceneLinks();

    if (!m_sceneDocuments.empty()) {
        SelectDocument(DocumentKind::Scene, m_sceneDocuments.front().id);
    } else if (!m_componentDocuments.empty()) {
        SelectDocument(DocumentKind::Component, m_componentDocuments.front().id);
    }

    m_forceRefresh = false;
    Touch();
}

fs::path UIDesigner::LoadDataRoot() const {
    if (m_projectRoot.empty()) {
        return {};
    }
    return m_projectRoot / "Data";
}

std::vector<fs::path> UIDesigner::EnumerateDocuments(DocumentKind kind) const {
    std::vector<fs::path> documents;
    const fs::path dataRoot = LoadDataRoot();
    if (dataRoot.empty()) {
        return documents;
    }

    const fs::path scriptRoot = kind == DocumentKind::Scene
        ? (dataRoot / "Scripts" / "scenes")
        : (dataRoot / "Scripts" / "components");

    if (!fs::exists(scriptRoot)) {
        return documents;
    }

    for (const auto& entry : fs::recursive_directory_iterator(scriptRoot)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lua") {
            documents.push_back(entry.path());
        }
    }

    std::sort(documents.begin(), documents.end());
    return documents;
}

std::string UIDesigner::DocumentKindLabel(DocumentKind kind) const {
    return kind == DocumentKind::Scene ? "Scene" : "Component";
}

std::string UIDesigner::ElementTypeLabel(ElementType type) const {
    switch (type) {
        case ElementType::Button:
            return "Button";
        case ElementType::Label:
            return "Label";
        case ElementType::Panel:
            return "Panel";
    }
    return "Element";
}

std::string UIDesigner::ResolvePreviewScenePath() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        if (document->kind == DocumentKind::Scene) {
            return NormalizePath(document->runtimePath);
        }
        if (!document->linkedScenes.empty()) {
            return document->linkedScenes.front();
        }
    }
    if (!m_sceneDocuments.empty()) {
        return m_sceneDocuments.front().runtimePath;
    }
    return "Scripts/scenes/title_scene.lua";
}

UIDesigner::ScriptDocument* UIDesigner::ActiveDocument() {
    return FindDocumentById(m_activeKind, m_activeDocumentId);
}

const UIDesigner::ScriptDocument* UIDesigner::ActiveDocument() const {
    return FindDocumentById(m_activeKind, m_activeDocumentId);
}

UIDesigner::ParsedElement* UIDesigner::ActiveElement() {
    if (ScriptDocument* document = ActiveDocument()) {
        auto it = std::find_if(document->elements.begin(), document->elements.end(), [&](const ParsedElement& element) {
            return element.id == m_activeElementId;
        });
        return it == document->elements.end() ? nullptr : &(*it);
    }
    return nullptr;
}

const UIDesigner::ParsedElement* UIDesigner::ActiveElement() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        auto it = std::find_if(document->elements.begin(), document->elements.end(), [&](const ParsedElement& element) {
            return element.id == m_activeElementId;
        });
        return it == document->elements.end() ? nullptr : &(*it);
    }
    return nullptr;
}

UIDesigner::ScriptDocument* UIDesigner::FindDocumentById(DocumentKind kind, int documentId) {
    auto& documents = kind == DocumentKind::Scene ? m_sceneDocuments : m_componentDocuments;
    auto it = std::find_if(documents.begin(), documents.end(), [&](const ScriptDocument& document) {
        return document.id == documentId;
    });
    return it == documents.end() ? nullptr : &(*it);
}

const UIDesigner::ScriptDocument* UIDesigner::FindDocumentById(DocumentKind kind, int documentId) const {
    const auto& documents = kind == DocumentKind::Scene ? m_sceneDocuments : m_componentDocuments;
    auto it = std::find_if(documents.begin(), documents.end(), [&](const ScriptDocument& document) {
        return document.id == documentId;
    });
    return it == documents.end() ? nullptr : &(*it);
}

UIDesigner::ScriptDocument* UIDesigner::FindDocument(DocumentKind kind, std::string_view runtimePath) {
    auto& documents = kind == DocumentKind::Scene ? m_sceneDocuments : m_componentDocuments;
    auto it = std::find_if(documents.begin(), documents.end(), [&](const ScriptDocument& document) {
        return document.runtimePath == runtimePath;
    });
    return it == documents.end() ? nullptr : &(*it);
}

const UIDesigner::ScriptDocument* UIDesigner::FindDocument(DocumentKind kind, std::string_view runtimePath) const {
    const auto& documents = kind == DocumentKind::Scene ? m_sceneDocuments : m_componentDocuments;
    auto it = std::find_if(documents.begin(), documents.end(), [&](const ScriptDocument& document) {
        return document.runtimePath == runtimePath;
    });
    return it == documents.end() ? nullptr : &(*it);
}

void UIDesigner::ParseDocument(ScriptDocument& document) {
    const auto rebuildLinks = [this]() {
        RebuildSceneLinks();
    };

    document.parseWarning.clear();
    document.linkedComponents.clear();
    document.linkedScenes.clear();
    document.backgroundAsset.clear();
    document.backgroundSpan = {};
    document.elements.clear();

    if (document.source.empty()) {
        document.parseWarning = "File is empty.";
        rebuildLinks();
        return;
    }

    const auto parseFloatOr = [&](const ValueSpan& span, float fallback) {
        if (!span.valid) {
            return fallback;
        }

        const std::string expression = Trim(ReadSpan(document.source, span));
        try {
            size_t consumed = 0;
            const float literal = std::stof(expression, &consumed);
            if (consumed == expression.size()) {
                return literal;
            }
        } catch (...) {
        }

        float value = 0.0f;
        if (TryEvaluateNumericExpression(expression, value)) {
            return value;
        }
        return fallback;
    };

    const auto parseIntOr = [&](const ValueSpan& span, int fallback) {
        return static_cast<int>(std::round(parseFloatOr(span, static_cast<float>(fallback))));
    };

    if (document.kind == DocumentKind::Scene) {
        size_t searchFrom = 0;
        while ((searchFrom = document.source.find("include(", searchFrom)) != std::string::npos) {
            const size_t quote = document.source.find('"', searchFrom);
            if (quote == std::string::npos) {
                break;
            }
            const size_t endQuote = document.source.find('"', quote + 1);
            if (endQuote == std::string::npos) {
                break;
            }
            const std::string path = NormalizePath(document.source.substr(quote + 1, endQuote - quote - 1));
            if (path.find("Scripts/components/") != std::string::npos) {
                document.linkedComponents.push_back(path);
            }
            searchFrom = endQuote + 1;
        }

        const size_t drawAuto = document.source.find("Engine.DrawAuto(");
        if (drawAuto != std::string::npos) {
            const std::string_view tail(document.source.data() + drawAuto, document.source.size() - drawAuto);
            const QuotedLiteralSpan backgroundSpan = FindFirstQuotedLiteral(tail, drawAuto);
            document.backgroundSpan = {backgroundSpan.start, backgroundSpan.end, backgroundSpan.valid};
            if (document.backgroundSpan.valid) {
                document.backgroundAsset = Trim(UnescapeLuaString(ReadSpan(document.source, document.backgroundSpan)));
            }
        }
    }

    const std::array<std::pair<std::string_view, ElementType>, 3> kinds{{
        {"UI.Button({", ElementType::Button},
        {"UI.Label({", ElementType::Label},
        {"UI.Panel({", ElementType::Panel},
    }};

    int nextElementId = 1;
    for (const auto& [marker, type] : kinds) {
        for (const UiCallBounds& bounds : FindUiCallBounds(document.source, marker)) {
            ParsedElement element;
            element.id = nextElementId++;
            element.type = type;
            element.callStart = bounds.callStart;
            element.callEnd = bounds.callEnd;
            element.blockStart = bounds.blockStart;
            element.blockEnd = bounds.blockEnd;

            const std::string_view blockView(document.source.data() + bounds.blockStart, bounds.blockEnd - bounds.blockStart);
            element.textSpan = FindQuotedValue(blockView, "text", bounds.blockStart);
            element.imageAssetSpan = FindQuotedValue(blockView, "imageAsset", bounds.blockStart);
            element.xSpan = FindExpressionValue(blockView, "x", bounds.blockStart);
            element.ySpan = FindExpressionValue(blockView, "y", bounds.blockStart);
            element.wSpan = FindExpressionValue(blockView, "w", bounds.blockStart);
            element.hSpan = FindExpressionValue(blockView, "h", bounds.blockStart);
            element.fontSizeSpan = FindExpressionValue(blockView, "fontSize", bounds.blockStart);
            element.colorSpan = FindTableValue(blockView, type == ElementType::Button ? "normalColor" : "color", bounds.blockStart);
            element.accentColorSpan = FindTableValue(blockView, type == ElementType::Button ? "bgColor" : "color", bounds.blockStart);
            element.accentColor2Span = FindTableValue(blockView, type == ElementType::Button ? "hoverBgColor" : "hoverColor", bounds.blockStart);

            element.text = Trim(UnescapeLuaString(ReadSpan(document.source, element.textSpan)));
            element.imageAsset = Trim(UnescapeLuaString(ReadSpan(document.source, element.imageAssetSpan)));
            element.x = parseFloatOr(element.xSpan, 0.0f);
            element.y = parseFloatOr(element.ySpan, 0.0f);
            element.w = parseFloatOr(element.wSpan, 0.0f);
            element.h = parseFloatOr(element.hSpan, 0.0f);
            element.fontSize = parseIntOr(element.fontSizeSpan, 24);

            const ImVec4 fallbackText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            const ImVec4 fallbackSurface = type == ElementType::Panel ? ImVec4(0.1f, 0.1f, 0.1f, 0.8f) : ImVec4(0.0f, 0.0f, 0.0f, 0.4f);
            const ImVec4 fallbackHover = type == ElementType::Button ? ImVec4(1.0f, 1.0f, 1.0f, 0.15f) : fallbackText;
            element.color = ColorFromNumbers(ParseColor(element.colorSpan, document.source), fallbackText);
            element.accentColor = ColorFromNumbers(ParseColor(element.accentColorSpan, document.source), fallbackSurface);
            element.accentColor2 = ColorFromNumbers(ParseColor(element.accentColor2Span, document.source), fallbackHover);

            element.name = ElementTypeLabel(type);
            if (!element.text.empty()) {
                element.name += ": " + element.text;
            } else if (!element.imageAsset.empty()) {
                element.name += ": " + fs::path(element.imageAsset).stem().string();
            }

            document.elements.push_back(std::move(element));
        }
    }

    if (document.kind == DocumentKind::Component && document.elements.empty()) {
        document.parseWarning = "No structured UI.Button / UI.Label / UI.Panel blocks were found in this component.";
    }

    rebuildLinks();
}

void UIDesigner::RebuildSceneLinks() {
    for (auto& component : m_componentDocuments) {
        component.linkedScenes.clear();
    }

    for (const auto& scene : m_sceneDocuments) {
        for (const std::string& componentPath : scene.linkedComponents) {
            if (ScriptDocument* component = FindDocument(DocumentKind::Component, componentPath)) {
                component->linkedScenes.push_back(scene.runtimePath);
            }
        }
    }
}

void UIDesigner::SelectDocument(DocumentKind kind, int documentId) {
    m_activeKind = kind;
    m_activeDocumentId = documentId;
    m_activeElementId = 0;
    if (const ScriptDocument* document = ActiveDocument()) {
        if (!document->elements.empty()) {
            m_activeElementId = document->elements.front().id;
        }
    }
}

void UIDesigner::SetSelection(DocumentKind kind, int documentId, int elementId) {
    m_activeKind = kind;
    m_activeDocumentId = documentId;
    m_activeElementId = elementId;
}

void UIDesigner::SaveActiveDocument() {
    if (ScriptDocument* document = ActiveDocument()) {
        std::ofstream out(document->path, std::ios::binary);
        if (!out.is_open()) {
            Log("Failed to save " + document->path.string());
            return;
        }
        out << document->source;
        document->dirty = false;
        Log("Saved " + document->runtimePath);
    }
}

void UIDesigner::ReplaceValue(ScriptDocument& document, const ValueSpan& span, std::string_view replacement) {
    if (!span.valid || span.end < span.start || span.end > document.source.size()) {
        return;
    }
    document.source.replace(span.start, span.end - span.start, replacement);
    document.dirty = true;
}

void UIDesigner::SetStringValue(ScriptDocument& document, ValueSpan& span, const std::string& value) {
    ReplaceValue(document, span, EscapeLuaString(value));
}

void UIDesigner::SetIntValue(ScriptDocument& document, ValueSpan& span, int value) {
    ReplaceValue(document, span, std::to_string(value));
}

void UIDesigner::SetColorValue(ScriptDocument& document, ValueSpan& span, const ImVec4& value) {
    ReplaceValue(document, span, FormatColorValue(value));
}

void UIDesigner::ApplyElementFrame(ScriptDocument& document, const ParsedElement& element, float x, float y, float w, float h) {
    struct Replacement {
        ValueSpan span;
        std::string value;
    };

    std::vector<Replacement> replacements;
    if (element.xSpan.valid) replacements.push_back({element.xSpan, std::to_string(static_cast<int>(std::round(x)))});
    if (element.ySpan.valid) replacements.push_back({element.ySpan, std::to_string(static_cast<int>(std::round(y)))});
    if (element.wSpan.valid) replacements.push_back({element.wSpan, std::to_string(static_cast<int>(std::round(std::max(0.0f, w))))});
    if (element.hSpan.valid) replacements.push_back({element.hSpan, std::to_string(static_cast<int>(std::round(std::max(0.0f, h))))});

    std::sort(replacements.begin(), replacements.end(), [](const Replacement& lhs, const Replacement& rhs) {
        return lhs.span.start > rhs.span.start;
    });

    for (const Replacement& replacement : replacements) {
        ReplaceValue(document, replacement.span, replacement.value);
    }
}

bool UIDesigner::UpsertStringProperty(ScriptDocument& document, const ParsedElement& element, const ValueSpan& span, std::string_view key, const std::string& value) {
    if (span.valid) {
        ValueSpan editableSpan = span;
        SetStringValue(document, editableSpan, NormalizePath(value));
        return true;
    }

    if (element.blockEnd <= element.blockStart + 1 || element.blockEnd > document.source.size()) {
        return false;
    }

    const size_t closingBracePos = element.blockEnd - 1;
    const size_t closingLineStart = ClosingBraceLineStart(document.source, closingBracePos);
    const std::string closingIndent = document.source.substr(closingLineStart, closingBracePos - closingLineStart);
    const std::string propertyIndent = closingIndent + "    ";

    size_t probe = closingLineStart;
    while (probe > element.blockStart + 1 && std::isspace(static_cast<unsigned char>(document.source[probe - 1])) != 0) {
        --probe;
    }
    const char previous = probe > element.blockStart + 1 ? document.source[probe - 1] : '{';
    const std::string prefix = previous == '{' ? "\n" : ",\n";
    document.source.insert(closingLineStart, prefix + propertyIndent + std::string(key) + " = " + EscapeLuaString(NormalizePath(value)));
    document.dirty = true;
    return true;
}

bool UIDesigner::InsertElementAfterReference(ScriptDocument& document, const ParsedElement& reference, std::string_view expression) {
    if (reference.callEnd > document.source.size()) {
        return false;
    }

    size_t next = reference.callEnd;
    while (next < document.source.size() && std::isspace(static_cast<unsigned char>(document.source[next])) != 0) {
        ++next;
    }

    const std::string indent = IndentForLine(document.source, reference.callStart);
    if (next < document.source.size() && document.source[next] == ',') {
        document.source.insert(next + 1, "\n" + indent + std::string(expression) + ",");
        document.dirty = true;
        return true;
    }

    if (next < document.source.size() && document.source[next] == '}') {
        document.source.insert(reference.callEnd, ",\n" + indent + std::string(expression));
        document.dirty = true;
        return true;
    }

    return false;
}

void UIDesigner::AddElement(ElementType type, bool imageButton) {
    ScriptDocument* document = ActiveDocument();
    if (!document) {
        return;
    }

    const ParsedElement* reference = ActiveElement();
    if (!reference && !document->elements.empty()) {
        reference = &document->elements.back();
    }
    if (!reference) {
        Log("Add element currently needs an existing list-style UI block in the script.");
        return;
    }

    const float baseX = reference->x + 32.0f;
    const float baseY = reference->y + 32.0f;
    const float baseW = reference->w > 0.0f ? reference->w : (type == ElementType::Panel ? 320.0f : 220.0f);
    const float baseH = reference->h > 0.0f ? reference->h : (type == ElementType::Panel ? 160.0f : 64.0f);
    const std::string elementIndent = IndentForLine(document->source, reference->callStart);
    const std::string propertyIndent = elementIndent + "    ";
    const std::string selectedAsset = m_selectedResourceCallback ? NormalizePath(m_selectedResourceCallback()) : std::string{};

    std::ostringstream block;
    switch (type) {
        case ElementType::Button:
            block << "UI.Button({\n"
                  << propertyIndent << "text = " << EscapeLuaString(imageButton ? "Image Button" : "New Button") << ",\n"
                  << propertyIndent << "x = " << static_cast<int>(std::round(baseX)) << ", y = " << static_cast<int>(std::round(baseY)) << ",\n"
                  << propertyIndent << "w = " << static_cast<int>(std::round(std::max(120.0f, baseW))) << ", h = " << static_cast<int>(std::round(std::max(44.0f, baseH))) << ",\n"
                  << propertyIndent << "fontSize = 24,\n";
            if (imageButton) {
                block << propertyIndent << "imageAsset = " << EscapeLuaString(selectedAsset.empty() ? "button.png" : selectedAsset) << ",\n";
            }
            block << propertyIndent << "normalColor = { 255, 255, 255, 255 },\n"
                  << propertyIndent << "hoverColor = { 255, 255, 255, 255 },\n"
                  << propertyIndent << "bgColor = { 0, 0, 0, 100 },\n"
                  << propertyIndent << "hoverBgColor = { 255, 255, 255, 40 },\n"
                  << propertyIndent << "onClick = function() end\n"
                  << elementIndent << "})";
            break;
        case ElementType::Label:
            block << "UI.Label({\n"
                  << propertyIndent << "text = " << EscapeLuaString("New Text") << ",\n"
                  << propertyIndent << "x = " << static_cast<int>(std::round(baseX)) << ", y = " << static_cast<int>(std::round(baseY)) << ",\n"
                  << propertyIndent << "fontSize = 24,\n"
                  << propertyIndent << "color = { 255, 255, 255, 255 }\n"
                  << elementIndent << "})";
            break;
        case ElementType::Panel:
            block << "UI.Panel({\n"
                  << propertyIndent << "x = " << static_cast<int>(std::round(baseX)) << ", y = " << static_cast<int>(std::round(baseY)) << ",\n"
                  << propertyIndent << "w = " << static_cast<int>(std::round(std::max(200.0f, baseW))) << ", h = " << static_cast<int>(std::round(std::max(120.0f, baseH))) << ",\n"
                  << propertyIndent << "color = { 0, 0, 0, 150 }\n"
                  << elementIndent << "})";
            break;
    }

    const int referenceId = reference->id;
    if (!InsertElementAfterReference(*document, *reference, block.str())) {
        Log("This script structure is not yet auto-insertable. The current add flow works on list-style UI blocks.");
        return;
    }

    ParseDocument(*document);
    Touch();

    for (size_t index = 0; index < document->elements.size(); ++index) {
        if (document->elements[index].id == referenceId && index + 1 < document->elements.size()) {
            m_activeElementId = document->elements[index + 1].id;
            return;
        }
    }
}

bool UIDesigner::ReplaceBackgroundAsset(ScriptDocument& document, const std::string& assetPath) {
    if (!document.backgroundSpan.valid) {
        return false;
    }
    ReplaceValue(document, document.backgroundSpan, EscapeLuaString(NormalizePath(assetPath)));
    return true;
}

UIDesigner::PreviewResult UIDesigner::BuildStaticPreview(const ScriptDocument& document) const {
    PreviewResult result;
    result.targetLabel = document.kind == DocumentKind::Scene
        ? ("Scene: " + document.displayName)
        : ("Component: " + document.displayName);

    PreviewCommand background;
    background.type = PreviewCommand::Type::Rect;
    background.position = ImVec2(0.0f, 0.0f);
    background.size = ImVec2(kPreviewWidth, kPreviewHeight);
    background.color = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);
    result.commands.push_back(background);

    const ScriptDocument* backgroundDocument = nullptr;
    if (document.kind == DocumentKind::Scene) {
        backgroundDocument = &document;
    } else if (!document.linkedScenes.empty()) {
        backgroundDocument = FindDocument(DocumentKind::Scene, document.linkedScenes.front());
        if (backgroundDocument) {
            result.targetLabel = "Component In Scene: " + backgroundDocument->displayName;
        }
    }

    if (backgroundDocument) {
        AppendDocumentPreviewCommands(*backgroundDocument, result, true);
    }

    if (document.kind == DocumentKind::Scene) {
        std::unordered_set<std::string> seen;
        for (const std::string& componentPath : document.linkedComponents) {
            if (!seen.insert(componentPath).second) {
                continue;
            }
            if (const ScriptDocument* component = FindDocument(DocumentKind::Component, componentPath)) {
                AppendDocumentPreviewCommands(*component, result, false);
            }
        }
    } else {
        AppendDocumentPreviewCommands(document, result, false);
    }

    if (!document.parseWarning.empty()) {
        result.error = document.parseWarning;
    }
    return result;
}

void UIDesigner::AppendDocumentPreviewCommands(const ScriptDocument& document, PreviewResult& preview, bool includeBackground) const {
    const auto resolveSize = [&](const ParsedElement& element) {
        float width = element.w;
        float height = element.h;
        if (element.type == ElementType::Label) {
            if (width <= 0.0f) {
                width = EstimateTextWidth(element.text.empty() ? element.name : element.text, static_cast<float>(element.fontSize));
            }
            if (height <= 0.0f) {
                height = std::max(20.0f, element.fontSize * 1.25f);
            }
        } else {
            if (width <= 0.0f) {
                width = std::max(160.0f, EstimateTextWidth(element.text.empty() ? element.name : element.text, static_cast<float>(element.fontSize)) + 40.0f);
            }
            if (height <= 0.0f) {
                height = std::max(44.0f, element.fontSize * 1.8f);
            }
        }
        return ImVec2(width, height);
    };

    if (includeBackground) {
        if (!document.backgroundAsset.empty()) {
            PreviewCommand bgLabel;
            bgLabel.type = PreviewCommand::Type::Text;
            bgLabel.position = ImVec2(24.0f, 24.0f);
            bgLabel.text = "BG: " + document.backgroundAsset;
            bgLabel.fontSize = 20.0f;
            bgLabel.color = ImVec4(0.78f, 0.78f, 0.80f, 1.0f);
            preview.commands.push_back(std::move(bgLabel));
        }

        PreviewCommand overlay;
        overlay.type = PreviewCommand::Type::Rect;
        overlay.position = ImVec2(0.0f, 0.0f);
        overlay.size = ImVec2(kPreviewWidth, kPreviewHeight);
        overlay.color = ImVec4(0.0f, 0.0f, 0.0f, 0.22f);
        preview.commands.push_back(std::move(overlay));
    }

    for (const ParsedElement& element : document.elements) {
        const ImVec2 size = resolveSize(element);
        preview.hitRegions.push_back(PreviewHitRegion{
            document.kind,
            element.type,
            document.id,
            element.id,
            ImVec2(element.x, element.y),
            size,
        });

        switch (element.type) {
            case ElementType::Panel: {
                PreviewCommand panel;
                panel.type = PreviewCommand::Type::Rect;
                panel.position = ImVec2(element.x, element.y);
                panel.size = size;
                panel.color = element.accentColor;
                preview.commands.push_back(std::move(panel));
                break;
            }
            case ElementType::Button: {
                PreviewCommand button;
                button.type = PreviewCommand::Type::Rect;
                button.position = ImVec2(element.x, element.y);
                button.size = size;
                button.color = element.imageAsset.empty() ? element.accentColor : ImVec4(0.15f, 0.15f, 0.15f, 0.85f);
                preview.commands.push_back(std::move(button));

                if (!element.imageAsset.empty()) {
                    PreviewCommand badge;
                    badge.type = PreviewCommand::Type::Text;
                    badge.position = ImVec2(element.x + 12.0f, element.y + 10.0f);
                    badge.text = fs::path(element.imageAsset).filename().string();
                    badge.fontSize = 16.0f;
                    badge.color = ImVec4(0.82f, 0.82f, 0.84f, 1.0f);
                    preview.commands.push_back(std::move(badge));
                }

                PreviewCommand text;
                text.type = PreviewCommand::Type::Text;
                text.text = element.text.empty() ? element.name : element.text;
                text.fontSize = static_cast<float>(element.fontSize);
                text.color = element.color;
                text.outlineColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
                text.outlineSize = 2;
                const float textWidth = EstimateTextWidth(text.text, text.fontSize);
                const float textHeight = text.fontSize * 1.25f;
                text.position = ImVec2(
                    element.x + std::max(0.0f, (size.x - textWidth) * 0.5f),
                    element.y + std::max(0.0f, (size.y - textHeight) * 0.5f));
                preview.commands.push_back(std::move(text));
                break;
            }
            case ElementType::Label: {
                PreviewCommand text;
                text.type = PreviewCommand::Type::Text;
                text.position = ImVec2(element.x, element.y);
                text.text = element.text.empty() ? element.name : element.text;
                text.fontSize = static_cast<float>(element.fontSize);
                text.color = element.color;
                text.outlineColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
                text.outlineSize = 2;
                preview.commands.push_back(std::move(text));
                break;
            }
        }
    }
}

std::string UIDesigner::NormalizePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::string UIDesigner::EscapeLuaString(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    escaped.push_back('"');
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string UIDesigner::UnescapeLuaString(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }

    std::string unescaped;
    unescaped.reserve(value.size());
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped) {
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            unescaped.push_back(ch);
            continue;
        }

        switch (ch) {
            case 'n':
                unescaped.push_back('\n');
                break;
            case 'r':
                break;
            case 't':
                unescaped.push_back('\t');
                break;
            case '\\':
            case '"':
                unescaped.push_back(ch);
                break;
            default:
                unescaped.push_back(ch);
                break;
        }
        escaped = false;
    }

    if (escaped) {
        unescaped.push_back('\\');
    }
    return unescaped;
}

std::string UIDesigner::Trim(std::string_view value) {
    return TrimView(value);
}

bool UIDesigner::IsWordBoundary(char ch) {
    return !(std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_');
}

UIDesigner::ValueSpan UIDesigner::FindQuotedValue(std::string_view source, std::string_view key, size_t absoluteOffset) {
    for (size_t pos = 0; (pos = source.find(key, pos)) != std::string_view::npos; pos += key.size()) {
        const bool leftBoundary = pos == 0 || IsWordBoundary(source[pos - 1]);
        const size_t afterKey = pos + key.size();
        const bool rightBoundary = afterKey >= source.size() || IsWordBoundary(source[afterKey]);
        if (!leftBoundary || !rightBoundary) {
            continue;
        }

        size_t cursor = afterKey;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '=') {
            continue;
        }
        ++cursor;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '"') {
            continue;
        }

        size_t end = cursor + 1;
        bool escaped = false;
        while (end < source.size()) {
            const char ch = source[end];
            if (!escaped && ch == '"') {
                return {absoluteOffset + cursor, absoluteOffset + end + 1, true};
            }
            escaped = !escaped && ch == '\\';
            ++end;
        }
    }
    return {};
}

UIDesigner::ValueSpan UIDesigner::FindExpressionValue(std::string_view source, std::string_view key, size_t absoluteOffset) {
    for (size_t pos = 0; (pos = source.find(key, pos)) != std::string_view::npos; pos += key.size()) {
        const bool leftBoundary = pos == 0 || IsWordBoundary(source[pos - 1]);
        const size_t afterKey = pos + key.size();
        const bool rightBoundary = afterKey >= source.size() || IsWordBoundary(source[afterKey]);
        if (!leftBoundary || !rightBoundary) {
            continue;
        }

        size_t cursor = afterKey;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '=') {
            continue;
        }
        ++cursor;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }

        const size_t start = cursor;
        int parenDepth = 0;
        while (cursor < source.size()) {
            const char ch = source[cursor];
            if (ch == '(') {
                ++parenDepth;
            } else if (ch == ')') {
                if (parenDepth == 0) {
                    break;
                }
                --parenDepth;
            } else if ((ch == ',' || ch == '\n' || ch == '\r' || ch == '}') && parenDepth == 0) {
                break;
            }
            ++cursor;
        }

        while (cursor > start && std::isspace(static_cast<unsigned char>(source[cursor - 1])) != 0) {
            --cursor;
        }
        if (cursor > start) {
            return {absoluteOffset + start, absoluteOffset + cursor, true};
        }
    }
    return {};
}

UIDesigner::ValueSpan UIDesigner::FindTableValue(std::string_view source, std::string_view key, size_t absoluteOffset) {
    for (size_t pos = 0; (pos = source.find(key, pos)) != std::string_view::npos; pos += key.size()) {
        const bool leftBoundary = pos == 0 || IsWordBoundary(source[pos - 1]);
        const size_t afterKey = pos + key.size();
        const bool rightBoundary = afterKey >= source.size() || IsWordBoundary(source[afterKey]);
        if (!leftBoundary || !rightBoundary) {
            continue;
        }

        size_t cursor = afterKey;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '=') {
            continue;
        }
        ++cursor;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= source.size() || source[cursor] != '{') {
            continue;
        }

        const size_t start = cursor;
        int depth = 0;
        while (cursor < source.size()) {
            if (source[cursor] == '{') {
                ++depth;
            } else if (source[cursor] == '}') {
                --depth;
                if (depth == 0) {
                    return {absoluteOffset + start, absoluteOffset + cursor + 1, true};
                }
            }
            ++cursor;
        }
    }
    return {};
}

std::vector<float> UIDesigner::ParseColor(ValueSpan span, const std::string& source) {
    std::vector<float> values;
    if (!span.valid || span.end <= span.start || span.end > source.size()) {
        return values;
    }

    std::string token = source.substr(span.start, span.end - span.start);
    token.erase(std::remove(token.begin(), token.end(), '{'), token.end());
    token.erase(std::remove(token.begin(), token.end(), '}'), token.end());

    std::stringstream stream(token);
    std::string chunk;
    while (std::getline(stream, chunk, ',')) {
        const std::string trimmed = Trim(chunk);
        if (!trimmed.empty()) {
            try {
                values.push_back(std::stof(trimmed));
            } catch (...) {
            }
        }
    }
    return values;
}

std::string UIDesigner::ReadSpan(const std::string& source, const ValueSpan& span) {
    if (!span.valid || span.end <= span.start || span.end > source.size()) {
        return {};
    }
    return source.substr(span.start, span.end - span.start);
}

float UIDesigner::EstimateTextWidth(const std::string& text, float fontSize) const {
    if (ImFont* font = ImGui::GetFont()) {
        return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str()).x;
    }
    return fontSize * static_cast<float>(text.size()) * 0.56f;
}

void UIDesigner::Log(const std::string& message) const {
    if (m_logCallback) {
        m_logCallback(message);
    }
}

}  // namespace PrismatiX::Editor
