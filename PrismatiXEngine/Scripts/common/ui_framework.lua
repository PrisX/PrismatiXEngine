local UI = {}
UI.__index = UI

local function clamp(value, minimum, maximum)
    if value < minimum then
        return minimum
    end
    if value > maximum then
        return maximum
    end
    return value
end

local function deep_copy(object)
    if type(object) ~= "table" then
        return object
    end

    local result = {}
    for key, value in pairs(object) do
        result[key] = deep_copy(value)
    end
    return result
end

local function normalize_color(color, alpha_scale)
    local source = color or {}
    return {
        source[1] or 255,
        source[2] or 255,
        source[3] or 255,
        math.floor((source[4] or 255) * (alpha_scale or 1.0)),
    }
end

local function draw_fill(x, y, w, h, color, radius)
    local rgba = normalize_color(color, 1.0)
    local corner_radius = radius or 0
    if corner_radius > 0 then
        Engine.DrawRoundedRect(x, y, w, h, corner_radius, rgba[1], rgba[2], rgba[3], rgba[4])
        return
    end
    Engine.DrawRect(x, y, w, h, rgba[1], rgba[2], rgba[3], rgba[4])
end

local function draw_image(asset_path, x, y, w, h, alpha)
    if asset_path == nil or asset_path == "" then
        return
    end
    Engine.DrawImage(asset_path, x, y, w, h, alpha or 255)
end

local function measure_text(text, font_name, font_size)
    if text == nil or text == "" then
        return { w = 0, h = 0 }
    end
    return Engine.MeasureText(text, font_name, font_size)
end

local function text_position(bounds, text, font_name, font_size, align_x, align_y)
    local size = measure_text(text, font_name, font_size)
    local x = bounds.x
    local y = bounds.y

    if align_x == "center" then
        x = bounds.x + math.floor((bounds.w - size.w) * 0.5)
    elseif align_x == "right" then
        x = bounds.x + bounds.w - size.w
    end

    if align_y == "center" then
        y = bounds.y + math.floor((bounds.h - size.h) * 0.5)
    elseif align_y == "bottom" then
        y = bounds.y + bounds.h - size.h
    end

    return x, y
end

local function animation_state(animation)
    animation = animation or {}
    local kind = animation.kind or "none"
    if kind == "none" then
        return 0, 0, 1.0, 1.0
    end

    local speed = animation.speed or 1.0
    local amplitude = animation.amplitude or 8.0
    local time = (Engine.GetTicks() / 1000.0) * speed
    local wave = math.sin(time * 2.0)

    if kind == "float" then
        return 0, wave * amplitude, 1.0, 1.0
    elseif kind == "pulse" then
        return 0, 0, 0.7 + 0.3 * ((wave + 1.0) * 0.5), 1.0 + 0.03 * wave
    elseif kind == "fade" then
        return 0, 0, 0.55 + 0.45 * ((wave + 1.0) * 0.5), 1.0
    elseif kind == "slide_x" then
        return wave * amplitude, 0, 1.0, 1.0
    elseif kind == "slide_y" then
        return 0, wave * amplitude, 1.0, 1.0
    end

    return 0, 0, 1.0, 1.0
end

local function normalized_bounds(element)
    local dx, dy, alpha_scale, scale = animation_state(element.animation)
    local base_w = element.w or 0
    local base_h = element.h or 0
    local scaled_w = base_w * scale
    local scaled_h = base_h * scale
    return {
        x = (element.x or 0) + dx - (scaled_w - base_w) * 0.5,
        y = (element.y or 0) + dy - (scaled_h - base_h) * 0.5,
        w = scaled_w,
        h = scaled_h,
        alpha_scale = alpha_scale,
    }
end

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
    self.layout = props.layout or "None"
    self.spacing = props.spacing or 0
    self.radius = props.radius or props.cornerRadius or 0
    self.children = {}

    if props.children then
        for _, child in ipairs(props.children) do
            self:add_child(child)
        end
    end

    return self
end

function UI:add_child(child)
    if not child then
        return nil
    end
    table.insert(self.children, child)
    child.parent = self
    return child
end

function UI:get_absolute_pos()
    local absolute_x, absolute_y = self.x, self.y
    if self.parent then
        local parent_x, parent_y = self.parent:get_absolute_pos()
        absolute_x = absolute_x + parent_x
        absolute_y = absolute_y + parent_y
    end
    return absolute_x, absolute_y
end

function UI:is_hovered()
    if not self.visible then
        return false
    end
    local absolute_x, absolute_y = self:get_absolute_pos()
    return Engine.IsMouseInRect(absolute_x, absolute_y, self.w, self.h)
