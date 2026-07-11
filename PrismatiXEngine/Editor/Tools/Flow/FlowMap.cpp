#define IMGUI_DEFINE_MATH_OPERATORS

#include "Editor/Tools/Flow/FlowMap.h"
#include "Engine/VN/Scenario/StoryMap.h"

#include <imgui_internal.h>
#include <widgets.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace ed = ax::NodeEditor;
using ax::Drawing::IconType;

namespace px::editor {

namespace {
constexpr float kNodeRounding = 12.0f;
constexpr float kNodeContentWidth = 360.0f;
constexpr float kPinSize = 18.0f;
constexpr float kHeaderTextureScale = 6.0f;

std::optional<px::vn::scenario::ScenarioDocument> ReadScenario(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream text; text << stream.rdbuf();
    auto parsed = px::vn::scenario::ParseScenario(text.str(), path.generic_string());
    return parsed ? std::optional(parsed.TakeValue()) : std::nullopt;
}
}

FlowMap::FlowMap() {
    ed::Config config;
    config.SettingsFile = nullptr;
    m_ctx = ed::CreateEditor(&config);
}

FlowMap::~FlowMap() {
    if (m_ctx) ed::DestroyEditor(m_ctx);
}

void FlowMap::SetHeaderTexture(ImTextureID texture, int width, int height) {
    m_headerTexture = texture;
    m_headerTextureWidth = width;
    m_headerTextureHeight = height;
}

void FlowMap::SetNodePositionByScript(const std::string& script, ImVec2 position) {
    for (FNode& node : m_nodes) {
        if (node.script == script) {
            node.pos = position;
            node.posSet = false;
            return;
        }
    }
}

void FlowMap::Rebuild(const std::vector<std::string>& scripts, const std::filesystem::path& projectRoot) {
    m_rebuilding = true;
    m_nodes.clear();
    m_links.clear();
    m_nextId = 1;
    m_nextLinkId = 100000;
    m_entryLinkId = 0;

    FNode entry;
    entry.id = m_nextId++;
    entry.pinOut = m_nextId++;
    entry.title = "START";
    entry.pos = ImVec2(0.0f, 80.0f);
    entry.isEntry = true;
    m_entryNodeId = entry.id;
    m_nodes.push_back(std::move(entry));

    for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
        FNode n;
        n.id = m_nextId++;
        n.pinIn = m_nextId++;
        n.pinOut = m_nextId++;
        n.chapterId = scripts[static_cast<std::size_t>(i)];
        n.title = std::filesystem::path(scripts[static_cast<std::size_t>(i)]).stem().string();
        n.script = scripts[static_cast<std::size_t>(i)];
        n.scriptMissing = !n.script.empty() && !std::filesystem::exists(projectRoot / n.script);
        if (const auto scenario = ReadScenario(projectRoot / n.script)) n.title = scenario->name;
        n.pos = ImVec2(260.0f + (i % 4) * 460.0f, 60.0f + (i / 4) * 220.0f);
        m_nodes.push_back(std::move(n));
    }

    // Entry link: Start -> chapter whose script is the project's startScript.
    if (!m_entryScript.empty()) {
        for (const FNode& n : m_nodes) {
            if (!n.isEntry && n.script == m_entryScript) {
                m_entryLinkId = m_nextLinkId;
                m_links.push_back(FLink{ m_nextLinkId++, FindNode(m_entryNodeId)->pinOut, n.pinIn });
                break;
            }
        }
    }

    std::unordered_map<Uuid, const FNode*, UuidHash> nodesByResource;
    for (const auto& node : m_nodes) if (!node.script.empty())
        if (const auto scenario = ReadScenario(projectRoot / node.script)) nodesByResource[scenario->id] = &node;
    for (const auto& from : m_nodes) {
        if (from.script.empty()) continue;
        const auto scenario = ReadScenario(projectRoot / from.script); if (!scenario) continue;
        for (const auto& link : px::vn::scenario::DeriveStoryLinks(*scenario)) {
            const auto target = nodesByResource.find(link.target.scenario);
            if (target != nodesByResource.end()) AddLink(from.pinOut, target->second->pinIn);
        }
    }
    m_rebuilding = false;
}

FlowMap::FNode* FlowMap::FindNode(int id) {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const FNode& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

const FlowMap::FNode* FlowMap::FindNode(int id) const {
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const FNode& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
}

const FlowMap::FNode* FlowMap::FindNodeForPin(int pinId) const {
    if (pinId <= 0) {
        return nullptr;  // The entry node has no input pin (0).
    }
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&](const FNode& node) {
        return node.pinIn == pinId || node.pinOut == pinId;
    });
    return it == m_nodes.end() ? nullptr : &(*it);
}

