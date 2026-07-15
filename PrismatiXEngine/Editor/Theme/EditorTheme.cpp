#include "Editor/Theme/EditorTheme.h"

namespace px::editor {
namespace {

constexpr ImVec4 Color(unsigned int rgb, float alpha = 1.0f) {
    return {static_cast<float>((rgb >> 16u) & 0xffu) / 255.0f,
            static_cast<float>((rgb >> 8u) & 0xffu) / 255.0f,
            static_cast<float>(rgb & 0xffu) / 255.0f, alpha};
}

}  // namespace

const EditorThemeTokens& EditorTheme() {
    static const EditorThemeTokens tokens{
        .colors = {.background = Color(0x141820),
                   .surfacePrimary = Color(0x20252d),
                   .surfaceSecondary = Color(0x292f39),
                   .surfaceRaised = Color(0x343b47),
                   .surfaceOverlay = Color(0x171b22, 0.96f),
                   .borderSubtle = Color(0x292e38),
                   .borderDefault = Color(0x3c4552),
                   .borderStrong = Color(0x596778),
                   .textPrimary = Color(0xd8dee9),
                   .textSecondary = Color(0xb6bfcc),
                   .textMuted = Color(0x8d98a8),
                   .textDisabled = Color(0x68717e),
                   .textInverse = Color(0x10141a),
                   .accentDefault = Color(0x478cbf),
                   .accentHover = Color(0x4f9ad1),
                   .accentActive = Color(0x69ade0),
                   .accentSubtle = Color(0x335f82),
                   .selectionBackground = Color(0x284d68),
                   .selectionBorder = Color(0x6eb6e6),
                   .selectionText = Color(0xeef8ff),
                   .focusRing = Color(0x8bcbff),
                   .success = Color(0x55c982),
                   .warning = Color(0xe2ad56),
                   .error = Color(0xf06468),
                   .info = Color(0x64a9df),
                   .canvasBackground = Color(0x171b22),
                   .canvasGridMinor = Color(0x39414d, 0.4f),
                   .canvasGridMajor = Color(0x566273, 0.6f),
                   .canvasGuide = Color(0x42d3c8),
                   .canvasSelection = Color(0x6eb6e6),
                   .canvasHandle = Color(0xeaf8ff),
                   .canvasAnchor = Color(0xf078be)}};
    return tokens;
}

ImVec4 WithAlpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

}  // namespace px::editor
