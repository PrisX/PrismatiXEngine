local SaveLoadMenu = {}
SaveLoadMenu.__index = SaveLoadMenu

function SaveLoadMenu.new(fontName, fontSize, screenW, screenH)
    local self = setmetatable({}, SaveLoadMenu)

    self.fontName = fontName
    self.fontSize = fontSize
    self.mode = "save"
    self.slots = {}
    self.screenW = screenW
    self.screenH = screenH

    local startX = 200
    local startY = 150
    local slotW = 260
    local slotH = 120
    local gapX = 40
    local gapY = 40

    for i = 1, 9 do
        local col = (i - 1) % 3
        local row = math.floor((i - 1) / 3)
        self.slots[i] = {
            id = i,
            x = startX + col * (slotW + gapX),
            y = startY + row * (slotH + gapY),
            w = slotW,
            h = slotH,
            isEmpty = true,
            displayText = "NO DATA",
            isHovered = false
        }
    end

    self.btnClose = { x = 1100, y = 50, w = 100, h = 50 }
    self.hoverClose = false

    return self
end

local function point_in_rect(px, py, rect)
    return px >= rect.x and px <= rect.x + rect.w and py >= rect.y and py <= rect.y + rect.h
end

function SaveLoadMenu:open(newMode)
    self.mode = newMode
    for _, slot in ipairs(self.slots) do
        local info = Engine.PeekSaveSlot(slot.id)
        slot.isEmpty = info.isEmpty
        slot.displayText = info.displayText
    end
end

function SaveLoadMenu:update(mouseX, mouseY, isClicked)
    self.hoverClose = point_in_rect(mouseX, mouseY, self.btnClose)
    if isClicked and self.hoverClose then
        return -1
    end

    for _, slot in ipairs(self.slots) do
        local slotRect = { x = slot.x, y = slot.y, w = slot.w, h = slot.h }
        slot.isHovered = point_in_rect(mouseX, mouseY, slotRect)

        if isClicked and slot.isHovered then
            if self.mode == "load" and slot.isEmpty then
                return 0
            end
            return slot.id
        end
    end

    return 0
end

function SaveLoadMenu:render()
    Engine.DrawRect(0, 0, self.screenW, self.screenH, 0, 0, 0, 230)

    local title = self.mode == "save" and "--- 存檔 SAVE ---" or "--- 讀檔 LOAD ---"
    local textR, textG, textB = 255, 255, 255
    local hoverR, hoverG, hoverB = 255, 215, 0

    Engine.DrawTextOutline(title, 50, 40, self.fontName, self.fontSize, textR, textG, textB, 0, 0, 0, 2)

    if self.hoverClose then
        Engine.DrawText("Return", self.btnClose.x, self.btnClose.y, self.fontName, self.fontSize, hoverR, hoverG, hoverB)
    else
        Engine.DrawText("Return", self.btnClose.x, self.btnClose.y, self.fontName, self.fontSize, textR, textG, textB)
    end

    for _, slot in ipairs(self.slots) do
        if slot.isHovered then
            Engine.DrawRect(slot.x, slot.y, slot.w, slot.h, 80, 80, 80, 255)
        else
            Engine.DrawRect(slot.x, slot.y, slot.w, slot.h, 40, 40, 40, 255)
        end

        Engine.DrawRectOutline(slot.x, slot.y, slot.w, slot.h, 200, 200, 200, 255)

        Engine.DrawText("No." .. tostring(slot.id), slot.x + 10, slot.y + 10, self.fontName, self.fontSize, 150, 150, 150)

        if slot.isEmpty then
            Engine.DrawText(slot.displayText, slot.x + 20, slot.y + 50, self.fontName, self.fontSize, 100, 100, 100)
        else
            Engine.DrawText(slot.displayText, slot.x + 20, slot.y + 50, self.fontName, self.fontSize, 255, 255, 255)
        end
    end
end

return SaveLoadMenu
