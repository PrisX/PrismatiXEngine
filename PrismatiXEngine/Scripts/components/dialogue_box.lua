local DialogueBox = {}
DialogueBox.__index = DialogueBox

function DialogueBox.new(fontName, fontSize)
    local self = setmetatable({}, DialogueBox)
    self.fontName = fontName
    self.fontSize = fontSize
    self.nameFontName = fontName
    self.nameFontSize = fontSize
    return self
end

function DialogueBox:set_name_font(fontName, fontSize)
    self.nameFontName = fontName
    self.nameFontSize = fontSize
end

function DialogueBox:render(state, screenW, screenH)
    if not state then return end

    local boxH = 140
    local boxY = screenH - boxH

    local boxRect = Engine.DrawAuto("dialoguebox.png", DisplayMode.FitWidthBottom, 255, 0, 0, 1.0)
    if boxRect.w == 0 then
        Engine.DrawRect(0, boxY, screenW, boxH, 0, 0, 0, 180)
    end

    if state.speaker ~= "" then
        local nameRect = Engine.DrawAuto("nameplate.png", DisplayMode.BottomLeft, 255, 120, -180, 0.7)
        local nx = nameRect.w > 0 and nameRect.x or 50
        local ny = nameRect.w > 0 and nameRect.y or (boxY - 42)
        local nw = nameRect.w > 0 and nameRect.w or 220
        local nh = nameRect.h > 0 and nameRect.h or 50
        
        local nameSize = Engine.MeasureText(state.speaker, self.nameFontName, self.nameFontSize)
        local tx = nx + math.floor((nw - nameSize.w) * 0.5)
        local ty = ny + math.floor((nh - nameSize.h) * 0.5)
        Engine.DrawTextOutline(state.speaker, tx, ty, self.nameFontName, self.nameFontSize, 255, 255, 255, 60, 30, 80, 1)
    end

    if state.currentText ~= "" then
        local tx = math.floor((screenW - 960) / 2)
        local ty = boxY - 20
        
        if state.effect ~= "" then
            local effects = _G.TextEffects
            if effects and effects[state.effect] then
                local handled = pcall(effects[state.effect], {
                    text = state.currentText,
                    x = tx, y = ty,
                    progress = state.effectProgress,
                })
                if handled then return end
            end
        end

        Engine.DrawTextOutline(state.currentText, tx, ty, self.fontName, self.fontSize, 255, 255, 255, 0, 0, 0, 1, 960, 255, true)
    end
end

return DialogueBox
