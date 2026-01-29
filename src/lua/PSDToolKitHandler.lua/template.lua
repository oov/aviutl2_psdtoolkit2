local M = {}

local util = require("PSDToolKitHandler.util")
local debug = require("PSDToolKitHandler.debug")
local dbg = debug.dbg

--- Load template file from PSDToolKit/template directory
-- @param template_name string Template filename
-- @return string Template content, or nil if template file not found
function M.load_template(template_name)
	local plugin_dir = util.get_plugin_dir()
	if not plugin_dir then
		dbg("PSDToolKit: Could not determine plugin directory for template")
		return nil
	end

	local template_path = plugin_dir .. "\\..\\PSDToolKit\\template\\" .. template_name
	local content = util.read_file(template_path)
	if not content then
		dbg("PSDToolKit: Template file not found: %s", template_path)
		return nil
	end

	return content
end

--- Calculate frame count from audio duration and project framerate
-- @param audio_duration_sec number Audio duration in seconds
-- @return number|nil End frame number (0-based), or nil if project data not found
function M.calculate_frame_count(audio_duration_sec)
	local project = gcmz.get_project_data()
	if not project then
		dbg("PSDToolKit: Could not get project data")
		return nil
	end

	-- fps = rate / scale
	local fps = project.rate / project.scale
	-- frame_count = duration * fps (round to nearest integer, subtract 1 for 0-based end frame)
	local frame_count = math.floor(audio_duration_sec * fps + 0.5)
	-- Ensure at least 1 frame
	if frame_count < 1 then
		frame_count = 1
	end
	return frame_count - 1 -- Return end frame (0-based, so subtract 1)
end

--- Generate voice.object content from template
-- @param wav_path string Path to WAV audio file
-- @param txt_content string|nil Text content to include
-- @return string|nil Generated object content, or nil on error
function M.generate_voice_object(wav_path, txt_content)
	-- Load template
	local template = M.load_template("voice.object.template")
	if not template then
		return nil
	end

	-- Get audio duration
	local media_info = gcmz.get_media_info(wav_path)
	if not media_info or not media_info.total_time then
		dbg("PSDToolKit: Could not get media info for: %s", wav_path)
		return nil
	end

	-- Calculate frame count
	local end_frame = M.calculate_frame_count(media_info.total_time)
	if not end_frame then
		return nil
	end

	-- Extract character ID from filename
	local chara_id = util.extract_chara_id(wav_path)

	-- Replace placeholders
	local content = template
	content = content:gsub("%%AUDIOFILE%%", wav_path)
	content = content:gsub("%%LENGTH%%", tostring(end_frame))
	content = content:gsub("%%TEXT%%", txt_content or "")
	content = content:gsub("%%CHARAID%%", chara_id)

	return content
end

--- Generate psd.object content from template
-- @param psd_path string Path to PSD file
-- @param tag string Tag value
-- @param layer string Layer value
-- @return string|nil Generated object content, or nil on error
function M.generate_psd_object(psd_path, tag, layer)
	-- Load template
	local template = M.load_template("psd.object.template")
	if not template then
		return nil
	end

	-- Replace placeholders
	local content = template
	content = content:gsub("%%PSDFILE%%", psd_path)
	content = content:gsub("%%TAG%%", tag)
	content = content:gsub("%%LAYER%%", layer)

	return content
end

return M
