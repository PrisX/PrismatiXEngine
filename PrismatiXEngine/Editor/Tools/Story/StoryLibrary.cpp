#include "Editor/Tools/Story/StoryLibrary.h"

#include "Engine/IO/AtomicFile.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace px::editor {
namespace {

std::string Text(const resource::NodeRecord& node, const char* key) {
    const auto found = node.properties.find(key);
    if (found == node.properties.end()) return {};
    const auto* value = found->second.TryGet<std::string>();
    return value ? *value : std::string{};
}

void SetText(resource::NodeRecord& node, const char* key, std::string value) {
    node.properties[key] = std::move(value);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string StableStem(std::string value) {
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte)) character = '_';
        else character = static_cast<char>(std::tolower(byte));
    }
    while (!value.empty() && value.front() == '_') value.erase(value.begin());
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "expression" : value;
}

}  // namespace

Status StoryLibrary::Open(const ProjectContext* project, const AssetDatabase* assets,
                          EditorTextures* textures, ResourceResolver resolver) {
    Close();
    m_project = project;
    m_assets = assets;
    m_textures = textures;
    m_resolver = std::move(resolver);
    if (!m_project || !m_project->IsOpen()) return Status::Ok();
    const auto path = m_project->root / "Content/Game.pxres";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
            .code="PXEDSTORY5201",.category="Editor.StoryLibrary",
            .message="找不到 Content/Game.pxres",.details=path.generic_string()});
    }
    std::ostringstream text; text << input.rdbuf();
    auto parsed = resource::ParseTypedDocument(text.str(), path.generic_string());
    if (!parsed) return Status::Fail(parsed.Diagnostics());
    if (parsed.Value().type != "GameCatalog") {
        return Status::Fail(diag::Diagnostic{.severity=diag::Severity::Error,
            .code="PXEDSTORY5202",.category="Editor.StoryLibrary",
            .message="Content/Game.pxres 不是 GameCatalog"});
    }
    m_document = parsed.TakeValue();
    m_loaded = true;
    m_dirty = false;
    RebuildCatalog();
    for (const auto& node : m_document.nodes) if (node.type == "Character") {
        m_selectedCharacter = node.id; break;
    }
    return Status::Ok();
}

void StoryLibrary::Close() {
    m_project = nullptr; m_assets = nullptr; m_textures = nullptr; m_resolver = {};
    m_document = {}; m_catalog = {}; m_selectedCharacter = {}; m_selectedExpression = {};
    m_loaded = false; m_dirty = false; m_search.clear(); m_assetSearch.clear();
}

bool StoryLibrary::Save() {
    if (!m_loaded || !m_project) return false;
    for (const auto& node : m_document.nodes) {
        if (node.type != "Character") continue;
        const std::string error = CharacterIdError(node);
        if (!error.empty()) {
            diag::Diagnostic diagnostic{.severity=diag::Severity::Error,
                .code="PXEDSTORY5203",.category="Editor.StoryLibrary",.message=error};
            diagnostic.source.path="Content/Game.pxres";
            diagnostic.source.nodeId=node.id.ToString();
            diagnostic.source.property="id";
            diag::Emit(std::move(diagnostic));
            return false;
        }
    }
    const Status valid = m_catalog.Load(resource::WriteTypedDocument(m_document),
                                        "Content/Game.pxres");
    if (!valid) return false;
    const Status written = io::AtomicFile::WriteText(
        m_project->root / "Content/Game.pxres", resource::WriteTypedDocument(m_document));
    if (!written) {
        for (const auto& diagnostic : written.Diagnostics()) diag::Emit(diagnostic);
        return false;
    }
    m_dirty = false;
    return true;
}

resource::NodeRecord* StoryLibrary::SelectedCharacter() {
    for (auto& node : m_document.nodes)
        if (node.id == m_selectedCharacter && node.type == "Character") return &node;
    return nullptr;
}

const resource::NodeRecord* StoryLibrary::SelectedCharacter() const {
    for (const auto& node : m_document.nodes)
        if (node.id == m_selectedCharacter && node.type == "Character") return &node;
    return nullptr;
}

resource::NodeRecord* StoryLibrary::SelectedExpression() {
    for (auto& node : m_document.nodes)
        if (node.id == m_selectedExpression && node.type == "CharacterExpression") return &node;
    return nullptr;
}

