local Component = include("Scripts/common/component.lua")
local BacklogMenu = setmetatable({}, Component)
BacklogMenu.__index = BacklogMenu

function BacklogMenu.new(fontName, fontSize)
    local self = setmetatable(Component.new(), BacklogMenu)
    
    self.fontName = fontName
    self.fontSize = fontSize
    self.scrollOffset = 0
    
    self.alpha = 0
    self.targetAlpha = 0
    
    return self
end

function BacklogMenu:toggle()
    if self.targetAlpha == 0 then
        self:fade_to(255, 20)
    else
        self:fade_to(0, 20)
    end
end

function BacklogMenu:update(mx, my, leftClick, rightClick)
    Component.update(self)

    if self.alpha <= 0 and self.targetAlpha == 0 then
        return
    end

    if rightClick then
        self:fade_to(0, 20)
    end
end

function BacklogMenu:render(winW, winH)
    if self.alpha <= 0 then return end

    local a = self.alpha
    Engine.DrawRect(0, 0, winW, winH, 0, 0, 0, a * 0.8)
    Engine.DrawText("HISTORY", 50, 30, self.fontName, 40, 200, 200, 255, a)

    local logs = Engine.GetBacklog()
    local y = 100 + self.scrollOffset
    
    for i = 1, #logs do
        local entry = logs[i]
        local speaker = entry.speaker ~= "" and (entry.speaker .. ": ") or ""
        Engine.DrawText(speaker .. entry.text, 70, y, self.fontName, self.fontSize, 255, 255, 255, a)
        y = y + 40
        if y > winH then break end
    end
end

return BacklogMenu
