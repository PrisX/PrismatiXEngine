#include "PdsScriptEditor.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>

#include <imgui.h>

namespace fs = std::filesystem;

namespace PrismatiX::Editor {

namespace {

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

PdsScriptEditor::PdsScriptEditor(LogCallback logCallback)
    : m_logCallback(std::move(logCallback)) {}

void PdsScriptEditor::Render() {
    EnsureDocuments();
    RenderToolbar();
    ImGui::Separator();

    if (ImGui::BeginTable("pds-editor-layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Scripts", ImGuiTableColumnFlags_WidthFixed, 280.0f);
        ImGui::TableSetupColumn("Document", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        RenderDocumentList();

        ImGui::TableSetColumnIndex(1);
        RenderEditorPane();

        ImGui::EndTable();
    }
}

void PdsScriptEditor::RenderInspector() {
    EnsureDocuments();
    const ScriptDocument* document = ActiveDocument();
    if (!document) {
        ImGui::TextDisabled("Select a .pds script under Data/Script.");
        return;
    }

    RenderDocumentSummary(*document);
    ImGui::Separator();

    ImGui::TextDisabled("Runtime Path");
    ImGui::TextWrapped("%s", document->runtimePath.c_str());
    ImGui::TextDisabled("Commands: %zu", document->commands.size());
    ImGui::TextDisabled("Dialogue: %d", document->textCount);
    ImGui::TextDisabled("Choices: %d", document->choiceCount);
    ImGui::TextDisabled("Labels: %d", document->labelCount);

    const std::string selectedResource = m_selectedResourceCallback ? m_selectedResourceCallback() : std::string{};
    if (!selectedResource.empty()) {
        ImGui::SeparatorText("Selected Asset");
        ImGui::TextWrapped("%s", selectedResource.c_str());
        ImGui::TextDisabled("Use the copied path inside commands like [bg file=...] or [bgm file=...].");
    }

    if (!document->commands.empty()) {
        ImGui::SeparatorText("Outline");
        ImGui::BeginChild("pds-outline", ImVec2(0.0f, 0.0f), false);
        const size_t visible = std::min<size_t>(document->commands.size(), 80);
        for (size_t index = 0; index < visible; ++index) {
            const CommandSummary& command = document->commands[index];
            ImGui::TextDisabled("%d", command.line);
            ImGui::SameLine();
            ImGui::TextUnformatted(command.kind.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", command.preview.c_str());
        }
        ImGui::EndChild();
    }
}

void PdsScriptEditor::NavigateToContent() {}

void PdsScriptEditor::ApplyAssetToSelection(const std::string& assetPath) {
    if (assetPath.empty()) {
        return;
    }
    ImGui::SetClipboardText(assetPath.c_str());
    Log("Copied asset path for scene script use: " + assetPath);
}

void PdsScriptEditor::SetHeaderTexture(ImTextureID texture, int width, int height) {
    (void)texture;
    (void)width;
    (void)height;
}

void PdsScriptEditor::SetSelectedResourceCallback(SelectedResourceCallback callback) {
    m_selectedResourceCallback = std::move(callback);
}

void PdsScriptEditor::SetProjectRoot(const fs::path& projectRoot) {
    m_projectRoot = projectRoot;
    m_forceRefresh = true;
}

void PdsScriptEditor::ResetToDefaults() {
    m_documents.clear();
    m_activeDocumentId = 0;
    m_nextDocumentId = 1;
    m_forceRefresh = true;
}

bool PdsScriptEditor::HasSelection() const {
    return ActiveDocument() != nullptr;
}

std::string PdsScriptEditor::GetSelectionSummary() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        return "PDS: " + document->displayName;
    }
    return "No PDS selected";
}

std::string PdsScriptEditor::GenerateLua() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        return document->source;
    }
    return {};
}

std::vector<std::string> PdsScriptEditor::BuildPreviewLines() const {
    std::vector<std::string> lines;
    if (const ScriptDocument* document = ActiveDocument()) {
        lines.push_back("PDS: " + document->displayName);
        lines.push_back("Runtime Path: " + document->runtimePath);
        lines.push_back("Commands: " + std::to_string(document->commands.size()));
        lines.push_back("Choices: " + std::to_string(document->choiceCount));
    }
    return lines;
}

