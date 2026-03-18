local DialogueBox = {}
DialogueBox.__index = DialogueBox
local Utils = _G.Utils

if type(Utils) ~= "table" then
    error("DialogueBox requires _G.Utils to be loaded before Scripts/components/dialogue_box.lua")
end

local function read_color(value, fallback)
    if type(value) ~= "table" then
        return { fallback[1], fallback[2], fallback[3] }
    end

    local r = Utils.clamp_channel(value.r or value[1], fallback[1])
    local g = Utils.clamp_channel(value.g or value[2], fallback[2])
    local b = Utils.clamp_channel(value.b or value[3], fallback[3])
    return { r, g, b }
end

local function build_effect_color(color)
    return {
        r = Utils.clamp_channel(color[1], 255),
        g = Utils.clamp_channel(color[2], 255),
        b = Utils.clamp_channel(color[3], 255),
        a = 255
    }
end

local function try_render_text_effect(box, text, x, y, wrapLength, alpha)
    if box.activeTextEffect == "" or text == "" then
        return false
    end

    local effects = rawget(_G, "TextEffects")
    if type(effects) ~= "table" then
        return false
    end

    local fx = effects[box.activeTextEffect]
    if type(fx) ~= "function" then
        return false
    end

    local ctx = {
        text = text,
        speaker = box.currentSpeakerName,
        x = x,
        y = y,
        wrapLength = wrapLength,
        alpha = alpha,
        fontName = box.fontName,
        fontSize = box.fontSize,
        effect = box.activeTextEffect,
        elapsedMs = box.effectElapsedMs,
        progress = box.effectProgress,
        textColor = build_effect_color(box.textColor),
        outlineColor = build_effect_color(box.outlineColor)
    }

    local ok, result = pcall(fx, ctx)
    if not ok then
        print("Text effect runtime error (" .. tostring(box.activeTextEffect) .. "): " .. tostring(result))
        box.activeTextEffect = ""
        return false
    end

    if result == nil then
        return true
    end

    return result == true
end

function DialogueBox.new(fontName, fontSize)
    local self = setmetatable({}, DialogueBox)

    self.fontName = fontName
    self.fontSize = fontSize
    self.nameFontName = fontName
    self.nameFontSize = fontSize

    self.currentDisplayText = ""
    self.displayedText = ""
    self.currentSpeakerName = ""

    self.fadeAlpha = 255

    self.textColor = { 255, 255, 255 }
    self.outlineColor = { 0, 0, 0 }
    self.activeTextEffect = ""
    self.effectElapsedMs = 0
    self.effectProgress = 1.0

    return self
end

function DialogueBox:set_name_font(fontName, fontSize)
    self.nameFontName = fontName
    self.nameFontSize = fontSize
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
        local nw = nameRect.w > 0 and nameRect.w or 220
        local nh = nameRect.h > 0 and nameRect.h or 50
        local nameSize = Engine.MeasureText(self.currentSpeakerName, self.nameFontName, self.nameFontSize)
        local tx = nx + math.floor((nw - nameSize.w) * 0.5)
        local ty = ny + math.floor((nh - nameSize.h) * 0.5)
        Engine.DrawTextOutline(self.currentSpeakerName, tx, ty, self.nameFontName, self.nameFontSize, 255, 255, 255, 60, 30, 80, 1)
    end

    if self.currentDisplayText ~= "" then
        local tx = math.floor((screenW - 960) / 2)
        local ty = boxY - 20
        local c = self.textColor
        local o = self.outlineColor

        if self.activeTextEffect == "" then
            if self.fadeAlpha < 255 then
                Engine.DrawTextOutline(self.currentDisplayText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, self.fadeAlpha, true)
                if self.displayedText ~= "" then
                    Engine.DrawTextOutline(self.displayedText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, 255, true)
                end
            else
                Engine.DrawTextOutline(self.currentDisplayText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, 255, true)
            end
        else
            if self.fadeAlpha < 255 then
                local handled = try_render_text_effect(self, self.currentDisplayText, tx, ty, 960, self.fadeAlpha)
                if not handled then
                    Engine.DrawTextOutline(self.currentDisplayText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, self.fadeAlpha, true)
                end
                if self.displayedText ~= "" then
                    Engine.DrawTextOutline(self.displayedText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, 255, true)
                end
            else
                local handled = try_render_text_effect(self, self.currentDisplayText, tx, ty, 960, 255)
                if not handled then
                    Engine.DrawTextOutline(self.currentDisplayText, tx, ty, self.fontName, self.fontSize, c[1], c[2], c[3], o[1], o[2], o[3], 1, 960, 255, true)
                end
            end
        end
    end
end

function DialogueBox:render_from_context(ctx)
    if type(ctx) ~= "table" then
        return false
    end

    if ctx.visible == false then
        return false
    end

    self.currentSpeakerName = tostring(ctx.speaker or "")
    self.currentDisplayText = tostring(ctx.currentText or "")
    self.displayedText = tostring(ctx.displayedText or "")
    self.fadeAlpha = Utils.clamp_channel(ctx.fadeAlpha, 255)
    self.textColor = read_color(ctx.textColor, { 255, 255, 255 })
    self.outlineColor = read_color(ctx.outlineColor, { 0, 0, 0 })
    self.activeTextEffect = tostring(ctx.effect or "")
    self.effectElapsedMs = Utils.to_number(ctx.elapsedMs, 0)
    self.effectProgress = Utils.to_number(ctx.progress, 1.0)

    local screenW = Utils.to_number(ctx.screenW, 1280)
    local screenH = Utils.to_number(ctx.screenH, 720)
    self:render(screenW, screenH)
    return true
end

return DialogueBox
