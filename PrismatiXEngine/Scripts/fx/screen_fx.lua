local ScreenEffects = {}
local Utils = _G.Utils

if type(Utils) ~= "table" then
	error("ScreenEffects requires _G.Utils to be loaded before Scripts/fx/screen_fx.lua")
end

local shake = {
	active = false,
	frame = 0,
	duration = 0,
	amplitude = 0,
	frequency = 0,
	decay = 0,
	offsetX = 0,
	offsetY = 0
}

function ScreenEffects.trigger_shake(options)
	local opts = options or {}
	shake.active = true
	shake.frame = 0
	shake.duration = math.max(1, math.floor(Utils.to_number(opts.duration, 20)))
	shake.amplitude = math.max(1, Utils.to_number(opts.amplitude, 18))
	shake.frequency = math.max(0.1, Utils.to_number(opts.frequency, 0.95))
	shake.decay = Utils.clamp(Utils.to_number(opts.decay, 1.8), 0.1, 8.0)
end

function ScreenEffects.stop_shake()
	shake.active = false
	shake.frame = 0
	shake.offsetX = 0
	shake.offsetY = 0
end

function ScreenEffects.update()
	if not shake.active then
		shake.offsetX = 0
		shake.offsetY = 0
		return
	end

	shake.frame = shake.frame + 1

	local p = shake.frame / shake.duration
	if p >= 1 then
		ScreenEffects.stop_shake()
		return
	end

	local falloff = (1 - p) ^ shake.decay
	local phase = shake.frame * shake.frequency

	shake.offsetX = math.floor(math.sin(phase * 1.73 + 0.35) * shake.amplitude * falloff)
	shake.offsetY = math.floor(math.cos(phase * 2.11 + 1.20) * shake.amplitude * 0.75 * falloff)
end

function ScreenEffects.get_offset()
	return shake.offsetX, shake.offsetY
end

function ScreenEffects.is_shaking()
	return shake.active
end

function _G.FX_Shake(args)
	local payload = args or {}
	local d = Utils.to_number(payload.duration, 20)
	local a = Utils.to_number(payload.amplitude, 18)
	local f = Utils.to_number(payload.frequency, 0.95)
	local dy = Utils.to_number(payload.decay, 1.8)
	
	ScreenEffects.trigger_shake({
		duration = d,
		amplitude = a,
		frequency = f,
		decay = dy
	})
end

function _G.FX_StopShake()
	ScreenEffects.stop_shake()
end

return ScreenEffects

