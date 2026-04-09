#pragma once
#include <SDL2/SDL.h>
#include <stack>
#include <memory>
#include <vector>
#include "Core/Interfaces/IScene.h"

namespace PrismatiX {
namespace UI {

class SceneManager {
    std::stack<std::unique_ptr<Interfaces::IScene>> scenes;
    
public:
    void Push(std::unique_ptr<Interfaces::IScene> scene) {
        if (!scenes.empty()) {
            scenes.top()->OnPause();
        }
        scenes.push(std::move(scene));
        scenes.top()->OnEnter();
    }
    
    void Pop() {
        if (!scenes.empty()) {
            scenes.top()->OnExit();
            scenes.pop();
            if (!scenes.empty()) {
                scenes.top()->OnResume();
            }
        }
    }
    
    void Replace(std::unique_ptr<Interfaces::IScene> scene) {
        if (!scenes.empty()) {
            scenes.top()->OnExit();
            scenes.pop();
        }
        scenes.push(std::move(scene));
        scenes.top()->OnEnter();
    }
    
    void Clear() {
        while (!scenes.empty()) {
            scenes.top()->OnExit();
            scenes.pop();
        }
    }
    
    void HandleEvent(const SDL_Event& event) {
        if (scenes.empty()) return;
        
        std::vector<Interfaces::IScene*> activeScenes;
        auto tempStack = scenes;
        while (!tempStack.empty()) {
            activeScenes.push_back(tempStack.top().get());
            if (tempStack.top()->BlocksInput()) break;
            tempStack.pop();
        }
        
        for (auto it = activeScenes.rbegin(); it != activeScenes.rend(); ++it) {
            (*it)->HandleEvent(event);
        }
    }
    
    void Update(float deltaTime) {
        if (scenes.empty()) return;
        
        std::vector<Interfaces::IScene*> activeScenes;
        auto tempStack = scenes;
        while (!tempStack.empty()) {
            activeScenes.push_back(tempStack.top().get());
            if (tempStack.top()->BlocksUpdate()) break;
            tempStack.pop();
        }
        
        for (auto* scene : activeScenes) {
            scene->Update(deltaTime);
        }
    }
    
    void Render() {
        if (scenes.empty()) return;
        
        std::vector<Interfaces::IScene*> activeScenes;
        auto tempStack = scenes;
        while (!tempStack.empty()) {
            activeScenes.push_back(tempStack.top().get());
            if (!tempStack.top()->IsTransparent()) break;
            tempStack.pop();
        }
        
        for (auto it = activeScenes.rbegin(); it != activeScenes.rend(); ++it) {
            (*it)->Render();
        }
    }
    
    bool IsEmpty() const { return scenes.empty(); }
    
    Interfaces::IScene* GetCurrent() { 
        return scenes.empty() ? nullptr : scenes.top().get(); 
    }
};

} // namespace UI
} // namespace PrismatiX
