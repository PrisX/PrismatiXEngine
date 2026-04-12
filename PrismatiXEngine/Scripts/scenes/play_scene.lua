local PlayScene = {}
PlayScene.__index = PlayScene

local Banner = include("Scripts/components/banner.lua")
local DialogueBox = include("Scripts/components/dialogue_box.lua")
local BacklogMenu = include("Scripts/components/backlog_menu.lua")
local Toolbar = include("Scripts/components/toolbar.lua")

function PlayScene.new(fontName, fontSize)
    local self = setmetatable({}, PlayScene)
    self.fontName = fontName
    self.fontSize = fontSize
    self.vn = nil
    self.dialogueBox = nil
    self.backlog = nil
    self.toolbar = nil
    self.bgmBanner = nil
    self.chapterBanner = nil
    return self
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

    self.vn:LoadScript("chapter1.pds")
    _G.PX.Notification:notify("Chapter 1: The Beginning", "info")
end

function PlayScene:update(mx, my, leftClick, rightClick)
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

    self.vn:Update(mx, my)

    local pendingBgm = self.vn:PopPendingBgmInfo()
    if pendingBgm then self.bgmBanner:show("Music: " .. pendingBgm) end
    
    local pendingChapter = self.vn:PopPendingChapterInfo()
    if pendingChapter then self.chapterBanner:show(pendingChapter) end

    self.bgmBanner:update()
    self.chapterBanner:update()

    if rightClick then
        self.backlog:toggle()
    end

    if leftClick then
        self.vn:HandleClick(mx, my)
    end
end

function PlayScene:render(winW, winH)
    self.vn:RenderBackground()
    self.vn:Render()

    local state = self.vn:GetDialogueState()
    self.dialogueBox:render(state, winW, winH)
    
    self.bgmBanner:render()
    self.chapterBanner:render()

    self.backlog:render(winW, winH)
    self.toolbar:render(winW)
end

function PlayScene:exit()
    -- Clean up.. uh maybe?
end

return PlayScene
