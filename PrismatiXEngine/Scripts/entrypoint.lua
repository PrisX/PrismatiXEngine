local loadedModules = {} -- Cache

local function include(path)
    if loadedModules[path] then return loadedModules[path] end
    local source = Engine.ReadAssetText(path, true)
    if not source then error("Failed to load script: " .. path) end
    local chunk, err = load(source, "@" .. path)
    if not chunk then error("Lua parse error in " .. path .. ": " .. err) end
    local result = chunk()
    loadedModules[path] = result
    return result
end

-- 1. Initialize Global Libraries
_G.include = include
_G.Utils = include("Scripts/common/utils.lua")
_G.Ease = include("Scripts/common/easing.lua")
_G.Runtime = include("Scripts/common/runtime_helpers.lua")
_G.UI = include("Scripts/common/ui_framework.lua")
_G.FX = include("Scripts/fx/screen_fx.lua")

-- 2. Import Global Systems
local SceneManager = include("Scripts/common/scene_manager.lua")
local Transition = include("Scripts/fx/transition.lua")
local NotificationManager = include("Scripts/components/notification.lua")

function Entrypoint()
    local fontName = "NotoSansTC-Bold.ttf"
    local fontSize = 32
    
    -- Initialize Global Systems
    _G.Scene = SceneManager.new()
    _G.Transition = Transition.new({ style = "dissolve", speed = 10 })
    _G.Notification = NotificationManager.new(fontName, 20)
    
    -- Run Splash Screens
    Runtime.run_splash_script("Scripts/engine_splash.lua")
    Runtime.run_splash_script("Scripts/splash.lua")

    -- Load Scenes
    local TitleScene = include("Scripts/scenes/title_scene.lua")
    local PlayScene = include("Scripts/scenes/play_scene.lua")

    -- Initial Scene Set
    _G.Scene:switch(TitleScene.new(fontName, fontSize))

    -- Main Loop
    while Engine.IsRunning() do
        -- Input & Events
        Engine.HandleEvents()
        local winW, winH = Engine.GetLogicalSize()
        local mx, my = Engine.GetMouseX(), Engine.GetMouseY()
        local leftClick = Engine.GetLeftClick()
        local rightClick = Engine.GetRightClick()

        -- Global Updates
        _G.FX.update()
        local ox, oy = _G.FX.get_offset()
        Engine.SetCameraOffset(ox, oy)
        
        _G.Notification:update()
        _G.Transition:update(winW)
        
        -- Scene Update
        _G.Scene:update(mx, my, leftClick, rightClick)
        
        -- Rendering Pass
        Engine.ClearScreen(0, 0, 0, 255)
        
        -- Scene Render
        _G.Scene:render(winW, winH)
        
        -- Global Overlay Rendering
        _G.Notification:render(winW, winH)
        _G.Transition:draw_fullscreen(winW, winH)
        
        Engine.PresentScreen()
    end
end
