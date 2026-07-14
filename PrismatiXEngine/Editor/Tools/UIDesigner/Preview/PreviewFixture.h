#pragma once

#include "Engine/UI/Binding.h"

#include <string>
#include <vector>

namespace px::editor {

struct PreviewDevicePreset {
    std::string id;
    std::string displayName;
    int width = 1280;
    int height = 720;
    float dpiScale = 1.0f;
    Rect safeArea{};
};

struct DesignerPreviewContext {
    std::string device = "hd-720";
    int width = 1280;
    int height = 720;
    std::string locale = "zh-TW";
    float uiScale = 1.0f;
    float dpiScale = 1.0f;
    Rect safeArea{};
    std::string inputMode = "MouseKeyboard";
    std::string forcedVisualState = "Normal";
    bool safeMode = true;
};

class PreviewFixture {
public:
    PreviewFixture();

    [[nodiscard]] static const std::vector<PreviewDevicePreset>& DevicePresets();
    [[nodiscard]] const DesignerPreviewContext& Context() const { return m_context; }
    [[nodiscard]] DesignerPreviewContext& Context() { return m_context; }
    [[nodiscard]] const ui::ObservableViewModel& ViewModel() const { return m_viewModel; }
    [[nodiscard]] ui::ObservableViewModel& ViewModel() { return m_viewModel; }

    Status SelectDevice(std::string_view id);
    Status SetLocale(std::string locale);
    Status SetUIScale(float scale);
    Status Define(std::string path, Variant value, bool writable = true);
    Status SetValue(std::string_view path, const Variant& value);
    [[nodiscard]] Result<Variant> Read(std::string_view path) const;

private:
    DesignerPreviewContext m_context;
    ui::ObservableViewModel m_viewModel;
};

}  // namespace px::editor
