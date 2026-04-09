#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <vector>

#include "Core/Models/SaveData.h"
#include "Core/Models/VNCommand.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

namespace PrismatiX {
namespace Systems {
class RenderSystem;
}
}  // namespace PrismatiX

namespace PrismatiX {
namespace Models {

struct ActiveCharacter {
    std::string name;
    std::string speakerName;
    std::string diff;
    int pos;

    float alpha = 0.0f;
    float targetAlpha = 255.0f;
    bool isExiting = false;

    float currentX = 0.0f;
    float targetX = 0.0f;

    std::string animation = "fade";
    std::string animationEase;
    std::string animationTrigger = "enter";
    int animationDuration = 18;
    int animationFrame = 0;
    bool animationActive = false;
    float renderOffsetX = 0.0f;
    float renderOffsetY = 0.0f;
    float renderScale = 1.0f;
};

struct NotificationOverlay {
    enum class Type { Chapter, BGM };
    std::string text;
    Type type = Type::Chapter;
    enum class State { Idle, SlideIn, Staying, FadeOut } state = State::Idle;
    float currentX = -600.0f;
    float targetX = 20.0f;
    int stayTimer = 0;
    float alpha = 255.0f;

    bool IsActive() const { return state != State::Idle; }
    
    void Show(const std::string& msg, Type t) {
        text = msg;
        type = t;
        state = State::SlideIn;
        alpha = 255.0f;
        stayTimer = 0;
        if (type == Type::Chapter) {
            currentX = -600.0f;
            targetX = -1.0f;
        } else {
            currentX = -400.0f;
            targetX = 20.0f;
        }
    }

    void Update() {
        if (!IsActive()) return;
        const float slideFactor = 0.18f;
        switch (state) {
            case State::SlideIn:
                if (PrismatiX::Utils::ExpDecay(currentX, targetX, slideFactor)) {
                    state = State::Staying;
                    stayTimer = (type == Type::Chapter) ? 300 : 180;
                }
                break;
            case State::Staying:
                if (--stayTimer <= 0) state = State::FadeOut;
                break;
            case State::FadeOut:
                if (PrismatiX::Utils::FadeOut(alpha, 4.0f)) state = State::Idle;
                break;
            default:
                break;
        }
    }
    void Render(Systems::RenderSystem& renderSystem, TTF_Font* font) const;
};

}  // namespace Models
}  // namespace PrismatiX
