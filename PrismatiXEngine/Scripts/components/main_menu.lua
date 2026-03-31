local MainMenu = {}
MainMenu.__index = MainMenu

local UI = _G.UI

function MainMenu.new(screenW, screenH, fontName, fontSize)
    local self = setmetatable({}, MainMenu)

    self.screenW = screenW
    self.screenH = screenH
    
    self.pendingAction = nil
    
    self.buttons = {
        UI.Button({
            text = "開始遊戲",
            x = screenW / 2 - 100, y = 400,
            w = 200, h = 60,
            font = fontName, fontSize = fontSize,
            onClick = function() self.pendingAction = "start" end
        }),
        UI.Button({
            text = "讀取進度",
            x = screenW / 2 - 100, y = 480,
            w = 200, h = 60,
            font = fontName, fontSize = fontSize,
            onClick = function() self.pendingAction = "load" end
        }),
        UI.Button({
            text = "離開遊戲",
            x = screenW / 2 - 100, y = 560,
            w = 200, h = 60,
            font = fontName, fontSize = fontSize,
            onClick = function() self.pendingAction = "exit" end
        })
    }

    return self
end

function MainMenu:update(mx, my, click)
    self.pendingAction = nil
    for _, btn in ipairs(self.buttons) do
        btn:update(mx, my, click)
    end
    return self.pendingAction
end

function MainMenu:render()
    for _, btn in ipairs(self.buttons) do
        btn:render()
    end
end

return MainMenu
