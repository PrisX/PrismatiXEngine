local Component = include("Scripts/common/component.lua")
local Banner = setmetatable({}, Component)
Banner.__index = Banner

function Banner.new(props)
    local self = setmetatable(Component.new(), Banner)
    
    self.font = props.font or "NotoSansTC-Bold.ttf"
    self.fontSize = props.fontSize or 24
    self.textColor = props.textColor or {255, 240, 180}
    self.outlineColor = props.outlineColor or {0, 0, 0}
    self.bgColor = props.bgColor or {20, 30, 50, 210}
    
    self.text = ""
    self.y = props.y or 20
    self.targetX_pos = props.targetX or 20
    self.stayDuration = props.stayDuration or 180
    self.timer = 0
    
    self.x = -600
    self.targetX = -600
    self.alpha = 0
    self.targetAlpha = 0
    
    return self
end

function Banner:show(text)
    self.text = text
    self.timer = self.stayDuration
    self.alpha = 255
    self:fade_to(255)
    self:move_to(self.targetX_pos, self.y, 0.18)
end

function Banner:update()
    Component.update(self)
    
    if self.timer > 0 then
        self.timer = self.timer - 1
        if self.timer <= 0 then
            self:fade_to(0, 5)
        end
    end
end

function Banner:render()
    if self.alpha <= 0 then return end
    
    local a = self.alpha
    local x, y = self.x, self.y
    
    local size = Engine.MeasureText(self.text, self.font, self.fontSize)
    local padX, padY = 20, 10
    local w, h = size.w + padX * 2, size.h + padY * 2
    
    Engine.DrawRect(x, y, w, h, self.bgColor[1], self.bgColor[2], self.bgColor[3], a * (self.bgColor[4]/255))
    
    Engine.DrawTextOutline(self.text, x + padX, y + padY, self.font, self.fontSize, self.textColor[1], self.textColor[2], self.textColor[3], self.outlineColor[1], self.outlineColor[2], self.outlineColor[3], 1, 0, a)
end

return Banner
