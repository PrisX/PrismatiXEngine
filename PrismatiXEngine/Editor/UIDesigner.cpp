#define IMGUI_DEFINE_MATH_OPERATORS

#include "UIDesigner.h"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace PrismatiX::Editor {

UIDesigner::UIDesigner(LogCallback logCallback)
    : m_logCallback(std::move(logCallback)) {
    SeedDefaults();
}

void UIDesigner::SetSelectedResourceCallback(SelectedResourceCallback callback) {
    m_selectedResourceCallback = std::move(callback);
}

void UIDesigner::SeedDefaults() {
    if (UIComponent* panel = AddComponent(ComponentType::Panel, ImVec2(72.0f, 72.0f))) {
        panel->name = "hero_panel";
        panel->size = ImVec2(520.0f, 220.0f);
        panel->tint = ImVec4(0.08f, 0.12f, 0.18f, 0.78f);
        panel->layout = "Vertical";
        panel->spacing = 16.0f;
        panel->motion.kind = "slide";
    }

    if (UIComponent* label = AddComponent(ComponentType::Label, ImVec2(108.0f, 112.0f))) {
        label->name = "chapter_title";
        label->text = "PrismatiX Editor";
        label->fontSize = 44;
        label->size = ImVec2(420.0f, 56.0f);
        label->motion.kind = "pulse";
    }

    if (UIComponent* button = AddComponent(ComponentType::Button, ImVec2(108.0f, 190.0f))) {
        button->name = "start_button";
        button->text = "Start Story";
        button->size = ImVec2(240.0f, 68.0f);
        button->motion.kind = "lift";
    }

    if (UIComponent* image = AddComponent(ComponentType::Image, ImVec2(760.0f, 92.0f))) {
        image->name = "hero_art";
        image->size = ImVec2(360.0f, 520.0f);
        image->assetPath = "title_bg.jpg";
        image->motion.kind = "fade";
    }

    if (UIComponent* choices = AddComponent(ComponentType::ChoiceStrip, ImVec2(108.0f, 500.0f))) {
        choices->name = "story_choices";
        choices->text = "Continue | Archive | Exit";
        choices->size = ImVec2(520.0f, 104.0f);
        choices->motion.kind = "fade";
    }
}

