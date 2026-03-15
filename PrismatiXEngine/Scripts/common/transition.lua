local Transition = {}
Transition.__index = Transition

local DISSOLVE_CELL_SIZE = 32

local function clamp(value, minValue, maxValue)
    return math.max(minValue, math.min(maxValue, value))
end

local function to_number(value, fallback)
    local parsed = tonumber(value)
    if parsed == nil then
        return fallback
    end
    return parsed
end

local function copy_color(color, fallback)
    if type(color) ~= "table" then
        return { fallback[1], fallback[2], fallback[3] }
    end

    return {
        to_number(color[1], fallback[1]),
        to_number(color[2], fallback[2]),
        to_number(color[3], fallback[3])
    }
end

local function normalize_style(style)
    local lowered = string.lower(tostring(style or "fade"))
    if lowered == "wipe" then
        return "wipe"
    elseif lowered == "dissolve" then
        return "dissolve"
    end
    return "fade"
end

local function default_ease_name(style)
    if style == "wipe" then
        return "ease_out_cubic"
    elseif style == "dissolve" then
        return "smoothstep"
    end
    return "linear"
end

local function hash01(ix, iy)
    local n = math.sin(ix * 12.9898 + iy * 78.233) * 43758.5453
    return n - math.floor(n)
end

local function draw_dissolve(width, height, progress, reverseOrder, color)
    local r = color[1] or 0
    local g = color[2] or 0
    local b = color[3] or 0
    local cellSize = DISSOLVE_CELL_SIZE

    local y = 0
    while y < height do
        local cellY = math.floor(y / cellSize)
        local drawH = math.min(cellSize, height - y)

        local x = 0
        while x < width do
            local cellX = math.floor(x / cellSize)
            local drawW = math.min(cellSize, width - x)
            local threshold = hash01(cellX + 1, cellY + 1)

            local shouldDraw
            if reverseOrder then
                shouldDraw = threshold >= progress
            else
                shouldDraw = threshold <= progress
            end

            if shouldDraw then
                Engine.DrawRect(x, y, drawW, drawH, r, g, b, 255)
            end

            x = x + cellSize
        end

        y = y + cellSize
    end
end

local function compute_fade_alpha_step(speed, peakAlpha)
    local safePeak = math.max(1, peakAlpha or 255)
    local safeSpeed = to_number(speed, 8)

    if safeSpeed == nil or safeSpeed <= 0 then
        safeSpeed = 8
    end

    -- Keep compatibility with old values while allowing large values as duration frames.
    if safeSpeed <= 32 then
        return safeSpeed
    end

    return safePeak / safeSpeed
end

local function parse_options(speedOrOptions, peakAlpha)
    if type(speedOrOptions) == "table" then
        return speedOrOptions
    end

    return {
        speed = speedOrOptions,
        peakAlpha = peakAlpha
    }
end

function Transition.new(speedOrOptions, peakAlpha)
    local options = parse_options(speedOrOptions, peakAlpha)

    local self = setmetatable({}, Transition)
    self.alpha = 0
    self.fadeProgress = 0
    self.wipeEdge = 0
    self.wipeProgress = 0
    self.dissolveProgress = 0
    self.phase = "idle"
    self.pending = nil
    self.defaultStyle = normalize_style(options.style)
    self.defaultSpeed = options.transitionSpeed or options.speed
    self.defaultPeakAlpha = options.peakAlpha or 255
    self.defaultWipeColor = copy_color(options.wipeColor, { 0, 0, 0 })
    self.defaultEase = options.ease

    self.style = self.defaultStyle
    self.speed = 0
    self.peakAlpha = self.defaultPeakAlpha
    self.wipeColor = copy_color(self.defaultWipeColor, { 0, 0, 0 })
    self.ease = self.defaultEase

    self:apply_options(nil)

    return self
end

function Transition:apply_options(override)
    local style = self.defaultStyle
    local speed = self.defaultSpeed
    local peakAlpha = self.defaultPeakAlpha
    local wipeColor = self.defaultWipeColor
    local ease = self.defaultEase

    if type(override) == "table" then
        if override.style ~= nil then
            style = normalize_style(override.style)
        end
        if override.transitionSpeed ~= nil then
            speed = to_number(override.transitionSpeed, speed)
        end
        if override.speed ~= nil then
            speed = to_number(override.speed, speed)
        end
        if override.peakAlpha ~= nil then
            peakAlpha = to_number(override.peakAlpha, peakAlpha)
        end
        if override.wipeColor ~= nil then
            wipeColor = copy_color(override.wipeColor, wipeColor)
        end
        if override.ease ~= nil then
            ease = override.ease
        end
    end

    if ease == nil or ease == "" then
        ease = default_ease_name(style)
    end

    self.style = style
    self.peakAlpha = peakAlpha
    self.wipeColor = copy_color(wipeColor, { 0, 0, 0 })
    self.ease = ease

    if self.style == "wipe" then
        -- wipe speed is pixels/frame
        self.speed = speed or 100
    elseif self.style == "dissolve" then
        -- dissolve speed is alpha/frame
        self.speed = speed or 8
    else
        -- fade speed is alpha/frame
        self.speed = speed or 8
        self.style = "fade"
    end
