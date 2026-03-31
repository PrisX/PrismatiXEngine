local UI = {}
UI.__index = UI

-- Global UI State
local components = {}
local nextID = 1

function UI.new_component(props)
    local self = setmetatable({}, UI)
    self.id = nextID
    nextID = nextID + 1
    
    self.x = props.x or 0
    self.y = props.y or 0
    self.w = props.w or 0
    self.h = props.h or 0
    self.alpha = props.alpha or 255
    self.visible = props.visible ~= false
    self.anchor = props.anchor or "TopLeft"
    
    self.children = {}
    return self
end

function UI:add_child(child)
    table.insert(self.children, child)
    child.parent = self
end

function UI:get_absolute_pos()
    if self.parent then
        local px, py = self.parent:get_absolute_pos()
        return px + self.x, py + self.y
    end
    return self.x, self.y
end

function UI:is_hovered()
    if not self.visible then return false end
    local ax, ay = self:get_absolute_pos()
    return Engine.IsMouseInRect(math.floor(ax), math.floor(ay), math.floor(self.w), math.floor(self.h))
end

function UI:render_base()
    if not self.visible then return false end
    return true
end

function UI:update_base(mx, my, click)
    if not self.visible then return end
    for _, child in ipairs(self.children) do
        child:update_base(mx, my, click)
    end
end

local Button = {}
Button.__index = Button
setmetatable(Button, UI)

function UI.Button(props)
    local self = UI.new_component(props)
    setmetatable(self, Button)
    
    self.text = props.text or ""
    self.font = props.font or "NotoSansTC-Bold.ttf"
    self.fontSize = props.fontSize or 24
    self.onClick = props.onClick
    self.normalColor = props.normalColor or {220, 220, 220}
    self.hoverColor = props.hoverColor or {255, 215, 0}
    
    -- Auto measure
    if self.w == 0 or self.h == 0 then
        local size = Engine.MeasureText(self.text, self.font, self.fontSize)
        self.w = props.w or (size.w + 40)
        self.h = props.h or (size.h + 20)
    end
    
    return self
end

function Button:render()
    if not self.visible then return end
    local ax, ay = self:get_absolute_pos()
    local hovered = self:is_hovered()
    
    ax, ay = math.floor(ax), math.floor(ay)
    local w, h = math.floor(self.w), math.floor(self.h)
    
    if hovered then
        Engine.DrawRect(ax, ay, w, h, 255, 255, 255, 40)
        local c = self.hoverColor
        Engine.DrawTextOutline(self.text, ax + 20, ay + 10, self.font, self.fontSize, c[1], c[2], c[3], 0, 0, 0, 2)
    else
        local c = self.normalColor
        Engine.DrawTextOutline(self.text, ax + 20, ay + 10, self.font, self.fontSize, c[1], c[2], c[3], 0, 0, 0, 2)
    end
end

function Button:update(mx, my, click)
    if not self.visible then return end
    if click and self:is_hovered() and self.onClick then
        self.onClick()
    end
end

_G.UI = UI
return UI
