#include "Editor/Assets/EditorTextures.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <exception>
#include <filesystem>

namespace px::editor {

EditorTextures::~EditorTextures() {
    Clear();
}

SDL_Texture* EditorTextures::Load(const std::string& absPath, int* outW, int* outH) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!m_renderer || absPath.empty()) return nullptr;

    try {
    auto it = m_cache.find(absPath);
    if (it == m_cache.end()) {
        Entry entry;
        std::error_code error;
        const auto path = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(absPath.data()), absPath.size()));
        const auto size = std::filesystem::file_size(path, error);
        constexpr std::uintmax_t kMaximumPreviewBytes = 512ull * 1024ull * 1024ull;
        if (error || size > kMaximumPreviewBytes) {
            diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
                .code = "PXIMAGE9201", .category = "Editor.Preview",
                .message = error ? "無法讀取圖片縮圖" : "圖片過大，已略過縮圖",
                .details = error ? error.message() : "縮圖預覽上限為 512 MB。"};
            diagnostic.source.path = absPath;
            diag::Emit(std::move(diagnostic));
        } else if (SDL_Surface* surface = IMG_Load(absPath.c_str())) {
            entry.texture = SDL_CreateTextureFromSurface(m_renderer, surface);
            entry.w = surface->w;
            entry.h = surface->h;
            SDL_DestroySurface(surface);
            if (entry.texture) {
                SDL_SetTextureScaleMode(entry.texture, SDL_SCALEMODE_LINEAR);
            } else {
                diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
                    .code = "PXIMAGE9202", .category = "Editor.Preview",
                    .message = "無法建立圖片縮圖材質", .details = SDL_GetError()};
                diagnostic.source.path = absPath;
                diag::Emit(std::move(diagnostic));
            }
        } else {
            diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
                .code = "PXIMAGE9203", .category = "Editor.Preview",
                .message = "圖片格式損壞或不受支援", .details = SDL_GetError()};
            diagnostic.source.path = absPath;
            diag::Emit(std::move(diagnostic));
        }
        it = m_cache.emplace(absPath, entry).first;
    }
    if (outW) *outW = it->second.w;
    if (outH) *outH = it->second.h;
    return it->second.texture;
    } catch (const std::exception& error) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
            .code = "PXIMAGE9204", .category = "Editor.Preview",
            .message = "建立圖片縮圖時發生錯誤", .details = error.what()};
        diagnostic.source.path = absPath;
        diag::Emit(std::move(diagnostic));
        return nullptr;
    } catch (...) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Warning,
            .code = "PXIMAGE9205", .category = "Editor.Preview",
            .message = "建立圖片縮圖時發生未知錯誤"};
        diagnostic.source.path = absPath;
        diag::Emit(std::move(diagnostic));
        return nullptr;
    }
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
