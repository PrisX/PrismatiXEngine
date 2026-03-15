local DialogueBox = {}
DialogueBox.__index = DialogueBox

local function utf8_chars(text)
    local chars = {}
    local i = 1
    local len = #text
    while i <= len do
        local c = string.byte(text, i)
        local step = 1
        -- UTF-8 神奇拆解，反正就是要用記憶體位置 https://stackoverflow.com/questions/45716356/utf-text-in-sdl2
        if c < 0x80 then -- English / Numbers (1 Byte)
            step = 1
        elseif c < 0xE0 then -- Idk tf is this (2 Bytes)
            step = 2
        elseif c < 0xF0 then -- Chinese / Japanese (3 Bytes)
            step = 3
        else -- Emoji (4 Bytes)
            step = 4
        end
        chars[#chars + 1] = string.sub(text, i, i + step - 1)
        i = i + step
    end
    return chars
end

function DialogueBox.new(fontName, fontSize)
    local self = setmetatable({}, DialogueBox)

    self.fontName = fontName
    self.fontSize = fontSize
    self.nameFontName = fontName
    self.nameFontSize = fontSize

    self.parsedCharacters = {}
    self.currentDisplayText = ""
    self.displayedText = ""
    self.currentSpeakerName = ""

    self.currentIndex = 0
    self.lastTime = 0
    self.textSpeed = 50

    self.fadeAlpha = 255
    self.fadeStartTime = 0
    self.fadeDuration = 150

    self.textColor = { 255, 255, 255 }
    self.outlineColor = { 0, 0, 0 }

    return self
end

function DialogueBox:set_name_font(fontName, fontSize)
    self.nameFontName = fontName
    self.nameFontSize = fontSize
end

function DialogueBox:set_text(name, text, speed, textColor, outlineColor)
    self.parsedCharacters = utf8_chars(text)
    self.currentSpeakerName = name or ""
    self.currentDisplayText = ""
    self.displayedText = ""
    self.currentIndex = 0
    self.textSpeed = speed or 50
    self.lastTime = os.clock() * 1000
    self.fadeAlpha = 255

    if textColor then
        self.textColor = textColor
    end
    if outlineColor then
        self.outlineColor = outlineColor
    end
end

function DialogueBox:update()
    local now = os.clock() * 1000
    if self.currentIndex < #self.parsedCharacters and (now - self.lastTime) >= self.textSpeed then
        self.displayedText = self.currentDisplayText
        self.currentIndex = self.currentIndex + 1
        self.currentDisplayText = self.currentDisplayText .. self.parsedCharacters[self.currentIndex]
        self.lastTime = now
        self.fadeAlpha = 0
        self.fadeStartTime = now
    end

    if self.fadeAlpha < 255 then
        local elapsed = now - self.fadeStartTime
        if elapsed >= self.fadeDuration then
            self.fadeAlpha = 255
        else
            self.fadeAlpha = math.floor((elapsed * 255) / self.fadeDuration)
        end
    end
end

function DialogueBox:show_all()
    if self.currentIndex < #self.parsedCharacters then
        self.currentDisplayText = table.concat(self.parsedCharacters)
        self.currentIndex = #self.parsedCharacters
    end
    self.displayedText = self.currentDisplayText
    self.fadeAlpha = 255
end

function DialogueBox:is_finished()
    return self.currentIndex >= #self.parsedCharacters
end

function DialogueBox:render(screenW, screenH)
    local boxH = 140
    local boxY = screenH - boxH

    local boxRect = Engine.DrawAuto("dialoguebox.png", DisplayMode.FitWidthBottom, 255, 0, 0, 1.0)
    if boxRect.w == 0 then
        Engine.DrawRect(0, boxY, screenW, boxH, 0, 0, 0, 180)
    end

    if self.currentSpeakerName ~= "" then
        local nameRect = Engine.DrawAuto("nameplate.png", DisplayMode.BottomLeft, 255, 120, -180, 0.7)
        local nx = nameRect.w > 0 and nameRect.x or 50
        local ny = nameRect.w > 0 and nameRect.y or (boxY - 42)
        Engine.DrawTextOutline(self.currentSpeakerName, nx + 24, ny + 8, self.nameFontName, self.nameFontSize, 255, 255, 255, 60, 30, 80, 1)
    end

    if self.currentDisplayText ~= "" then
        local tx = math.floor((screenW - 960) / 2)
        local ty = boxY + 20
        local c = self.textColor
        local o = self.outlineColor

        if self.fadeAlpha < 255 then
            Engine.DrawTextOutline(self.currentDisplayText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, self.fadeAlpha, true)
            if self.displayedText ~= "" then
                Engine.DrawTextOutline(self.displayedText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, 255, true)
            end
        else
            Engine.DrawTextOutline(self.currentDisplayText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, 255, true)
        end
    end
end

return DialogueBox
