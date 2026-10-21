#pragma once
#include "Engine/Graphics/WebBackend.h"
#if defined(PRISMATIX_WEB_GL_EFFECTS)
#include <cstdint>
#include <string_view>
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Vertex;
struct SDL_Surface;
namespace px::graphics {
SDL_Texture* CreateWebTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface);
bool InitializeWebEffects();
void DestroyWebEffects();
int LoadWebEffect(std::string_view id, std::string_view fingerprint);
bool ApplyWebEffect(SDL_Renderer* renderer, int effect, SDL_Texture* source,
                    SDL_Texture* incoming, SDL_Texture* output,
                    const void* uniforms, std::uint32_t uniformBytes);
bool DrawWebEffectGeometry(SDL_Renderer* renderer, int effect,
                           SDL_Texture* texture, const SDL_Vertex* vertices,
                           int count, const int* indices, int indexCount,
                           const void* uniforms, std::uint32_t uniformBytes);
}
#endif
