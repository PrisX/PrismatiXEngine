local UI = {}
UI.__index = UI

-- Helper to deep copy tables
local function deep_copy(obj)
    if type(obj) ~= 'table' then return obj end
    local res = {}
    for k, v in pairs(obj) do res[k] = deep_copy(v) end
    return res
end

-- Base Component
function UI.new_component(props)
    local self = setmetatable({}, UI)
    props = props or {}
    
    self.id = props.id
    self.x = props.x or 0
    self.y = props.y or 0
    self.w = props.w or 0
    self.h = props.h or 0
    self.padding = props.padding or 0
    self.alpha = props.alpha or 255
    self.visible = props.visible ~= false
    self.layout = props.layout or "None" -- "None", "Vertical", "Horizontal"
    self.spacing = props.spacing or 0
    
    self.children = {}
    if props.children then
        for _, child in ipairs(props.children) do self:add_child(child) end
    end
    
    return self
end

function UI:add_child(child)
    if not child then return end
    table.insert(self.children, child)
    child.parent = self
    return child
end

function UI:get_absolute_pos()
    local ax, ay = self.x, self.y
    if self.parent then
        local px, py = self.parent:get_absolute_pos()
        ax = ax + px
        ay = ay + py
    end
    return ax, ay
end

function UI:is_hovered()
    if not self.visible then return false end
    local ax, ay = self:get_absolute_pos()
    return Engine.IsMouseInRect(ax, ay, self.w, self.h)
end

-- Tweening
UI.tweens = {}
function UI.tween(target, property, toValue, duration)
    table.insert(UI.tweens, {
        target = target, property = property, from = target[property],
        to = toValue, startTime = Engine.GetTicks(), duration = duration or 500
    })
end

function UI.update_tweens()
    local now = Engine.GetTicks()
    for i = #UI.tweens, 1, -1 do
        local t = UI.tweens[i]
        local progress = math.min((now - t.startTime) / t.duration, 1.0)
        t.target[t.property] = t.from + (t.to - t.from) * progress
        if progress >= 1.0 then table.remove(UI.tweens, i) end
    end
end

function UI:update(mx, my, click)
    if not self.visible then return end
    UI.update_tweens()
    
    if self.layout == "Vertical" then
        local currentY = self.padding
        for _, child in ipairs(self.children) do
            child.x = self.padding
            child.y = currentY
            currentY = currentY + child.h + self.spacing
        end
    elseif self.layout == "Horizontal" then
        local currentX = self.padding
        for _, child in ipairs(self.children) do
            child.x = currentX
            child.y = self.padding
            currentX = currentX + child.w + self.spacing
        end
    end

    for _, child in ipairs(self.children) do
        if child.update then child:update(mx, my, click) end
    end
end

function UI:render()
    if not self.visible then return end
    for _, child in ipairs(self.children) do
        if child.render then child:render() end
    end
end

-- Label
local Label = setmetatable({}, {__index = UI})
Label.__index = Label
function UI.Label(props)
    local self = UI.new_component(props)
    setmetatable(self, Label)
    self.text = props.text or ""
    self.font = props.font or "NotoSansTC-Regular.ttf"
    self.fontSize = props.fontSize or 24
    self.color = props.color or {255, 255, 255}
    if self.w == 0 then
        local size = Engine.MeasureText(self.text, self.font, self.fontSize)
        self.w, self.h = size.w, size.h
    end
    return self
end
function Label:render()
    local ax, ay = self:get_absolute_pos()
    local c = self.color
    Engine.DrawTextOutline(self.text, ax, ay, self.font, self.fontSize, c[1], c[2], c[3], 0, 0, 0, 2, 0, self.alpha, self.shadow)
end

-- Button
local Button = setmetatable({}, {__index = UI})
Button.__index = Button
function UI.Button(props)
    local self = UI.new_component(props)
    setmetatable(self, Button)
    self.text = props.text or ""
    self.font = props.font or "NotoSansTC-Bold.ttf"
    self.fontSize = props.fontSize or 24
    self.onClick = props.onClick
    self.normalColor = props.normalColor or {200, 200, 200}
    self.hoverColor = props.hoverColor or {255, 255, 255}
    self.bgColor = props.bgColor or {0, 0, 0, 100}
    self.hoverBgColor = props.hoverBgColor or {255, 255, 255, 40}
    if self.w == 0 then
        local size = Engine.MeasureText(self.text, self.font, self.fontSize)
        self.w, self.h = size.w + 40, size.h + 20
    end
    return self