std::vector<resource::NodeRecord*> StoryLibrary::Expressions(const Uuid& character) {
    std::vector<resource::NodeRecord*> result;
    for (auto& node : m_document.nodes)
        if (node.parent == character && node.type == "CharacterExpression") result.push_back(&node);
    return result;
}

void StoryLibrary::MarkChanged() {
    m_dirty = true;
    ++m_revision;
    RebuildCatalog();
}

void StoryLibrary::RebuildCatalog() {
    vn::GameCatalog updated;
    if (updated.Load(resource::WriteTypedDocument(m_document), "Content/Game.pxres"))
        m_catalog = std::move(updated);
}

std::string StoryLibrary::UniqueCharacterId() const {
    int index = 1;
    for (;;) {
        const std::string candidate = "character_" + std::to_string(index++);
        bool used = false;
        for (const auto& node : m_document.nodes)
            if (node.type == "Character" && Text(node, "id") == candidate) { used = true; break; }
        if (!used) return candidate;
    }
}

std::string StoryLibrary::UniqueExpressionId(const Uuid& parent, std::string base) const {
    base = StableStem(std::move(base));
    std::string candidate = base;
    int suffix = 2;
    for (;;) {
        bool used = false;
        for (const auto& node : m_document.nodes)
            if (node.parent == parent && node.type == "CharacterExpression" &&
                Text(node, "id") == candidate) { used = true; break; }
        if (!used) return candidate;
        candidate = base + "_" + std::to_string(suffix++);
    }
}

std::string StoryLibrary::CharacterIdError(const resource::NodeRecord& character) const {
    const std::string id = Text(character, "id");
    if (id.empty()) return "角色 ID 不可為空。";
    for (const auto& candidate : m_document.nodes) {
        if (candidate.type == "Character" && candidate.id != character.id &&
            Text(candidate, "id") == id)
            return "角色 ID 已被其他角色使用：" + id;
    }
    return {};
}

void StoryLibrary::AddCharacter() {
    resource::NodeRecord node;
    node.id = Uuid::Random(); node.name = "Character"; node.type = "Character";
    SetText(node, "id", UniqueCharacterId()); SetText(node, "name", "新角色");
    SetText(node, "voiceDirectory", ""); SetText(node, "defaultExpression", "");
    m_document.nodes.push_back(std::move(node));
    m_selectedCharacter = m_document.nodes.back().id; m_selectedExpression = {};
    MarkChanged();
}

void StoryLibrary::AddExpression(const std::string& runtimePath) {
    auto* character = SelectedCharacter();
    if (!character || !m_resolver) return;
    const Uuid characterId = character->id;
    const bool needsDefault = Text(*character, "defaultExpression").empty();
    const auto reference = m_resolver(runtimePath);
    if (!reference) return;
    const std::string stem = std::filesystem::path(runtimePath).stem().string();
    resource::NodeRecord node;
    node.id = Uuid::Random(); node.parent = characterId;
    node.name = stem; node.type = "CharacterExpression";
    const std::string expressionId = UniqueExpressionId(characterId, stem);
    SetText(node, "id", expressionId); SetText(node, "name", stem);
    node.properties["image"] = *reference;
    if (m_assetPickerReplace && !m_selectedExpression.Empty()) {
        if (auto* expression = SelectedExpression()) expression->properties["image"] = *reference;
    } else {
        m_document.nodes.push_back(std::move(node));
        m_selectedExpression = m_document.nodes.back().id;
        if (needsDefault) {
            for (auto& candidate : m_document.nodes)
                if (candidate.id == characterId) {
                    SetText(candidate, "defaultExpression", expressionId);
                    break;
                }
        }
    }
    MarkChanged();
}

void StoryLibrary::RemoveSelectedExpression() {
    auto* character = SelectedCharacter(); auto* expression = SelectedExpression();
    if (!character || !expression) return;
    const Uuid characterId = character->id;
    const std::string removed = Text(*expression, "id");
    m_document.nodes.erase(std::remove_if(m_document.nodes.begin(), m_document.nodes.end(),
        [&](const auto& node) { return node.id == m_selectedExpression; }), m_document.nodes.end());
    m_selectedExpression = {};
    auto expressions = Expressions(characterId);
    for (auto& candidate : m_document.nodes)
        if (candidate.id == characterId && Text(candidate, "defaultExpression") == removed) {
            SetText(candidate, "defaultExpression",
                    expressions.empty() ? std::string{} : Text(*expressions.front(), "id"));
            break;
        }
    MarkChanged();
}

