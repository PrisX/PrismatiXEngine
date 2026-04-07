#include "VNStage.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "Core/EngineConfig.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

VNStage::VNStage(ResourceManager& resMgr, RenderSystem& renSys) : resourceManager(resMgr), renderSystem(renSys) {}

void VNStage::SetBackground(const std::string& bgName, const std::string& transition) {
    if (currentBgName == bgName) return;

    previousBgTexture = currentBgTexture;
    currentBgName = bgName;
    currentBgTexture = resourceManager.LoadTexture(bgName);

    if (currentBgTexture) {
        bgFadeAlpha = 0.0f;
    }
    else {
        bgFadeAlpha = 255.0f;
    }
}

void VNStage::SetCharacter(const std::string& name, const std::string& diff, int pos, const std::string& transition) {
    auto& chara = activeCharacters[name];
    chara.name = name;
    chara.diff = diff;
    chara.pos = pos;
    chara.isExiting = false;
    chara.targetAlpha = 255.0f;
    chara.animation = transition.empty() ? "fade" : transition;
    chara.animationActive = true;
    chara.animationFrame = 0;

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
        it->second.animation = transition.empty() ? "fade" : transition;
        it->second.animationActive = true;
        it->second.animationFrame = 0;
        RecalculatePositions();
    }
}

void VNStage::Update() {
    // Background fade
    if (currentBgTexture && bgFadeAlpha < 255.0f) {
        TransitionUtils::FadeIn(bgFadeAlpha, 10.0f);
        if (bgFadeAlpha >= 255.0f) {
            previousBgTexture = nullptr;
        }
    }

    // Characters update
    for (auto it = activeCharacters.begin(); it != activeCharacters.end();) {
        auto& chara = it->second;

        EasingUtils::ExpDecay(chara.alpha, chara.targetAlpha, 0.08f);
        EasingUtils::ExpDecay(chara.currentX, chara.targetX, 0.15f);

        if (chara.animationActive) {
            chara.animationFrame++;
            float p = (float)chara.animationFrame / chara.animationDuration;
            if (p >= 1.0f) {
                chara.animationActive = false;
                chara.renderOffsetX = 0;
                chara.renderOffsetY = 0;
                chara.renderScale = 1.0f;
            }
            else {
                if (chara.animation == "bounce") {
                    chara.renderOffsetY = -30.0f * std::sin(p * std::numbers::pi);
                }
                else if (chara.animation == "shake") {
                    chara.renderOffsetX = 5.0f * std::sin(p * std::numbers::pi * 4.0f);
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
        std::sort(sortedCharacters.begin(), sortedCharacters.end(), [](ActiveCharacter* a, ActiveCharacter* b) { return a->pos < b->pos; });
        sortDirty = false;
    }
}

void VNStage::Render() {
    // Render Background
    if (previousBgTexture) {
        renderSystem.DrawTextureAuto(previousBgTexture, DisplayMode::Fill, 255);
    }
    if (currentBgTexture) {
        renderSystem.DrawTextureAuto(currentBgTexture, DisplayMode::Fill, static_cast<Uint8>(bgFadeAlpha));
    }

    // Render Characters
    for (auto* chara : sortedCharacters) {
        std::string fileName = chara->name + "_" + chara->diff + ".png";
        SDL_Texture* tex = resourceManager.LoadTexture(fileName);
        if (tex) {
            int texW, texH;
            SDL_QueryTexture(tex, NULL, NULL, &texW, &texH);

            float targetHeight = 600.0f;
            float scale = (targetHeight / (float)texH) * chara->renderScale;
            int finalW = (int)(texW * scale);
            int finalH = (int)(texH * scale);

            int x = (int)(chara->currentX + chara->renderOffsetX) - (finalW / 2);
            int y = (EngineConfig::kDefaultScreenHeight - finalH) + (int)chara->renderOffsetY;

            renderSystem.DrawTexture(tex, x, y, finalW, finalH, (Uint8)chara->alpha);
        }
    }
}

void VNStage::RecalculatePositions() {
    std::vector<ActiveCharacter*> nonExiting;
    for (auto& pair : activeCharacters) {
        if (!pair.second.isExiting) nonExiting.push_back(&pair.second);
    }
    std::sort(nonExiting.begin(), nonExiting.end(), [](ActiveCharacter* a, ActiveCharacter* b) { return a->pos < b->pos; });

    int total = (int)nonExiting.size();
    if (total > 0) {
        int sectionWidth = EngineConfig::kDefaultScreenWidth / (total + 1);
        for (int i = 0; i < total; i++) {
            nonExiting[i]->targetX = (float)(sectionWidth * (i + 1));
        }
    }
    sortDirty = true;
}

std::vector<SavedCharacter> VNStage::GetSavedCharacters() const {
    std::vector<SavedCharacter> saved;
    for (auto& pair : activeCharacters) {
        if (!pair.second.isExiting) {
            saved.push_back({ pair.second.name, pair.second.diff, pair.second.pos });
        }
    }
    return saved;
}

void VNStage::RestoreCharacters(const std::vector<SavedCharacter>& savedChars) {
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
