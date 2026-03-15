local Toolbar = {}
Toolbar.__index = Toolbar

function Toolbar.new(fontName, fontSize, screenH)
    local self = setmetatable({}, Toolbar)

    self.fontName = fontName
    self.fontSize = fontSize

    self.hiddenY = screenH
    self.visibleY = screenH - 50
    self.currentY = self.hiddenY
    self.targetY = self.hiddenY
    self.isPinned = false

    local saveSize = Engine.MeasureText("Save", fontName, fontSize)
    local loadSize = Engine.MeasureText("Load", fontName, fontSize)
    local pinSize = Engine.MeasureText("Pin", fontName, fontSize)

    self.btnSave = { x = 950, y = 0, w = saveSize.w, h = saveSize.h }
    self.btnLoad = { x = 1050, y = 0, w = loadSize.w, h = loadSize.h }
    self.btnPin = { x = 1150, y = 0, w = pinSize.w, h = pinSize.h }

    self.hoverSave = false
    self.hoverLoad = false
    self.hoverPin = false

    return self
end

local function point_in_rect(px, py, rect)
    return px >= rect.x and px <= rect.x + rect.w and py >= rect.y and py <= rect.y + rect.h
end

function Toolbar:is_mouse_over(mouseY)
    return mouseY >= self.currentY
end

function Toolbar:update(mouseX, mouseY, isClicked)
    local inTriggerArea = mouseY > (self.visibleY - 20)
    if self.isPinned or inTriggerArea then
        self.targetY = self.visibleY
    else
        self.targetY = self.hiddenY
    end

    self.currentY = Ease.exp_decay(self.currentY, self.targetY, 0.15, 0.05)

    local textOffsetY = math.floor((50 - self.btnSave.h) / 2)
    self.btnSave.y = math.floor(self.currentY) + textOffsetY
    self.btnLoad.y = math.floor(self.currentY) + textOffsetY
    self.btnPin.y = math.floor(self.currentY) + textOffsetY

    if self.currentY > self.visibleY + 5.0 then
        self.hoverSave = false
        self.hoverLoad = false
        self.hoverPin = false
        return ""
    end

    self.hoverSave = point_in_rect(mouseX, mouseY, self.btnSave)
    self.hoverLoad = point_in_rect(mouseX, mouseY, self.btnLoad)
    self.hoverPin = point_in_rect(mouseX, mouseY, self.btnPin)

    if isClicked then
        if self.hoverSave then
            return "OpenSave"
        end
        if self.hoverLoad then
            return "OpenLoad"
        end
        if self.hoverPin then
            self.isPinned = not self.isPinned
            return "TogglePin"
        end
    end

    return ""
end

function Toolbar:render(screenW)
    if self.currentY >= self.hiddenY - 1.0 then
        return
    end

    Engine.DrawRect(0, math.floor(self.currentY), screenW, 50, 0, 0, 0, 180)

    local idle = { 200, 200, 200 }
    local hover = { 255, 215, 0 }
    local pinned = { 100, 255, 100 }

    local saveColor = self.hoverSave and hover or idle
    local loadColor = self.hoverLoad and hover or idle

    local pinColor = idle
    if self.isPinned then
        pinColor = pinned
    elseif self.hoverPin then
        pinColor = hover
    end

    Engine.DrawText("Save", self.btnSave.x, self.btnSave.y, self.fontName, self.fontSize, saveColor[1], saveColor[2], saveColor[3])
    Engine.DrawText("Load", self.btnLoad.x, self.btnLoad.y, self.fontName, self.fontSize, loadColor[1], loadColor[2], loadColor[3])
    Engine.DrawText(self.isPinned and "Unpin" or "Pin", self.btnPin.x, self.btnPin.y, self.fontName, self.fontSize, pinColor[1], pinColor[2], pinColor[3])
end

return Toolbar
