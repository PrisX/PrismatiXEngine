#include "Editor/Core/EditorTextures.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

namespace px::editor {

EditorTextures::~EditorTextures() {
    Clear();
}

SDL_Texture* EditorTextures::Load(const std::string& absPath, int* outW, int* outH) {
    auto it = m_cache.find(absPath);
    if (it == m_cache.end()) {
        Entry entry;
        if (SDL_Surface* surface = IMG_Load(absPath.c_str())) {
            entry.texture = SDL_CreateTextureFromSurface(m_renderer, surface);
            entry.w = surface->w;
            entry.h = surface->h;
            SDL_DestroySurface(surface);
            if (entry.texture) {
                SDL_SetTextureScaleMode(entry.texture, SDL_SCALEMODE_LINEAR);
            }
        }
        it = m_cache.emplace(absPath, entry).first;
    }
    if (outW) *outW = it->second.w;
    if (outH) *outH = it->second.h;
    return it->second.texture;
}

ImTextureID EditorTextures::LoadId(const std::string& absPath, int* outW, int* outH) {
    return reinterpret_cast<ImTextureID>(Load(absPath, outW, outH));
}

void EditorTextures::Clear() {
    for (auto& [path, entry] : m_cache) {
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
        }
    }
    m_cache.clear();
}

}