void UIDesigner::Render(float deltaSeconds) {
    ImGui::BeginChild("ui-toolbar", ImVec2(0.0f, 42.0f), false);
    if (ImGui::Button(m_previewAnimation ? "Pause Motion" : "Play Motion")) {
        m_previewAnimation = !m_previewAnimation;
    }

    ImGui::SameLine();
    ImGui::Checkbox("Show Grid", &m_showGrid);

    ImGui::SameLine();
    if (ImGui::Button("Center Selection")) {
        CenterSelectedComponent();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%zu components", m_components.size());
    ImGui::EndChild();

    ImGui::Separator();

    if (ImGui::BeginTable("ui-workspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Palette", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        RenderPalette();

        ImGui::TableSetColumnIndex(1);
        RenderCanvas(deltaSeconds);

        ImGui::EndTable();
    }
}

void UIDesigner::RenderPalette() {
    ImGui::BeginChild("ui-palette", ImVec2(0.0f, 0.0f), false);
    ImGui::TextUnformatted("Component Palette");
    ImGui::TextDisabled("Drag cards onto the canvas to lay out Lua-driven UI.");
    ImGui::Separator();

    const std::array<ComponentType, 5> types{
        ComponentType::Panel,
        ComponentType::Label,
        ComponentType::Button,
        ComponentType::Image,
        ComponentType::ChoiceStrip,
    };

    for (ComponentType type : types) {
        const std::string label = ComponentLabel(type);
        ImGui::PushID(static_cast<int>(type));
        ImGui::Button(label.c_str(), ImVec2(-1.0f, 42.0f));
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const int payload = static_cast<int>(type);
            ImGui::SetDragDropPayload("PX_UI_COMPONENT_TYPE", &payload, sizeof(payload));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Motion");
    ImGui::BulletText("fade: soften alpha");
    ImGui::BulletText("slide: enter from the left");
    ImGui::BulletText("lift: hover upward");
    ImGui::BulletText("pulse: gentle emphasis");
    ImGui::EndChild();
}

void UIDesigner::RenderCanvas(float deltaSeconds) {
    ImGui::BeginChild("ui-canvas-region", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("ui-canvas-hitbox", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImRect availableRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    const float scale = std::min(availableRect.GetWidth() / m_designSize.x, availableRect.GetHeight() / m_designSize.y);
    const ImVec2 previewSize(m_designSize.x * scale, m_designSize.y * scale);
    const ImVec2 previewMin(
        availableRect.Min.x + (availableRect.GetWidth() - previewSize.x) * 0.5f,
        availableRect.Min.y + (availableRect.GetHeight() - previewSize.y) * 0.5f);
    const ImRect canvasRect(previewMin, previewMin + previewSize);

    RenderCanvasContents(canvasRect, deltaSeconds, true);
    HandleCanvasInteractions(canvasRect, scale);
    RenderCanvasContextMenu(canvasRect, scale);

    if (ImGui::BeginDragDropTargetCustom(availableRect, ImGui::GetID("ui-canvas-drop-target"))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PX_UI_COMPONENT_TYPE")) {
            const ComponentType type = static_cast<ComponentType>(*static_cast<const int*>(payload->Data));
            AddComponent(type, ScreenToDesign(canvasRect, scale, ImGui::GetMousePos()));
        }

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")) {
            std::string asset(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
            ApplyAssetToSelection(asset);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();
}

void UIDesigner::RenderCanvasContents(const ImRect& rect, float deltaSeconds, bool interactive) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(rect.Min, rect.Max, IM_COL32(10, 14, 24, 255), 20.0f);
    drawList->AddRectFilledMultiColor(
        rect.Min,
        rect.Max,
        IM_COL32(16, 22, 38, 255),
        IM_COL32(22, 30, 50, 255),
        IM_COL32(8, 12, 18, 255),
        IM_COL32(18, 24, 34, 255));
    drawList->AddRect(rect.Min, rect.Max, IM_COL32(97, 128, 168, 180), 20.0f, ImDrawFlags_RoundCornersAll, 1.5f);

    if (m_showGrid) {
        const float minor = rect.GetWidth() / 16.0f;
        for (float x = rect.Min.x; x <= rect.Max.x; x += minor) {
            drawList->AddLine(ImVec2(x, rect.Min.y), ImVec2(x, rect.Max.y), IM_COL32(255, 255, 255, 12));
        }
        for (float y = rect.Min.y; y <= rect.Max.y; y += minor) {
            drawList->AddLine(ImVec2(rect.Min.x, y), ImVec2(rect.Max.x, y), IM_COL32(255, 255, 255, 10));
        }
    }

    drawList->AddText(ImVec2(rect.Min.x + 16.0f, rect.Min.y + 14.0f), IM_COL32(238, 242, 248, 255), "Live UI Composition");
    drawList->AddText(ImVec2(rect.Min.x + 16.0f, rect.Min.y + 34.0f), IM_COL32(156, 170, 188, 255), "1280 x 720 design stage");

    const float scale = rect.GetWidth() / m_designSize.x;
    for (const auto& component : m_components) {
        if (!component.visible) {
            continue;
        }
        RenderComponent(component, rect, scale, deltaSeconds, interactive && component.id == m_selectedId);
    }
}

void UIDesigner::RenderComponent(const UIComponent& component, const ImRect& rect, float scale, float deltaSeconds, bool selected) const {
    const ImRect componentRect = ComponentRect(component, rect, scale, deltaSeconds);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 fill = ComponentFill(component, 1.0f);
    const ImU32 border = selected ? IM_COL32(255, 214, 143, 255) : IM_COL32(108, 132, 166, 255);
    const ImU32 textColor = IM_COL32(238, 242, 248, 255);

    switch (component.type) {
        case ComponentType::Panel:
            drawList->AddRectFilled(componentRect.Min, componentRect.Max, fill, 18.0f);
            drawList->AddRect(componentRect.Min, componentRect.Max, border, 18.0f, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.0f);
            break;
        case ComponentType::Label:
            drawList->AddText(nullptr, component.fontSize * scale, componentRect.Min, textColor, component.text.c_str());
            break;
        case ComponentType::Button:
            drawList->AddRectFilled(componentRect.Min, componentRect.Max, fill, 16.0f);
            drawList->AddRect(componentRect.Min, componentRect.Max, border, 16.0f, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.0f);
            drawList->AddText(
                nullptr,
                component.fontSize * scale,
                ImVec2(componentRect.Min.x + 18.0f * scale, componentRect.Min.y + (componentRect.GetHeight() - component.fontSize * scale) * 0.5f),
                textColor,
                component.text.c_str());
            break;
        case ComponentType::Image:
            drawList->AddRectFilled(componentRect.Min, componentRect.Max, IM_COL32(28, 38, 58, 240), 20.0f);
            drawList->AddRect(componentRect.Min, componentRect.Max, border, 20.0f, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.0f);
            drawList->AddLine(componentRect.Min, componentRect.Max, IM_COL32(130, 170, 220, 110), 1.0f);
            drawList->AddLine(ImVec2(componentRect.Min.x, componentRect.Max.y), ImVec2(componentRect.Max.x, componentRect.Min.y), IM_COL32(130, 170, 220, 110), 1.0f);
            drawList->AddText(ImVec2(componentRect.Min.x + 14.0f, componentRect.Min.y + 14.0f), textColor, component.name.c_str());
            drawList->AddText(ImVec2(componentRect.Min.x + 14.0f, componentRect.Min.y + 36.0f), IM_COL32(162, 176, 194, 255), component.assetPath.empty() ? "Drop image asset" : component.assetPath.c_str());
            break;
        case ComponentType::ChoiceStrip: {
            drawList->AddRectFilled(componentRect.Min, componentRect.Max, fill, 18.0f);
            drawList->AddRect(componentRect.Min, componentRect.Max, border, 18.0f, ImDrawFlags_RoundCornersAll, selected ? 2.0f : 1.0f);
            const float gap = 12.0f * scale;
            const float buttonWidth = (componentRect.GetWidth() - gap * 4.0f) / 3.0f;
            const float buttonHeight = componentRect.GetHeight() - gap * 2.0f;
            std::array<const char*, 3> options{"Continue", "Archive", "Exit"};
            for (int index = 0; index < 3; ++index) {
                const ImVec2 min(componentRect.Min.x + gap + index * (buttonWidth + gap), componentRect.Min.y + gap);
                const ImVec2 max(min.x + buttonWidth, min.y + buttonHeight);
                drawList->AddRectFilled(min, max, IM_COL32(20, 28, 42, 220), 12.0f);
                drawList->AddRect(min, max, IM_COL32(120, 146, 182, 220), 12.0f);
                drawList->AddText(ImVec2(min.x + 16.0f * scale, min.y + 16.0f * scale), textColor, options[index]);
            }
            break;
        }
    }

    if (selected && component.type != ComponentType::Label) {
        drawList->AddRect(
            componentRect.Min - ImVec2(3.0f, 3.0f),
            componentRect.Max + ImVec2(3.0f, 3.0f),
            IM_COL32(255, 214, 143, 190),
            20.0f,
            ImDrawFlags_RoundCornersAll,
            1.5f);
    }
}

void UIDesigner::HandleCanvasInteractions(const ImRect& rect, float scale) {
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool insideCanvas = rect.Contains(mouse);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && insideCanvas) {
        m_draggingId = 0;
        for (auto it = m_components.rbegin(); it != m_components.rend(); ++it) {
            if (ComponentRect(*it, rect, scale, 0.0f).Contains(mouse)) {
                m_selectedId = it->id;
                m_draggingId = it->id;
                const ImVec2 designPoint = ScreenToDesign(rect, scale, mouse);
                m_dragOffset = ImVec2(designPoint.x - it->position.x, designPoint.y - it->position.y);
                break;
            }
        }

        if (m_draggingId == 0) {
            m_selectedId = 0;
        }
    }

    if (m_draggingId != 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (UIComponent* selected = FindComponent(m_draggingId)) {
            const ImVec2 designPoint = ScreenToDesign(rect, scale, mouse);
            selected->position.x = std::clamp(designPoint.x - m_dragOffset.x, 0.0f, m_designSize.x - selected->size.x);
            selected->position.y = std::clamp(designPoint.y - m_dragOffset.y, 0.0f, m_designSize.y - selected->size.y);
        }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_draggingId = 0;
    }
}

void UIDesigner::RenderCanvasContextMenu(const ImRect& rect, float scale) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && rect.Contains(ImGui::GetMousePos())) {
        ImGui::OpenPopup("UI Canvas Context");
    }

    if (ImGui::BeginPopup("UI Canvas Context")) {
        const ImVec2 dropPoint = ScreenToDesign(rect, scale, ImGui::GetMousePos());
        if (ImGui::MenuItem("Add Panel")) {
            AddComponent(ComponentType::Panel, dropPoint);
        }
        if (ImGui::MenuItem("Add Label")) {
            AddComponent(ComponentType::Label, dropPoint);
        }
        if (ImGui::MenuItem("Add Button")) {
            AddComponent(ComponentType::Button, dropPoint);
        }
        if (ImGui::MenuItem("Add Image")) {
            AddComponent(ComponentType::Image, dropPoint);
        }
        if (ImGui::MenuItem("Add Choice Strip")) {
            AddComponent(ComponentType::ChoiceStrip, dropPoint);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Selection", nullptr, false, m_selectedId != 0)) {
            DeleteSelectedComponent();
        }
        ImGui::EndPopup();
    }
}

UIDesigner::UIComponent* UIDesigner::AddComponent(ComponentType type, const ImVec2& designPosition) {
    UIComponent component;
    component.id = m_nextId++;
    component.type = type;
    component.position = designPosition;
    component.name = ComponentLabel(type);

    switch (type) {
        case ComponentType::Panel:
            component.size = ImVec2(360.0f, 180.0f);
            component.text = "Panel";
            component.tint = ImVec4(0.10f, 0.14f, 0.22f, 0.78f);
            break;
        case ComponentType::Label:
            component.size = ImVec2(320.0f, 48.0f);
            component.text = "Story Title";
            component.fontSize = 34;
            component.tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            break;
        case ComponentType::Button:
            component.size = ImVec2(220.0f, 64.0f);
            component.text = "Click Me";
            component.tint = ImVec4(0.19f, 0.28f, 0.44f, 0.92f);
            break;
        case ComponentType::Image:
            component.size = ImVec2(320.0f, 220.0f);
            component.text = "Image";
            component.tint = ImVec4(0.12f, 0.16f, 0.22f, 1.0f);
            break;
        case ComponentType::ChoiceStrip:
            component.size = ImVec2(460.0f, 96.0f);
            component.text = "Choice";
            component.tint = ImVec4(0.07f, 0.10f, 0.15f, 0.94f);
            break;
    }

    m_components.push_back(component);
    m_selectedId = component.id;
    Log("Added UI component: " + ComponentLabel(type));
    return &m_components.back();
}

UIDesigner::UIComponent* UIDesigner::FindComponent(int id) {
    auto it = std::find_if(m_components.begin(), m_components.end(), [&](const UIComponent& component) {
        return component.id == id;
    });
    return it == m_components.end() ? nullptr : &(*it);
}

const UIDesigner::UIComponent* UIDesigner::FindComponent(int id) const {
    auto it = std::find_if(m_components.begin(), m_components.end(), [&](const UIComponent& component) {
        return component.id == id;
    });
    return it == m_components.end() ? nullptr : &(*it);
}

void UIDesigner::DeleteSelectedComponent() {
    if (m_selectedId == 0) {
        return;
    }

    m_components.erase(std::remove_if(m_components.begin(), m_components.end(), [&](const UIComponent& component) {
        return component.id == m_selectedId;
    }), m_components.end());
    m_selectedId = 0;
}

void UIDesigner::CenterSelectedComponent() {
    if (UIComponent* selected = FindComponent(m_selectedId)) {
        selected->position = ImVec2(
            (m_designSize.x - selected->size.x) * 0.5f,
            (m_designSize.y - selected->size.y) * 0.5f);
    }
}

std::string UIDesigner::ComponentLabel(ComponentType type) const {
    switch (type) {
        case ComponentType::Panel:
            return "Panel";
        case ComponentType::Label:
            return "Label";
        case ComponentType::Button:
            return "Button";
        case ComponentType::Image:
            return "Image";
        case ComponentType::ChoiceStrip:
            return "Choice Strip";
    }
    return "Component";
}

ImU32 UIDesigner::ComponentFill(const UIComponent& component, float alphaMultiplier) const {
    const float alpha = std::clamp(component.tint.w * alphaMultiplier, 0.0f, 1.0f);
    return ImColor(component.tint.x, component.tint.y, component.tint.z, alpha);
}

ImRect UIDesigner::ComponentRect(const UIComponent& component, const ImRect& canvasRect, float scale, float deltaSeconds) const {
    ImVec2 offset(0.0f, 0.0f);
    float alphaMultiplier = 1.0f;
    if (m_previewAnimation) {
        const float time = static_cast<float>(ImGui::GetTime()) + component.motion.delay;
        const float wave = 0.5f + 0.5f * std::sin(time * (2.4f / std::max(component.motion.duration, 0.01f)));
        if (component.motion.kind == "slide") {
            offset.x = -(1.0f - wave) * component.motion.amplitude;
        } else if (component.motion.kind == "lift") {
            offset.y = -(wave - 0.5f) * component.motion.amplitude * 0.35f;
        } else if (component.motion.kind == "fade") {
            alphaMultiplier = 0.55f + 0.45f * wave;
        }
        (void)alphaMultiplier;
    }

    const ImVec2 min = DesignToScreen(canvasRect, scale, component.position + offset);
    const ImVec2 max = DesignToScreen(canvasRect, scale, component.position + offset + component.size);
    return ImRect(min, max);
}

ImVec2 UIDesigner::ScreenToDesign(const ImRect& rect, float scale, const ImVec2& screenPoint) const {
    return ImVec2(
        std::clamp((screenPoint.x - rect.Min.x) / scale, 0.0f, m_designSize.x),
        std::clamp((screenPoint.y - rect.Min.y) / scale, 0.0f, m_designSize.y));
}

ImVec2 UIDesigner::DesignToScreen(const ImRect& rect, float scale, const ImVec2& designPoint) const {
    return ImVec2(
        rect.Min.x + designPoint.x * scale,
        rect.Min.y + designPoint.y * scale);
}

void UIDesigner::RenderInspector() {
    if (UIComponent* selected = FindComponent(m_selectedId)) {
        ImGui::TextUnformatted(selected->name.c_str());
        ImGui::TextDisabled("%s", ComponentLabel(selected->type).c_str());
        ImGui::Separator();

        ImGui::InputText("Name", &selected->name);
        ImGui::InputText("Text", &selected->text);
        RenderAssetPathEditor(selected->assetPath, "selected-component");
        ImGui::DragFloat2("Position", &selected->position.x, 1.0f, 0.0f, 2000.0f, "%.0f");
        ImGui::DragFloat2("Size", &selected->size.x, 1.0f, 16.0f, 2000.0f, "%.0f");
        ImGui::DragInt("Font Size", &selected->fontSize, 1.0f, 10, 96);
        ImGui::Checkbox("Visible", &selected->visible);
        ImGui::ColorEdit4("Tint", &selected->tint.x, ImGuiColorEditFlags_AlphaBar);

        const std::array<const char*, 3> layouts{"None", "Vertical", "Horizontal"};
        int layoutIndex = selected->layout == "Vertical" ? 1 : (selected->layout == "Horizontal" ? 2 : 0);
        if (ImGui::Combo("Layout", &layoutIndex, layouts.data(), static_cast<int>(layouts.size()))) {
            selected->layout = layouts[layoutIndex];
        }
        ImGui::DragFloat("Spacing", &selected->spacing, 0.25f, 0.0f, 64.0f, "%.1f");

        const std::array<const char*, 4> motions{"fade", "slide", "lift", "pulse"};
        int motionIndex = 0;
        for (int index = 0; index < static_cast<int>(motions.size()); ++index) {
            if (selected->motion.kind == motions[index]) {
                motionIndex = index;
                break;
            }
        }
        if (ImGui::Combo("Motion", &motionIndex, motions.data(), static_cast<int>(motions.size()))) {
            selected->motion.kind = motions[motionIndex];
        }
        ImGui::DragFloat("Duration", &selected->motion.duration, 0.01f, 0.05f, 5.0f, "%.2f");
        ImGui::DragFloat("Delay", &selected->motion.delay, 0.01f, 0.0f, 5.0f, "%.2f");
        ImGui::DragFloat("Amplitude", &selected->motion.amplitude, 1.0f, 0.0f, 400.0f, "%.0f");

        return;
    }

    ImGui::TextDisabled("Select a component on the canvas, or drop an image asset to create one.");
}

void UIDesigner::RenderPreview() {
    ImGui::BeginChild("ui-preview", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("ui-preview-surface", avail);
    const ImRect outer(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const float scale = std::min(outer.GetWidth() / m_designSize.x, outer.GetHeight() / m_designSize.y);
    const ImVec2 previewSize(m_designSize.x * scale, m_designSize.y * scale);
    const ImVec2 min(
        outer.Min.x + (outer.GetWidth() - previewSize.x) * 0.5f,
        outer.Min.y + (outer.GetHeight() - previewSize.y) * 0.5f);
    RenderCanvasContents(ImRect(min, min + previewSize), 0.0f, false);
    ImGui::EndChild();
}

void UIDesigner::ApplyAssetToSelection(const std::string& assetPath) {
    if (UIComponent* selected = FindComponent(m_selectedId)) {
        selected->assetPath = assetPath;
        if (selected->type != ComponentType::Image) {
            selected->type = ComponentType::Image;
            selected->name = "image_component";
        }
        Log("Applied asset to UI selection: " + assetPath);
        return;
    }

    if (UIComponent* image = AddComponent(ComponentType::Image, ImVec2((m_designSize.x - 320.0f) * 0.5f, (m_designSize.y - 220.0f) * 0.5f))) {
        image->assetPath = assetPath;
        Log("Created image component from explorer asset: " + assetPath);
    }
}

void UIDesigner::RenderAssetPathEditor(std::string& assetPath, std::string_view idSuffix) {
    const std::string fieldId = "##asset-path-" + std::string(idSuffix);
    ImGui::TextUnformatted("Asset Path");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(fieldId.c_str(), &assetPath, ImGuiInputTextFlags_ReadOnly);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PX_RESOURCE_PATH")) {
            assetPath.assign(static_cast<const char*>(payload->Data), payload->DataSize > 0 ? payload->DataSize - 1 : 0);
        }
        ImGui::EndDragDropTarget();
    }

    const std::string selectedResource = m_selectedResourceCallback ? m_selectedResourceCallback() : std::string{};
    const bool hasSelectedResource = !selectedResource.empty();
    if (!assetPath.empty()) {
        ImGui::TextDisabled("Bound from project browser.");
    } else {
        ImGui::TextDisabled("Pick from the project browser or drag a resource here.");
    }

    if (!hasSelectedResource) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(("Use Explorer Selection##" + std::string(idSuffix)).c_str())) {
        assetPath = selectedResource;
    }
    if (!hasSelectedResource) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(("Clear##" + std::string(idSuffix)).c_str())) {
        assetPath.clear();
    }
}