end

function Transition:is_active()
    return self.phase ~= "idle"
end

function Transition:start(action, transitionOptions)
    if self:is_active() then
        return false
    end

    self:apply_options(transitionOptions)
    self.phase = "enter"
    self.alpha = 0
    self.fadeProgress = 0
    self.wipeEdge = 0
    self.wipeProgress = 0
    self.dissolveProgress = 0
    self.pending = action
    return true
end

function Transition:update(width)
    local viewportWidth = width or 0

    if self.style == "wipe" then
        local safeWidth = math.max(0, viewportWidth)
        local speed = self.speed
        if speed == nil or speed <= 0 then
            speed = 1
        end

        local durationFrames = math.max(1, safeWidth / speed)
        local progressStep = 1.0 / durationFrames

        if self.phase == "enter" then
            self.wipeProgress = clamp(self.wipeProgress + progressStep, 0, 1)
            local easedT = Ease.sample(self.ease, self.wipeProgress)
            self.wipeEdge = Ease.lerp(0, safeWidth, easedT)
            if self.wipeProgress >= 1 then
                local pendingAction = self.pending
                self.pending = nil
                if pendingAction then
                    pendingAction()
                end

                self.phase = "leave"
                self.wipeEdge = 0
                self.wipeProgress = 0
            end
        elseif self.phase == "leave" then
            self.wipeProgress = clamp(self.wipeProgress + progressStep, 0, 1)
            local easedT = Ease.sample(self.ease, self.wipeProgress)
            self.wipeEdge = Ease.lerp(0, safeWidth, easedT)
            if self.wipeProgress >= 1 then
                self.phase = "idle"
                self.wipeEdge = 0
                self.wipeProgress = 0
            end
        end

        return
    end

    if self.style == "dissolve" then
        local speed = self.speed
        local alphaStep = compute_fade_alpha_step(speed, 255)
        local progressStep = alphaStep / 255.0

        if self.phase == "enter" then
            self.dissolveProgress = clamp(self.dissolveProgress + progressStep, 0, 1)
            if self.dissolveProgress >= 1 then
                local pendingAction = self.pending
                self.pending = nil
                if pendingAction then
                    pendingAction()
                end
                self.phase = "leave"
                self.dissolveProgress = 0
            end
        elseif self.phase == "leave" then
            self.dissolveProgress = clamp(self.dissolveProgress + progressStep, 0, 1)
            if self.dissolveProgress >= 1 then
                self.phase = "idle"
                self.dissolveProgress = 0
            end
        end

        return
    end

    local peakAlpha = math.min(255, math.max(0, self.peakAlpha or 255))
    local speed = self.speed
    local alphaStep = compute_fade_alpha_step(speed, peakAlpha)

    if self.phase == "enter" then
        self.alpha = clamp(self.alpha + alphaStep, 0, peakAlpha)
        if self.alpha >= peakAlpha then
            local pendingAction = self.pending
            self.pending = nil
            if pendingAction then
                pendingAction()
            end
            self.phase = "leave"
        end
    elseif self.phase == "leave" then
        self.alpha = clamp(self.alpha - alphaStep, 0, peakAlpha)
        if self.alpha <= 0 then
            self.phase = "idle"
            self.alpha = 0
        end
    end
end

function Transition:draw_fullscreen(width, height)
    if self.style == "wipe" then
        local r = self.wipeColor[1] or 0
        local g = self.wipeColor[2] or 0
        local b = self.wipeColor[3] or 0

        if self.phase == "enter" then
            local w = clamp(math.floor(self.wipeEdge), 0, width)
            if w > 0 then
                Engine.DrawRect(0, 0, w, height, r, g, b, 255)
            end
        elseif self.phase == "leave" then
            local x = clamp(math.floor(self.wipeEdge), 0, width)
            local w = width - x
            if w > 0 then
                Engine.DrawRect(x, 0, w, height, r, g, b, 255)
            end
        end

        return
    end

    if self.style == "dissolve" then
        local easedT = Ease.sample(self.ease, self.dissolveProgress)
        if self.phase == "enter" then
            draw_dissolve(width, height, easedT, false, self.wipeColor)
        elseif self.phase == "leave" then
            draw_dissolve(width, height, easedT, true, self.wipeColor)
        end
        return
    end

    if self.alpha > 0 then
        Engine.DrawRect(0, 0, width, height, 0, 0, 0, self.alpha)
    end
end

return Transition