end

UI.tweens = {}

function UI.tween(target, property, to_value, duration)
    table.insert(UI.tweens, {
        target = target,
        property = property,
        from = target[property],
        to = to_value,
        startTime = Engine.GetTicks(),
        duration = duration or 500,
    })
end

function UI.update_tweens()
    local now = Engine.GetTicks()
    for index = #UI.tweens, 1, -1 do
        local tween = UI.tweens[index]
        local progress = math.min((now - tween.startTime) / tween.duration, 1.0)
        tween.target[tween.property] = tween.from + (tween.to - tween.from) * progress
        if progress >= 1.0 then
            table.remove(UI.tweens, index)
        end
    end
end

function UI:update(mx, my, click)
    if not self.visible then
        return
    end

    UI.update_tweens()

    if self.layout == "Vertical" then
        local current_y = self.padding
        for _, child in ipairs(self.children) do
            child.x = self.padding
            child.y = current_y
            current_y = current_y + child.h + self.spacing
        end
    elseif self.layout == "Horizontal" then
        local current_x = self.padding
        for _, child in ipairs(self.children) do
            child.x = current_x
            child.y = self.padding
            current_x = current_x + child.w + self.spacing
        end
    end

    for _, child in ipairs(self.children) do
        if child.update then
            child:update(mx, my, click)
        end
    end
end

function UI:render()
    if not self.visible then
        return
    end
    for _, child in ipairs(self.children) do
        if child.render then
            child:render()
        end
    end
end

local Label = setmetatable({}, { __index = UI })
Label.__index = Label

function UI.Label(props)
    local self = UI.new_component(props)
    setmetatable(self, Label)
    self.text = props.text or ""
    self.font = props.font or "NotoSansTC-Regular.ttf"
    self.fontSize = props.fontSize or 24
    self.color = props.color or { 255, 255, 255, 255 }
    self.outlineColor = props.outlineColor or { 0, 0, 0, 255 }
    self.outlineSize = props.outlineSize or 2
    self.shadow = props.shadow
    self.alignX = props.alignX or "left"
    self.alignY = props.alignY or "top"
    if self.w == 0 then
        local size = measure_text(self.text, self.font, self.fontSize)
        self.w, self.h = size.w, size.h
    end
    return self
end

function Label:render()
    local absolute_x, absolute_y = self:get_absolute_pos()
    local color = normalize_color(self.color, self.alpha / 255.0)
    local bounds = { x = absolute_x, y = absolute_y, w = self.w, h = self.h }
    local tx, ty = text_position(bounds, self.text, self.font, self.fontSize, self.alignX, self.alignY)
    Engine.DrawTextOutline(
        self.text,
        tx,
        ty,
        self.font,
        self.fontSize,
        color[1],
        color[2],
        color[3],
        self.outlineColor[1] or 0,
        self.outlineColor[2] or 0,
        self.outlineColor[3] or 0,
        self.outlineSize,
        0,
        color[4],
        self.shadow
    )
end

local Button = setmetatable({}, { __index = UI })
Button.__index = Button

function UI.Button(props)
    local self = UI.new_component(props)
    setmetatable(self, Button)
    self.text = props.text or ""
    self.font = props.font or "NotoSansTC-Bold.ttf"
    self.fontSize = props.fontSize or 24
    self.onClick = props.onClick
    self.action = props.action
    self.normalColor = props.normalColor or { 255, 255, 255, 255 }
    self.hoverColor = props.hoverColor or { 255, 255, 255, 255 }
    self.bgColor = props.bgColor or { 0, 0, 0, 100 }
    self.hoverBgColor = props.hoverBgColor or { 255, 255, 255, 40 }
    self.textAlignX = props.textAlignX or "center"
    self.textAlignY = props.textAlignY or "center"
    self.image = props.image or ""
    self.hoverImage = props.hoverImage or self.image
    self.pressedImage = props.pressedImage or self.hoverImage
    self.radius = props.radius or props.cornerRadius or 14
    if self.w == 0 then
        local size = measure_text(self.text, self.font, self.fontSize)
        self.w = size.w + 40
        self.h = size.h + 20
    end
    return self
end

