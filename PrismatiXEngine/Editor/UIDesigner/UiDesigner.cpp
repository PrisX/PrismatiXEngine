#include "Editor/UIDesigner/UiDesigner.h"

#include "Logger.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace px::editor {

using px::Color;
using px::Rect;
using px::ui::Anchor;
using px::ui::NodeType;
using px::ui::UiNode;

namespace {
const std::array<const char*, 11> kNodeTypes = { "frame",      "panel",        "button",
                                                 "image",      "text",         "dialogue_box",
                                                 "choice_list", "backlog",     "save_grid",
                                                 "gallery_grid", "component" };
const std::array<const char*, 9> kAnchors = { "topleft", "top",        "topright",
                                              "left",    "center",     "right",
                                              "bottomleft", "bottom",   "bottomright" };
const std::array<const char*, 11> kActions = {
    "",          "scene.start",   "scene.switch", "save.open",   "load.open",    "gallery.open",
    "settings.open", "backlog.open", "app.quit",  "choice",      "lua"
};
const std::array<const char*, 4> kBinds = { "", "saves", "gallery", "music" };

int IndexOf(const std::array<const char*, 11>& arr, const std::string& v) {
    for (int i = 0; i < static_cast<int>(arr.size()); ++i)
        if (v == arr[i]) return i;
    return 0;
}
int IndexOf4(const std::array<const char*, 4>& arr, const std::string& v) {
    for (int i = 0; i < static_cast<int>(arr.size()); ++i)
        if (v == arr[i]) return i;
    return 0;
}
}

void UiDesigner::SetScene(px::ui::UiScene* scene, std::string path) {
    m_scene = scene;
    m_path = std::move(path);
    m_selected = -1;
    m_dirty = false;
}

void UiDesigner::MarkEdited(bool structural) {
    m_dirty = true;
    if (structural && m_onStructure) m_onStructure();
    if (m_onEdit) m_onEdit();
}

Rect UiDesigner::CanvasRect(const UiNode& node) const {
    const int col = static_cast<int>(node.anchor) % 3;
    const int row = static_cast<int>(node.anchor) / 3;
    const float cw = static_cast<float>(m_scene->canvasW);
    const float ch = static_cast<float>(m_scene->canvasH);
    const float ox = col == 0 ? 0.0f : (col == 1 ? cw * 0.5f : cw);
    const float oy = row == 0 ? 0.0f : (row == 1 ? ch * 0.5f : ch);
    return Rect{ ox + node.rect.x, oy + node.rect.y, node.rect.w, node.rect.h };
}

int UiDesigner::HitTest(float cx, float cy) const {
    std::vector<int> order(m_scene->nodes.size());
    for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return m_scene->nodes[a].order < m_scene->nodes[b].order; });
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const UiNode& n = m_scene->nodes[*it];
        if (!n.visible) continue;
        if (CanvasRect(n).Contains(cx, cy)) return *it;
    }
    return -1;
}

void UiDesigner::AddNode(NodeType type) {
    UiNode n;
    n.type = type;
    n.id = std::string(px::ui::ToString(type)) + "_" + std::to_string(m_scene->nodes.size());
    n.anchor = Anchor::Center;
    n.rect = Rect{ -110, -32, 220, 64 };
    n.order = static_cast<int>(m_scene->nodes.size());
    if (type == NodeType::Text) n.text = "Text";
    if (type == NodeType::Button) n.text = "Button";
    m_scene->nodes.push_back(std::move(n));
    m_selected = static_cast<int>(m_scene->nodes.size()) - 1;
    MarkEdited(true);
}

