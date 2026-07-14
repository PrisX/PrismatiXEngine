#include "Editor/Tools/UIDesigner/Preview/PreviewFixture.h"

#include <algorithm>
#include <cmath>

namespace px::editor {
namespace {

diag::Diagnostic Error(std::string code, std::string message, std::string details = {}) {
    return {.severity = diag::Severity::Error,
            .code = std::move(code),
            .category = "Editor.UIPreview",
            .message = std::move(message),
            .details = std::move(details)};
}

}  // namespace

PreviewFixture::PreviewFixture() {
    (void)m_viewModel.Define("game.locale", Variant(m_context.locale), true);
    (void)m_viewModel.Define("game.uiScale", Variant(static_cast<double>(m_context.uiScale)), true);
    (void)m_viewModel.Define("game.dialogue.speaker", Variant(std::string("Illya")), true);
    (void)m_viewModel.Define("game.dialogue.text",
                             Variant(std::string("Preview fixture text / 預覽文字")), true);
    (void)m_viewModel.Define("game.canContinue", Variant(true), true);
    (void)m_viewModel.Define("game.progress", Variant(0.65), true);
}

const std::vector<PreviewDevicePreset>& PreviewFixture::DevicePresets() {
    static const std::vector<PreviewDevicePreset> presets{
        {"hd-720", "1280 × 720", 1280, 720, 1.0f, {0, 0, 1280, 720}},
        {"full-hd", "1920 × 1080", 1920, 1080, 1.0f, {0, 0, 1920, 1080}},
        {"wuxga", "16:10 · 1920 × 1200", 1920, 1200, 1.0f, {0, 0, 1920, 1200}},
        {"ultrawide", "21:9 · 2560 × 1080", 2560, 1080, 1.0f, {0, 0, 2560, 1080}},
        {"high-dpi", "1280 × 720 · 2× DPI", 1280, 720, 2.0f, {0, 0, 1280, 720}},
        {"custom", "Custom", 1280, 720, 1.0f, {0, 0, 1280, 720}},
    };
    return presets;
}

Status PreviewFixture::SelectDevice(std::string_view id) {
    const auto& presets = DevicePresets();
    const auto found = std::find_if(presets.begin(), presets.end(),
                                    [&](const auto& preset) { return preset.id == id; });
    if (found == presets.end()) {
        return Status::Fail(Error("PXEDPREV5101", "Unknown preview device preset",
                                  std::string(id)));
    }
    m_context.device = found->id;
    if (found->id != "custom") {
        m_context.width = found->width;
        m_context.height = found->height;
        m_context.dpiScale = found->dpiScale;
        m_context.safeArea = found->safeArea;
    }
    return Status::Ok();
}

Status PreviewFixture::SetLocale(std::string locale) {
    if (locale.empty()) {
        return Status::Fail(Error("PXEDPREV5102", "Preview locale cannot be empty"));
    }
    m_context.locale = std::move(locale);
    return m_viewModel.Write("game.locale", Variant(m_context.locale));
}

Status PreviewFixture::SetUIScale(float scale) {
    if (!std::isfinite(scale) || scale < 0.5f || scale > 4.0f) {
        return Status::Fail(Error("PXEDPREV5103", "Preview UI scale must be between 0.5 and 4"));
    }
    m_context.uiScale = scale;
    return m_viewModel.Write("game.uiScale", Variant(static_cast<double>(scale)));
}

Status PreviewFixture::Define(std::string path, Variant value, bool writable) {
    return m_viewModel.Define(std::move(path), std::move(value), writable);
}

Status PreviewFixture::SetValue(std::string_view path, const Variant& value) {
    return m_viewModel.Write(path, value);
}

Result<Variant> PreviewFixture::Read(std::string_view path) const {
    return m_viewModel.Read(path);
}

}  // namespace px::editor
