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

function Entrypoint()
    -- Global Functions
    _G.Ease = include("Scripts/common/easing.lua")
    local Runtime = include("Scripts/common/runtime_helpers.lua")
    local Transition = include("Scripts/common/transition.lua")
    include("Scripts/common/portrait_animations.lua")

    local settings = {
        fontName = "NotoSansTC-Bold.ttf",
        fontSize = 28,
        nameFontName = "NotoSerifTC-Bold.ttf",
        nameFontSize = 28,
        transitionStyle = "fade",
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

    local userSplash = Runtime.normalize_script_path(settings.splashScript, "Scripts/splash.lua")
    if not Runtime.run_splash_script(userSplash, false) then
        return
    end

    if not Runtime.run_splash_script("Scripts/engine_splash.lua", true) then
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

    local transition = Transition.new({
        style = settings.transitionStyle,
        transitionSpeed = settings.transitionSpeed,
        ease = settings.transitionEase
    })
    local nextTransitionOptions = nil

    local function clone_transition_options(options)
        if type(options) ~= "table" then
            return nil
        end

        return {
            style = options.style or options.transition,
            transitionSpeed = options.transitionSpeed or options.speed,
            ease = options.ease
        }
    end

    local function build_transition_options(payload)
        if type(payload) ~= "table" then
            return nil
        end

        local options = {}
        local hasAny = false

        if payload.transition ~= nil and payload.transition ~= "" then
            options.style = payload.transition
            hasAny = true
        end

        if payload.transitionSpeed ~= nil and payload.transitionSpeed ~= "" then
            options.transitionSpeed = payload.transitionSpeed
            hasAny = true
        elseif payload.speed ~= nil and payload.speed ~= "" then
            options.transitionSpeed = payload.speed
            hasAny = true
        end

        if payload.ease ~= nil and payload.ease ~= "" then
            options.ease = payload.ease
            hasAny = true
        end

        return hasAny and options or nil
    end

    local function consume_next_transition_options()
        local options = nextTransitionOptions
        nextTransitionOptions = nil
        return options
    end

    local function start_transition(action, overrideOptions)
        local transitionOptions = overrideOptions
        if transitionOptions == nil then
            transitionOptions = consume_next_transition_options()
        else
            nextTransitionOptions = nil
        end
        return transition:start(action, transitionOptions)
    end

    function _G.SetNextTransition(options)
        nextTransitionOptions = clone_transition_options(options)
    end

    _G.VNController = controller
    _G.VN = _G.VN or {}
    _G.VN.controller = controller
    _G.VN.QueueScriptTransition = function(target, transitionStyle, transitionSpeed, transitionEase)
        local speedValue = transitionSpeed
        if speedValue ~= nil then
            speedValue = tostring(speedValue)
        end
        controller:QueueScriptTransition(target, transitionStyle, speedValue, transitionEase)
    end

    local mainMenuAlpha = 0.0
    local mainMenuFadeSpeed = 3.0

    while Engine.IsRunning() do
        Engine.HandleEvents()

        local isFading = transition:is_active()
        local input = Runtime.read_input_frame(isFading)
        local mx = input.mx
        local my = input.my
        local leftClick = input.leftClick
        local rightClick = input.rightClick
        local wheelY = input.wheelY

        Runtime.safe_call_global("OnEngineFrameUpdate", currentState, mx, my, leftClick, rightClick, wheelY)

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
                local pendingInlineTransition = controller:PopInlineTransition()
                if pendingInlineTransition then
                    local transitionOptions = build_transition_options(pendingInlineTransition)

                    start_transition(function()
                        controller:ContinueScript()
                    end, transitionOptions)
                else
                    local pendingScriptTransition = controller:PopScriptTransition()
                    if pendingScriptTransition then
                        local transitionOptions = build_transition_options(pendingScriptTransition)

                        start_transition(function()
                            controller:LoadScript(pendingScriptTransition.target)
                            currentState = stateInGame
                        end, transitionOptions)
                    end
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

        transition:update(winW)
        Runtime.safe_call_global("OnEngineFrameRender", currentState, winW, winH)
        transition:draw_fullscreen(winW, winH)
        Engine.PresentScreen()
    end
end
