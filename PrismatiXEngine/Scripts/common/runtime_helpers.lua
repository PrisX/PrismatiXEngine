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
