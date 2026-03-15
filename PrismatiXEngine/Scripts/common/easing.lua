local Ease = {}

local function clamp01(t)
    if t < 0 then
        return 0
    end
    if t > 1 then
        return 1
    end
    return t
end

local function normalize_name(name)
    local key = tostring(name or "linear")
    key = string.lower(key)
    key = string.gsub(key, "-", "_")
    return key
end

function Ease.approach(current, target, step)
    if current < target then
        local nextValue = math.min(target, current + step)
        return nextValue, nextValue == target
    elseif current > target then
        local nextValue = math.max(target, current - step)
        return nextValue, nextValue == target
    end

    return current, true
end

function Ease.linear(t)
    return clamp01(t)
end

function Ease.ease_in_quad(t)
    local x = clamp01(t)
    return x * x
end

function Ease.ease_out_quad(t)
    local x = clamp01(t)
    return x * (2 - x)
end

function Ease.ease_in_out_quad(t)
    local x = clamp01(t)
    if x < 0.5 then
        return 2 * x * x
    end
    return -1 + (4 - 2 * x) * x
end

function Ease.ease_out_cubic(t)
    local x = clamp01(t) - 1
    return x * x * x + 1
end

function Ease.ease_in_out_cubic(t)
    local x = clamp01(t)
    if x < 0.5 then
        return 4 * x * x * x
    end
    local f = x - 1
    return 1 + 4 * f * f * f
end

function Ease.ease_out_back(t)
    local x = clamp01(t)
    local c1 = 1.70158
    local c3 = c1 + 1.0
    local f = x - 1.0
    return 1.0 + c3 * f * f * f + c1 * f * f
end

function Ease.smoothstep(t)
    local x = clamp01(t)
    return x * x * (3 - 2 * x)
end

function Ease.lerp(fromValue, toValue, t)
    return fromValue + (toValue - fromValue) * clamp01(t)
end

local EASING_MAP = {
    linear = Ease.linear,
    in_quad = Ease.ease_in_quad,
    out_quad = Ease.ease_out_quad,
    in_out_quad = Ease.ease_in_out_quad,
    out_cubic = Ease.ease_out_cubic,
    in_out_cubic = Ease.ease_in_out_cubic,
    out_back = Ease.ease_out_back,
    smoothstep = Ease.smoothstep,
    ease_in_quad = Ease.ease_in_quad,
    ease_out_quad = Ease.ease_out_quad,
    ease_in_out_quad = Ease.ease_in_out_quad,
    ease_out_cubic = Ease.ease_out_cubic,
    ease_in_out_cubic = Ease.ease_in_out_cubic,
    ease_out_back = Ease.ease_out_back,
}

function Ease.sample(name, t)
    local key = normalize_name(name)
    local fn = EASING_MAP[key] or Ease.linear
    return fn(t)
end

function Ease.fade_in(current, step, maxValue)
    local peak = maxValue or 255
    return Ease.approach(current, peak, step)
end

function Ease.fade_out(current, step, minValue)
    local floor = minValue or 0
    return Ease.approach(current, floor, step)
end

-- 其實指數衰減搬過來也還好，每個語言搞 Easing 都差不多
function Ease.exp_decay(current, target, factor, snapEpsilon)
    local epsilon = snapEpsilon or 0.01
    local nextValue = current + (target - current) * factor
    if math.abs(nextValue - target) <= epsilon then
        return target, true
    end
    return nextValue, false
end

-- 我真的不知道該用駝峰還是底線，所以兩個都寫w
Ease.Linear = Ease.linear
Ease.EaseInQuad = Ease.ease_in_quad
Ease.EaseOutQuad = Ease.ease_out_quad
Ease.EaseInOutQuad = Ease.ease_in_out_quad
Ease.EaseOutCubic = Ease.ease_out_cubic
Ease.EaseInOutCubic = Ease.ease_in_out_cubic
Ease.EaseOutBack = Ease.ease_out_back
Ease.SmoothStep = Ease.smoothstep
Ease.Lerp = Ease.lerp

return Ease