void StoryLibrary::RemoveSelectedCharacter() {
    if (m_selectedCharacter.Empty()) return;
    const Uuid removed = m_selectedCharacter;
    m_document.nodes.erase(std::remove_if(m_document.nodes.begin(), m_document.nodes.end(),
        [&](const auto& node) { return node.id == removed || node.parent == removed; }),
        m_document.nodes.end());
    m_selectedCharacter = {}; m_selectedExpression = {};
    for (const auto& node : m_document.nodes) if (node.type == "Character") {
        m_selectedCharacter = node.id; break;
    }
    MarkChanged();
}

void StoryLibrary::RenderCharacterList() {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##character-search", "搜尋角色…", &m_search);
    if (ImGui::Button("＋ 角色")) AddCharacter();
    ImGui::SameLine(); ImGui::BeginDisabled(!SelectedCharacter());
    if (ImGui::Button("刪除")) ImGui::OpenPopup("刪除角色？");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("刪除角色？", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("將刪除角色與其所有表情。已存在的 Scenario 引用會在 Problems 顯示。");
        if (ImGui::Button("刪除", {100,0})) { RemoveSelectedCharacter(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("取消", {100,0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    const std::string needle = Lower(m_search);
    for (const auto& node : m_document.nodes) {
        if (node.type != "Character") continue;
        const std::string id = Text(node, "id"), name = Text(node, "name");
        if (!needle.empty() && Lower(id + " " + name).find(needle) == std::string::npos) continue;
        const std::string label = (name.empty() ? id : name) + "##" + node.id.ToString();
        if (ImGui::Selectable(label.c_str(), node.id == m_selectedCharacter)) {
            m_selectedCharacter = node.id; m_selectedExpression = {};
        }
        ImGui::SameLine(); ImGui::TextDisabled("%s", id.c_str());
    }
}

void StoryLibrary::OpenAssetPicker(const bool replace) {
    m_assetPickerReplace = replace; m_assetPickerRequested = true; m_assetSearch.clear();
}

void StoryLibrary::RenderCharacterEditor() {
    auto* character = SelectedCharacter();
    if (!character) { ImGui::TextDisabled("新增或選取一個角色。"); return; }
    std::string id = Text(*character, "id");
    if (ImGui::InputTextWithHint("角色 ID", "例如 alice、heroine_01", &id)) {
        SetText(*character, "id", id);
        MarkChanged();
    }
    const std::string idError = CharacterIdError(*character);
    if (!idError.empty()) ImGui::TextColored({1.0f,.35f,.38f,1.0f}, "%s", idError.c_str());
    else {
        ImGui::TextDisabled("Scenario 會以此 ID 引用角色。");
        ImGui::SameLine();
        if (ImGui::SmallButton("複製 ID")) ImGui::SetClipboardText(id.c_str());
    }
    std::string name = Text(*character, "name");
    if (ImGui::InputText("顯示名稱", &name)) { SetText(*character, "name", name); MarkChanged(); }
    std::string voice = Text(*character, "voiceDirectory");
    if (ImGui::InputTextWithHint("語音目錄", "Content/Audio/Voice/...", &voice)) {
        SetText(*character, "voiceDirectory", voice); MarkChanged();
    }

    ImGui::SeparatorText("表情立繪");
    auto expressions = Expressions(character->id);
    std::string defaultExpression = Text(*character, "defaultExpression");
    if (ImGui::BeginCombo("預設表情", defaultExpression.empty() ? "尚未設定" : defaultExpression.c_str())) {
        for (auto* expression : expressions) {
            const std::string expressionId = Text(*expression, "id");
            const std::string expressionName = Text(*expression, "name");
            if (ImGui::Selectable((expressionName + "##default-" + expressionId).c_str(),
                                  expressionId == defaultExpression)) {
                SetText(*character, "defaultExpression", expressionId); MarkChanged();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("＋ 從素材加入表情")) OpenAssetPicker(false);
    ImGui::SameLine(); ImGui::BeginDisabled(!SelectedExpression());
    if (ImGui::Button("更換圖片")) OpenAssetPicker(true);
    ImGui::SameLine(); if (ImGui::Button("刪除表情")) RemoveSelectedExpression();
    ImGui::EndDisabled();

    const float tile = 112.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    int column = 0;
    for (auto* expression : expressions) {
        if (column && (column + 1) * (tile + 12.0f) <= available) ImGui::SameLine();
        else column = 0;
        ImGui::BeginGroup(); ImGui::PushID(expression->id.ToString().c_str());
        ImTextureID texture{}; int width = 0, height = 0;
        const auto image = expression->properties.find("image");
        const auto* reference = image == expression->properties.end() ? nullptr :
            image->second.TryGet<ResourceRefValue>();
        if (reference && m_project && m_textures)
            texture = m_textures->LoadId((m_project->root / reference->lastKnownPath).string(),
                                         &width, &height);
        const bool selected = expression->id == m_selectedExpression;
        if (texture) {
            const float scale = std::min(tile / std::max(1, width), tile / std::max(1, height));
            const ImVec2 size{std::max(1.0f, width * scale), std::max(1.0f, height * scale)};
            if (ImGui::ImageButton("##expression", texture, size)) m_selectedExpression = expression->id;
        } else if (ImGui::Button("無預覽##expression", {tile, tile})) m_selectedExpression = expression->id;
        if (selected) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                          IM_COL32(75,160,255,255), 3.0f, 0, 2.0f);
        std::string expressionName = Text(*expression, "name");
        ImGui::SetNextItemWidth(tile);
        if (ImGui::InputText("##expression-name", &expressionName)) {
            SetText(*expression, "name", expressionName); MarkChanged();
        }
        ImGui::TextDisabled("%s", Text(*expression, "id").c_str());
        ImGui::PopID(); ImGui::EndGroup();
        ++column;
    }
}

void StoryLibrary::RenderAssetPicker() {
    if (m_assetPickerRequested) { ImGui::OpenPopup("選擇立繪圖片"); m_assetPickerRequested = false; }
    ImGui::SetNextWindowSize({720,520}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("選擇立繪圖片", nullptr)) return;
    ImGui::InputTextWithHint("##asset-search", "搜尋圖片名稱或路徑…", &m_assetSearch);
    ImGui::Separator();
    if (ImGui::BeginChild("##asset-results", {0,-36}, ImGuiChildFlags_Borders)) {
        const std::string needle = Lower(m_assetSearch);
        const float tile = 96.0f, cell = 116.0f;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
        int shown = 0;
        if (m_assets) for (const auto& asset : m_assets->Assets()) {
            if (asset.type != "image") continue;
            if (!needle.empty() && Lower(asset.runtimePath).find(needle) == std::string::npos) continue;
            if (shown % columns) ImGui::SameLine();
            ImGui::BeginGroup(); ImGui::PushID(asset.runtimePath.c_str());
            ImTextureID texture{}; int width=0,height=0;
            if (m_textures) texture=m_textures->LoadId(asset.absolutePath.string(),&width,&height);
            bool chosen=false;
            if(texture){const float scale=std::min(tile/std::max(1,width),tile/std::max(1,height));chosen=ImGui::ImageButton("##asset",texture,{std::max(1.0f,width*scale),std::max(1.0f,height*scale)});}
            else chosen=ImGui::Button("圖片##asset",{tile,tile});
            ImGui::TextWrapped("%s", asset.absolutePath.filename().string().c_str());
            if(chosen){AddExpression(asset.runtimePath);ImGui::CloseCurrentPopup();}
            ImGui::PopID(); ImGui::EndGroup(); ++shown;
        }
        if (!shown) ImGui::TextDisabled("沒有符合的圖片。");
    }
    ImGui::EndChild();
    if (ImGui::Button("取消", {100,0})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void StoryLibrary::Render() {
    if (!m_loaded) { ImGui::TextDisabled("尚未載入 GameCatalog。"); return; }
    if (ImGui::Button(m_dirty ? "儲存 Game.pxres ●" : "儲存 Game.pxres")) Save();
    ImGui::SameLine(); ImGui::TextDisabled("角色 %zu", m_catalog.Characters().size());
    ImGui::Separator();
    if (ImGui::BeginTable("##story-library", 2,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Characters", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("Editor");
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("##character-list")) RenderCharacterList(); ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("##character-editor")) RenderCharacterEditor(); ImGui::EndChild();
        ImGui::EndTable();
    }
    RenderAssetPicker();
}

}  // namespace px::editor