const FlowMap::FLink* FlowMap::FindLink(int id) const {
    auto it = std::find_if(m_links.begin(), m_links.end(), [&](const FLink& link) { return link.id == id; });
    return it == m_links.end() ? nullptr : &(*it);
}

bool FlowMap::IsInputPin(int pinId) const {
    const FNode* node = FindNodeForPin(pinId);
    return node && node->pinIn == pinId;
}

bool FlowMap::IsOutputPin(int pinId) const {
    const FNode* node = FindNodeForPin(pinId);
    return node && node->pinOut == pinId;
}

bool FlowMap::IsPinLinked(int pinId) const {
    return std::any_of(m_links.begin(), m_links.end(), [&](const FLink& link) {
        return link.fromPin == pinId || link.toPin == pinId;
    });
}

bool FlowMap::CanLink(int fromPin, int toPin) const {
    const FNode* from = FindNodeForPin(fromPin);
    const FNode* to = FindNodeForPin(toPin);
    return from && to && from != to && IsOutputPin(fromPin) && IsInputPin(toPin);
}

void FlowMap::AddLink(int fromPin, int toPin) {
    if (!CanLink(fromPin, toPin)) {
        return;
    }
    const FNode* from = FindNodeForPin(fromPin);
    if (from && from->isEntry) {
        // Re-pointing the Start node moves the entry link and updates the project entry script.
        m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const FLink& link) {
            return link.id == m_entryLinkId;
        }), m_links.end());
        const FNode* to = FindNodeForPin(toPin);
        m_entryLinkId = m_nextLinkId;
        m_links.push_back(FLink{ m_nextLinkId++, fromPin, toPin });
        if (to && !to->script.empty()) {
            m_entryScript = to->script;
            if (m_entryChanged) m_entryChanged(to->script);
        }
        return;
    }
    const bool exists = std::any_of(m_links.begin(), m_links.end(), [&](const FLink& link) {
        return link.fromPin == fromPin && link.toPin == toPin;
    });
    if (exists) {
        return;
    }
    m_links.push_back(FLink{ m_nextLinkId++, fromPin, toPin });

    if (!m_rebuilding && m_linkAdded) {
        const FNode* to = FindNodeForPin(toPin);
        if (from && to && !from->script.empty() && !to->script.empty()) {
            m_linkAdded(from->script, to->script);
        }
    }
}

void FlowMap::RemoveLink(int id) {
    if (id == m_entryLinkId) {
        return;  // The entry link is re-pointed by dragging, never deleted.
    }
    if (!m_rebuilding && m_linkRemoved) {
        if (const FLink* link = FindLink(id)) {
            const FNode* from = FindNodeForPin(link->fromPin);
            const FNode* to = FindNodeForPin(link->toPin);
            if (from && to && !from->isEntry && !from->script.empty() && !to->script.empty()) {
                m_linkRemoved(from->script, to->script);
            }
        }
    }
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const FLink& link) {
        return link.id == id;
    }), m_links.end());
}

void FlowMap::RemoveNode(int id) {
    if (id == m_entryNodeId) {
        return;
    }
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const FLink& link) {
        const FNode* from = FindNodeForPin(link.fromPin);
        const FNode* to = FindNodeForPin(link.toPin);
        return (from && from->id == id) || (to && to->id == id);
    }), m_links.end());
    if (!m_rebuilding && m_chapterRemoved) {
        if (const FNode* node = FindNode(id); node && !node->chapterId.empty()) {
            m_chapterRemoved(node->chapterId);
        }
    }
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const FNode& node) {
        return node.id == id;
    }), m_nodes.end());
}

void FlowMap::DeleteSelection() {
    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) {
        return;
    }

    std::vector<ed::LinkId> links(static_cast<size_t>(count));
    const int linkCount = ed::GetSelectedLinks(links.data(), count);
    for (int i = 0; i < linkCount; ++i) {
        RemoveLink(static_cast<int>(links[static_cast<size_t>(i)].Get()));
    }

    std::vector<ed::NodeId> nodes(static_cast<size_t>(count));
    const int nodeCount = ed::GetSelectedNodes(nodes.data(), count);
    for (int i = 0; i < nodeCount; ++i) {
        RemoveNode(static_cast<int>(nodes[static_cast<size_t>(i)].Get()));
    }

    ed::ClearSelection();
}

