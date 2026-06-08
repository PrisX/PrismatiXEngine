#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace px::gfx {
class Renderer2D;
class AssetCache;
}

namespace px::vn {

class Stage {
public:
    Stage(gfx::Renderer2D& renderer, gfx::AssetCache& assets);

    void SetBackground(const std::string& path, bool transition = true);
    void SetCharacter(const std::string& name, const std::string& imagePath, int slot,
                      bool transition = true);
    void ClearCharacter(const std::string& name, bool transition = true);
    void ClearAll();

    void Update(float dt);
    void Render();

    struct SavedActor {
        std::string name;
        std::string imagePath;
        int slot = 2;
    };
    [[nodiscard]] std::vector<SavedActor> Snapshot() const;
    void Restore(const std::vector<SavedActor>& actors);
    [[nodiscard]] const std::string& BackgroundPath() const { return m_bgPath; }

private:
    struct Actor {
        std::string imagePath;
        int slot = 2;
        float alpha = 0.0f;
        float targetAlpha = 255.0f;
        float x = 0.0f;
        float targetX = 0.0f;
        bool exiting = false;
    };

    [[nodiscard]] float SlotCenterX(int slot) const;

    gfx::Renderer2D& m_renderer;
    gfx::AssetCache& m_assets;

    std::string m_bgPath;
    std::string m_prevBgPath;
    float m_bgFade = 1.0f;

    std::unordered_map<std::string, Actor> m_actors;
};

}
