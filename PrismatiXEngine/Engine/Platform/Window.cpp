#include "Engine/Platform/Window.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

namespace px {

Window::~Window() {
    Destroy();
}

bool Window::Create(const std::string& title, int width, int height, bool resizable,
                    const graphics::GraphicsTier graphicsTier) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(title.c_str(), width, height, flags);
    if (!m_window) {
        PX_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    m_graphics = std::make_unique<graphics::GraphicsDevice>();
    if (!m_graphics->Create(m_window, graphicsTier)) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        m_graphics.reset();
        return false;
    }

    SDL_SetRenderVSync(Renderer(), 1);
    PX_LOG_INFO("Window created: {}x{} ('{}')", width, height, title);
    return true;
}

void Window::Destroy() {
    m_graphics.reset();
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void Window::Clear(Color color) {
    SDL_Renderer* renderer = Renderer();
    if (!renderer) {
        return;
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
}

void Window::Present() {
    if (Renderer()) {
        SDL_RenderPresent(Renderer());
    }
}

void Window::GetSize(int& width, int& height) const {
    width = 0;
    height = 0;
    if (m_window) {
        SDL_GetWindowSize(m_window, &width, &height);
    }
}

bool Window::Resize(const int width, const int height) {
    if (!m_window || width <= 0 || height <= 0) return false;
    return SDL_SetWindowSize(m_window, width, height);
}

void Window::SetVSync(bool enabled) {
    if (Renderer()) {
        SDL_SetRenderVSync(Renderer(), enabled ? 1 : 0);
    }
}

void Window::SetAspectRatio(float ratio) {
    if (m_window) {
        SDL_SetWindowAspectRatio(m_window, ratio, ratio);
    }
}

SDL_Renderer* Window::Renderer() const {
    return m_graphics ? m_graphics->Renderer() : nullptr;
}

bool Window::Valid() const { return m_window != nullptr && Renderer() != nullptr; }

}