bool UIDesigner::HasSelection() const {
    return m_selectedId != 0;
}

std::string UIDesigner::GetSelectionSummary() const {
    if (const UIComponent* component = FindComponent(m_selectedId)) {
        return component->name + " (" + ComponentLabel(component->type) + ")";
    }
    return "Nothing selected";
}

std::string UIDesigner::GenerateLua() const {
    auto to255 = [](float value) {
        return static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };

    std::ostringstream lua;
    lua << "-- Generated by PrismatiXEditor UI Designer\n";
    lua << "local UI = _G.UI\n\n";
    lua << "local function build_generated_ui()\n";
    lua << "    local root = UI.Panel({ id = \"root\", x = 0, y = 0, w = 1280, h = 720, color = { 0, 0, 0, 0 } })\n\n";

    for (const auto& component : m_components) {
        const std::string symbol = "component_" + std::to_string(component.id);
        const int red = to255(component.tint.x);
        const int green = to255(component.tint.y);
        const int blue = to255(component.tint.z);
        const int alpha = to255(component.tint.w);

        if (component.type == ComponentType::Panel) {
            lua << "    local " << symbol << " = UI.Panel({ id = " << NormalizeLuaString(component.name)
                << ", x = " << static_cast<int>(component.position.x)
                << ", y = " << static_cast<int>(component.position.y)
                << ", w = " << static_cast<int>(component.size.x)
                << ", h = " << static_cast<int>(component.size.y)
                << ", layout = " << NormalizeLuaString(component.layout)
                << ", spacing = " << component.spacing
                << ", color = { " << red << ", " << green << ", " << blue << ", " << alpha << " } })\n";
        } else if (component.type == ComponentType::Label) {
            lua << "    local " << symbol << " = UI.Label({ id = " << NormalizeLuaString(component.name)
                << ", x = " << static_cast<int>(component.position.x)
                << ", y = " << static_cast<int>(component.position.y)
                << ", w = " << static_cast<int>(component.size.x)
                << ", h = " << static_cast<int>(component.size.y)
                << ", text = " << NormalizeLuaString(component.text)
                << ", fontSize = " << component.fontSize
                << ", color = { " << red << ", " << green << ", " << blue << " } })\n";
        } else if (component.type == ComponentType::Button) {
            lua << "    local " << symbol << " = UI.Button({ id = " << NormalizeLuaString(component.name)
                << ", x = " << static_cast<int>(component.position.x)
                << ", y = " << static_cast<int>(component.position.y)
                << ", w = " << static_cast<int>(component.size.x)
                << ", h = " << static_cast<int>(component.size.y)
                << ", text = " << NormalizeLuaString(component.text)
                << ", fontSize = " << component.fontSize
                << ", bgColor = { " << red << ", " << green << ", " << blue << ", " << alpha << " } })\n";
        } else if (component.type == ComponentType::Image) {
            lua << "    local " << symbol << " = UI.Panel({ id = " << NormalizeLuaString(component.name)
                << ", x = " << static_cast<int>(component.position.x)
                << ", y = " << static_cast<int>(component.position.y)
                << ", w = " << static_cast<int>(component.size.x)
                << ", h = " << static_cast<int>(component.size.y)
                << ", color = { " << red << ", " << green << ", " << blue << ", " << alpha << " } })\n";
            lua << "    " << symbol << ".asset = " << NormalizeLuaString(component.assetPath) << "\n";
            lua << "    -- TODO: bind " << symbol << ".asset to your actual image widget/render helper.\n";
        } else if (component.type == ComponentType::ChoiceStrip) {
            lua << "    local " << symbol << " = UI.Panel({ id = " << NormalizeLuaString(component.name)
                << ", x = " << static_cast<int>(component.position.x)
                << ", y = " << static_cast<int>(component.position.y)
                << ", w = " << static_cast<int>(component.size.x)
                << ", h = " << static_cast<int>(component.size.y)
                << ", layout = \"Horizontal\", spacing = 12, color = { " << red << ", " << green << ", " << blue << ", " << alpha << " } })\n";
            lua << "    " << symbol << ":add_child(UI.Button({ text = \"Continue\", w = 140, h = 64 }))\n";
            lua << "    " << symbol << ":add_child(UI.Button({ text = \"Archive\", w = 140, h = 64 }))\n";
            lua << "    " << symbol << ":add_child(UI.Button({ text = \"Exit\", w = 140, h = 64 }))\n";
        }

        lua << "    root:add_child(" << symbol << ")\n";
        lua << "    -- Motion preset: " << component.motion.kind << " duration=" << component.motion.duration
            << " delay=" << component.motion.delay << " amplitude=" << component.motion.amplitude << "\n\n";
    }

    lua << "    return root\n";
    lua << "end\n\n";
    lua << "return {\n";
    lua << "    build = build_generated_ui,\n";
    lua << "}\n";
    return lua.str();
}

