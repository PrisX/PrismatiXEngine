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

-- Global Variables
_G.include = include
_G.Utils = include("Scripts/common/utils.lua")
_G.Ease = include("Scripts/common/easing.lua")
_G.Runtime = include("Scripts/common/runtime_helpers.lua")

-- UI Framework
_G.UI = include("Scripts/common/ui_framework.lua")
local Banner = include("Scripts/components/banner.lua")
local DialogueBox = include("Scripts/components/dialogue_box.lua")
local MainMenu = include("Scripts/components/main_menu.lua")
local SaveLoadMenu = include("Scripts/components/save_load_menu.lua")

function Entrypoint()
    local fontName = "NotoSansTC-Bold.ttf"
    local fontSize = 32
    local dialogueBox = DialogueBox.new(fontName, 26)
    local saveLoadMenu = SaveLoadMenu.new(fontName, 24)

    local bgmBanner = Banner.new({
        y = 20, stayDuration = 150,
        bgColor = {20, 30, 50, 210}, textColor = {180, 220, 255}
    })

    local chapterBanner = Banner.new({
        y = 100, stayDuration = 200,
        bgColor = {40, 20, 20, 220}, textColor = {255, 240, 180}
    })

    local vn = Engine.CreateVNController(fontName, fontSize, fontName, fontSize)
    local currentState = "Splash" -- Splash, Title, Playing, SaveLoad

    local function start_game()
        currentState = "Playing"
        vn:LoadScript("chapter1.pds")
    end

    local titleMenu = MainMenu.new(1280, 720, fontName, fontSize)

    -- Handle Splash logic once
    local splashPlayed = false

    while Engine.IsRunning() do
        Engine.HandleEvents()
        local winW, winH = Engine.GetLogicalSize()
        local mx, my = Engine.GetMouseX(), Engine.GetMouseY()
        local leftClick = Engine.GetLeftClick()
        local rightClick = Engine.GetRightClick()

        Engine.ClearScreen(0, 0, 0, 255)

        if currentState == "Splash" then
            if not splashPlayed then
                include("Scripts/engine_splash.lua")
                if _G.SplashScreen then _G.SplashScreen() end
                splashPlayed = true
                currentState = "Title"
            else
                currentState = "Title"
            end
        elseif currentState == "Title" then
            Engine.DrawAuto("title_bg.png", DisplayMode.Fill, 255)
            local action = titleMenu:update(mx, my, leftClick)
            if action == "start" then
                start_game()
            elseif action == "load" then
                currentState = "SaveLoad"
                saveLoadMenu:set_mode("load", "Title")
            elseif action == "exit" then
                break
            end
            titleMenu:render()
        elseif currentState == "Playing" then
            vn:Update(mx, my)

            local pendingBgm = vn:PopPendingBgmInfo()
            if pendingBgm then bgmBanner:show("Music: " .. pendingBgm) end
            
            local pendingChapter = vn:PopPendingChapterInfo()
            if pendingChapter then chapterBanner:show(pendingChapter) end

            bgmBanner:update()
            chapterBanner:update()

            vn:RenderBackground()
            vn:Render()

            local ctx = vn:GetDialogueBoxContext()
            dialogueBox:render_from_context(ctx)
            
            if rightClick then
                vn:ToggleBacklog()
            end

            if leftClick and not vn:IsShowingBacklog() then
                vn:HandleClick(mx, my)
            end
        elseif currentState == "SaveLoad" then
            local action = saveLoadMenu:update(mx, my, leftClick, rightClick)
            if action == "back" then currentState = saveLoadMenu.returnState end
            saveLoadMenu:render()
        end

        bgmBanner:render()
        chapterBanner:render()
        Engine.PresentScreen()
    end
end
