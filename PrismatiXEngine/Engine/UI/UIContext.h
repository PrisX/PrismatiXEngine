#pragma once

#include "Engine/UI/Binding.h"
#include "Engine/UI/InputRouter.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/Animation.h"

#include <memory>
#include <optional>
#include <vector>

namespace px { class Input; }
namespace px::graphics { class Renderer2D; }

namespace px::ui {

class UIContext {
public:
    UIContext();
    Status SetRoot(std::unique_ptr<Control> root);
    [[nodiscard]] Control* Root() const { return m_root.get(); }
    [[nodiscard]] Theme& ActiveTheme() { return m_theme; }
    void SetTheme(Theme theme){m_theme=std::move(theme);if(m_root)m_root->InvalidateLayout();}
    [[nodiscard]] CommandRegistry& Commands() { return m_commands; }
    [[nodiscard]] FormatterRegistry& Formatters() { return m_formatters; }

    [[nodiscard]] bool Update(const Input& input, int viewportWidth, int viewportHeight,float deltaSeconds=0.0f);
    Status SetAnimation(AnimationClip clip,bool autoplay=true);
    Status PlayAnimation();
    void Render(graphics::Renderer2D& renderer);
    void SetDiagnosticOverlayEnabled(bool value) { m_showDiagnostics = value; }

private:
    void WireCommands(Control& node);
    void RenderDiagnosticOverlay(graphics::Renderer2D& renderer);

    std::unique_ptr<Control> m_root;
    std::unique_ptr<InputRouter> m_input;
    Theme m_theme;
    FormatterRegistry m_formatters;
    CommandRegistry m_commands;
    int m_width = 0;
    int m_height = 0;
    bool m_showDiagnostics = true;
    std::optional<AnimationClip> m_animation;
    std::unique_ptr<AnimationPlayer> m_animationPlayer;
};

}  // namespace px::ui
