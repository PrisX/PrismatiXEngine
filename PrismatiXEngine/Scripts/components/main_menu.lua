local MainMenu = {}
MainMenu.__index = MainMenu

function MainMenu.new(screenW, screenH, fontName, fontSize)
    local self = setmetatable({}, MainMenu)

    self.fontName = fontName
    self.fontSize = fontSize
    self.buttons = {}

    local btnW = 250
    local btnH = 50
    local startX = 100
    local startY = screenH - 250
    local gap = 60

    self.buttons[1] = { text = "Start Game", action = "Start", x = startX, y = startY, w = btnW, h = btnH, isHovered = false }
    self.buttons[2] = { text = "Load Game", action = "Load", x = startX, y = startY + gap, w = btnW, h = btnH, isHovered = false }
    self.buttons[3] = { text = "Exit", action = "Exit", x = startX, y = startY + gap * 2, w = btnW, h = btnH, isHovered = false }

    return self
end

function MainMenu:update(mouseX, mouseY, isClicked)
    for _, btn in ipairs(self.buttons) do
        local inX = mouseX >= btn.x and mouseX <= btn.x + btn.w
        local inY = mouseY >= btn.y and mouseY <= btn.y + btn.h
        btn.isHovered = inX and inY

        if isClicked and btn.isHovered then
            return btn.action
        end
    end

    return ""
end

function MainMenu:render()
    for _, btn in ipairs(self.buttons) do
        if btn.isHovered then
            Engine.DrawRect(btn.x, btn.y, btn.w, btn.h, 255, 255, 255, 40)
            Engine.DrawTextOutline(btn.text, btn.x + 20, btn.y + 10, self.fontName, self.fontSize, 255, 215, 0, 0, 0, 0, 2)
        else
            Engine.DrawTextOutline(btn.text, btn.x + 20, btn.y + 10, self.fontName, self.fontSize, 220, 220, 220, 0, 0, 0, 2)
        end
    end
end

return MainMenu
