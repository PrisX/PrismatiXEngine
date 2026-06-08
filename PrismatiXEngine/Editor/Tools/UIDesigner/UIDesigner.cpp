#include "Editor/Tools/UIDesigner/UIDesigner.h"

#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <initializer_list>

#include "Engine/Support/Logger.h"

namespace px::editor {

using px::Color;
using px::Rect;
using px::ui::Anchor;
using px::ui::NodeType;
using px::ui::UINode;

namespace {
const std::array<const char*, 11> kNodeTypes = { "frame", "panel", "button", "image", "text", "dialogue_box", "choice_list", "backlog", "save_grid", "gallery_grid", "component" };
const std::array<const char*, 9> kAnchors = { "topleft", "top", "topright", "left", "center", "right", "bottomleft", "bottom", "bottomright" };
const std::array<const char*, 11> kActions = { "", "scene.start", "scene.switch", "save.open", "load.open", "gallery.open", "settings.open", "backlog.open", "app.quit", "choice", "lua" };
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

enum HandleBits { H_L = 1, H_R = 2, H_T = 4, H_B = 8 };

struct SnapResult {
    bool found = false;
    float shift = 0.0f;
    float guide = 0.0f;
};

SnapResult SnapAxis(std::initializer_list<float> candidates, const std::vector<float>& targets, float threshold) {
    SnapResult best;
    float bestDist = threshold;
    for (float c : candidates) {
        for (float t : targets) {
            const float d = std::abs(c - t);
            if (d <= bestDist) {
                bestDist = d;
                best.found = true;
                best.shift = t - c;
                best.guide = t;
            }
        }
    }
    return best;
}
}  // namespace

void UIDesigner::SetScene(px::ui::UIScene* scene, std::string path) {
    m_scene = scene;
    m_path = std::move(path);
    m_selected = -1;
    m_dirty = false;
}

void UIDesigner::MarkEdited(bool structural) {
    m_dirty = true;
    if (structural && m_onStructure) m_onStructure();
    if (m_onEdit) m_onEdit();
}

void UIDesigner::AnchorOffset(const UINode& node, float& ox, float& oy) const {
    const int col = static_cast<int>(node.anchor) % 3;
    const int row = static_cast<int>(node.anchor) / 3;
    const float cw = static_cast<float>(m_scene->canvasW);
    const float ch = static_cast<float>(m_scene->canvasH);
    ox = col == 0 ? 0.0f : (col == 1 ? cw * 0.5f : cw);
    oy = row == 0 ? 0.0f : (row == 1 ? ch * 0.5f : ch);
}

Rect UIDesigner::CanvasRect(const UINode& node) const {
    float ox = 0.0f, oy = 0.0f;
    AnchorOffset(node, ox, oy);
    return Rect{ ox + node.rect.x, oy + node.rect.y, node.rect.w, node.rect.h };
}

int UIDesigner::HitTest(float cx, float cy) const {
    std::vector<int> order(m_scene->nodes.size());
    for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return m_scene->nodes[a].order < m_scene->nodes[b].order; });
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const UINode& n = m_scene->nodes[*it];
        if (!n.visible) continue;
        if (CanvasRect(n).Contains(cx, cy)) return *it;
    }
    return -1;
}

void UIDesigner::AddNode(NodeType type) {
    UINode n;
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

void UIDesigner::RemoveSelectedNode() {
    if (!m_scene || m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) {
        return;
    }
    m_scene->nodes.erase(m_scene->nodes.begin() + m_selected);
    m_selected = -1;
    m_dragging = false;
    m_resizeHandle = 0;
    MarkEdited(true);
}

void UIDesigner::RenderHierarchy() {
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
    if (ImGui::Button("Delete")) {
        RemoveSelectedNode();
    }
    ImGui::SameLine();
    if (ImGui::Button(m_dirty ? "Save*" : "Save")) Save();

    if (ImGui::Checkbox("Grid (G)", &m_showGrid)) {
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragInt("cell", &m_gridSize, 1.0f, 2, 256)) {
        m_gridSize = std::max(2, m_gridSize);
    }
    ImGui::TextDisabled("Drag snaps to grid / guides. Hold Alt to move freely.");
    ImGui::Separator();

    std::vector<std::string> layers;
    for (const UINode& n : m_scene->nodes) {
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
            UINode& n = m_scene->nodes[i];
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
            const std::string label = (n.id.empty() ? "(node)" : n.id) + "  [" + px::ui::ToString(n.type) + "]";
            if (ImGui::Selectable(label.c_str(), m_selected == i)) {
                m_selected = i;
            }
            ImGui::PopID();
        }
        ImGui::Unindent(6.0f);
    }
}