std::vector<PdsScriptEditor::ExportDocument> PdsScriptEditor::BuildExportDocuments() const {
    std::vector<ExportDocument> exports;
    const fs::path dataRoot = DataRoot();
    if (dataRoot.empty()) {
        return exports;
    }

    exports.reserve(m_documents.size());
    for (const ScriptDocument& document : m_documents) {
        ExportDocument exportDocument;
        exportDocument.label = "PDS: " + document.displayName;
        exportDocument.relativePath = fs::relative(document.path, dataRoot);
        exportDocument.content = document.source;
        exports.push_back(std::move(exportDocument));
    }
    return exports;
}

std::string PdsScriptEditor::CurrentDocumentRuntimePath() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        return document->runtimePath;
    }
    return {};
}

fs::path PdsScriptEditor::CurrentDocumentPath() const {
    if (const ScriptDocument* document = ActiveDocument()) {
        return document->path;
    }
    return {};
}

void PdsScriptEditor::EnsureDocuments() {
    if (m_forceRefresh) {
        RefreshDocuments(true);
    }
}

void PdsScriptEditor::RefreshDocuments(bool force) {
    if (!force && !m_forceRefresh) {
        return;
    }

    const int previousActiveId = m_activeDocumentId;
    const std::string previousPath = CurrentDocumentRuntimePath();

    m_documents.clear();
    m_activeDocumentId = 0;
    m_nextDocumentId = 1;

    const fs::path dataRoot = DataRoot();
    for (const fs::path& path : EnumerateDocuments()) {
        ScriptDocument document;
        document.id = m_nextDocumentId++;
        document.path = path;
        document.runtimePath = fs::relative(path, dataRoot).generic_string();
        document.displayName = path.filename().string();

        std::ifstream in(path, std::ios::binary);
        if (in.is_open()) {
            document.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        ParseDocument(document);
        m_documents.push_back(std::move(document));
    }

    if (!previousPath.empty()) {
        auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](const ScriptDocument& document) {
            return document.runtimePath == previousPath;
        });
        if (it != m_documents.end()) {
            m_activeDocumentId = it->id;
        }
    }

    if (m_activeDocumentId == 0 && previousActiveId != 0) {
        auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](const ScriptDocument& document) {
            return document.id == previousActiveId;
        });
        if (it != m_documents.end()) {
            m_activeDocumentId = it->id;
        }
    }

    if (m_activeDocumentId == 0 && !m_documents.empty()) {
        m_activeDocumentId = m_documents.front().id;
    }

    m_forceRefresh = false;
}

void PdsScriptEditor::RenderToolbar() {
    ImGui::BeginChild("pds-toolbar", ImVec2(0.0f, 72.0f), false);
    if (ImGui::Button("Refresh")) {
        RefreshDocuments(true);
    }

    ScriptDocument* document = ActiveDocument();
    ImGui::SameLine();
    if (!document || !document->dirty) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save")) {
        SaveDocument(*document);
    }
    if (!document || !document->dirty) {
        ImGui::EndDisabled();
    }

    const bool hasDirtyDocuments = std::any_of(m_documents.begin(), m_documents.end(), [](const ScriptDocument& item) {
        return item.dirty;
    });
    ImGui::SameLine();
    if (!hasDirtyDocuments) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save All")) {
        SaveAllDirtyDocuments();
    }
    if (!hasDirtyDocuments) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%zu scripts", m_documents.size());
    ImGui::EndChild();
}

void PdsScriptEditor::RenderDocumentList() {
    ImGui::BeginChild("pds-script-list", ImVec2(0.0f, 0.0f), false);
    if (m_documents.empty()) {
        ImGui::TextDisabled("No .pds files were found under Data/Script.");
        ImGui::TextDisabled("This tab edits chapter1.pds / chapter2.pds style script files.");
        ImGui::EndChild();
        return;
    }

    for (const ScriptDocument& document : m_documents) {
        std::string label = document.displayName;
        if (document.dirty) {
            label += " *";
        }
        if (ImGui::Selectable(label.c_str(), document.id == m_activeDocumentId)) {
            m_activeDocumentId = document.id;
        }
    }
    ImGui::EndChild();
}

void PdsScriptEditor::RenderEditorPane() {
    ScriptDocument* document = ActiveDocument();
    if (!document) {
        ImGui::TextDisabled("Select a .pds script under Data/Script.");
        return;
    }

    ImGui::BeginChild("pds-source-editor", ImVec2(0.0f, 0.0f), false);
    RenderDocumentSummary(*document);
    ImGui::Separator();

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
    if (ImGui::InputTextMultiline("##pds-source-buffer", &document->source, ImVec2(-1.0f, -1.0f), flags)) {
        document->dirty = true;
        ParseDocument(*document);
    }
    ImGui::EndChild();
}

