#include "VNStage.h"

#include <algorithm>
#include <cmath>

#include "Core/EngineConfig.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/Models/VNModels.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

namespace PrismatiX::VN {

VNStage::VNStage(PrismatiX::Services::ResourceManager& resMgr, PrismatiX::Systems::RenderSystem& renSys) : resourceManager(resMgr), renderSystem(renSys) {}

void VNStage::SetBackground(const std::string& bgName, const std::string& transition) {
    if (currentBgName == bgName) return;

    if (transition.empty()) {
        previousBgTexture = currentBgTexture;
        currentBgName = bgName;
        currentBgTexture = resourceManager.LoadTexture(bgName);
        if (currentBgTexture) bgFadeAlpha = 0.0f;
        else bgFadeAlpha = 255.0f;
        return;
    }

    if (transition == "fast") {
        bgTransition.speed = 15.0f;
    } else if (transition == "slow") {
        bgTransition.speed = 2.0f;
    } else {
        bgTransition.speed = 8.0f;
    }

    auto onPeak = [this, bgName]() {
        previousBgTexture = nullptr;
        currentBgName = bgName;
        currentBgTexture = resourceManager.LoadTexture(bgName);
        bgFadeAlpha = 255.0f;
    };
    
    bgTransition.Start(onPeak, PrismatiX::Utils::TransitionType::BlackFade);
}

void VNStage::SetCharacter(const std::string& name, const std::string& diff, int pos, const std::string& transition) {
    auto& chara = activeCharacters[name];
    chara.name = name;
    chara.diff = diff;
    chara.pos = pos;
    chara.isExiting = false;
    chara.targetAlpha = 255.0f;
    chara.anim.Start(transition.empty() ? "fade" : transition, 18);

    if (chara.alpha <= 0.0f) {
        chara.alpha = 0.0f;
        chara.currentX = (float)EngineConfig::kDefaultScreenWidth / 2.0f;
    }

    RecalculatePositions();
}

void VNStage::ClearCharacter(const std::string& name, const std::string& transition) {
    auto it = activeCharacters.find(name);
    if (it != activeCharacters.end()) {
        it->second.isExiting = true;
        it->second.targetAlpha = 0.0f;
        it->second.anim.Start(transition.empty() ? "fade" : transition, 18);
        RecalculatePositions();
    }
}

void VNStage::Update() {
    // Background fade
    if (bgTransition.IsActive()) {
        bgTransition.Update();
    } else if (currentBgTexture && bgFadeAlpha < 255.0f) {
        PrismatiX::Utils::FadeIn(bgFadeAlpha, 10.0f);
        if (bgFadeAlpha >= 255.0f) {
            previousBgTexture = nullptr;
        }
    }

    // Characters update
    for (auto it = activeCharacters.begin(); it != activeCharacters.end();) {
        auto& chara = it->second;

        PrismatiX::Utils::ExpDecay(chara.alpha, chara.targetAlpha, 0.08f);
        PrismatiX::Utils::ExpDecay(chara.currentX, chara.targetX, 0.15f);

        if (chara.anim.IsRunning()) {
            float p = chara.anim.Progress();
            chara.anim.Tick();
            if (!chara.anim.IsRunning()) {
                chara.anim.offsetX = 0;
                chara.anim.offsetY = 0;
                chara.anim.scale   = 1.0f;
            }
            else {
                if (chara.anim.type == "bounce") {
                    chara.anim.offsetY = -30.0f * std::sin(p * 3.14159265f);
                }
                else if (chara.anim.type == "shake") {
                    chara.anim.offsetX = 5.0f * std::sin(p * 3.14159265f * 4.0f);
                }
            }
        }

        if (chara.isExiting && chara.alpha < 1.0f) {
            it = activeCharacters.erase(it);
            sortDirty = true;
        }
        else {
            ++it;
        }
    }

    if (sortDirty) {
        sortedCharacters.clear();
        for (auto& pair : activeCharacters) {
            sortedCharacters.push_back(&pair.second);
        }
        std::sort(sortedCharacters.begin(), sortedCharacters.end(), [](PrismatiX::Models::ActiveCharacter* a, PrismatiX::Models::ActiveCharacter* b) { return a->pos < b->pos; });
        sortDirty = false;
    }
}

void VNStage::Render() {
    // Render Background
    if (previousBgTexture) {
        renderSystem.DrawTextureAuto(previousBgTexture, PrismatiX::Systems::DisplayMode::Fill, 255);
    }
    if (currentBgTexture) {
        renderSystem.DrawTextureAuto(currentBgTexture, PrismatiX::Systems::DisplayMode::Fill, static_cast<Uint8>(bgFadeAlpha));
    }

    if (bgTransition.IsActive()) {
        bgTransition.Draw(renderSystem.GetRenderer(), EngineConfig::kDefaultScreenWidth, EngineConfig::kDefaultScreenHeight);
    }

    // Render Characters
    for (auto* chara : sortedCharacters) {
        if (!chara->cachedTexture || chara->cachedDiff != chara->diff) {
            std::string fileName = chara->name + "_" + chara->diff + ".png";
            chara->cachedTexture = resourceManager.LoadTexture(fileName);
            chara->cachedDiff = chara->diff;
        }
        
        SDL_Texture* tex = chara->cachedTexture;
        if (tex) {
            int texW, texH;
            SDL_QueryTexture(tex, NULL, NULL, &texW, &texH);

            float targetHeight = 600.0f;
            float scale = (targetHeight / (float)texH) * chara->anim.scale;
            int finalW = (int)(texW * scale);
            int finalH = (int)(texH * scale);

            int x = (int)(chara->currentX + chara->anim.offsetX) - (finalW / 2);
            int y = (EngineConfig::kDefaultScreenHeight - finalH) + (int)chara->anim.offsetY;

            renderSystem.DrawTexture(tex, x, y, finalW, finalH, (Uint8)chara->alpha);
        }
    }
}

void VNStage::RecalculatePositions() {
    std::vector<PrismatiX::Models::ActiveCharacter*> nonExiting;
    for (auto& pair : activeCharacters) {
        if (!pair.second.isExiting) nonExiting.push_back(&pair.second);
    }
    std::sort(nonExiting.begin(), nonExiting.end(), [](PrismatiX::Models::ActiveCharacter* a, PrismatiX::Models::ActiveCharacter* b) { return a->pos < b->pos; });

    int total = (int)nonExiting.size();
    if (total > 0) {
        int sectionWidth = EngineConfig::kDefaultScreenWidth / (total + 1);
        for (int i = 0; i < total; i++) {
            nonExiting[i]->targetX = (float)(sectionWidth * (i + 1));
        }
    }
    sortDirty = true;
}

std::vector<PrismatiX::Models::SavedCharacter> VNStage::GetSavedCharacters() const {
    std::vector<PrismatiX::Models::SavedCharacter> saved;
    for (auto& pair : activeCharacters) {
        if (!pair.second.isExiting) {
            saved.push_back({ pair.second.name, pair.second.diff, pair.second.pos });
        }
    }
    return saved;
}

void VNStage::RestoreCharacters(const std::vector<PrismatiX::Models::SavedCharacter>& savedChars) {
    activeCharacters.clear();
    for (const auto& sc : savedChars) {
        auto& ac = activeCharacters[sc.name];
        ac.name = sc.name;
        ac.diff = sc.diff;
        ac.pos = sc.pos;
        ac.alpha = 255.0f;
        ac.targetAlpha = 255.0f;
        ac.isExiting = false;
    }
    RecalculatePositions();
    for (auto& pair : activeCharacters) {
        pair.second.currentX = pair.second.targetX;
    }
}

void VNStage::RestoreBackground(const std::string& bgName) {
    currentBgName = bgName;
    currentBgTexture = resourceManager.LoadTexture(bgName);
    previousBgTexture = nullptr;
    bgFadeAlpha = 255.0f;
}

} // namespace PrismatiX::VN
