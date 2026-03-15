local ChapterBanner = {}
ChapterBanner.__index = ChapterBanner

function ChapterBanner.new()
    local self = setmetatable({}, ChapterBanner)

    self.text = ""
    self.state = "idle"
    self.currentX = -600.0
    self.targetX = -1.0
    self.stayTimer = 0
    self.alpha = 255.0

    return self
end

function ChapterBanner:is_active()
    return self.state ~= "idle"
end

function ChapterBanner:show(chapterText)
    self.text = chapterText
    self.state = "slide_in"
    self.currentX = -600.0
    self.alpha = 255.0
    self.stayTimer = 0
end

function ChapterBanner:update()
    if not self:is_active() then
        return
    end

    if self.state == "slide_in" then
        local reachedTarget
        self.currentX, reachedTarget = Ease.exp_decay(self.currentX, self.targetX, 0.18, 0.5)
        if reachedTarget then
            self.state = "stay"
            self.stayTimer = 300
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

function ChapterBanner:render(fontName, fontSize)
    if not self:is_active() or self.alpha <= 0 then
        return
    end

    local a = math.floor(self.alpha)
    local drawRect = Engine.DrawAuto("chapterinfo.png", DisplayMode.TopLeft, a, math.floor(self.currentX), 20, 0.7)
    if drawRect.w > 0 then
        local textSize = Engine.MeasureText(self.text, fontName, fontSize)
        local tx = drawRect.x + math.floor((drawRect.w - textSize.w) / 2)
        local ty = drawRect.y + math.floor((drawRect.h - textSize.h) / 2)
        Engine.DrawTextOutline(self.text, tx, ty, fontName, fontSize, 255, 240, 180, 0, 0, 0, 1)
    else
        local x = math.floor(self.currentX)
        local y = 20
        Engine.DrawRect(x, y, 380, 56, 20, 20, 40, math.floor(a * 0.85))
        Engine.DrawTextOutline(self.text, x + 20, y + 12, fontName, fontSize, 255, 240, 180, 0, 0, 0, 1)
    end
end

return ChapterBanner
