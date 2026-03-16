local PortraitAnimations = {}

local function sample_ease(ctx, fallback)
    local easeName = ctx.ease
    if easeName == nil or easeName == "" then
        easeName = fallback or "linear"
    end
    return Ease.sample(easeName, ctx.progress or 0)
end

local function register(name, fn)
    PortraitAnimations[name] = fn
    Engine.RegisterPortraitAnimation(name, fn)
end

register("fade", function(ctx)
    return {}
end)

register("none", function(ctx)
    return {}
end)

register("slide_left", function(ctx)
    local t = sample_ease(ctx, "ease_out_cubic")
    return {
        offsetX = Ease.lerp(-140, 0, t)
    }
end)

register("slide_right", function(ctx)
    local t = sample_ease(ctx, "ease_out_cubic")
    return {
        offsetX = Ease.lerp(140, 0, t)
    }
end)

register("slide_up", function(ctx)
    local t = sample_ease(ctx, "ease_out_cubic")
    return {
        offsetY = Ease.lerp(96, 0, t)
    }
end)

register("slide_down", function(ctx)
    local t = sample_ease(ctx, "ease_out_cubic")
    return {
        offsetY = Ease.lerp(-96, 0, t)
    }
end)

register("pop", function(ctx)
    local t = sample_ease(ctx, "ease_out_back")
    return {
        scale = Ease.lerp(0.82, 1.0, t)
    }
end)

register("bounce", function(ctx)
    local eased = sample_ease(ctx, "ease_out_cubic")
    local amplitude = Ease.lerp(42, 0, eased)
    local wave = math.abs(math.sin((ctx.progress or 0) * math.pi * 2.25))
    return {
        offsetY = -amplitude * wave
    }
end)

register("zoom", function(ctx)
    local t = sample_ease(ctx, "smoothstep")
    return {
        scale = Ease.lerp(1.14, 1.0, t)
    }
end)

return PortraitAnimations