void PdsScriptEditor::RenderDocumentSummary(const ScriptDocument& document) const {
    ImGui::TextUnformatted(document.displayName.c_str());
    ImGui::TextDisabled("%s", document.runtimePath.c_str());
    ImGui::TextDisabled("%zu commands, %d labels, %d choices", document.commands.size(), document.labelCount, document.choiceCount);
}

void PdsScriptEditor::ParseDocument(ScriptDocument& document) {
    document.commands.clear();
    document.labelCount = 0;
    document.choiceCount = 0;
    document.textCount = 0;

    std::istringstream stream(document.source);
    std::string line;
    int lineNumber = 1;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            ++lineNumber;
            continue;
        }

        if (!trimmed.empty() && trimmed.front() == '*') {
            ++document.labelCount;
            document.commands.push_back({lineNumber, "label", Clip(trimmed, 96)});
            ++lineNumber;
            continue;
        }

        if (trimmed.size() >= 2 && trimmed.front() == '[') {
            const size_t closingBracket = trimmed.find(']');
            const std::string commandBody = closingBracket == std::string::npos ? trimmed.substr(1) : trimmed.substr(1, closingBracket - 1);
            const size_t separator = commandBody.find_first_of(" \t");
            std::string kind = separator == std::string::npos ? commandBody : commandBody.substr(0, separator);
            if (kind.empty()) {
                kind = "command";
            }
            if (kind == "choice") {
                ++document.choiceCount;
            } else if (kind == "text") {
                ++document.textCount;
            }
            document.commands.push_back({lineNumber, kind, Clip(trimmed, 96)});
            ++lineNumber;
            continue;
        }

        document.commands.push_back({lineNumber, "content", Clip(trimmed, 96)});
        ++lineNumber;
    }
}

void PdsScriptEditor::SaveDocument(ScriptDocument& document) {
    std::ofstream out(document.path, std::ios::binary);
    if (!out.is_open()) {
        Log("Failed to save " + document.path.string());
        return;
    }
    out << document.source;
    document.dirty = false;
    Log("Saved " + document.runtimePath);
}

void PdsScriptEditor::SaveAllDirtyDocuments() {
    for (ScriptDocument& document : m_documents) {
        if (document.dirty) {
            SaveDocument(document);
        }
    }
}

PdsScriptEditor::ScriptDocument* PdsScriptEditor::ActiveDocument() {
    auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](const ScriptDocument& document) {
        return document.id == m_activeDocumentId;
    });
    return it == m_documents.end() ? nullptr : &(*it);
}

const PdsScriptEditor::ScriptDocument* PdsScriptEditor::ActiveDocument() const {
    auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](const ScriptDocument& document) {
        return document.id == m_activeDocumentId;
    });
    return it == m_documents.end() ? nullptr : &(*it);
}

fs::path PdsScriptEditor::DataRoot() const {
    if (m_projectRoot.empty()) {
        return {};
    }
    return m_projectRoot / "Data";
}

fs::path PdsScriptEditor::ScriptRoot() const {
    const fs::path dataRoot = DataRoot();
    if (dataRoot.empty()) {
        return {};
    }
    return dataRoot / "Script";
}

std::vector<fs::path> PdsScriptEditor::EnumerateDocuments() const {
    std::vector<fs::path> documents;
    const fs::path scriptRoot = ScriptRoot();
    if (scriptRoot.empty() || !fs::exists(scriptRoot)) {
        return documents;
    }

    for (const auto& entry : fs::recursive_directory_iterator(scriptRoot)) {
        if (entry.is_regular_file() && ToLowerCopy(entry.path().extension().string()) == ".pds") {
            documents.push_back(entry.path());
        }
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

std::string PdsScriptEditor::Trim(std::string_view value) {
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

std::string PdsScriptEditor::Clip(std::string_view value, size_t maxLength) {
    if (value.size() <= maxLength) {
        return std::string(value);
    }
    std::string clipped(value.substr(0, maxLength));
    clipped += "...";
    return clipped;
}

void PdsScriptEditor::Log(const std::string& message) const {
    if (m_logCallback) {
        m_logCallback(message);
    }
}

}  // namespace PrismatiX::Editor
