local loadedModules = {}

local function include(path)
    if loadedModules[path] then
        return loadedModules[path]
    end

    local source = Engine.ReadAssetText(path, true)
    if not source then
        error("Failed to load script: " .. path)
    end

    local chunk, err = load(source, "@" .. path)
    if not chunk then
        error("Lua parse error in " .. path .. ": " .. err)
    end

    local result = chunk()
    loadedModules[path] = result or true
    return result
end

_G.include = include
_G.Utils = include("Scripts/common/utils.lua")
_G.Ease = include("Scripts/common/easing.lua")
_G.Runtime = include("Scripts/common/runtime_helpers.lua")
_G.UI = include("Scripts/common/ui_framework.lua")

local SceneManager = include("Scripts/common/scene_manager.lua")
local Transition = include("Scripts/fx/transition.lua")
local NotificationManager = include("Scripts/components/notification.lua")
local FX = include("Scripts/fx/screen_fx.lua")

local function resolve_preview_scene()
    return _G.__preview_scene_path or "Scripts/scenes/title_scene.lua"
end

function EditorPreviewBoot()
    local font_name = _G.__preview_font_name or "NotoSansTC-Bold.ttf"
    local font_size = _G.__preview_font_size or 32

    _G.PX = {
        Scene = SceneManager.new(),
        Transition = Transition.new({ style = "dissolve", speed = 10 }),
        Notification = NotificationManager.new(font_name, 20),
        FX = FX,
        GeneratedSceneScript = _G.__preview_generated_scene_script,
    }

    -- Compatibility
    _G.Scene = _G.PX.Scene
    _G.Transition = _G.PX.Transition
    _G.Notification = _G.PX.Notification
    _G.FX = _G.PX.FX

    local SceneModule = include(resolve_preview_scene())
    _G.PX.Scene:switch(SceneModule.new(font_name, font_size, _G.PX.GeneratedSceneScript))
end

function EditorPreviewFrame()
    local win_w, win_h = Engine.GetLogicalSize()
    local mouse_x, mouse_y = Engine.GetMouseX(), Engine.GetMouseY()
    local left_click = Engine.GetLeftClick()
    local right_click = Engine.GetRightClick()

    _G.PX.FX.update()
    local offset_x, offset_y = _G.PX.FX.get_offset()
    Engine.SetCameraOffset(offset_x, offset_y)

    _G.PX.Notification:update()
    _G.PX.Transition:update(win_w)
    _G.PX.Scene:update(mouse_x, mouse_y, left_click, right_click)

    Engine.ClearScreen(0, 0, 0, 255)
    _G.PX.Scene:render(win_w, win_h)
    _G.PX.Notification:render(win_w, win_h)
    _G.PX.Transition:draw_fullscreen(win_w, win_h)

    Engine.ResetCameraOffset()
end
