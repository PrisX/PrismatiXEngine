local PlayScene = {}
PlayScene.__index = PlayScene

local Banner = include("Scripts/components/banner.lua")
local DialogueBox = include("Scripts/components/dialogue_box.lua")
local BacklogMenu = include("Scripts/components/backlog_menu.lua")

function PlayScene.new(fontName, fontSize)
    local self = setmetatable({}, PlayScene)
    self.fontName = fontName
    self.fontSize = fontSize
    self.vn = nil
    self.dialogueBox = nil
    self.backlog = nil
    self.bgmBanner = nil
    self.chapterBanner = nil
    return self
end

function PlayScene:enter()
    self.vn = Engine.CreateVNController(self.fontName, self.fontSize, self.fontName, self.fontSize)
    self.dialogueBox = DialogueBox.new(self.fontName, 26)
    self.backlog = BacklogMenu.new(self.fontName, 22)
    
    self.bgmBanner = Banner.new({
        y = 20, stayDuration = 150,
        bgColor = {20, 30, 50, 210}, textColor = {180, 220, 255}
    })

    self.chapterBanner = Banner.new({
        y = 100, stayDuration = 200,
        bgColor = {40, 20, 20, 220}, textColor = {255, 240, 180}
    })

    self.vn:LoadScript("chapter1.pds")
    _G.Notification:notify("Chapter 1: The Beginning", "info")
end

function PlayScene:update(mx, my, leftClick, rightClick)
    self.backlog:update(mx, my, leftClick, rightClick)

    if self.backlog.targetAlpha > 0 then
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
end

function PlayScene:exit()
    -- Clean up.. uh maybe?
end

return PlayScene
