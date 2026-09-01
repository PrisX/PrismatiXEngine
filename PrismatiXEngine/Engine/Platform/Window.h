#pragma once

#include <string>
#include <memory>

#include "Engine/Core/Types.h"
#include "Engine/Graphics/GraphicsDevice.h"

struct SDL_Window;
struct SDL_Renderer;

namespace px {

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const std::string& title, int width, int height,
                bool resizable = true,
                graphics::GraphicsTier graphicsTier = graphics::GraphicsTier::Basic);
    void Destroy();

    void Clear(Color color);
    void Present();

    void GetSize(int& width, int& height) const;
    bool Resize(int width, int height);
    void SetVSync(bool enabled);

    void SetAspectRatio(float ratio);

    [[nodiscard]] SDL_Window* Handle() const { return m_window; }
    [[nodiscard]] SDL_Renderer* Renderer() const;
    [[nodiscard]] graphics::GraphicsDevice* Graphics() const { return m_graphics.get(); }
    [[nodiscard]] bool Valid() const;

private:
    SDL_Window* m_window = nullptr;
    std::unique_ptr<graphics::GraphicsDevice> m_graphics;
};

}  // namespace px