end
function Button:render()
    local ax, ay = self:get_absolute_pos()
    local hovered = self:is_hovered()
    local bg = hovered and self.hoverBgColor or self.bgColor
    Engine.DrawRect(ax, ay, self.w, self.h, bg[1], bg[2], bg[3], (bg[4] or 255) * (self.alpha / 255))
    local c = hovered and self.hoverColor or self.normalColor
    local size = Engine.MeasureText(self.text, self.font, self.fontSize)
    Engine.DrawTextOutline(self.text, ax + (self.w - size.w)/2, ay + (self.h - size.h)/2, self.font, self.fontSize, c[1], c[2], c[3], 0, 0, 0, 2, 0, self.alpha)
end
function Button:update(mx, my, click)
    UI.update(self, mx, my, click)
    if click and self:is_hovered() and self.onClick then
        Engine.PlaySFX("se_click.wav")
        self.onClick()
    end
end

-- Panel
local Panel = setmetatable({}, {__index = UI})
Panel.__index = Panel
function UI.Panel(props)
    local self = UI.new_component(props)
    setmetatable(self, Panel)
    self.color = props.color or {0, 0, 0, 150}
    return self
end
function Panel:render()
    local ax, ay = self:get_absolute_pos()
    Engine.DrawRect(ax, ay, self.w, self.h, self.color[1], self.color[2], self.color[3], (self.color[4] or 255) * (self.alpha / 255))
    UI.render(self)
end

-- TabView
local TabView = setmetatable({}, {__index = UI})
TabView.__index = TabView
function UI.TabView(props)
    local self = UI.new_component(props)
    setmetatable(self, TabView)
    self.tabs = props.tabs or {}
    self.activeTab = props.activeTab or 1
    self.tabHeight = props.tabHeight or 40
    self:rebuild()
    return self
end
function TabView:rebuild()
    self.children = {}
    local header = UI.new_component({x = 0, y = 0, w = self.w, h = self.tabHeight, layout = "Horizontal", spacing = 5})
    self:add_child(header)
    for i, tab in ipairs(self.tabs) do
        header:add_child(UI.Button({
            text = tab.name, w = 120, h = self.tabHeight, fontSize = 18,
            bgColor = (self.activeTab == i) and {100, 100, 100, 200} or {50, 50, 50, 150},
            onClick = function() self.activeTab = i; self:rebuild() end
        }))
    end
    if self.tabs[self.activeTab] then
        local content = self.tabs[self.activeTab].content
        content.x, content.y = 0, self.tabHeight + 10
        content.w, content.h = self.w, self.h - self.tabHeight - 10
        self:add_child(content)
    end
end

-- Slider
local Slider = setmetatable({}, {__index = UI})
Slider.__index = Slider
function UI.Slider(props)
    local self = UI.new_component(props)
    setmetatable(self, Slider)
    self.min, self.max, self.value = props.min or 0, props.max or 100, props.value or 50
    self.onChanged, self.label = props.onChanged, props.label or ""
    self.w, self.h = props.w or 200, props.h or 30
    return self
end
function Slider:render()
    local ax, ay = self:get_absolute_pos()
    if self.label ~= "" then Engine.DrawText(self.label, ax, ay - 25, "NotoSansTC-Regular.ttf", 18, 255, 255, 255) end
    Engine.DrawRect(ax, ay + 10, self.w, 10, 50, 50, 50, 255)
    local hx = ax + ((self.value - self.min) / (self.max - self.min)) * self.w
    Engine.DrawRect(hx - 5, ay, 10, 30, 255, 255, 255, 255)
    Engine.DrawText(tostring(math.floor(self.value)), ax + self.w + 10, ay, "NotoSansTC-Regular.ttf", 18, 255, 255, 255)
end
function Slider:update(mx, my, click)
    if not self.visible then return end
    if Engine.GetLeftClick() then
        local ax, ay = self:get_absolute_pos()
        if mx >= ax and mx <= ax + self.w and my >= ay and my <= ay + self.h then
            self.value = self.min + ((mx - ax) / self.w) * (self.max - self.min)
            if self.onChanged then self.onChanged(self.value) end
        end
    end
end

_G.UI = UI
return UI
