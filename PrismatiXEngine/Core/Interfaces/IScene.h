#pragma once
#include <SDL2/SDL.h>

namespace PrismatiX {
namespace Interfaces {

class IScene {
public:
    virtual ~IScene() = default;

    // Lifecycle
    virtual void OnEnter() {}
    virtual void OnExit() {}
    virtual void OnPause() {}
    virtual void OnResume() {}

    // Main loop
    virtual void HandleEvent(const SDL_Event& event) = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void Render() = 0;

    // Properties
    virtual bool IsTransparent() const { return false; }
    virtual bool BlocksUpdate() const { return true; }
    virtual bool BlocksInput() const { return true; }

    // Scene name for debugging
    virtual const char* GetName() const { return "Scene"; }
};

}  // namespace Interfaces
}  // namespace PrismatiX
