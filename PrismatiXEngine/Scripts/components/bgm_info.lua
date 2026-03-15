local BGMInfo = {}
BGMInfo.__index = BGMInfo

function BGMInfo.new()
    local self = setmetatable({}, BGMInfo)

    self.text = ""
    self.isMusicNotification = false

    self.state = "idle"
    self.currentX = -400.0
    self.targetX = 20.0
    self.stayTimer = 0
    self.alpha = 255.0

    return self
end

function BGMInfo:is_active()
    return self.state ~= "idle"
end

function BGMInfo:show(message, isMusic)
    self.text = message
    self.isMusicNotification = isMusic or false
    self.state = "slide_in"
    self.currentX = -400.0
    self.targetX = 20.0
    self.alpha = 255.0
    self.stayTimer = 0
end

function BGMInfo:update()
    if not self:is_active() then
        return
    end

    if self.state == "slide_in" then
        local reachedTarget
        self.currentX, reachedTarget = Ease.exp_decay(self.currentX, self.targetX, 0.18, 0.5)
        if reachedTarget then
            self.state = "stay"
            self.stayTimer = 180
        end
    elseif self.state == "stay" then
        self.stayTimer = self.stayTimer - 1
        if self.stayTimer <= 0 then
            self.state = "fade_out"
        end
    elseif self.state == "fade_out" then
        local reachedZero
        self.alpha, reachedZero = Ease.fade_out(self.alpha, 4, 0)
        if reachedZero then
            self.state = "idle"
        end
    end
end

function BGMInfo:render(fontName, fontSize)
    if not self:is_active() or self.alpha <= 0 then
        return
    end

    local displayText = self.isMusicNotification and "♪  " .. self.text or self.text
    local textSize = Engine.MeasureText(displayText, fontName, fontSize)

    local padX = 16
    local padY = 8
    local boxX = math.floor(self.currentX)
    local boxY = 20
    local boxW = textSize.w + padX * 2
    local boxH = textSize.h + padY * 2

    local a = math.floor(self.alpha)
    local bgA = math.floor(a * 0.82)

    if self.isMusicNotification then
        Engine.DrawRect(boxX, boxY, boxW, boxH, 20, 30, 50, bgA)
        Engine.DrawRect(boxX, boxY, 4, boxH, 100, 180, 255, a)
        Engine.DrawTextOutline(displayText, boxX + padX, boxY + padY, fontName, fontSize, 180, 220, 255, 0, 0, 0, 1)
    else
        Engine.DrawRect(boxX, boxY, boxW, boxH, 20, 20, 20, bgA)
        Engine.DrawRect(boxX, boxY, 4, boxH, 255, 220, 80, a)
        Engine.DrawTextOutline(displayText, boxX + padX, boxY + padY, fontName, fontSize, 255, 255, 255, 0, 0, 0, 1)
    end
end

return BGMInfo
