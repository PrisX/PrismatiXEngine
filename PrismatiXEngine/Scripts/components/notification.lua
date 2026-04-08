local Component = include("Scripts/common/component.lua")
local NotificationManager = {}
NotificationManager.__index = NotificationManager

local NotificationItem = setmetatable({}, Component)
NotificationItem.__index = NotificationItem

function NotificationItem.new(text, type, font, size)
    local self = setmetatable(Component.new(), NotificationItem)
    self.text = text
    self.type = type or "info"
    self.font = font
    self.fontSize = size
    
    self.alpha = 0
    self.targetAlpha = 255
    self.timer = 3000
    
    return self
end

function NotificationManager.new(fontName, fontSize)
    local self = setmetatable({}, NotificationManager)
    self.fontName = fontName
    self.fontSize = fontSize
    self.notifications = {}
    return self
end

function NotificationManager:notify(text, type)
    local item = NotificationItem.new(text, type, self.fontName, self.fontSize)
    item.x = 1280
    item.targetX = 1280 - 320
    table.insert(self.notifications, item)
end

function NotificationManager:update()
    local dt = 16
    for i = #self.notifications, 1, -1 do
        local n = self.notifications[i]
        
        local targetY = (i - 1) * 60 + 50
        n:move_to(n.targetX, targetY, 0.2)
        
        if n.timer > 0 then
            n.timer = n.timer - dt
            if n.timer <= 0 then n:fade_to(0, 10) end
        end
        
        n:update()
        
        if n.alpha <= 0 and n.targetAlpha == 0 then
            table.remove(self.notifications, i)
        end
    end
end

function NotificationManager:render(screenW, screenH)
    for _, n in ipairs(self.notifications) do
        local x, y = n.x, n.y
        local a = n.alpha
        
        Engine.DrawRect(x, y, 300, 50, 20, 20, 25, a * 0.8)
        
        local r, g, b = 100, 200, 255
        if n.type == "error" then r, g, b = 255, 100, 100 end
        Engine.DrawRect(x, y, 5, 50, r, g, b, a)

        Engine.DrawText(n.text, x + 20, y + 10, n.font, n.fontSize, 255, 255, 255, a)
    end
end

return NotificationManager
