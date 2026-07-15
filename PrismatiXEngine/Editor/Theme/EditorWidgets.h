#pragma once

#include "Editor/Theme/EditorTheme.h"

#include <functional>
#include <string>
#include <string_view>

namespace px::editor::widgets {

enum class MessageKind { Info, Success, Warning, Error };

bool SegmentedButton(const char* label, bool selected,
                     const ImVec2& size = {0, 0});
bool ToolbarButton(const char* label, const char* tooltip, bool selected = false,
                   bool enabled = true, const char* disabledReason = nullptr,
                   const ImVec2& size = {0, 0});
bool SearchField(const char* id, const char* hint, std::string& value);
bool SearchField(const char* id, const char* hint, char* value,
                 std::size_t capacity);
void StatusChip(const char* label, MessageKind kind = MessageKind::Info);
void InlineMessage(MessageKind kind, std::string_view text,
                   std::string_view actionLabel = {},
                   const std::function<void()>& action = {});
void EmptyState(std::string_view title, std::string_view description,
                std::string_view actionLabel = {},
                const std::function<void()>& action = {});
bool Section(const char* id, const char* label, bool defaultOpen = true,
             const char* badge = nullptr);
void Tooltip(const char* text, const char* shortcut = nullptr,
             const char* disabledReason = nullptr);

}  // namespace px::editor::widgets