void UiDesigner::RenderHierarchy() {
    if (!m_scene) {
        ImGui::TextDisabled("Open a .pxui in the Preview (UI mode).");
        return;
    }
    if (ImGui::Button("Add")) ImGui::OpenPopup("addNode");
    if (ImGui::BeginPopup("addNode")) {
        for (const char* t : kNodeTypes) {
            if (ImGui::Selectable(t)) AddNode(px::ui::NodeTypeFromString(t));
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && m_selected >= 0 &&
        m_selected < static_cast<int>(m_scene->nodes.size())) {
        m_scene->nodes.erase(m_scene->nodes.begin() + m_selected);
        m_selected = -1;
        MarkEdited(true);
    }
    ImGui::SameLine();
    if (ImGui::Button(m_dirty ? "Save*" : "Save")) Save();
    ImGui::Separator();

    std::vector<std::string> layers;
    for (const UiNode& n : m_scene->nodes) {
        const std::string L = n.layer.empty() ? "(default)" : n.layer;
        if (std::find(layers.begin(), layers.end(), L) == layers.end()) layers.push_back(L);
    }
    if (layers.empty()) {
        ImGui::TextDisabled("No nodes. Use Add.");
    }
    for (const std::string& L : layers) {
        if (!ImGui::CollapsingHeader(L.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        ImGui::Indent(6.0f);
        for (int i = 0; i < static_cast<int>(m_scene->nodes.size()); ++i) {
            UiNode& n = m_scene->nodes[i];
            const std::string nl = n.layer.empty() ? "(default)" : n.layer;
            if (nl != L) {
                continue;
            }
            ImGui::PushID(i);
            bool vis = n.visible;
            if (ImGui::Checkbox("##v", &vis)) {
                n.visible = vis;
                MarkEdited();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(n.locked ? "[L]" : "[ ]")) {
                n.locked = !n.locked;
                MarkEdited();
            }
            ImGui::SameLine();
            const std::string label = (n.id.empty() ? "(node)" : n.id) + "  [" +
                                      px::ui::ToString(n.type) + "]";
            if (ImGui::Selectable(label.c_str(), m_selected == i)) {
                m_selected = i;
            }
            ImGui::PopID();
        }
        ImGui::Unindent(6.0f);
    }
}

void UiDesigner::RenderInspector(const std::string& selectedAssetPath) {
    if (!m_scene) {
        ImGui::TextDisabled("No scene.");
        return;
    }
    if (m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) {
        ImGui::TextDisabled("Select a node in the Hierarchy or canvas.");
        return;
    }
    UiNode& n = m_scene->nodes[m_selected];

    auto colorField = [&](const char* label, Color& c) {
        float v[4] = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
        if (ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_AlphaBar)) {
            c = { (std::uint8_t)(v[0] * 255), (std::uint8_t)(v[1] * 255), (std::uint8_t)(v[2] * 255),
                  (std::uint8_t)(v[3] * 255) };
            MarkEdited();
        }
    };

    if (ImGui::InputText("id", &n.id)) MarkEdited();
    if (ImGui::InputText("layer", &n.layer)) MarkEdited();

    int typeIdx = static_cast<int>(n.type);
    if (ImGui::Combo("type", &typeIdx, kNodeTypes.data(), static_cast<int>(kNodeTypes.size()))) {
        n.type = static_cast<NodeType>(typeIdx);
        MarkEdited(true);
    }

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        int anchorIdx = static_cast<int>(n.anchor);
        if (ImGui::Combo("anchor", &anchorIdx, kAnchors.data(), static_cast<int>(kAnchors.size()))) {
            n.anchor = static_cast<Anchor>(anchorIdx);
            MarkEdited();
        }
        float rect[4] = { n.rect.x, n.rect.y, n.rect.w, n.rect.h };
        if (ImGui::DragFloat4("x,y,w,h", rect, 1.0f)) {
            n.rect = { rect[0], rect[1], rect[2], rect[3] };
            MarkEdited();
        }
        if (ImGui::DragFloat("opacity", &n.opacity, 1.0f, 0.0f, 255.0f)) MarkEdited();
        if (ImGui::DragInt("order", &n.order)) MarkEdited();
    }

    if (ImGui::CollapsingHeader("Style", ImGuiTreeNodeFlags_DefaultOpen)) {
        colorField("bg", n.bgColor);
        colorField("hover", n.hoverColor);
        colorField("border", n.borderColor);
        colorField("text col", n.textColor);
        if (ImGui::DragFloat("radius", &n.radius, 0.5f, 0.0f, 64.0f)) MarkEdited();
        if (ImGui::DragFloat("border top", &n.borderTopHeight, 0.5f, 0.0f, 32.0f)) MarkEdited();
        if (ImGui::Checkbox("text shadow", &n.textShadow)) MarkEdited();
    }

    if (ImGui::CollapsingHeader("Text", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputText("text", &n.text)) MarkEdited();
        if (ImGui::InputText("font", &n.font)) MarkEdited();
        if (ImGui::DragInt("font size", &n.fontSize, 1, 6, 200)) MarkEdited();
        int alignIdx = n.align == "left" ? 0 : 1;
        const char* aligns[] = { "left", "center" };
        if (ImGui::Combo("align", &alignIdx, aligns, 2)) {
            n.align = aligns[alignIdx];
            MarkEdited();
        }
    }

    if (ImGui::CollapsingHeader("Image")) {
        if (ImGui::InputText("image", &n.image)) MarkEdited();
        ImGui::SameLine();
        if (ImGui::SmallButton("use##img") && !selectedAssetPath.empty()) {
            n.image = selectedAssetPath;
            MarkEdited();
        }
        if (ImGui::InputText("hover image", &n.hoverImage)) MarkEdited();
    }

    if (ImGui::CollapsingHeader("Action / Bind", ImGuiTreeNodeFlags_DefaultOpen)) {
        int actIdx = IndexOf(kActions, n.actionType);
        if (ImGui::Combo("action", &actIdx, kActions.data(), static_cast<int>(kActions.size()))) {
            n.actionType = kActions[actIdx];
            MarkEdited();
        }
        if (ImGui::InputText("target", &n.actionTarget)) MarkEdited();
        if (ImGui::InputText("arg", &n.actionArg)) MarkEdited();
        int bindIdx = IndexOf4(kBinds, n.bind);
        if (ImGui::Combo("bind", &bindIdx, kBinds.data(), static_cast<int>(kBinds.size()))) {
            n.bind = kBinds[bindIdx];
            MarkEdited();
        }
    }
}

void UiDesigner::RenderAnimation() {
    if (!m_scene) {
        ImGui::TextDisabled("No scene.");
        return;
    }
    if (m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) {
        ImGui::TextDisabled("Select a node to edit its animations.");
        return;
    }
    UiNode& n = m_scene->nodes[m_selected];
    ImGui::Text("Animations: %s", n.id.empty() ? "(node)" : n.id.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Clip")) {
        px::ui::AnimClip clip;
        clip.name = "clip" + std::to_string(n.animations.size());
        clip.keys = { { 0.0f, "opacity", 0.0f }, { 0.4f, "opacity", 255.0f } };
        n.animations.push_back(std::move(clip));
        MarkEdited();
    }
    ImGui::Separator();

    const char* triggers[] = { "scene.enter", "hover", "click" };
    const char* easings[] = { "linear", "outCubic", "inCubic", "outQuad", "inOutQuad" };
    const char* props[] = { "opacity", "x", "y", "scale" };
    auto combo = [](const char* label, std::string& value, const char* const* opts, int count) {
        int idx = 0;
        for (int i = 0; i < count; ++i)
            if (value == opts[i]) idx = i;
        if (ImGui::Combo(label, &idx, opts, count)) {
            value = opts[idx];
            return true;
        }
        return false;
    };

    for (int ci = 0; ci < static_cast<int>(n.animations.size()); ++ci) {
        px::ui::AnimClip& clip = n.animations[ci];
        ImGui::PushID(ci);
        if (ImGui::CollapsingHeader((clip.name.empty() ? ("clip " + std::to_string(ci)) : clip.name).c_str(),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::InputText("name", &clip.name)) MarkEdited();
            if (combo("trigger", clip.trigger, triggers, 3)) MarkEdited();
            if (combo("easing", clip.easing, easings, 5)) MarkEdited();
            if (ImGui::DragFloat("duration", &clip.duration, 0.01f, 0.0f, 30.0f)) MarkEdited();
            if (ImGui::DragFloat("delay", &clip.delay, 0.01f, 0.0f, 30.0f)) MarkEdited();
            if (ImGui::Checkbox("loop", &clip.loop)) MarkEdited();

            ImGui::TextDisabled("Keyframes (time, property, value)");
            if (ImGui::SmallButton("+ Key")) {
                clip.keys.push_back({ 0.0f, "opacity", 0.0f });
                MarkEdited();
            }
            for (int ki = 0; ki < static_cast<int>(clip.keys.size()); ++ki) {
                px::ui::AnimKey& k = clip.keys[ki];
                ImGui::PushID(ki);
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("t", &k.time, 0.01f, 0.0f, 60.0f)) MarkEdited();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (combo("##p", k.property, props, 4)) MarkEdited();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::DragFloat("v", &k.value, 1.0f)) MarkEdited();
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    clip.keys.erase(clip.keys.begin() + ki);
                    MarkEdited();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("Delete Clip")) {
                n.animations.erase(n.animations.begin() + ci);
                MarkEdited();
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
}

bool UiDesigner::CanvasInput(ImVec2 p0, float scale, bool hovered) {
    if (!m_scene || scale <= 0.0f) return false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float cx = (mouse.x - p0.x) / scale;
    const float cy = (mouse.y - p0.y) / scale;

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_dragging = m_resizing = false;
    }

    bool consumed = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool startedResize = false;
        if (m_selected >= 0 && m_selected < static_cast<int>(m_scene->nodes.size())) {
            const Rect cr = CanvasRect(m_scene->nodes[m_selected]);
            const float hx = cr.x + cr.w, hy = cr.y + cr.h;
            if (std::abs(cx - hx) <= 8.0f / scale && std::abs(cy - hy) <= 8.0f / scale) {
                m_resizing = true;
                m_rectStart = m_scene->nodes[m_selected].rect;
                m_grabCanvasX = cx;
                m_grabCanvasY = cy;
                startedResize = true;
            }
        }
        if (!startedResize) {
            m_selected = HitTest(cx, cy);
            if (m_selected >= 0) {
                m_dragging = true;
                m_rectStart = m_scene->nodes[m_selected].rect;
                m_grabCanvasX = cx;
                m_grabCanvasY = cy;
            }
        }
        consumed = true;
    }

    if ((m_dragging || m_resizing) && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_selected >= 0) {
        UiNode& n = m_scene->nodes[m_selected];
        const float dx = cx - m_grabCanvasX;
        const float dy = cy - m_grabCanvasY;
        if (m_dragging) {
            n.rect.x = std::round(m_rectStart.x + dx);
            n.rect.y = std::round(m_rectStart.y + dy);
        } else {
            n.rect.w = std::max(8.0f, std::round(m_rectStart.w + dx));
            n.rect.h = std::max(8.0f, std::round(m_rectStart.h + dy));
        }
        MarkEdited();
        consumed = true;
    }
    return consumed;
}

void UiDesigner::DrawOverlay(ImVec2 p0, float scale) {
    if (!m_scene) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 canvasEnd(p0.x + m_scene->canvasW * scale, p0.y + m_scene->canvasH * scale);
    dl->AddRect(p0, canvasEnd, IM_COL32(90, 100, 120, 140));

    if (m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) return;
    const Rect cr = CanvasRect(m_scene->nodes[m_selected]);
    const ImVec2 a(p0.x + cr.x * scale, p0.y + cr.y * scale);
    const ImVec2 b(a.x + cr.w * scale, a.y + cr.h * scale);
    dl->AddRect(a, b, IM_COL32(120, 200, 255, 255), 0.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(b.x - 5, b.y - 5), ImVec2(b.x + 5, b.y + 5),
                      IM_COL32(120, 200, 255, 255));
}

bool UiDesigner::Save() {
    if (!m_scene || m_path.empty()) return false;
    const std::string text = px::ui::WritePxui(*m_scene);
    std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        PX_LOG_ERROR("UiDesigner: cannot save '{}'", m_path);
        return false;
    }
    out << text;
    m_dirty = false;
    PX_LOG_INFO("Saved UI scene '{}'", m_path);
    return true;
}

}
