local SceneManager = {}
SceneManager.__index = SceneManager

function SceneManager.new()
    local self = setmetatable({}, SceneManager)
    self.currentScene = nil
    self.nextScene = nil
    return self
end

function SceneManager:switch(newScene)
    if self.currentScene and self.currentScene.exit then
        self.currentScene:exit()
    end
    
    self.currentScene = newScene
    
    if self.currentScene and self.currentScene.enter then
        self.currentScene:enter()
    end
end

function SceneManager:update(mx, my, leftClick, rightClick)
    if self.currentScene and self.currentScene.update then
        self.currentScene:update(mx, my, leftClick, rightClick)
    end
end

function SceneManager:render(winW, winH)
    if self.currentScene and self.currentScene.render then
        self.currentScene:render(winW, winH)
    end
end

return SceneManager
