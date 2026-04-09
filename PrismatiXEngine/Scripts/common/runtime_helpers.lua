local Runtime = {}

function Runtime.normalize_script_path(rawPath, fallbackPath)
    local resolved = rawPath
    if resolved == nil or resolved == "" then
        resolved = fallbackPath
    end

    if string.find(resolved, "/", 1, true) == nil and string.find(resolved, "\\", 1, true) == nil then
        resolved = "Scripts/" .. resolved
    end

    return resolved
end

-- Global Background State for Splash/Simple scripts
local currentBg = nil
local currentBgMode = 0

function Engine.FadeInBg(path, mode, duration, r, g, b, a)
    currentBg = path
    currentBgMode = mode
    local startTicks = Engine.GetTicks()
    local endTicks = startTicks + (duration or 1000)
    
    while Engine.IsRunning() do
        local now = Engine.GetTicks()
        local progress = (now - startTicks) / (duration or 1000)
        if progress > 1.0 then progress = 1.0 end
        
        Engine.HandleEvents()
        local winW, winH = Engine.GetLogicalSize()
        Engine.ClearScreen(0, 0, 0, 255)
        
        if currentBg then
            Engine.DrawAuto(currentBg, currentBgMode, progress * 255)
        end
        
        Engine.PresentScreen()
        if progress >= 1.0 then break end
    end
end

function Engine.FadeOutBg(path, mode, duration, r, g, b, a)
    local startTicks = Engine.GetTicks()
    local endTicks = startTicks + (duration or 1000)
    
    while Engine.IsRunning() do
        local now = Engine.GetTicks()
        local progress = 1.0 - ((now - startTicks) / (duration or 1000))
        if progress < 0.0 then progress = 0.0 end
        
        Engine.HandleEvents()
        local winW, winH = Engine.GetLogicalSize()
        Engine.ClearScreen(0, 0, 0, 255)
        
        if currentBg then
            Engine.DrawAuto(currentBg, currentBgMode, progress * 255)
        end
        
        Engine.PresentScreen()
        if progress <= 0.0 then 
            currentBg = nil
            break 
        end
    end
end

function Runtime.run_splash_script(path, required)
    if not Engine.RunScript(path, required) then
        return false
    end

    return Engine.CallGlobal("SplashScreen", required)
end

function Runtime.safe_call_global(name, ...)
    local fn = _G[name]
    if type(fn) ~= "function" then
        return
    end

    local ok, err = pcall(fn, ...)
    if not ok then
        print("Lua callback error (" .. tostring(name) .. "): " .. tostring(err))
    end
end

function Runtime.read_input_frame(isFading)
    return {
        mx = Engine.GetMouseX(),
        my = Engine.GetMouseY(),
        leftClick = Engine.GetLeftClick(),
        rightClick = Engine.GetRightClick(),
        wheelY = isFading and 0 or Engine.GetMouseWheelY()
    }
end

return Runtime