void UIDesigner::RenderInspector(const std::string& selectedAssetPath) {
    if (!m_scene) {
        ImGui::TextDisabled("No scene.");
        return;
    }
    if (m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) {
        ImGui::TextDisabled("Select a node in the Hierarchy or canvas.");
        return;
    }
    UINode& n = m_scene->nodes[m_selected];

    auto colorField = [&](const char* label, Color& c) {
        float v[4] = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
        if (ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_AlphaBar)) {
            c = { (std::uint8_t)(v[0] * 255), (std::uint8_t)(v[1] * 255), (std::uint8_t)(v[2] * 255), (std::uint8_t)(v[3] * 255) };
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

void UIDesigner::RenderAnimation() {
    if (!m_scene) {
        ImGui::TextDisabled("No scene.");
        return;
    }
    if (m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) {
        ImGui::TextDisabled("Select a node to edit its animations.");
        return;
    }
    UINode& n = m_scene->nodes[m_selected];
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
        if (ImGui::CollapsingHeader((clip.name.empty() ? ("clip " + std::to_string(ci)) : clip.name).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
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

int UIDesigner::HandleHitTest(const Rect& cr, float cx, float cy, float scale) const {
    const float hs = 8.0f / scale;
    const float L = cr.x, R = cr.x + cr.w, T = cr.y, B = cr.y + cr.h;
    const float MX = cr.x + cr.w * 0.5f, MY = cr.y + cr.h * 0.5f;
    auto near = [&](float px, float py) { return std::abs(cx - px) <= hs && std::abs(cy - py) <= hs; };
    if (near(L, T)) return H_L | H_T;
    if (near(R, T)) return H_R | H_T;
    if (near(L, B)) return H_L | H_B;
    if (near(R, B)) return H_R | H_B;
    if (near(MX, T)) return H_T;
    if (near(MX, B)) return H_B;
    if (near(L, MY)) return H_L;
    if (near(R, MY)) return H_R;
    return 0;
}

void UIDesigner::SnapNode(UINode& n, int handle, float scale) {
    float ox = 0.0f, oy = 0.0f;
    AnchorOffset(n, ox, oy);

    if (m_showGrid) {
        const float g = static_cast<float>(m_gridSize);
        if (handle == 0) {
            const float gx = std::round((ox + n.rect.x + n.rect.w * 0.5f) / g) * g;
            const float gy = std::round((oy + n.rect.y + n.rect.h * 0.5f) / g) * g;
            n.rect.x = gx - n.rect.w * 0.5f - ox;
            n.rect.y = gy - n.rect.h * 0.5f - oy;
        }
        else {
            if (handle & H_L) {
                const float L = std::round((ox + n.rect.x) / g) * g;
                n.rect.w += (ox + n.rect.x) - L;
                n.rect.x = L - ox;
            }
            if (handle & H_R) {
                const float R = std::round((ox + n.rect.x + n.rect.w) / g) * g;
                n.rect.w = R - ox - n.rect.x;
            }
            if (handle & H_T) {
                const float T = std::round((oy + n.rect.y) / g) * g;
                n.rect.h += (oy + n.rect.y) - T;
                n.rect.y = T - oy;
            }
            if (handle & H_B) {
                const float B = std::round((oy + n.rect.y + n.rect.h) / g) * g;
                n.rect.h = B - oy - n.rect.y;
            }
            n.rect.w = std::max(8.0f, n.rect.w);
            n.rect.h = std::max(8.0f, n.rect.h);
        }
        return;
    }

    const float cw = static_cast<float>(m_scene->canvasW);
    const float ch = static_cast<float>(m_scene->canvasH);
    std::vector<float> tx{ 0.0f, cw * 0.5f, cw };
    std::vector<float> ty{ 0.0f, ch * 0.5f, ch };
    for (int i = 0; i < static_cast<int>(m_scene->nodes.size()); ++i) {
        if (i == m_selected || !m_scene->nodes[i].visible) continue;
        const Rect r = CanvasRect(m_scene->nodes[i]);
        tx.push_back(r.x);
        tx.push_back(r.x + r.w * 0.5f);
        tx.push_back(r.x + r.w);
        ty.push_back(r.y);
        ty.push_back(r.y + r.h * 0.5f);
        ty.push_back(r.y + r.h);
    }

    const float thr = 6.0f / scale;
    const float left = ox + n.rect.x;
    const float top = oy + n.rect.y;
    const float w = n.rect.w, h = n.rect.h;

    if (handle == 0) {
        const SnapResult sx = SnapAxis({ left, left + w * 0.5f, left + w }, tx, thr);
        const SnapResult sy = SnapAxis({ top, top + h * 0.5f, top + h }, ty, thr);
        if (sx.found) {
            n.rect.x += sx.shift;
            m_guideX.push_back(sx.guide);
        }
        if (sy.found) {
            n.rect.y += sy.shift;
            m_guideY.push_back(sy.guide);
        }
    }
    else {
        if (handle & H_L) {
            const SnapResult s = SnapAxis({ left }, tx, thr);
            if (s.found) {
                n.rect.x += s.shift;
                n.rect.w -= s.shift;
                m_guideX.push_back(s.guide);
            }
        }
        if (handle & H_R) {
            const SnapResult s = SnapAxis({ left + w }, tx, thr);
            if (s.found) {
                n.rect.w += s.shift;
                m_guideX.push_back(s.guide);
            }
        }
        if (handle & H_T) {
            const SnapResult s = SnapAxis({ top }, ty, thr);
            if (s.found) {
                n.rect.y += s.shift;
                n.rect.h -= s.shift;
                m_guideY.push_back(s.guide);
            }
        }
        if (handle & H_B) {
            const SnapResult s = SnapAxis({ top + h }, ty, thr);
            if (s.found) {
                n.rect.h += s.shift;
                m_guideY.push_back(s.guide);
            }
        }
        n.rect.w = std::max(8.0f, n.rect.w);
        n.rect.h = std::max(8.0f, n.rect.h);
    }
}

bool UIDesigner::CanvasInput(ImVec2 p0, float scale, bool hovered) {
    if (!m_scene || scale <= 0.0f) return false;
    m_guideX.clear();
    m_guideY.clear();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float cx = (mouse.x - p0.x) / scale;
    const float cy = (mouse.y - p0.y) / scale;

    if (hovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        m_showGrid = !m_showGrid;
    }
    if (hovered && !ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        RemoveSelectedNode();
        return true;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_dragging = false;
        m_resizeHandle = 0;
    }

    bool consumed = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int handle = 0;
        if (m_selected >= 0 && m_selected < static_cast<int>(m_scene->nodes.size())) {
            handle = HandleHitTest(CanvasRect(m_scene->nodes[m_selected]), cx, cy, scale);
        }
        if (handle != 0) {
            m_resizeHandle = handle;
            m_rectStart = m_scene->nodes[m_selected].rect;
            m_grabCanvasX = cx;
            m_grabCanvasY = cy;
        }
        else {
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

    if ((m_dragging || m_resizeHandle) && ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_selected >= 0) {
        UINode& n = m_scene->nodes[m_selected];
        const float dx = cx - m_grabCanvasX;
        const float dy = cy - m_grabCanvasY;
        if (m_dragging) {
            n.rect.x = std::round(m_rectStart.x + dx);
            n.rect.y = std::round(m_rectStart.y + dy);
        }
        else {
            Rect r = m_rectStart;
            if (m_resizeHandle & H_L) {
                r.x = m_rectStart.x + dx;
                r.w = m_rectStart.w - dx;
            }
            if (m_resizeHandle & H_R) {
                r.w = m_rectStart.w + dx;
            }
            if (m_resizeHandle & H_T) {
                r.y = m_rectStart.y + dy;
                r.h = m_rectStart.h - dy;
            }
            if (m_resizeHandle & H_B) {
                r.h = m_rectStart.h + dy;
            }
            if (r.w < 8.0f) {
                if (m_resizeHandle & H_L) r.x += (r.w - 8.0f);
                r.w = 8.0f;
            }
            if (r.h < 8.0f) {
                if (m_resizeHandle & H_T) r.y += (r.h - 8.0f);
                r.h = 8.0f;
            }
            n.rect = { std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h) };
        }
        if (!ImGui::GetIO().KeyAlt) {
            SnapNode(n, m_resizeHandle, scale);
        }
        MarkEdited();
        consumed = true;
    }
    return consumed;
}

void UIDesigner::DrawOverlay(ImVec2 p0, float scale) {
    if (!m_scene) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 canvasEnd(p0.x + m_scene->canvasW * scale, p0.y + m_scene->canvasH * scale);
    dl->AddRect(p0, canvasEnd, IM_COL32(90, 100, 120, 140));

    if (m_showGrid && m_gridSize > 0) {
        const ImU32 gridCol = IM_COL32(255, 255, 255, 22);
        for (int gx = m_gridSize; gx < m_scene->canvasW; gx += m_gridSize) {
            const float x = p0.x + gx * scale;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, canvasEnd.y), gridCol);
        }
        for (int gy = m_gridSize; gy < m_scene->canvasH; gy += m_gridSize) {
            const float y = p0.y + gy * scale;
            dl->AddLine(ImVec2(p0.x, y), ImVec2(canvasEnd.x, y), gridCol);
        }
    }

    const ImU32 guideCol = IM_COL32(255, 80, 160, 220);
    for (float gx : m_guideX) {
        const float x = p0.x + gx * scale;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, canvasEnd.y), guideCol);
    }
    for (float gy : m_guideY) {
        const float y = p0.y + gy * scale;
        dl->AddLine(ImVec2(p0.x, y), ImVec2(canvasEnd.x, y), guideCol);
    }

    if (m_selected < 0 || m_selected >= static_cast<int>(m_scene->nodes.size())) return;
    const Rect cr = CanvasRect(m_scene->nodes[m_selected]);
    const ImVec2 a(p0.x + cr.x * scale, p0.y + cr.y * scale);
    const ImVec2 b(a.x + cr.w * scale, a.y + cr.h * scale);
    const ImU32 sel = IM_COL32(120, 200, 255, 255);
    dl->AddRect(a, b, sel, 0.0f, 0, 2.0f);

    const float mcx = (a.x + b.x) * 0.5f, mcy = (a.y + b.y) * 0.5f;
    dl->AddLine(ImVec2(mcx - 7, mcy), ImVec2(mcx + 7, mcy), sel, 1.5f);
    dl->AddLine(ImVec2(mcx, mcy - 7), ImVec2(mcx, mcy + 7), sel, 1.5f);

    const float hx[3] = { a.x, (a.x + b.x) * 0.5f, b.x };
    const float hy[3] = { a.y, (a.y + b.y) * 0.5f, b.y };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (r == 1 && c == 1) continue;
            const ImVec2 hc(hx[c], hy[r]);
            dl->AddRectFilled(ImVec2(hc.x - 4, hc.y - 4), ImVec2(hc.x + 4, hc.y + 4), sel);
            dl->AddRect(ImVec2(hc.x - 4, hc.y - 4), ImVec2(hc.x + 4, hc.y + 4), IM_COL32(20, 30, 45, 255));
        }
    }
}

bool UIDesigner::Save() {
    if (!m_scene || m_path.empty()) return false;
    const std::string text = px::ui::WritePXUI(*m_scene);
    std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        PX_LOG_ERROR("UIDesigner: cannot save '{}'", m_path);
        return false;
    }
    out << text;
    m_dirty = false;
    PX_LOG_INFO("Saved UI scene '{}'", m_path);
    return true;
}

}  // namespace px::editor
