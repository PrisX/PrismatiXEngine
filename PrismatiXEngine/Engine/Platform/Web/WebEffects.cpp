#include "Engine/Platform/Web/WebEffects.h"
#if defined(PRISMATIX_WEB_GL_EFFECTS)
#include <SDL3/SDL.h>
#include <emscripten/emscripten.h>
#include <string>
#include <vector>
namespace px::graphics {
namespace {
EM_ASYNC_JS(int, Initialize, (), {
    try { await Module.prismatix.effects.initialize(GLctx); return 1; }
    catch (error) { Module.prismatix.error(error); return 0; }
});
EM_JS(void, Destroy, (), { Module.prismatix.effects.destroy(); });
EM_ASYNC_JS(int, Load, (const char* id, const char* fingerprint), {
    try { return await Module.prismatix.effects.load(UTF8ToString(id), UTF8ToString(fingerprint)); }
    catch (error) { Module.prismatix.error(error); return 0; }
});
EM_ASYNC_JS(int, Pass, (int effect, int source, int incoming, int output,
                       int width, int height, const void* data, int bytes), {
    try {
        const uniforms = HEAPU8.slice(data, data + bytes);
        await Module.prismatix.effects.pass(effect, GL.textures[source], GL.textures[incoming],
            GL.textures[output], width, height, uniforms);
        return 1;
    } catch (error) { Module.prismatix.error(error); return 0; }
});
EM_ASYNC_JS(int, Geometry, (int effect, int texture, const float* vertices,
                           int count, int width, int height, int target,
                           int blend, const int* clip, const void* data, int bytes,
                           int textureWidth, int textureHeight, int linear), {
    try {
        const uniforms = HEAPU8.slice(data, data + bytes);
        const geometry = HEAPF32.slice(vertices >> 2, (vertices >> 2) + count * 8);
        const scissor = clip ? HEAP32.slice(clip >> 2, (clip >> 2) + 4) : null;
        await Module.prismatix.effects.geometry(effect, GL.textures[texture], geometry,
            width, height, !!target, blend, scissor, uniforms,
            textureWidth, textureHeight, !!linear);
        return 1;
    } catch (error) { Module.prismatix.error(error); return 0; }
});
int TextureId(SDL_Texture* texture) {
    return texture ? static_cast<int>(SDL_GetNumberProperty(SDL_GetTextureProperties(texture),
        SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0)) : 0;
}
}
SDL_Texture* CreateWebTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface) {
    // SDL's GLES renderer stores BGRA surfaces in RGBA GL textures and swaps
    // channels in its own shader. Custom shaders need canonical RGBA input.
    if (!surface) return nullptr;
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (!rgba) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, rgba);
    SDL_DestroySurface(rgba);
    return texture;
}
bool InitializeWebEffects() { return Initialize() != 0; }
void DestroyWebEffects() { Destroy(); }
int LoadWebEffect(const std::string_view id, const std::string_view fingerprint) {
    return Load(std::string(id).c_str(), std::string(fingerprint).c_str());
}
bool ApplyWebEffect(SDL_Renderer* renderer, const int effect, SDL_Texture* source,
                    SDL_Texture* incoming, SDL_Texture* output,
                    const void* uniforms, const std::uint32_t uniformBytes) {
    if (!renderer || !source || !output || !TextureId(source) || !TextureId(output)) return false;
    float width = 0, height = 0;
    SDL_GetTextureSize(output, &width, &height);
    SDL_FlushRenderer(renderer);
    return Pass(effect, TextureId(source), TextureId(incoming), TextureId(output),
                static_cast<int>(width), static_cast<int>(height), uniforms,
                static_cast<int>(uniformBytes)) != 0;
}
bool DrawWebEffectGeometry(SDL_Renderer* renderer, const int effect,
                           SDL_Texture* texture, const SDL_Vertex* vertices,
                           const int count, const int* indices, const int indexCount,
                           const void* uniforms, const std::uint32_t uniformBytes) {
    if (!renderer || !vertices || count <= 0) return false;
    int width = 0, height = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &width, &height);
    // Stage and composition targets use logical coordinates at native resolution.
    // SDL applies letterboxing only when those completed targets reach the window.
    const auto* target = SDL_GetRenderTarget(renderer);
    SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
    float textureWidth = 1, textureHeight = 1;
    SDL_ScaleMode scaleMode = SDL_SCALEMODE_LINEAR;
    if (texture) {
        SDL_GetTextureSize(texture, &textureWidth, &textureHeight);
        SDL_GetTextureScaleMode(texture, &scaleMode);
    }
    if (texture) SDL_GetTextureBlendMode(texture, &blend);
    else SDL_GetRenderDrawBlendMode(renderer, &blend);
    SDL_Rect clip{};
    const bool clipped = SDL_RenderClipEnabled(renderer);
    if (clipped) SDL_GetRenderClipRect(renderer, &clip);
    std::vector<float> packed;
    const int total = indices ? indexCount : count;
    packed.reserve(static_cast<std::size_t>(total) * 8);
    for (int i = 0; i < total; ++i) {
        const int index = indices ? indices[i] : i;
        if (index < 0 || index >= count) return false;
        const auto& vertex = vertices[index];
        packed.insert(packed.end(), {vertex.position.x, vertex.position.y,
            vertex.color.r, vertex.color.g, vertex.color.b, vertex.color.a,
            vertex.tex_coord.x, vertex.tex_coord.y});
    }
    SDL_FlushRenderer(renderer);
    return Geometry(effect, TextureId(texture), packed.data(), total, width, height,
                    target != nullptr, static_cast<int>(blend), clipped ? &clip.x : nullptr,
                    uniforms, static_cast<int>(uniformBytes), static_cast<int>(textureWidth),
                    static_cast<int>(textureHeight), scaleMode == SDL_SCALEMODE_LINEAR) != 0;
}
}
#endif
