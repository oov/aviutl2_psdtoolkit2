--- PSDToolKit %VERSION% by oov
local M = {}

local debug = require("PSDToolKit.debug")
local dbg = debug.dbg
local i18n = require("PSDToolKit.i18n").i18n
local FrameState = require("PSDToolKit.FrameState")
local CurrentPSD = require("PSDToolKit.CurrentPSD")
local Voice = require("PSDToolKit.Voice")
local VoiceStates = require("PSDToolKit.VoiceStates")
local OverwriterStates = require("PSDToolKit.OverwriterStates")
local SubObjectStates = require("PSDToolKit.SubObjectStates")
local Animations = require("PSDToolKit.Animations")
local LabFile = require("PSDToolKit.LabFile")
local LipSync = require("PSDToolKit.LipSync")
local LipSyncLab = require("PSDToolKit.LipSyncLab")
local ValueCache = require("PSDToolKit.ValueCache")
local util = require("PSDToolKit.util")

local empty_sub_object = {
	x = 0,
	y = 0,
	z = 0,
	frame = 0,
	time = 0,
	totalframe = 1,
	totaltime = 1,
	notfound = true,
}

local last_cache_index = nil

--- Handles new frame processing start.
-- Resets per-frame state and initializes the module for the current frame.
function M.new_frame()
	FrameState.clear()
	CurrentPSD.clear()
	VoiceStates:clear()
	OverwriterStates:clear()
	SubObjectStates:clear()

	local ptk = obj.module("PSDToolKit")
	if not ptk then
		error("PSDToolKit script module is not available")
	end
	local debug_mode, cache_index = ptk.get_debug_mode()
	debug.set_debug(debug_mode)

	-- Clear caches when cache_index changes (project load or cache clear)
	if last_cache_index ~= cache_index then
		ValueCache.clear()
		LabFile.clear()
		LipSync.clear()
		LipSyncLab.clear()
		last_cache_index = cache_index
		dbg("PSDToolKit: cache cleared (cache_index=%d)", cache_index)
	end
end

--- Execute a function with error handling for PSD scripts.
-- If the function throws an error, it will be caught and displayed
-- using set_psd_error. This is used to wrap auto-generated scripts
-- and animation scripts to provide consistent error handling.
-- @param fn function: The function to execute
function M.psdcall(fn)
	if not FrameState.is_initialized() then
		FrameState.set_error()
		obj.load(
			"text",
			i18n({
				ja_JP = "「最初に置くやつ@PSDToolKit」を配置してください",
				en_US = 'Please place "最初に置くやつ@PSDToolKit" first',
				zh_CN = "请先放置“初始放置项@PSDToolKit”",
			})
		)
		return
	end
	local ok, err = pcall(fn)
	if not ok then
		if FrameState.has_error() then
			return -- Already in error state, don't overwrite
		end
		FrameState.set_error()
		obj.load("text", tostring(err))
	end
end

--- sets Voice state for given id and layer.
-- @param id string: The identifier for the voice state. Can be nil or empty.
-- @param text string: The message text.
-- @param audio string: The audio file.
-- @param obj table: An object containing properties like layer, x, y, z, frame, time, totalframe, totaltime.
function M.set_voice(id, text, audio, obj)
	local v = Voice.new(text, audio, obj.x, obj.y, obj.z, obj.frame, obj.time, obj.totalframe, obj.totaltime, obj.id)

	-- Capture fourier data at this object's time
	if audio and audio ~= "" then
		local n, sample_rate, buf = obj.getaudio(nil, audio, "fourier")
		if n > 0 and buf then
			v.fourier_data = buf
			v.fourier_sample_rate = sample_rate
			v.fourier_n = n
		end
	end

	dbg("VoiceStates:set id=%s layer=%d text=%s audio=%s", id, obj.layer, util.truncate_string(text, 10), audio)
	VoiceStates:set(id, obj.layer, v)
	SubObjectStates:set(id, obj.layer, obj)
end

--- Set layer selector overwriter values for a character ID.
-- This is called by the "パーツ上書き@PSDToolKit" object.
-- @param id string: Character ID (can be nil or empty)
-- @param values table: Overwriter values {p1=number, p2=number, ...}
-- @param obj table: The AviUtl object
function M.set_layer_selector_overwriter(id, values, obj)
	if FrameState.has_error() then
		return
	end
	if not values then
		error("values cannot be nil")
	end
	OverwriterStates:set(id, obj.layer, values)
	SubObjectStates:set(id, obj.layer, obj)
	dbg("PSDToolKit:set_layer_selector_overwriter id=%s layer=%d", tostring(id), obj.layer)
end

--- Prints a message using the voice state for a given ID.
-- @param opts table|nil: Optional parameters for the mes function.
-- @param obj table: An object to pass to the mes function of the voice state.
-- @return table: The voice state object used to print the message.
function M.mes(opts, obj)
	local ok, err = pcall(function()
		if opts == nil then
			error("opts cannot be nil")
		end
		local id = opts.id
		if type(id) ~= "string" and type(id) ~= "number" then
			error("opts.id must be a string or number")
		end
		local vs = VoiceStates:get(id)
		if vs == nil then
			return empty_sub_object
		end
		local text = vs.text
		if opts ~= nil then
			local t = type(opts.modifier)
			if t == "function" then
				text = opts.modifier(text)
			elseif t == "string" and opts.modifier ~= "" then
				text = opts.modifier .. text
			end
		end
		obj.mes(text)
		obj.ox = obj.ox + vs.x
		obj.oy = obj.oy + vs.y
		obj.oz = obj.oz + vs.z
		return vs
	end)
	if not ok then
		mes(tostring(err))
	end
end

M.init_psd = CurrentPSD.init
M.draw_psd = CurrentPSD.draw
M.add_lipsync = CurrentPSD.add_lipsync
M.add_lipsync_lab = CurrentPSD.add_lipsync_lab
M.add_blinker = CurrentPSD.add_blinker
M.add_layer_selector = CurrentPSD.add_layer_selector
M.add_state = CurrentPSD.add_state
M.add_state_legacy = CurrentPSD.add_state_legacy

-- Animation functions (delegated to Animations)
M.bounce = Animations.bounce
M.enter_exit = Animations.enter_exit

return M
