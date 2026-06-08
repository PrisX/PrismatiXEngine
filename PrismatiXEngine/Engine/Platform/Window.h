#pragma once

#include "Engine/Common/Types.h"

#include <string>

struct SDL_Window;
struct SDL_Renderer;

namespace px {

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const std::string& title, int width, int height, bool resizable = true);
    void Destroy();

    void Clear(Color color);
    void Present();

    void GetSize(int& width, int& height) const;
    void SetVSync(bool enabled);

    [[nodiscard]] SDL_Window* Handle() const { return m_window; }
    [[nodiscard]] SDL_Renderer* Renderer() const { return m_renderer; }
    [[nodiscard]] bool Valid() const { return m_window != nullptr && m_renderer != nullptr; }

private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
};

}
