local Ease = {}

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

return Ease