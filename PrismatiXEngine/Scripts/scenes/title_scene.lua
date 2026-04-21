local TitleScene = {}
TitleScene.__index = TitleScene

local MainMenu = include("Scripts/components/main_menu.lua")

local function resolve_play_script()
    if _G.PX and type(_G.PX.GeneratedSceneScript) == "string" and _G.PX.GeneratedSceneScript ~= "" then
        return _G.PX.GeneratedSceneScript
    end
    return "Script/chapter1.pds"
end

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
    _G.PX.Notification:notify("Welcome to PrismatiX", "info")
end

function TitleScene:update(mx, my, leftClick, rightClick)
    local action = self.menu:update(mx, my, leftClick)
    
    if action == "start" then
        _G.PX.Transition:start(function()
            local PlayScene = include("Scripts/scenes/play_scene.lua")
            _G.PX.Scene:switch(PlayScene.new(self.fontName, self.fontSize, resolve_play_script()))
        end)
    elseif action == "exit" then
        _G.PX.Notification:notify("Exit requested", "info")
        Engine.Quit()
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