function Button:render()
    local absolute_x, absolute_y = self:get_absolute_pos()
    local hovered = self:is_hovered()
    local background = hovered and self.hoverBgColor or self.bgColor
    local image = hovered and self.hoverImage or self.image
    local alpha = self.alpha

    if image ~= nil and image ~= "" then
        draw_image(image, absolute_x, absolute_y, self.w, self.h, alpha)
    else
        draw_fill(absolute_x, absolute_y, self.w, self.h, background, self.radius)
    end

    local text_color = normalize_color(hovered and self.hoverColor or self.normalColor, alpha / 255.0)
    local bounds = { x = absolute_x, y = absolute_y, w = self.w, h = self.h }
    local tx, ty = text_position(bounds, self.text, self.font, self.fontSize, self.textAlignX, self.textAlignY)
    Engine.DrawTextOutline(
        self.text,
        tx,
        ty,
        self.font,
        self.fontSize,
        text_color[1],
        text_color[2],
        text_color[3],
        0,
        0,
        0,
        2,
        0,
        text_color[4]
    )
end

function Button:update(mx, my, click)
    UI.update(self, mx, my, click)
    if click and self:is_hovered() then
        if self.onClick then
            self.onClick()
        end
        return self.action
    end
    return nil
end

local Panel = setmetatable({}, { __index = UI })
Panel.__index = Panel

function UI.Panel(props)
    local self = UI.new_component(props)
    setmetatable(self, Panel)
    self.color = props.color or { 0, 0, 0, 150 }
    self.image = props.image or ""
    return self
end

function Panel:render()
    local absolute_x, absolute_y = self:get_absolute_pos()
    draw_fill(absolute_x, absolute_y, self.w, self.h, normalize_color(self.color, self.alpha / 255.0), self.radius)
    if self.image ~= nil and self.image ~= "" then
        draw_image(self.image, absolute_x, absolute_y, self.w, self.h, self.alpha)
    end
    UI.render(self)
end

local TabView = setmetatable({}, { __index = UI })
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
    local header = UI.new_component({ x = 0, y = 0, w = self.w, h = self.tabHeight, layout = "Horizontal", spacing = 5 })
    self:add_child(header)
    for index, tab in ipairs(self.tabs) do
        header:add_child(UI.Button({
            text = tab.name,
            w = 120,
            h = self.tabHeight,
            fontSize = 18,
            bgColor = (self.activeTab == index) and { 100, 100, 100, 200 } or { 50, 50, 50, 150 },
            onClick = function()
                self.activeTab = index
                self:rebuild()
            end,
        }))
    end
    if self.tabs[self.activeTab] then
        local content = self.tabs[self.activeTab].content
        content.x = 0
        content.y = self.tabHeight + 10
        content.w = self.w
        content.h = self.h - self.tabHeight - 10
        self:add_child(content)
    end
end

local Slider = setmetatable({}, { __index = UI })
Slider.__index = Slider

function UI.Slider(props)
    local self = UI.new_component(props)
    setmetatable(self, Slider)
    self.min = props.min or 0
    self.max = props.max or 100
    self.value = props.value or 50
    self.onChanged = props.onChanged
    self.label = props.label or ""
    self.w = props.w or 200
    self.h = props.h or 30
    return self
end

function Slider:render()
    local absolute_x, absolute_y = self:get_absolute_pos()
    if self.label ~= "" then
        Engine.DrawText(self.label, absolute_x, absolute_y - 25, "NotoSansTC-Regular.ttf", 18, 255, 255, 255)
    end
    Engine.DrawRect(absolute_x, absolute_y + 10, self.w, 10, 50, 50, 50, 255)
    local handle_x = absolute_x + ((self.value - self.min) / (self.max - self.min)) * self.w
    Engine.DrawRect(handle_x - 5, absolute_y, 10, 30, 255, 255, 255, 255)
    Engine.DrawText(tostring(math.floor(self.value)), absolute_x + self.w + 10, absolute_y, "NotoSansTC-Regular.ttf", 18, 255, 255, 255)
end

function Slider:update(mx, my, click)
    if not self.visible then
        return
    end
    if Engine.GetLeftClick() then
        local absolute_x, absolute_y = self:get_absolute_pos()
        if mx >= absolute_x and mx <= absolute_x + self.w and my >= absolute_y and my <= absolute_y + self.h then
            self.value = self.min + ((mx - absolute_x) / self.w) * (self.max - self.min)
            if self.onChanged then
                self.onChanged(self.value)
            end
        end
    end
end

local ComposedScene = {}
ComposedScene.__index = ComposedScene

local function element_layer_value(element)
    return (element.layer or 0) * 1000 + (element.order or 0)
end

local function sorted_elements(elements, descending)
    local result = deep_copy(elements or {})
    table.sort(result, function(left, right)
        local left_value = element_layer_value(left)
        local right_value = element_layer_value(right)
        if descending then
            return left_value > right_value
        end
        return left_value < right_value
    end)
    return result
