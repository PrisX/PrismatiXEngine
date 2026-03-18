local Utils = {}

function Utils.to_number(value, fallback)
    local parsed = tonumber(value)
    if parsed == nil then
        return fallback
    end
    return parsed
end

function Utils.clamp(value, minValue, maxValue)
    if value < minValue then
        return minValue
    end

    if value > maxValue then
        return maxValue
    end

    return value
end

function Utils.clamp_channel(value, fallback)
    local parsed = Utils.to_number(value, fallback)
    if parsed == nil then
        return fallback
    end

    if parsed < 0 then
        return 0
    end

    if parsed > 255 then
        return 255
    end

    return math.floor(parsed + 0.5)
end

return Utils
