local Component = {}
Component.__index = Component

function Component.new()
    local self = setmetatable({}, Component)
    self.x = 0
    self.y = 0
    self.alpha = 255
    self.visible = true
    
    self.targetX = 0
    self.targetY = 0
    self.targetAlpha = 255
    
    self.moveSpeed = 0.15
    self.fadeSpeed = 10
    
    self.children = {}
    return self
end

function Component:fade_to(target, speed)
    self.targetAlpha = target
    self.fadeSpeed = speed or 10
end

function Component:move_to(x, y, factor)
    self.targetX = x
    self.targetY = y
    self.moveSpeed = factor or 0.15
end

function Component:update_transforms()
    if self.alpha < self.targetAlpha then
        self.alpha = math.min(self.alpha + self.fadeSpeed, self.targetAlpha)
    elseif self.alpha > self.targetAlpha then
        self.alpha = math.max(self.alpha - self.fadeSpeed, self.targetAlpha)
    end

    local dx = self.targetX - self.x
    local dy = self.targetY - self.y
    
    if math.abs(dx) > 0.1 then self.x = self.x + dx * self.moveSpeed end
    if math.abs(dy) > 0.1 then self.y = self.y + dy * self.moveSpeed end
end

function Component:add(child)
    table.insert(self.children, child)
    return child
end

function Component:update()
    if not self.visible then return end
    self:update_transforms()
    for _, child in ipairs(self.children) do
        if child.update then child:update() end
    end
end

function Component:render()
    if not self.visible or self.alpha <= 0 then return end
    for _, child in ipairs(self.children) do
        if child.render then child:render() end
    end
end

return Component