end

function UI.ComposedScene(props)
    local self = setmetatable({}, ComposedScene)
    self.defaultFont = props.defaultFont or "NotoSansTC-Bold.ttf"
    self.defaultFontSize = props.defaultFontSize or 30
    self.background = deep_copy(props.background or { color = { 0, 0, 0, 255 } })
    self.elements = deep_copy(props.elements or {})
    self.hoveredId = nil
    return self
end

function ComposedScene:_draw_background()
    local win_w, win_h = Engine.GetLogicalSize()
    local background_color = self.background.color or { 0, 0, 0, 255 }
    draw_fill(0, 0, win_w, win_h, background_color, 0)
    if self.background.image and self.background.image ~= "" then
        Engine.DrawAuto(self.background.image, DisplayMode.Fill, self.background.alpha or 255)
    end
end

function ComposedScene:_draw_element(element)
    if element.visible == false then
        return
    end

    local bounds = normalized_bounds(element)
    local hovered = self.hoveredId ~= nil and self.hoveredId == element.id
    local alpha_scale = clamp((element.opacity or 255) / 255.0, 0.0, 1.0) * bounds.alpha_scale
    local fill_color = normalize_color(hovered and (element.hoverColor or element.color) or element.color, alpha_scale)
    local radius = element.radius or 0

    if element.kind == "rect" or element.kind == "shape" then
        draw_fill(bounds.x, bounds.y, bounds.w, bounds.h, fill_color, radius)
    elseif element.kind == "image" then
        if element.color then
            draw_fill(bounds.x, bounds.y, bounds.w, bounds.h, fill_color, radius)
        end
        draw_image(element.image, bounds.x, bounds.y, bounds.w, bounds.h, fill_color[4])
    elseif element.kind == "button" or element.kind == "image_button" then
        if element.kind == "image_button" and (element.image or "") ~= "" then
            local selected_image = hovered and (element.hoverImage or element.image) or element.image
            draw_image(selected_image, bounds.x, bounds.y, bounds.w, bounds.h, fill_color[4])
        else
            draw_fill(bounds.x, bounds.y, bounds.w, bounds.h, fill_color, radius)
            if element.image and element.image ~= "" then
                draw_image(element.image, bounds.x, bounds.y, bounds.w, bounds.h, fill_color[4])
            end
        end
        if element.text and element.text ~= "" then
            local font_name = element.font or self.defaultFont
            local font_size = element.fontSize or self.defaultFontSize
            local text_color = normalize_color(hovered and (element.hoverTextColor or element.textColor) or element.textColor or { 255, 255, 255, 255 }, alpha_scale)
            local tx, ty = text_position(bounds, element.text, font_name, font_size, element.textAlignX or "center", element.textAlignY or "center")
            Engine.DrawTextOutline(
                element.text,
                tx,
                ty,
                font_name,
                font_size,
                text_color[1],
                text_color[2],
                text_color[3],
                0,
                0,
                0,
                element.outlineSize or 2,
                0,
                text_color[4]
            )
        end
    elseif element.kind == "text" then
        local font_name = element.font or self.defaultFont
        local font_size = element.fontSize or self.defaultFontSize
        local text_color = normalize_color(element.textColor or element.color or { 255, 255, 255, 255 }, alpha_scale)
        local tx, ty = text_position(bounds, element.text or "", font_name, font_size, element.textAlignX or "left", element.textAlignY or "top")
        Engine.DrawTextOutline(
            element.text or "",
            tx,
            ty,
            font_name,
            font_size,
            text_color[1],
            text_color[2],
            text_color[3],
            0,
            0,
            0,
            element.outlineSize or 2,
            element.wrapWidth or 0,
            text_color[4],
            element.shadow
        )
    end
end

function ComposedScene:update(mx, my, left_click, right_click)
    self.hoveredId = nil
    for _, element in ipairs(sorted_elements(self.elements, true)) do
        if element.visible ~= false and (element.kind == "button" or element.kind == "image_button") then
            local bounds = normalized_bounds(element)
            if mx >= bounds.x and mx <= bounds.x + bounds.w and my >= bounds.y and my <= bounds.y + bounds.h then
                self.hoveredId = element.id
                if left_click then
                    if element.onClick then
                        element.onClick(element)
                    end
                    return element.action
                end
                break
            end
        end
    end
    return nil
end

function ComposedScene:render()
    self:_draw_background()
    for _, element in ipairs(sorted_elements(self.elements, false)) do
        self:_draw_element(element)
    end
end

_G.UI = UI
return UI
