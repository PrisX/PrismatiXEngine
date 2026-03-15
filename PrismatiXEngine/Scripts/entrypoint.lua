local loadedModules = {} -- Cache


local function include(path) -- 建議別動，如果你不知道這在幹麻
    if loadedModules[path] then
        return loadedModules[path]
    end

    local source = Engine.ReadAssetText(path, true)
    if not source then
        error("Unable to load Lua module: " .. path)
    end

    local chunk, err = load(source, "@" .. path)
    if not chunk then
        error("Failed to compile module " .. path .. ": " .. tostring(err))
    end

    local moduleValue = chunk()
    loadedModules[path] = moduleValue
    return moduleValue
end

local function normalize_script_path(rawPath, fallbackPath) -- 建議別動，如果你不知道這在幹麻
    local resolved = rawPath
    if resolved == nil or resolved == "" then
        resolved = fallbackPath
    end

    if string.find(resolved, "/", 1, true) == nil and string.find(resolved, "\\", 1, true) == nil then
        resolved = "Scripts/" .. resolved
    end

    return resolved
end

local function run_splash_script(path, required) -- 建議別動，如果你不知道這在幹麻
    if not Engine.RunScript(path, required) then
        return false
    end

    return Engine.CallGlobal("SplashScreen", required)
end

local function safe_call_global(name, ...) -- 建議別動，如果你不知道這在幹麻
    local fn = _G[name]
    if type(fn) ~= "function" then
        return
    end

    local ok, err = pcall(fn, ...)
    if not ok then
        print("Lua callback error (" .. tostring(name) .. "): " .. tostring(err))
    end
end

