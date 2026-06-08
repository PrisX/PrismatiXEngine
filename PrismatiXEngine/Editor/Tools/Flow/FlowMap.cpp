#define IMGUI_DEFINE_MATH_OPERATORS

#include "Editor/Tools/Flow/FlowMap.h"

#include <imgui_internal.h>
#include <widgets.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace ed = ax::NodeEditor;
using ax::Drawing::IconType;

namespace px::editor {

namespace {
constexpr float kNodeRounding = 12.0f;
constexpr float kNodeContentWidth = 360.0f;
constexpr float kPinSize = 18.0f;
constexpr float kHeaderTextureScale = 6.0f;

std::string ExtractTarget(const std::string& line) {
    const std::size_t key = line.find("target=\"");
    if (key == std::string::npos) return {};
    const std::size_t start = key + 8;
    const std::size_t end = line.find('"', start);
    if (end == std::string::npos) return {};
    return line.substr(start, end - start);
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

void FlowMap::Rebuild(const px::project::Database& db, const std::filesystem::path& projectRoot) {
    m_nodes.clear();
    m_links.clear();
    m_nextId = 1;
    m_nextLinkId = 100000;

    for (int i = 0; i < static_cast<int>(db.chapters.size()); ++i) {
        FNode n;
        n.id = m_nextId++;
        n.pinIn = m_nextId++;
        n.pinOut = m_nextId++;
        n.title = db.chapters[i].title.empty() ? db.chapters[i].id : db.chapters[i].title;
        n.script = db.chapters[i].script;
        n.pos = ImVec2(60.0f + (i % 4) * 260.0f, 60.0f + (i / 4) * 180.0f);
        m_nodes.push_back(std::move(n));
    }

    for (const FNode& from : m_nodes) {
        if (from.script.empty()) continue;
        std::ifstream in(projectRoot / "Data" / "Script" / from.script);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("[jump") == std::string::npos && line.find("[choice") == std::string::npos) {
                continue;
            }
            const std::string target = ExtractTarget(line);
            if (target.empty() || target[0] == '*') continue;
            for (const FNode& to : m_nodes) {
                if (&to != &from && (to.script == target || to.script == target + ".pds")) {
                    AddLink(from.pinOut, to.pinIn);
                }
            }
        }
    }
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
    const bool exists = std::any_of(m_links.begin(), m_links.end(), [&](const FLink& link) {
        return link.fromPin == fromPin && link.toPin == toPin;
    });
    if (exists) {
        return;
    }
    m_links.push_back(FLink{ m_nextLinkId++, fromPin, toPin });
}

void FlowMap::RemoveLink(int id) {
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const FLink& link) {
        return link.id == id;
    }), m_links.end());
}

void FlowMap::RemoveNode(int id) {
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(), [&](const FLink& link) {
        const FNode* from = FindNodeForPin(link.fromPin);
        const FNode* to = FindNodeForPin(link.toPin);
        return (from && from->id == id) || (to && to->id == id);
    }), m_links.end());
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
            if (ed::AcceptDeletedItem()) {
                RemoveLink(static_cast<int>(linkId.Get()));
                deleted = true;
            }
        }

        ed::NodeId nodeId = 0;
        while (ed::QueryDeletedNode(&nodeId)) {
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

    if (ImGui::BeginPopup("Flow Create Menu")) {
        if (ImGui::MenuItem("Add Chapter")) {
            pendingCreateChapter = true;
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("Flow Node Menu")) {
        if (const FNode* node = FindNode(m_contextNodeId)) {
            ImGui::TextUnformatted(node->title.c_str());
            ImGui::TextDisabled("%s", node->script.empty() ? "(no script)" : node->script.c_str());
            ImGui::Separator();
            ImGui::BeginDisabled(node->script.empty());
            if (ImGui::MenuItem("Open in Story")) {
                pendingOpen = node->script;
            }
            ImGui::EndDisabled();
        }
        if (ImGui::MenuItem("Remove from Flow")) {
            RemoveNode(m_contextNodeId);
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("Flow Link Menu")) {
        if (FindLink(m_contextLinkId) && ImGui::MenuItem("Delete Link", "Delete")) {
            RemoveLink(m_contextLinkId);
        }
        ImGui::EndPopup();
    }
}

void FlowMap::Render() {
    if (m_nodes.empty()) {
        ImGui::TextDisabled("No chapters. Right-click the canvas to add one.");
    } else {
        ImGui::TextDisabled("Story flow — chapters + cross-script jumps.");
    }

    ed::SetCurrentEditor(m_ctx);
    ed::PushStyleVar(ed::StyleVar_NodeRounding, kNodeRounding);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 1.4f);
    ed::Begin("flow_map");

    const auto renderPin = [&](const FNode& node, int pinId, bool input) {
        const bool linked = IsPinLinked(pinId);
        const ImColor color(103, 219, 177);
        ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
        ed::PushStyleVar(ed::StyleVar_PivotAlignment, input ? ImVec2(0.0f, 0.5f) : ImVec2(1.0f, 0.5f));
        ed::BeginPin(ed::PinId(pinId), input ? ed::PinKind::Input : ed::PinKind::Output);
        if (input) {
            ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), IconType::Circle, linked, color, ImColor(25, 30, 41));
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextDisabled("In");
        } else {
            const float offset = std::max(0.0f, ImGui::GetContentRegionAvail().x - kPinSize);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
            ax::Widgets::Icon(ImVec2(kPinSize, kPinSize), IconType::Circle, linked, color, ImColor(25, 30, 41));
        }
        ed::EndPin();
        ed::PopStyleVar(2);
    };

    std::string pendingOpen;
    bool pendingCreateChapter = false;
    ImVec2 pendingCreatePosition(0.0f, 0.0f);

    for (FNode& n : m_nodes) {
        if (!n.posSet) {
            ed::SetNodePosition(ed::NodeId(n.id), n.pos);
            n.posSet = true;
        }
        const bool selected = ed::IsNodeSelected(ed::NodeId(n.id));
        const ImColor accent(103, 219, 177);
        const ImColor fill = selected ? ImColor(28, 37, 54, 250) : ImColor(19, 24, 38, 240);
        const ImColor border = selected ? accent : ImColor(70, 86, 111, 220);
        ImRect headerRect;

        ed::PushStyleColor(ed::StyleColor_NodeBg, fill);
        ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(n.id);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));

        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kNodeContentWidth - 24.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.98f, 1.0f, 1.0f), "%s", n.title.c_str());
        ImGui::TextColored(ImVec4(0.62f, 0.70f, 0.80f, 1.0f), "%s", n.script.empty() ? "(no script)" : n.script.c_str());
        ImGui::PopTextWrapPos();
        headerRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        if (ImGui::BeginTable("flow-node-layout", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoKeepColumnsVisible, ImVec2(kNodeContentWidth, 0.0f))) {
            ImGui::TableSetupColumn("inputs", ImGuiTableColumnFlags_WidthFixed, 88.0f);
            ImGui::TableSetupColumn("outputs", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            renderPin(n, n.pinIn, true);
            ImGui::TableSetColumnIndex(1);
            renderPin(n, n.pinOut, false);
            ImGui::EndTable();
        }
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
        ed::Link(ed::LinkId(l.id), ed::PinId(l.fromPin), ed::PinId(l.toPin),
                 ImColor(103, 219, 177), 2.4f);
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
