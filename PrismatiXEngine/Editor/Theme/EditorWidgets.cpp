#include "Editor/Theme/EditorWidgets.h"

#include <imgui_stdlib.h>

#include <algorithm>

namespace px::editor::widgets {
namespace {

ImVec4 MessageColor(MessageKind kind) {
    const auto& colors = EditorTheme().colors;
    switch (kind) {
        case MessageKind::Success: return colors.success;
        case MessageKind::Warning: return colors.warning;
        case MessageKind::Error: return colors.error;
        case MessageKind::Info: return colors.info;
    }
    return colors.info;
}

}  // namespace

bool SegmentedButton(const char* label, bool selected, const ImVec2& size) {
    const auto& colors = EditorTheme().colors;
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.selectionBackground);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.accentSubtle);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.selectionText);
    }
    const bool pressed = ImGui::Button(label, size);
    if (selected) ImGui::PopStyleColor(3);
    return pressed;
}

bool ToolbarButton(const char* label, const char* tooltip, bool selected,
                   bool enabled, const char* disabledReason, const ImVec2& size) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = SegmentedButton(label, selected, size);
    ImGui::EndDisabled();
    Tooltip(tooltip, nullptr, enabled ? nullptr : disabledReason);
    return enabled && pressed;
}

bool SearchField(const char* id, const char* hint, std::string& value) {
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::InputTextWithHint(id, hint, &value);
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape) && !value.empty()) {
        value.clear();
        return true;
    }
    return changed;
}

bool SearchField(const char* id, const char* hint, char* value,
                 std::size_t capacity) {
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::InputTextWithHint(id, hint, value, capacity);
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape) && value[0] != 0) {
        value[0] = 0;
        return true;
    }
    return changed;
}

void StatusChip(const char* label, MessageKind kind) {
    const auto color = MessageColor(kind);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("● %s", label);
    ImGui::PopStyleColor();
}

void InlineMessage(MessageKind kind, std::string_view text,
                   std::string_view actionLabel,
                   const std::function<void()>& action) {
    const auto& theme = EditorTheme();
    // This helper may be emitted more than once in the same parent window. Scope
    // its fixed child/button labels by semantic content so each message owns a
    // stable ImGui ID instead of every instance sharing "##inline-message".
    ImGui::PushID(static_cast<int>(kind));
    const char* textBegin = text.empty() ? "" : text.data();
    ImGui::PushID(textBegin, textBegin + text.size());
    const char* actionBegin = actionLabel.empty() ? "" : actionLabel.data();
    ImGui::PushID(actionBegin, actionBegin + actionLabel.size());
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          WithAlpha(MessageColor(kind), 0.10f));
    ImGui::PushStyleColor(ImGuiCol_Border,
                          WithAlpha(MessageColor(kind), 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme.metrics.radius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme.metrics.space8, theme.metrics.space6));
    const float actionWidth = actionLabel.empty()
                                  ? 0.0f
                                  : ImGui::CalcTextSize(actionLabel.data()).x + 28.0f;
    const float textWidth = std::max(80.0f, ImGui::GetContentRegionAvail().x - actionWidth);
    ImGui::BeginChild("##inline-message", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushStyleColor(ImGuiCol_Text, MessageColor(kind));
    ImGui::TextWrapped("%.*s", static_cast<int>(text.size()), text.data());
    ImGui::PopStyleColor();
    if (!actionLabel.empty() && action) {
        ImGui::SameLine(textWidth);
        if (ImGui::SmallButton(std::string(actionLabel).c_str())) action();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

void EmptyState(std::string_view title, std::string_view description,
                std::string_view actionLabel,
                const std::function<void()>& action) {
    const auto& theme = EditorTheme();
    const char* titleBegin = title.empty() ? "" : title.data();
    const char* descriptionBegin = description.empty() ? "" : description.data();
    ImGui::PushID(titleBegin, titleBegin + title.size());
    ImGui::PushID(descriptionBegin, descriptionBegin + description.size());
    ImGui::Dummy(ImVec2(0, theme.metrics.space16));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.colors.textPrimary);
    ImGui::TextWrapped("%.*s", static_cast<int>(title.size()), title.data());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.colors.textMuted);
    ImGui::TextWrapped("%.*s", static_cast<int>(description.size()),
                       description.data());
    ImGui::PopStyleColor();
    if (!actionLabel.empty() && action &&
        ImGui::Button(std::string(actionLabel).c_str()))
        action();
    ImGui::PopID();
    ImGui::PopID();
}

bool Section(const char* id, const char* label, bool defaultOpen,
             const char* badge) {
    ImGui::PushID(id);
    std::string heading = label;
    if (badge && *badge) heading += std::string("  ") + badge;
    const bool open = ImGui::CollapsingHeader(
        heading.c_str(), defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopID();
    return open;
}

void Tooltip(const char* text, const char* shortcut,
             const char* disabledReason) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ||
        ((!text || !*text) && (!disabledReason || !*disabledReason)))
        return;
    ImGui::BeginTooltip();
    if (text && *text) ImGui::TextUnformatted(text);
    if (shortcut && *shortcut) ImGui::TextDisabled("Shortcut: %s", shortcut);
    if (disabledReason && *disabledReason) {
        ImGui::Separator();
        ImGui::TextColored(EditorTheme().colors.warning, "%s", disabledReason);
    }
    ImGui::EndTooltip();
}

}  // namespace px::editor::widgets
