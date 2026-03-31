local Banner = {}
Banner.__index = Banner

local Ease = _G.Ease
local Utils = _G.Utils

function Banner.new(props)
    local self = setmetatable({}, Banner)
    
    self.font = props.font or "NotoSansTC-Bold.ttf"
    self.fontSize = props.fontSize or 24
    self.textColor = props.textColor or {255, 240, 180}
    self.outlineColor = props.outlineColor or {0, 0, 0}
    self.bgColor = props.bgColor or {20, 30, 50, 210}
    self.image = props.image
    
    self.text = ""
    self.state = "Idle" -- Idle, SlideIn, Staying, FadeOut
    self.currentX = -600
    self.targetX = props.targetX or 20
    self.y = props.y or 20
    self.alpha = 255
    self.stayTimer = 0
    self.stayDuration = props.stayDuration or 180
    
    return self
end

function Banner:show(text)
    self.text = text
    self.state = "SlideIn"
    self.currentX = -600
    self.alpha = 255
    self.stayTimer = 0
end

function Banner:update()
    if self.state == "Idle" then return end
    
    local slideFactor = 0.18
    if self.state == "SlideIn" then
        local delta = self.targetX - self.currentX
        self.currentX = self.currentX + delta * slideFactor
        if math.abs(delta) < 1 then
            self.currentX = self.targetX
            self.state = "Staying"
            self.stayTimer = self.stayDuration
        end
    elseif self.state == "Staying" then
        self.stayTimer = self.stayTimer - 1
        if self.stayTimer <= 0 then
            self.state = "FadeOut"
        end
    elseif self.state == "FadeOut" then
        self.alpha = self.alpha - 4
        if self.alpha <= 0 then
            self.alpha = 0
            self.state = "Idle"
        end
    end
end

function Banner:render()
    if self.state == "Idle" or self.alpha <= 0 then return end
    
    local a = math.floor(self.alpha)
    local x = math.floor(self.currentX)
    local y = self.y
    
    if self.image then
        local rect = Engine.DrawAuto(self.image, DisplayMode.TopLeft, a, x, y, 0.7)
        if self.text ~= "" then
            Engine.DrawTextOutline(self.text, rect.x + 20, rect.y + 10, self.font, self.fontSize, self.textColor[1], self.textColor[2], self.textColor[3], self.outlineColor[1], self.outlineColor[2], self.outlineColor[3], 1, 0, a)
        end
    else
        local size = Engine.MeasureText(self.text, self.font, self.fontSize)
        local padX, padY = 20, 10
        local w, h = size.w + padX * 2, size.h + padY * 2
        
        Engine.DrawRect(x, y, w, h, self.bgColor[1], self.bgColor[2], self.bgColor[3], math.floor(a * (self.bgColor[4]/255)))
        Engine.DrawTextOutline(self.text, x + padX, y + padY, self.font, self.fontSize, self.textColor[1], self.textColor[2], self.textColor[3], self.outlineColor[1], self.outlineColor[2], self.outlineColor[3], 1, 0, a)
    end
end

return Banner
