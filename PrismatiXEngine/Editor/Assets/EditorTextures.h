#pragma once

#include <imgui.h>

#include <string>
#include <unordered_map>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::editor {

class EditorTextures {
public:
    explicit EditorTextures(SDL_Renderer* renderer) : m_renderer(renderer) {}
    ~EditorTextures();

    EditorTextures(const EditorTextures&) = delete;
    EditorTextures& operator=(const EditorTextures&) = delete;

    SDL_Texture* Load(const std::string& absPath, int* outW = nullptr, int* outH = nullptr);
    [[nodiscard]] ImTextureID LoadId(const std::string& absPath, int* outW = nullptr,
                                     int* outH = nullptr);

    void Clear();

private:
    struct Entry {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
    };
    SDL_Renderer* m_renderer;
    std::unordered_map<std::string, Entry> m_cache;
};

}
