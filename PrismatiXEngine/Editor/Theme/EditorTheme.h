#pragma once

#include <imgui.h>

namespace px::editor {

struct EditorColorTokens {
    ImVec4 background;
    ImVec4 surfacePrimary;
    ImVec4 surfaceSecondary;
    ImVec4 surfaceRaised;
    ImVec4 surfaceOverlay;
    ImVec4 borderSubtle;
    ImVec4 borderDefault;
    ImVec4 borderStrong;
    ImVec4 textPrimary;
    ImVec4 textSecondary;
    ImVec4 textMuted;
    ImVec4 textDisabled;
    ImVec4 textInverse;
    ImVec4 accentDefault;
    ImVec4 accentHover;
    ImVec4 accentActive;
    ImVec4 accentSubtle;
    ImVec4 selectionBackground;
    ImVec4 selectionBorder;
    ImVec4 selectionText;
    ImVec4 focusRing;
    ImVec4 success;
    ImVec4 warning;
    ImVec4 error;
    ImVec4 info;
    ImVec4 canvasBackground;
    ImVec4 canvasGridMinor;
    ImVec4 canvasGridMajor;
    ImVec4 canvasGuide;
    ImVec4 canvasSelection;
    ImVec4 canvasHandle;
    ImVec4 canvasAnchor;
};

struct EditorMetricTokens {
    float space2 = 2.0f;
    float space4 = 4.0f;
    float space6 = 6.0f;
    float space8 = 8.0f;
    float space12 = 12.0f;
    float space16 = 16.0f;
    float space24 = 24.0f;
    float radius = 4.0f;
    float modalRadius = 6.0f;
    float toolbarHeight = 30.0f;
    float treeRowHeight = 26.0f;
    float propertyRowHeight = 28.0f;
    float focusRing = 2.0f;
};

struct EditorThemeTokens {
    EditorColorTokens colors;
    EditorMetricTokens metrics;
};

[[nodiscard]] const EditorThemeTokens& EditorTheme();
[[nodiscard]] ImVec4 WithAlpha(ImVec4 color, float alpha);

}  // namespace px::editor
