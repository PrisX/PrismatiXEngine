local PlayScene = {}
PlayScene.__index = PlayScene

local Banner = include("Scripts/components/banner.lua")
local DialogueBox = include("Scripts/components/dialogue_box.lua")
local BacklogMenu = include("Scripts/components/backlog_menu.lua")
local Toolbar = include("Scripts/components/toolbar.lua")

local function resolve_script_path(path)
    if type(path) == "string" and path ~= "" then
        return path
    end
    if _G.PX and type(_G.PX.GeneratedSceneScript) == "string" and _G.PX.GeneratedSceneScript ~= "" then
        return _G.PX.GeneratedSceneScript
    end
    return "chapter1.pds"
end

local function to_number(value, fallback)
    local parsed = tonumber(value)
    if parsed == nil then
        return fallback
    end
    return parsed
end

function PlayScene.new(fontName, fontSize, scriptPath)
    local self = setmetatable({}, PlayScene)
    self.fontName = fontName
    self.fontSize = fontSize
    self.scriptPath = resolve_script_path(scriptPath)
    self.vn = nil
    self.dialogueBox = nil
    self.backlog = nil
    self.toolbar = nil
    self.bgmBanner = nil
    self.chapterBanner = nil
    self.generatedUI = nil
    self.lastMouseX = 0
    self.lastMouseY = 0
    return self
end

function PlayScene:install_editor_hooks()
    local scene = self

    _G.PXEditorTransition = function(args)
        if not (_G.PX and _G.PX.Transition) then
            return
        end

        _G.PX.Transition:start(nil, {
            style = args.style or "fade",
            speed = to_number(args.speed, 10)
        })
    end

    _G.PXEditorAnimateActor = function(args)
        local _ = args
    end

    _G.PXEditorSpawnUI = function(args)
        local path = args.ui_script or args.script
        if type(path) ~= "string" or path == "" then
            return
        end

        local ok, module = pcall(include, path)
        if not ok or type(module) ~= "table" then
            return
        end

        local winW, winH = Engine.GetLogicalSize()
        if type(module.build) == "function" then
            scene.generatedUI = module.build(winW, winH, scene.fontName, scene.fontSize)
        elseif type(module.new) == "function" then
            scene.generatedUI = module.new(winW, winH, scene.fontName, scene.fontSize)
        else
            scene.generatedUI = nil
        end
    end
end

function PlayScene:clear_editor_hooks()
    _G.PXEditorTransition = nil
    _G.PXEditorAnimateActor = nil
    _G.PXEditorSpawnUI = nil
end

function PlayScene:get_choice_rect(index, total, winW, winH)
    local width = math.min(winW - 160, 520)
    local height = 58
    local spacing = 14
    local totalHeight = total * height + math.max(0, total - 1) * spacing
    local x = math.floor((winW - width) * 0.5)
    local startY = math.floor(winH - 280 - totalHeight)
    local y = startY + (index - 1) * (height + spacing)
    return x, y, width, height
end

function PlayScene:render_choices(winW, winH)
    local choices = self.vn and self.vn:GetChoices() or {}
    if #choices == 0 then
        return
    end

    for index, choice in ipairs(choices) do
        local x, y, w, h = self:get_choice_rect(index, #choices, winW, winH)
        local hovered = Engine.IsMouseInRect(x, y, w, h)
        local bg = hovered and {52, 76, 118, 240} or {20, 28, 42, 224}
        local border = hovered and {255, 214, 143, 200} or {118, 144, 184, 120}

        Engine.DrawRect(x, y, w, h, bg[1], bg[2], bg[3], bg[4])
        Engine.DrawRect(x, y, w, 2, border[1], border[2], border[3], border[4])
        Engine.DrawRect(x, y + h - 2, w, 2, border[1], border[2], border[3], border[4])
        Engine.DrawRect(x, y, 2, h, border[1], border[2], border[3], border[4])
        Engine.DrawRect(x + w - 2, y, 2, h, border[1], border[2], border[3], border[4])
        Engine.DrawTextOutline(choice.text, x + 22, y + 16, self.fontName, 24, 240, 243, 247, 0, 0, 0, 2, 0, 255)
    end
end

function PlayScene:enter()
    local winW, winH = Engine.GetLogicalSize()
    self.vn = Engine.CreateVNController()
    self.dialogueBox = DialogueBox.new(self.fontName, 26)
    self.backlog = BacklogMenu.new(self.fontName, 22)
    self.toolbar = Toolbar.new(self.fontName, 18, winH)
    
    self.bgmBanner = Banner.new({
        y = 20, stayDuration = 150,
        bgColor = {20, 30, 50, 210}, textColor = {180, 220, 255}
    })

    self.chapterBanner = Banner.new({
        y = 100, stayDuration = 200,
        bgColor = {40, 20, 20, 220}, textColor = {255, 240, 180}
    })

    self:install_editor_hooks()
    self.vn:LoadScript(self.scriptPath)
    _G.PX.Notification:notify("Loaded scene script", "info")
end

function PlayScene:update(mx, my, leftClick, rightClick)
    self.lastMouseX = mx
    self.lastMouseY = my
    self.backlog:update(mx, my, leftClick, rightClick)

    if self.backlog.targetAlpha > 0 then
        return
    end

    local toolbarCmd = self.toolbar:update(mx, my, leftClick)
    if toolbarCmd == "OpenSave" then
        local ok = Engine.SaveGame(1, self.vn)
        _G.PX.Notification:notify(ok and "Game Saved (Slot 1)" or "Save Failed", ok and "info" or "warn")
    elseif toolbarCmd == "OpenLoad" then
        local ok = Engine.LoadGame(1, self.vn)
        _G.PX.Notification:notify(ok and "Game Loaded (Slot 1)" or "No Save Data", ok and "info" or "warn")
    end

    if self.toolbar:is_mouse_over(my) then
        return
    end

    if self.generatedUI and self.generatedUI.update then
        self.generatedUI:update(mx, my, leftClick)
    end

    self.vn:Update(mx, my)

    local choices = self.vn:GetChoices()
    if #choices > 0 and leftClick then
        local winW, winH = Engine.GetLogicalSize()
        for index, _ in ipairs(choices) do
            local x, y, w, h = self:get_choice_rect(index, #choices, winW, winH)
            if Engine.IsMouseInRect(x, y, w, h) then
                self.vn:SelectChoice(index)
                break
            end
        end
    end

    local pendingBgm = self.vn:PopPendingBgmInfo()
    if pendingBgm then self.bgmBanner:show("Music: " .. pendingBgm) end
    
    local pendingChapter = self.vn:PopPendingChapterInfo()
    if pendingChapter then self.chapterBanner:show(pendingChapter) end

    self.bgmBanner:update()
    self.chapterBanner:update()

    if rightClick then
        self.backlog:toggle()
    end

    if leftClick and #choices == 0 then
        self.vn:HandleClick(mx, my)
    end
end

function PlayScene:render(winW, winH)
    self.vn:RenderBackground()
    self.vn:Render()

    if self.generatedUI and self.generatedUI.render then
        self.generatedUI:render()
    end

    local state = self.vn:GetDialogueState()
    self.dialogueBox:render(state, winW, winH)
    self:render_choices(winW, winH)
    
    self.bgmBanner:render()
    self.chapterBanner:render()

    self.backlog:render(winW, winH)
    self.toolbar:render(winW)
end

function PlayScene:exit()
    self:clear_editor_hooks()
    self.generatedUI = nil
end

return PlayScene