void FlowMap::HandleInteractions() {
    if (m_readOnly) return;
    if (ed::BeginCreate(ImColor(137, 208, 255), 2.5f)) {
        ed::PinId start = 0;
        ed::PinId end = 0;
        if (ed::QueryNewLink(&start, &end)) {
            int fromPin = static_cast<int>(start.Get());
            int toPin = static_cast<int>(end.Get());
            if (IsInputPin(fromPin) && IsOutputPin(toPin)) {
                std::swap(fromPin, toPin);
            }

            if (CanLink(fromPin, toPin)) {
                ImGui::SetTooltip("Create link");
                if (ed::AcceptNewItem(ImColor(136, 255, 178), 3.0f)) {
                    AddLink(fromPin, toPin);
                }
            } else {
                ImGui::SetTooltip("Connect an output pin to another chapter's input pin");
                ed::RejectNewItem(ImColor(255, 96, 96), 2.0f);
            }
        }
    }
    ed::EndCreate();

    bool deleted = false;
    if (ed::BeginDelete()) {
        ed::LinkId linkId = 0;
        while (ed::QueryDeletedLink(&linkId)) {
            if (static_cast<int>(linkId.Get()) == m_entryLinkId) {
                ed::RejectDeletedItem();
                continue;
            }
            if (ed::AcceptDeletedItem()) {
                RemoveLink(static_cast<int>(linkId.Get()));
                deleted = true;
            }
        }

        ed::NodeId nodeId = 0;
        while (ed::QueryDeletedNode(&nodeId)) {
            if (static_cast<int>(nodeId.Get()) == m_entryNodeId) {
                ed::RejectDeletedItem();
                continue;
            }
            if (ed::AcceptDeletedItem()) {
                RemoveNode(static_cast<int>(nodeId.Get()));
                deleted = true;
            }
        }
    }
    ed::EndDelete();

    const ImGuiIO& io = ImGui::GetIO();
    const bool deletePressed =
        ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (!deleted && deletePressed && !io.WantTextInput &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        DeleteSelection();
    }
}

