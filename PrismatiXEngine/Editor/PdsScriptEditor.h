#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

namespace PrismatiX::Editor {

class PdsScriptEditor {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using SelectedResourceCallback = std::function<std::string()>;

    struct ExportDocument {
        std::string label;
        std::filesystem::path relativePath;
        std::string content;
    };

    explicit PdsScriptEditor(LogCallback logCallback = {});

    void Render();
    void RenderInspector();
    void NavigateToContent();
    void ApplyAssetToSelection(const std::string& assetPath);
    void SetHeaderTexture(ImTextureID texture, int width, int height);
    void SetSelectedResourceCallback(SelectedResourceCallback callback);
    void SetProjectRoot(const std::filesystem::path& projectRoot);
    void ResetToDefaults();

    [[nodiscard]] bool HasSelection() const;
    [[nodiscard]] std::string GetSelectionSummary() const;
    [[nodiscard]] std::string GenerateLua() const;
    [[nodiscard]] std::vector<std::string> BuildPreviewLines() const;
    [[nodiscard]] std::vector<ExportDocument> BuildExportDocuments() const;
    [[nodiscard]] std::string CurrentDocumentRuntimePath() const;
    [[nodiscard]] std::filesystem::path CurrentDocumentPath() const;

private:
    struct CommandSummary {
        int line = 0;
        std::string kind;
        std::string preview;
    };

    struct ScriptDocument {
        int id = 0;
        std::filesystem::path path;
        std::string runtimePath;
        std::string displayName;
        std::string source;
        bool dirty = false;
        int labelCount = 0;
        int choiceCount = 0;
        int textCount = 0;
        std::vector<CommandSummary> commands;
    };

    void EnsureDocuments();
    void RefreshDocuments(bool force);
    void RenderToolbar();
    void RenderDocumentList();
    void RenderEditorPane();
    void RenderDocumentSummary(const ScriptDocument& document) const;
    void ParseDocument(ScriptDocument& document);
    void SaveDocument(ScriptDocument& document);
    void SaveAllDirtyDocuments();
    [[nodiscard]] ScriptDocument* ActiveDocument();
    [[nodiscard]] const ScriptDocument* ActiveDocument() const;
    [[nodiscard]] std::filesystem::path DataRoot() const;
    [[nodiscard]] std::filesystem::path ScriptRoot() const;
    [[nodiscard]] std::vector<std::filesystem::path> EnumerateDocuments() const;
    [[nodiscard]] static std::string Trim(std::string_view value);
    [[nodiscard]] static std::string Clip(std::string_view value, size_t maxLength);
    void Log(const std::string& message) const;

    LogCallback m_logCallback;
    SelectedResourceCallback m_selectedResourceCallback;
    std::filesystem::path m_projectRoot;
    std::vector<ScriptDocument> m_documents;
    int m_activeDocumentId = 0;
    int m_nextDocumentId = 1;
    bool m_forceRefresh = true;
};

}  // namespace PrismatiX::Editor
