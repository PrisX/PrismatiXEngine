local Gallery = {}
Gallery.__index = Gallery

function Gallery.new()
    local self = setmetatable({}, Gallery)
    return self
end

function Gallery:render(screenW, screenH)
    local rect = Engine.DrawAuto("gallery_bg.png", DisplayMode.Fit, 255, 0, 0, 1.0)
    if rect.w == 0 then
        Engine.DrawRect(0, 0, screenW, screenH, 15, 15, 25, 255)
    end
end

return Gallery