void FlowMap::RenderContextMenus(std::string& pendingOpen, bool& pendingCreateChapter,
                                 ImVec2& pendingCreatePosition) {
    ed::NodeId contextNodeId = 0;
    ed::LinkId contextLinkId = 0;
    if (ed::ShowNodeContextMenu(&contextNodeId)) {
        m_contextNodeId = static_cast<int>(contextNodeId.Get());
        ImGui::OpenPopup("Flow Node Menu");
    } else if (ed::ShowLinkContextMenu(&contextLinkId)) {
        m_contextLinkId = static_cast<int>(contextLinkId.Get());
        ImGui::OpenPopup("Flow Link Menu");
    } else if (ed::ShowBackgroundContextMenu()) {
        pendingCreatePosition = ed::ScreenToCanvas(ImGui::GetMousePos());
        ImGui::OpenPopup("Flow Create Menu");
    }

    if (m_readOnly) {
        if (ImGui::BeginPopup("Flow Create Menu")) {
            ImGui::TextDisabled("Story Map is currently read-only.");
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("Flow Node Menu")) {
            if (const FNode* node = FindNode(m_contextNodeId)) {
                ImGui::TextUnformatted(node->title.c_str());
                if (!node->isEntry && !node->script.empty() && !node->scriptMissing &&
                    ImGui::MenuItem("Open in Story")) pendingOpen = node->script;
                ImGui::TextDisabled("Read-only Story Map");
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("Flow Link Menu")) {
            ImGui::TextDisabled("This explicit Scenario target cannot be edited in read-only mode.");
            ImGui::EndPopup();
        }
        return;
    }

    if (ImGui::BeginPopup("Flow Create Menu")) {
        if (ImGui::MenuItem("Add Chapter")) {
            pendingCreateChapter = true;
        }
        ImGui::EndPopup();
    }

    bool openRename = false;
    if (ImGui::BeginPopup("Flow Node Menu")) {
        const FNode* node = FindNode(m_contextNodeId);
        const bool isEntry = node && node->isEntry;
        if (node) {
            ImGui::TextUnformatted(node->title.c_str());
            if (!isEntry) {
                if (node->scriptMissing) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s (missing)",
                                       node->script.c_str());
                } else {
                    ImGui::TextDisabled("%s", node->script.empty() ? "(no script)" : node->script.c_str());
                }
            }
            ImGui::Separator();
            if (!isEntry) {
                ImGui::BeginDisabled(node->script.empty() || node->scriptMissing);
                if (ImGui::MenuItem("Open in Story")) {
                    pendingOpen = node->script;
                }
                ImGui::EndDisabled();

                if (ImGui::MenuItem("Choose Script...")) {
                    m_scriptPickerNodeId = node->id;
                    m_scriptPickerOpen = true;
                }
                if (ImGui::MenuItem("Rename...")) {
                    openRename = true;
                    std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", node->title.c_str());
                }
                ImGui::Separator();

                ImGui::BeginDisabled(node->script.empty() || node->scriptMissing);
                const bool isCurrentEntry = !node->script.empty() && node->script == m_entryScript;
                if (ImGui::MenuItem("Set as Entry Point", nullptr, isCurrentEntry, !isCurrentEntry)) {
                    const std::string script = node->script;
                    m_entryScript = script;
                    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const FLink& l) {
                        return l.id == m_entryLinkId;
                    }), m_links.end());
                    if (const FNode* entry = FindNode(m_entryNodeId)) {
                        m_entryLinkId = m_nextLinkId;
                        m_links.push_back(FLink{ m_nextLinkId++, entry->pinOut, node->pinIn });
                    }
                    if (m_entryChanged) m_entryChanged(script);
                }
                ImGui::EndDisabled();
            }
        }
        if (!isEntry && ImGui::MenuItem("Delete Chapter")) {
            RemoveNode(m_contextNodeId);
        }
        ImGui::EndPopup();
    }

    if (openRename) {
        ImGui::OpenPopup("Flow Rename Chapter");
    }
    if (ImGui::BeginPopup("Flow Rename Chapter")) {
        ImGui::TextDisabled("Chapter title");
        ImGui::SetNextItemWidth(240);
        const bool submitted = ImGui::InputText("##flowRename", m_renameBuffer,
                                                sizeof(m_renameBuffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (submitted || ImGui::Button("OK")) {
            if (FNode* node = FindNode(m_contextNodeId); node && m_renameBuffer[0] != 0) {
                node->title = m_renameBuffer;
                if (m_titleChanged) m_titleChanged(node->chapterId, node->title);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("Flow Link Menu")) {
        if (FindLink(m_contextLinkId) && ImGui::MenuItem("Delete Link", "Delete")) {
            RemoveLink(m_contextLinkId);
        }
        ImGui::EndPopup();
    }

    if (m_scriptPickerOpen) {
        ImGui::OpenPopup("Flow Script Picker");
        m_scriptPickerOpen = false;
        m_newScriptBuffer[0] = 0;
    }
    if (ImGui::BeginPopup("Flow Script Picker")) {
        const FNode* node = FindNode(m_scriptPickerNodeId);
        if (!node) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled("Script for \"%s\"", node->title.c_str());
            ImGui::Separator();
            if (m_availableScripts.empty()) {
                ImGui::TextDisabled("(no .pxscenario files yet)");
            }
            for (const std::string& script : m_availableScripts) {
                const bool current = script == node->script;
                if (ImGui::Selectable(script.c_str(), current)) {
                    if (!current && m_scriptChanged) m_scriptChanged(node->chapterId, script);
                    ImGui::CloseCurrentPopup();
                }
            }
            if (node->scriptMissing) {
                ImGui::Separator();
                if (ImGui::Selectable(("Create file \"" + node->script + "\"").c_str())) {
                    if (m_createScript) m_createScript(node->script);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("New script");
            ImGui::SetNextItemWidth(150.0f);
            const bool submit = ImGui::InputTextWithHint("##newscript", "name", m_newScriptBuffer,
                                                         sizeof(m_newScriptBuffer),
                                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((submit || ImGui::Button("Create")) && m_newScriptBuffer[0] != 0) {
                std::string name = m_newScriptBuffer;
                if (!name.ends_with(".pxscenario")) name += ".pxscenario";
                if (!name.starts_with("Content/")) name = "Content/Scenario/" + name;
                if (m_scriptChanged) m_scriptChanged(node->chapterId, name);
                if (m_createScript) m_createScript(name);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void FlowMap::RenderEntryNode(FNode& node) {
    const ImColor accent(255, 196, 88);
    const bool selected = ed::IsNodeSelected(ed::NodeId(node.id));
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImColor(34, 28, 16, 245));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, selected ? accent : ImColor(150, 116, 52, 230));
    ed::BeginNode(ed::NodeId(node.id));
    ImGui::PushID(node.id);

    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.42f, 1.0f), "%s", node.title.c_str());
    ImGui::TextDisabled("%s", m_entryScript.empty() ? "(drag to a chapter)" : m_entryScript.c_str());

    ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
    ed::PushStyleVar(ed::StyleVar_PivotAlignment, ImVec2(1.0f, 0.5f));
    ed::BeginPin(ed::PinId(node.pinOut), ed::PinKind::Output);
    ImGui::TextDisabled("Entry");
    ImGui::SameLine(0.0f, 6.0f);
    ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), IconType::Flow, IsPinLinked(node.pinOut),
                      ImColor(255, 220, 150), ImColor(34, 28, 16));
    ed::EndPin();
    ed::PopStyleVar(2);

    ImGui::PopID();
    ed::EndNode();
    ed::PopStyleColor(2);
}

void FlowMap::SyncMovedNodes() {
    for (FNode& n : m_nodes) {
        if (!n.posSet) continue;
        const ImVec2 p = ed::GetNodePosition(ed::NodeId(n.id));
        if (std::abs(p.x - n.pos.x) > 0.5f || std::abs(p.y - n.pos.y) > 0.5f) {
            n.pos = p;
            if (m_layoutChanged) m_layoutChanged(n.isEntry ? std::string{} : n.script, p);
        }
    }
}

void FlowMap::Render() {
    if (m_readOnly) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                           "Story Map is read-only for this workspace.");
    }
    if (m_nodes.empty()) {
        ImGui::TextDisabled("No chapters. Right-click the canvas to add one.");
    } else {
        ImGui::TextDisabled("Story flow — drag from START to choose the game's entry chapter.");
    }

    ed::SetCurrentEditor(m_ctx);
    ed::PushStyleVar(ed::StyleVar_NodeRounding, kNodeRounding);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 1.4f);
    ed::Begin("flow_map");

    std::string pendingOpen;
    bool pendingCreateChapter = false;
    ImVec2 pendingCreatePosition(0.0f, 0.0f);

    for (FNode& n : m_nodes) {
        if (!n.posSet) {
            ed::SetNodePosition(ed::NodeId(n.id), n.pos);
            n.posSet = true;
        }
        if (n.isEntry) {
            RenderEntryNode(n);
            continue;
        }
        const bool selected = ed::IsNodeSelected(ed::NodeId(n.id));
        const ImColor accent = n.scriptMissing ? ImColor(235, 96, 96) : ImColor(103, 219, 177);
        const ImColor fill = selected ? ImColor(28, 37, 54, 250) : ImColor(19, 24, 38, 240);
        const ImColor border = n.scriptMissing
                                   ? (selected ? ImColor(255, 120, 120) : ImColor(190, 80, 80, 235))
                                   : (selected ? accent : ImColor(70, 86, 111, 220));
        ImRect headerRect;

        ed::PushStyleColor(ed::StyleColor_NodeBg, fill);
        ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(n.id);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));

        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kNodeContentWidth);
        ImGui::TextColored(ImVec4(0.95f, 0.98f, 1.0f, 1.0f), "%s", n.title.c_str());
        ImGui::PopTextWrapPos();
        headerRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        // In-node script picker button. The combo itself is deferred to the
        // Suspend/Resume region (an in-node popup trips the EndNode assertion).
        ImGui::PushStyleColor(ImGuiCol_Button, n.scriptMissing ? ImVec4(0.32f, 0.16f, 0.16f, 1.0f)
                                                               : ImVec4(0.12f, 0.16f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, n.scriptMissing
                                                          ? ImVec4(0.44f, 0.22f, 0.22f, 1.0f)
                                                          : ImVec4(0.17f, 0.25f, 0.35f, 1.0f));
        std::string pickLabel;
        if (n.script.empty()) pickLabel = "+ Select script";
        else if (n.scriptMissing) pickLabel = n.script + "  (missing!)";
        else pickLabel = n.script;
        ImGui::BeginDisabled(m_readOnly);
        if (ImGui::Button((pickLabel + "##pick" + std::to_string(n.id)).c_str(),
                          ImVec2(kNodeContentWidth, 0.0f))) {
            m_scriptPickerNodeId = n.id;
            m_scriptPickerOpen = true;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        // Pins row: In flush left, Out flush right. Use a fixed node width — a
        // node-editor node reports a huge GetContentRegionAvail, so offsetting by
        // it would stretch the pin's hover rect across the whole canvas.
        const float rowStartX = ImGui::GetCursorPosX();
        const ImColor pinColor(245, 248, 255);
        const ImColor pinBg(25, 30, 41);

        ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
        ed::PushStyleVar(ed::StyleVar_PivotAlignment, ImVec2(0.0f, 0.5f));
        ed::BeginPin(ed::PinId(n.pinIn), ed::PinKind::Input);
        ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), IconType::Flow, IsPinLinked(n.pinIn),
                          pinColor, pinBg);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextDisabled("In");
        ed::EndPin();
        ed::PopStyleVar(2);

        const float outWidth = kPinSize + 6.0f + ImGui::CalcTextSize("Out").x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowStartX + kNodeContentWidth - outWidth);
        ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
        ed::PushStyleVar(ed::StyleVar_PivotAlignment, ImVec2(1.0f, 0.5f));
        ed::BeginPin(ed::PinId(n.pinOut), ed::PinKind::Output);
        ImGui::TextDisabled("Out");
        ImGui::SameLine(0.0f, 6.0f);
        ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), IconType::Flow, IsPinLinked(n.pinOut),
                          pinColor, pinBg);
        ed::EndPin();
        ed::PopStyleVar(2);
        ImGui::EndGroup();

        ImGui::PopStyleVar(3);
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleColor(2);

        if (ImDrawList* draw = ed::GetNodeBackgroundDrawList(ed::NodeId(n.id))) {
            const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            draw->AddRectFilled(rect.Min, rect.Max, fill, kNodeRounding, ImDrawFlags_RoundCornersAll);

            const float halfBorder = ed::GetStyle().NodeBorderWidth * 0.5f;
            const ImVec2 headerMin(rect.Min.x + halfBorder, rect.Min.y + halfBorder);
            const ImVec2 headerMax(rect.Max.x - halfBorder, std::min(rect.Max.y, headerRect.Max.y + 10.0f));
            if (headerMax.x > headerMin.x && headerMax.y > headerMin.y) {
                if (m_headerTexture && m_headerTextureWidth > 0 && m_headerTextureHeight > 0) {
                    const ImVec2 uv(
                        std::clamp((headerMax.x - headerMin.x) / (kHeaderTextureScale * static_cast<float>(m_headerTextureWidth)), 0.0f, 1.0f),
                        std::clamp((headerMax.y - headerMin.y) / (kHeaderTextureScale * static_cast<float>(m_headerTextureHeight)), 0.0f, 1.0f));
                    ImColor headerTint = accent;
                    headerTint.Value.w = selected ? 0.90f : 0.84f;
                    draw->AddImageRounded(m_headerTexture, headerMin, headerMax, ImVec2(0.0f, 0.0f), uv, headerTint, kNodeRounding, ImDrawFlags_RoundCornersTop);
                    draw->AddRectFilled(headerMin, headerMax, selected ? IM_COL32(8, 12, 20, 56) : IM_COL32(8, 12, 20, 84), kNodeRounding, ImDrawFlags_RoundCornersTop);
                } else {
                    ImColor head = accent;
                    head.Value.w = selected ? 0.50f : 0.38f;
                    draw->AddRectFilled(headerMin, headerMax, head, kNodeRounding, ImDrawFlags_RoundCornersTop);
                }
            }

            draw->AddRect(rect.Min, rect.Max, border, kNodeRounding, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.2f);
        }
    }
    for (const FLink& l : m_links) {
        const bool isEntryLink = l.id == m_entryLinkId;
        ed::Link(ed::LinkId(l.id), ed::PinId(l.fromPin), ed::PinId(l.toPin),
                 isEntryLink ? ImColor(255, 196, 88) : ImColor(103, 219, 177),
                 isEntryLink ? 3.2f : 2.4f);
    }

    HandleInteractions();
    if (const ed::NodeId openedNode = ed::GetDoubleClickedNode()) {
        if (const FNode* node = FindNode(static_cast<int>(openedNode.Get())); node && !node->script.empty()) {
            pendingOpen = node->script;
        }
    }

    ed::Suspend();
    RenderContextMenus(pendingOpen, pendingCreateChapter, pendingCreatePosition);
    ed::Resume();

    SyncMovedNodes();
    ed::End();
    ed::PopStyleVar(2);
    ed::SetCurrentEditor(nullptr);

    if (!pendingOpen.empty() && m_open) {
        m_open(pendingOpen);
    }
    if (pendingCreateChapter && m_createChapter) {
        m_createChapter(pendingCreatePosition);
    }
}

}