function Entrypoint()
    -- Global Functions
    _G.Ease = include("Scripts/common/easing.lua")

    local settings = {
        fontName = "NotoSansTC-Bold.ttf",
        fontSize = 28,
        nameFontName = "NotoSerifTC-Bold.ttf",
        nameFontSize = 28,
        initialBg = "bg.jpg",
        startScript = "chapter1.pds",
        splashScript = "Scripts/splash.lua"
    }

    local MainMenu = include("Scripts/components/main_menu.lua")
    local SaveLoadMenu = include("Scripts/components/save_load_menu.lua")
    local Toolbar = include("Scripts/components/toolbar.lua")
    Engine.RunScript("Scripts/effects.lua", false)

    local stateMainMenu = "MainMenu"
    local stateInGame = "InGame"
    local stateSaveMenu = "SaveMenu"
    local stateLoadMenu = "LoadMenu"

    local winW, winH = Engine.GetLogicalSize()
    local fontName = settings.fontName
    local fontSize = settings.fontSize
    local nameFontName = settings.nameFontName
    local nameFontSize = settings.nameFontSize

    local titleBg = settings.initialBg
    local startScriptFile = settings.startScript

    local userSplash = normalize_script_path(settings.splashScript, "Scripts/splash.lua")
    if not run_splash_script(userSplash, false) then
        return
    end

    if not run_splash_script("Scripts/engine_splash.lua", true) then
        return
    end

    local controller = Engine.CreateVNController(fontName, fontSize, nameFontName, nameFontSize)
    if not controller then
        error("Failed to create VNController")
    end

    local titleMenu = MainMenu.new(winW, winH, fontName, fontSize)
    local saveLoadMenu = SaveLoadMenu.new(fontName, fontSize, winW, winH)
    local bottomToolbar = Toolbar.new(fontName, fontSize, winH)

    local currentState = stateMainMenu
    local previousState = stateMainMenu

    local transition = {
        alpha = 0,
        phase = "idle",
        speed = 8,
        pending = nil
    }

    local function transition_active()
        return transition.phase ~= "idle"
    end

    local function start_transition(action)
        if transition_active() then
            return false
        end
        transition.phase = "enter"
        transition.alpha = 0
        transition.pending = action
        return true
    end

    local function update_transition()
        if transition.phase == "enter" then
            local reachedPeak
            transition.alpha, reachedPeak = Ease.fade_in(transition.alpha, transition.speed, 255)
            if reachedPeak then
                local pending = transition.pending
                transition.pending = nil
                if pending then
                    pending()
                end
                transition.phase = "leave"
            end
        elseif transition.phase == "leave" then
            local reachedClear
            transition.alpha, reachedClear = Ease.fade_out(transition.alpha, transition.speed, 0)
            if reachedClear then
                transition.phase = "idle"
            end
        end
    end

    local function draw_transition()
        if transition.alpha > 0 then
            Engine.DrawRect(0, 0, winW, winH, 0, 0, 0, transition.alpha)
        end
    end

    local mainMenuAlpha = 0.0
    local mainMenuFadeSpeed = 3.0

    while Engine.IsRunning() do
        Engine.HandleEvents()

        local isFading = transition_active()
        local mx = Engine.GetMouseX()
        local my = Engine.GetMouseY()
        local leftClick = Engine.GetLeftClick()
        local rightClick = Engine.GetRightClick()
        local wheelY = isFading and 0 or Engine.GetMouseWheelY()

        safe_call_global("OnEngineFrameUpdate", currentState, mx, my, leftClick, rightClick, wheelY)

        Engine.ClearScreen()

        if currentState == stateMainMenu then
            mainMenuAlpha = Ease.fade_in(mainMenuAlpha, mainMenuFadeSpeed, 255)
            Engine.DrawAuto(titleBg, DisplayMode.Fill, math.floor(mainMenuAlpha), 0, 0, 1.0)

            local action = titleMenu:update(mx, my, (not isFading) and leftClick)
            if not isFading then
                if action == "Start" then
                    Engine.PlaySFX("click.wav")
                    start_transition(function()
                        controller:LoadScript(startScriptFile)
                        currentState = stateInGame
                    end)
                elseif action == "Load" then
                    Engine.PlaySFX("click.wav")
                    start_transition(function()
                        previousState = stateMainMenu
                        currentState = stateLoadMenu
                        saveLoadMenu:open("load")
                    end)
                elseif action == "Exit" then
                    break
                end
            end

            titleMenu:render()
        elseif currentState == stateInGame then
            controller:RenderBackground()

            if not isFading then
                if controller:IsShowingBacklog() then
                    if wheelY ~= 0 then
                        controller:ScrollBacklog(wheelY > 0 and 1 or -1)
                    end

                    if rightClick or wheelY < 0 then
                        controller:ToggleBacklog()
                    end
                else
                    local toolbarAction = bottomToolbar:update(mx, my, leftClick)

                    if toolbarAction == "OpenSave" then
                        start_transition(function()
                            previousState = stateInGame
                            currentState = stateSaveMenu
                            saveLoadMenu:open("save")
                        end)
                    elseif toolbarAction == "OpenLoad" then
                        start_transition(function()
                            previousState = stateInGame
                            currentState = stateLoadMenu
                            saveLoadMenu:open("load")
                        end)
                    elseif toolbarAction == "TogglePin" then
                        Engine.PlaySFX("click.wav")
                    end

                    local blockingClick = bottomToolbar:is_mouse_over(my)
                    if (leftClick and not blockingClick) or wheelY < 0 then
                        controller:HandleClick(mx, my)
                    elseif wheelY > 0 then
                        controller:ToggleBacklog()
                    end
                end
            end

            controller:Update(mx, my)

            if not isFading then
                local pendingScriptTarget = controller:ConsumePendingScriptTransition()
                if pendingScriptTarget then
                    start_transition(function()
                        controller:LoadScript(pendingScriptTarget)
                        currentState = stateInGame
                    end)
                end
            end

            controller:Render()
            bottomToolbar:render(winW)
        elseif currentState == stateSaveMenu or currentState == stateLoadMenu then
            controller:RenderBackground()
            controller:Render()

            local selectedSlot = 0
            if not isFading then
                selectedSlot = saveLoadMenu:update(mx, my, leftClick)
            end

            if not isFading then
                if selectedSlot == -1 or rightClick then
                    start_transition(function()
                        currentState = previousState
                    end)
                elseif selectedSlot > 0 then
                    if currentState == stateSaveMenu then
                        if controller:SaveToSlot(selectedSlot) then
                            Engine.PlaySFX("click.wav")
                        end
                        saveLoadMenu:open("save")
                    else
                        local capturedSlot = selectedSlot
                        start_transition(function()
                            if controller:LoadFromSlot(capturedSlot) then
                                Engine.PlaySFX("click.wav")
                                currentState = stateInGame
                            end
                        end)
                    end
                end
            end

            saveLoadMenu:render()
        end

        update_transition()
        safe_call_global("OnEngineFrameRender", currentState, winW, winH)
        draw_transition()
        Engine.PresentScreen()
    end
end
