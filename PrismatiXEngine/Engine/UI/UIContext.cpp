#include "Engine/UI/UIContext.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/Widgets.h"

#include <algorithm>

namespace px::ui {

UIContext::UIContext() = default;

Status UIContext::SetRoot(std::unique_ptr<Control> root) {
    if (!root) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Fatal, .code = "PXUI2401",
                                    .category = "UI.Runtime", .message = "UI root cannot be null"};
        diag::Emit(diagnostic); return Status::Fail(std::move(diagnostic));
    }
    m_animation.reset();m_root = std::move(root); m_input = std::make_unique<InputRouter>(*m_root); WireCommands(*m_root);
    m_animationPlayer=std::make_unique<AnimationPlayer>(*m_root);
    m_width = m_height = 0; return Status::Ok();
}

void UIContext::WireCommands(Control& node) {
    if (auto* button = dynamic_cast<Button*>(&node); button && !button->Command().empty()) {
        const std::string command = button->Command();
        button->SetOnActivated([this, command] { const Status status=m_commands.Execute(command);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic); });
    }
    for (const auto& child : node.Children()) if (auto* control = dynamic_cast<Control*>(child.get())) WireCommands(*control);
}

bool UIContext::Update(const Input& input, int viewportWidth, int viewportHeight,float deltaSeconds) {
    if (!m_root) return false;
    m_root->Update(deltaSeconds);
    if (m_width != viewportWidth || m_height != viewportHeight || m_root->LayoutDirty()) {
        m_width = viewportWidth; m_height = viewportHeight;
        (void)m_root->Measure({static_cast<float>(m_width), static_cast<float>(m_height)});
        m_root->Arrange({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)});
    }
    if (m_input) m_input->Update(input);
    if(m_animationPlayer){const Status status=m_animationPlayer->Update(deltaSeconds);if(!status)for(const auto& diagnostic:status.Diagnostics())diag::Emit(diagnostic);}
    if (m_root->LayoutDirty()) {
        (void)m_root->Measure({static_cast<float>(m_width), static_cast<float>(m_height)});
        m_root->Arrange({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)});
    }
    return m_input && m_input->LastFrameConsumed();
}

Status UIContext::SetAnimation(AnimationClip clip,bool autoplay){m_animation=std::move(clip);if(!m_animationPlayer)return Status::Ok();return autoplay?m_animationPlayer->Play(*m_animation):Status::Ok();}
Status UIContext::PlayAnimation(){if(!m_animation||!m_animationPlayer){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXUI2402",.category="UI.Animation",.message="UI scene has no animation to play"};diag::Emit(d);return Status::Fail(std::move(d));}return m_animationPlayer->Play(*m_animation);}

void UIContext::Render(graphics::Renderer2D& renderer) {
    if (m_root) m_root->Draw(renderer, m_theme);
    if (m_showDiagnostics) RenderDiagnosticOverlay(renderer);
}

void UIContext::RenderDiagnosticOverlay(graphics::Renderer2D& renderer) {
    auto diagnostics = diag::Global().Snapshot();
    std::erase_if(diagnostics, [](const diag::Diagnostic& item) { return item.severity < diag::Severity::Error; });
    if (diagnostics.empty()) return;
    constexpr float width = 560.0f, lineHeight = 30.0f, padding = 16.0f;
    const std::size_t count = std::min<std::size_t>(5, diagnostics.size());
    const float height = padding * 2.0f + lineHeight * static_cast<float>(count + 1);
    const Rect area{std::max(0.0f, static_cast<float>(m_width) - width - 18.0f), 18.0f, width, height};
    renderer.DrawRoundedRect(area, 8.0f, {79, 25, 32, 245});
    renderer.DrawText("PrismatiX diagnostics", area.x + padding, area.y + padding,
                      "Content/Fonts/NotoSansTC-Bold.ttf", 22, {255, 224, 228, 255});
    for (std::size_t i = 0; i < count; ++i) {
        std::string text = diagnostics[diagnostics.size() - count + i].code + "  " + diagnostics[diagnostics.size() - count + i].message;
        if (text.size() > 76) text.resize(73), text += "...";
        renderer.DrawText(text, area.x + padding, area.y + padding + lineHeight * static_cast<float>(i + 1),
                          "Content/Fonts/NotoSansTC-Bold.ttf", 18, {255, 238, 240, 255});
    }
}

}  // namespace px::ui