std::vector<std::string> UIDesigner::BuildPreviewLines() const {
    std::vector<std::string> lines;
    lines.push_back("Components: " + std::to_string(m_components.size()));

    auto it = std::find_if(m_components.begin(), m_components.end(), [&](const UIComponent& component) {
        return component.type == ComponentType::Button;
    });
    if (it != m_components.end()) {
        lines.push_back("Primary CTA: " + it->text);
    }

    it = std::find_if(m_components.begin(), m_components.end(), [&](const UIComponent& component) {
        return component.type == ComponentType::Image;
    });
    if (it != m_components.end()) {
        lines.push_back("Hero Art: " + (it->assetPath.empty() ? std::string("Drop asset") : it->assetPath));
    }

    if (const UIComponent* selected = FindComponent(m_selectedId)) {
        lines.push_back("Selection: " + selected->name);
        lines.push_back("Motion: " + selected->motion.kind);
    }

    return lines;
}

std::string UIDesigner::NormalizeLuaString(const std::string& value) const {
    std::string sanitized;
    sanitized.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                sanitized += "\\\\";
                break;
            case '"':
                sanitized += "\\\"";
                break;
            case '\n':
                sanitized += "\\n";
                break;
            case '\r':
                break;
            default:
                sanitized += ch;
                break;
        }
    }
    return "\"" + sanitized + "\"";
}

void UIDesigner::Log(const std::string& message) const {
    if (m_logCallback) {
        m_logCallback(message);
    }
}

}  // namespace PrismatiX::Editor
