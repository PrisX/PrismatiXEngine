-- Module loader
local loadedModules = {}

local function include(path)
    if loadedModules[path] then return loadedModules[path] end
    local source = Engine.ReadAssetText(path, true)
    if not source then error("Failed to load script: " .. path) end
    local chunk, err = load(source, "@" .. path)
    if not chunk then error("Lua parse error in " .. path .. ": " .. err) end
    local result = chunk()
    loadedModules[path] = result or true
    return result
end

_G.include = include

-- Global functions
_G.Utils   = include("Scripts/common/utils.lua")
_G.Runtime = include("Scripts/common/runtime_helpers.lua")
_G.UI      = include("Scripts/common/ui_framework.lua")
_G.Ease    = include("Scripts/common/easing.lua")

-- System modules
local SceneManager        = include("Scripts/common/scene_manager.lua")
local Transition          = include("Scripts/fx/transition.lua")
local NotificationManager = include("Scripts/components/notification.lua")
local FX                  = include("Scripts/fx/screen_fx.lua")

function Entrypoint()
    local fontName = "NotoSansTC-Bold.ttf"
    local fontSize = 32

    _G.PX = {
        Scene        = SceneManager.new(),
        Transition   = Transition.new({ style = "dissolve", speed = 10 }),
        Notification = NotificationManager.new(fontName, 20),
        FX           = FX,
    }

    -- Splash screens
    Runtime.run_splash_script("Scripts/engine_splash.lua")
    Runtime.run_splash_script("Scripts/splash.lua")

    -- Pre-load scenes
    local TitleScene = include("Scripts/scenes/title_scene.lua")
    include("Scripts/scenes/play_scene.lua")

    -- Start
    _G.PX.Scene:switch(TitleScene.new(fontName, fontSize))

    -- Main loop
    while Engine.IsRunning() do
        Engine.HandleEvents()
        local winW, winH = Engine.GetLogicalSize()
        local mx,   my   = Engine.GetMouseX(), Engine.GetMouseY()
        local leftClick  = Engine.GetLeftClick()
        local rightClick = Engine.GetRightClick()

        _G.PX.FX.update()
        local ox, oy = _G.PX.FX.get_offset()
        Engine.SetCameraOffset(ox, oy)

        _G.PX.Notification:update()
        _G.PX.Transition:update(winW)
        _G.PX.Scene:update(mx, my, leftClick, rightClick)

        Engine.ClearScreen(0, 0, 0, 255)
        _G.PX.Scene:render(winW, winH)

        _G.PX.Notification:render(winW, winH)
        _G.PX.Transition:draw_fullscreen(winW, winH)

        Engine.PresentScreen()
    end
end
