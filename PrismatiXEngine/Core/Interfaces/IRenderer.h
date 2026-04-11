#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>

namespace PrismatiX::Interfaces {

class IRenderer {
public:
    virtual ~IRenderer() = default;
    
    // Texture rendering
    virtual void DrawTexture(
        SDL_Texture* texture,
        int x, int y,
        float scale = 1.0f,
        int alpha = 255
    ) = 0;
    
    virtual void DrawTextureEx(
        SDL_Texture* texture,
        SDL_Rect* srcRect,
        SDL_Rect* dstRect,
        float angle = 0.0f,
        SDL_Point* center = nullptr,
        SDL_RendererFlip flip = SDL_FLIP_NONE
    ) = 0;
    
    // Text rendering
    virtual void DrawText(
        TTF_Font* font,
        const std::string& text,
        SDL_Color color,
        int x, int y,
        int maxWidth = -1,
        bool withOutline = false,
        SDL_Color outlineColor = {0, 0, 0, 255},
        int outlineSize = 2
    ) = 0;
    
    virtual SDL_Texture* RenderTextCached(
        TTF_Font* font,
        const std::string& text,
        SDL_Color color,
        SDL_Color outlineColor = {0, 0, 0, 255},
        int outlineSize = 0,
        Uint32 wrapLength = 0
    ) = 0;
    
    // Shape rendering
    virtual void DrawRect(
        int x, int y,
        int w, int h,
        SDL_Color color,
        bool filled = false
    ) = 0;
    
    virtual void DrawLine(
        int x1, int y1,
        int x2, int y2,
        SDL_Color color
    ) = 0;
    
    // Rendering control
    virtual void Clear(SDL_Color color = {0, 0, 0, 255}) = 0;
    virtual void Present() = 0;
    
    // Render target management
    virtual SDL_Texture* CreateTexture(int w, int h) = 0;
    virtual void SetRenderTarget(SDL_Texture* target) = 0;
    virtual void ResetRenderTarget() = 0;
    
    // Blend mode
    virtual void SetBlendMode(SDL_BlendMode blendMode) = 0;
    
    // Viewport/Camera
    virtual void SetViewport(int x, int y, int w, int h) = 0;
    virtual void ResetViewport() = 0;
    
    // Utility
    virtual SDL_Renderer* GetSDLRenderer() = 0;
    virtual void GetOutputSize(int& w, int& h) = 0;
};

} // namespace PrismatiX::Interfaces
