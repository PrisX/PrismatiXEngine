#include "Engine/Platform/Window.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

namespace px {

Window::~Window() {
    Destroy();
}

bool Window::Create(const std::string& title, int width, int height, bool resizable) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(title.c_str(), width, height, flags);
    if (!m_window) {
        PX_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_renderer) {
        PX_LOG_ERROR("SDL_CreateRenderer failed: {}", SDL_GetError());
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        return false;
    }

    SDL_SetRenderVSync(m_renderer, 1);
    PX_LOG_INFO("Window created: {}x{} ('{}')", width, height, title);
    return true;
}

void Window::Destroy() {
    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void Window::Clear(Color color) {
    if (!m_renderer) {
        return;
    }
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(m_renderer);
}

void Window::Present() {
    if (m_renderer) {
        SDL_RenderPresent(m_renderer);
    }
}

void Window::GetSize(int& width, int& height) const {
    width = 0;
    height = 0;
    if (m_window) {
        SDL_GetWindowSize(m_window, &width, &height);
    }
}

void Window::SetVSync(bool enabled) {
    if (m_renderer) {
        SDL_SetRenderVSync(m_renderer, enabled ? 1 : 0);
    }
}

}
