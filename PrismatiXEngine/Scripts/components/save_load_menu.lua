local SaveLoadMenu = {}
SaveLoadMenu.__index = SaveLoadMenu

local UI = _G.UI

function SaveLoadMenu.new(fontName, fontSize)
    local self = setmetatable({}, SaveLoadMenu)
    self.fontName = fontName
    self.fontSize = fontSize
    self.mode = "save"
    self.returnState = "Playing"
    
    self.backButton = UI.Button({
        text = "返回",
        x = 50, y = 650, w = 150, h = 50,
        font = fontName, fontSize = fontSize,
        onClick = function() self.pendingAction = "back" end
    })
    
    return self
end

function SaveLoadMenu:set_mode(mode, returnState)
    self.mode = mode
    self.returnState = returnState
end

function SaveLoadMenu:update(mx, my, click, rightClick)
    self.pendingAction = nil
    self.backButton:update(mx, my, click)
    if rightClick then self.pendingAction = "back" end
    return self.pendingAction
end

function SaveLoadMenu:render()
    Engine.ClearScreen(20, 20, 30, 255)
    Engine.DrawTextOutline(self.mode:upper(), 1280/2 - 50, 50, self.fontName, 48, 255, 255, 255, 0, 0, 0, 2)
    Engine.DrawTextOutline("wip", 1280/2 - 200, 300, self.fontName, 24, 200, 200, 200, 0, 0, 0, 1)
    self.backButton:render()
end

return SaveLoadMenu
