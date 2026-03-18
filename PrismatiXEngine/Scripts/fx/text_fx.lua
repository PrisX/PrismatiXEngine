local runtimeFx = {
    flash = nil
}
local Utils = _G.Utils

if type(Utils) ~= "table" then
    error("text_fx requires _G.Utils to be loaded before Scripts/fx/text_fx.lua")
end

-- [text name="AAAAAAAA" effect=""]
Engine.RegisterTextEffect("shake", function(ctx)
    local t = Utils.to_number(ctx.elapsedMs, 0)
    local dx = math.floor(math.sin(t * 0.05) * 3)
    local dy = math.floor(math.cos(t * 0.04) * 2)

    local tc = ctx.textColor
    local oc = ctx.outlineColor

    Engine.DrawTextOutline(
        ctx.text,
        ctx.x + dx,
        ctx.y + dy,
        ctx.fontName,
        ctx.fontSize,
        tc.r,
        tc.g,
        tc.b,
        oc.r,
        oc.g,
        oc.b,
        1,
        ctx.wrapLength,
        ctx.alpha,
        true
    )

    return true
end)

Engine.RegisterTextEffect("pulse", function(ctx)
    local t = Utils.to_number(ctx.elapsedMs, 0)
    local factor = 0.85 + 0.15 * math.sin(t * 0.01)

    local tc = ctx.textColor
    local oc = ctx.outlineColor

    local tr = Utils.clamp(math.floor(tc.r * factor), 0, 255)
    local tg = Utils.clamp(math.floor(tc.g * factor), 0, 255)
    local tb = Utils.clamp(math.floor(tc.b * factor), 0, 255)

    Engine.DrawTextOutline(
        ctx.text,
        ctx.x,
        ctx.y,
        ctx.fontName,
        ctx.fontSize,
        tr,
        tg,
        tb,
        oc.r,
        oc.g,
        oc.b,
        1,
        ctx.wrapLength,
        ctx.alpha,
        true
    )

    return true
end)

-- [lua fn=""]
function StartScreenFlash(args)
    local duration = math.max(1, Utils.to_number(args.duration, 20))
    runtimeFx.flash = {
        timer = duration,
        duration = duration,
        r = Utils.clamp(math.floor(Utils.to_number(args.r, 255)), 0, 255),
        g = Utils.clamp(math.floor(Utils.to_number(args.g, 255)), 0, 255),
        b = Utils.clamp(math.floor(Utils.to_number(args.b, 255)), 0, 255),
        a = Utils.clamp(math.floor(Utils.to_number(args.a, 180)), 0, 255)
    }
end

function OnEngineFrameUpdate(currentState, mx, my, leftClick, rightClick, wheelY)
    if runtimeFx.flash then
        runtimeFx.flash.timer = runtimeFx.flash.timer - 1
        if runtimeFx.flash.timer <= 0 then
            runtimeFx.flash = nil
        end
    end
end

function OnEngineFrameRender(currentState, screenW, screenH)
    if runtimeFx.flash then
        local p = runtimeFx.flash.timer / runtimeFx.flash.duration
        local alpha = Utils.clamp(math.floor(runtimeFx.flash.a * p), 0, 255)
        Engine.DrawRect(0, 0, screenW, screenH, runtimeFx.flash.r, runtimeFx.flash.g, runtimeFx.flash.b, alpha)
    end
end
