#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL_mixer.h>
#include <string>
#include <vector>

namespace PrismatiX {
namespace Interfaces {

class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;
    
    // File system
    virtual bool MountArchive(const std::string& archivePath) = 0;
    virtual void ScanDirectory(const std::string& root) = 0;
    virtual std::vector<char> ExtractFile(const std::string& fileName) = 0;
    virtual std::string LoadText(const std::string& path) = 0;
    
    // Asset loading
    virtual SDL_Texture* LoadTexture(const std::string& fileName) = 0;
    virtual TTF_Font* LoadFont(const std::string& fileName, int fontSize) = 0;
    virtual Mix_Chunk* LoadSFX(const std::string& fileName) = 0;
    virtual Mix_Music* LoadBGM(const std::string& fileName, std::vector<char>& outBuffer) = 0;
    
    // Cache management
    virtual void CleanTextures() = 0;
    virtual void CleanFonts() = 0;
    virtual void CleanAudio() = 0;
    virtual void CleanAll() = 0;
};

} // namespace Interfaces
} // namespace PrismatiX
