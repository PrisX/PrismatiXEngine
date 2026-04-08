local TitleScene = {}
TitleScene.__index = TitleScene

local MainMenu = include("Scripts/components/main_menu.lua")

function TitleScene.new(fontName, fontSize)
    local self = setmetatable({}, TitleScene)
    self.fontName = fontName
    self.fontSize = fontSize
    self.menu = nil
    return self
end

function TitleScene:enter()
    local winW, winH = Engine.GetLogicalSize()
    self.menu = MainMenu.new(winW, winH, self.fontName, self.fontSize)
    _G.Notification:notify("Welcome to PrismatiX", "info")
end

function TitleScene:update(mx, my, leftClick, rightClick)
    local action = self.menu:update(mx, my, leftClick)
    
    if action == "start" then
        _G.Transition:start(function()
            local PlayScene = include("Scripts/scenes/play_scene.lua")
            _G.Scene:switch(PlayScene.new(self.fontName, self.fontSize))
        end)
    elseif action == "exit" then
        _G.Notification:notify("Exit requested", "info")
    end
end

function TitleScene:render(winW, winH)
    Engine.DrawAuto("title_bg.jpg", DisplayMode.Fill, 255)
    self.menu:render()
end

function TitleScene:exit()
    self.menu = nil
end

return TitleScene
