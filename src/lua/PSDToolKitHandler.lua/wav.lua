-- Audio file (*.wav) and *.object file processing for PSDToolKit handler
local M = {}

local ini = require("ini")
local util = require("PSDToolKitHandler.util")
local debug = require("PSDToolKitHandler.debug")
local dbg = debug.dbg

--- Escape newlines in text content for use in object files
-- Converts actual newlines (CR, LF, CRLF) to literal "\n" string
-- @param text string Text content to escape
-- @return string Text with newlines escaped
local function escape_newlines(text)
	if not text then
		return text
	end
	-- Convert CRLF, CR, LF to literal \n
	text = text:gsub("\r\n", "\\n")
	text = text:gsub("\r", "\\n")
	text = text:gsub("\n", "\\n")
	return text
end

--- Check if file list contains *.wav or *.object
-- @param files table List of file objects with filepath field
-- @return boolean True if list contains WAV or object file
function M.has_wav_or_object(files)
	for _, file in ipairs(files) do
		local ext = util.get_extension(file.filepath)
		if ext == "wav" or ext == "object" then
			return true
		end
	end
	return false
end

--- Get audio and text from single *.wav file
-- Validates that files contain exactly one *.wav file and reads associated *.txt file if present
-- @param files table List of file objects
-- @return string|nil, string|nil WAV path and text content if WAV found, nil otherwise
function M.get_audio_text_from_single_wav(files)
	if #files ~= 1 then
		return nil, nil
	end
	local file = files[1]
	if util.get_extension(file.filepath) ~= "wav" then
		return nil, nil
	end
	local txt_path = util.get_basename_without_ext(file.filepath) .. ".txt"
	local txt_content = util.read_text_file_utf8(txt_path)
	if not txt_content then
		txt_content = ""
	else
		txt_content = escape_newlines(txt_content)
	end
	return file.filepath, txt_content
end

--- Get audio and text from *.wav and *.txt pair with same basename
-- @param files table List of file objects
-- @return string|nil, string|nil WAV path and text content if pair found, nil otherwise
function M.get_audio_text_from_wav_txt_pair(files)
	if #files ~= 2 then
		return nil, nil
	end

	local wav_file = nil
	local txt_file = nil

	for _, file in ipairs(files) do
		local ext = util.get_extension(file.filepath)
		if ext == "wav" then
			wav_file = file
		elseif ext == "txt" then
			txt_file = file
		end
	end

	if not wav_file or not txt_file then
		return nil, nil
	end

	-- Check if they have the same basename
	local wav_base = util.get_basename_without_ext(wav_file.filepath)
	local txt_base = util.get_basename_without_ext(txt_file.filepath)
	if wav_base ~= txt_base then
		return nil, nil
	end

	-- Read text content from file with auto encoding detection
	local txt_content = util.read_text_file_utf8(txt_file.filepath)
	if not txt_content then
		return nil, nil
	end
	txt_content = escape_newlines(txt_content)

	return wav_file.filepath, txt_content
end

--- Get audio and text from single *.object file
-- Validates that files contain exactly one *.object file and extracts audio+text from it
-- @param files table List of file objects
-- @return string|nil, string|nil WAV path and text content if valid, nil otherwise
function M.get_audio_text_from_single_object(files)
	-- Check if files contain exactly one *.object file
	if #files ~= 1 then
		return nil, nil
	end
	local object_file = files[1]
	if util.get_extension(object_file.filepath) ~= "object" then
		return nil, nil
	end

	-- Read and parse object file as INI
	local content = util.read_file(object_file.filepath)
	if not content then
		return nil, nil
	end

	local ok, object_ini = pcall(ini.new, content)
	if not ok then
		dbg("PSDToolKit: Failed to parse object file as INI")
		return nil, nil
	end

	-- Check that [2] section does NOT exist (exactly 2 objects: [0] and [1])
	if object_ini:sectionexists("2") then
		dbg("PSDToolKit: Object file has more than 2 objects")
		return nil, nil
	end

	-- Check that [0] and [1] sections exist
	if not object_ini:sectionexists("0") or not object_ini:sectionexists("1") then
		dbg("PSDToolKit: Object file missing [0] or [1] section")
		return nil, nil
	end

	-- Get frame values and check if start frames match
	-- Parse "start,end" format to extract start frame number
	local f = object_ini:get("0", "frame", "")
	f = f:match("^(%d+),")
	local start0 = f and tonumber(f) or nil

	f = object_ini:get("1", "frame", "")
	f = f:match("^(%d+),")
	local start1 = f and tonumber(f) or nil

	if not start0 or not start1 then
		dbg("PSDToolKit: Could not parse frame values")
		return nil, nil
	end

	if start0 ~= start1 then
		dbg("PSDToolKit: Objects have different start frames: %s vs %s", start0, start1)
		return nil, nil
	end

	-- Check [0.0] and [1.0] for effect.name
	-- Looking for exactly one "音声ファイル" and one "テキスト"
	local audio_file_path = nil
	local text_content = nil

	for obj_idx = 0, 1 do
		local subsect = tostring(obj_idx) .. ".0"
		if object_ini:sectionexists(subsect) then
			local effect_name = object_ini:get(subsect, "effect.name", "")

			-- Check for audio file object
			if effect_name == "音声ファイル" then
				-- Get the audio file path from "ファイル" key
				local file_val = object_ini:get(subsect, "ファイル", "")
				if file_val ~= "" then
					audio_file_path = file_val
				end

			-- Check for text object
			elseif effect_name == "テキスト" then
				-- Get text content from "テキスト" key
				local text_val = object_ini:get(subsect, "テキスト", "")
				if text_val ~= "" then
					text_content = text_val
				end
			end
		end
	end

	-- Verify we found both audio and text
	if not audio_file_path or audio_file_path == "" then
		dbg("PSDToolKit: Object file does not contain audio file object")
		return nil, nil
	end

	if not text_content then
		dbg("PSDToolKit: Object file does not contain text object")
		return nil, nil
	end

	-- Check if audio file is *.wav
	if util.get_extension(audio_file_path) ~= "wav" then
		dbg("PSDToolKit: Audio file is not a WAV file: %s", audio_file_path)
		return nil, nil
	end

	dbg("PSDToolKit: Found valid audio+text object: %s", audio_file_path)
	return audio_file_path, text_content
end

--- Extract wav path and text content from files based on trigger conditions
-- @param files table List of file objects
-- @param state table Drop state with from_external_api and shift flags
-- @param config table Configuration table
-- @return string|nil, string|nil, string|nil WAV path, text content, and trigger name if matched, nil otherwise
function M.extract_wav_and_txt(files, state, config)
	local from_external = state.from_external_api

	-- Condition 1: manual_shift_wav - single *.wav with Shift key (manual only)
	if not from_external and config.manual_shift_wav ~= 0 and state.shift then
		local wav_path, txt_content = M.get_audio_text_from_single_wav(files)
		if wav_path then
			dbg("PSDToolKit: Triggered by manual_shift_wav")
			return wav_path, txt_content, "manual_shift_wav"
		end
	end

	-- Condition 2: manual_wav_txt_pair - *.wav and *.txt pair (manual only)
	if not from_external and config.manual_wav_txt_pair ~= 0 then
		local wav_path, txt_content = M.get_audio_text_from_wav_txt_pair(files)
		if wav_path then
			dbg("PSDToolKit: Triggered by manual_wav_txt_pair")
			return wav_path, txt_content, "manual_wav_txt_pair"
		end
	end

	-- Condition 3: manual_object_audio_text - *.object with audio+text (manual only)
	if not from_external and config.manual_object_audio_text ~= 0 then
		local wav_path, txt_content = M.get_audio_text_from_single_object(files)
		if wav_path then
			dbg("PSDToolKit: Triggered by manual_object_audio_text")
			return wav_path, txt_content, "manual_object_audio_text"
		end
	end

	-- Condition 4: external_wav_txt_pair - *.wav and *.txt pair (external API only)
	if from_external and config.external_wav_txt_pair ~= 0 then
		local wav_path, txt_content = M.get_audio_text_from_wav_txt_pair(files)
		if wav_path then
			dbg("PSDToolKit: Triggered by external_wav_txt_pair")
			return wav_path, txt_content, "external_wav_txt_pair"
		end
	end

	-- Condition 5: external_object_audio_text - *.object with audio+text (external API only)
	if from_external and config.external_object_audio_text ~= 0 then
		local wav_path, txt_content = M.get_audio_text_from_single_object(files)
		if wav_path then
			dbg("PSDToolKit: Triggered by external_object_audio_text")
			return wav_path, txt_content, "external_object_audio_text"
		end
	end

	return nil, nil, nil
end

--- Process wav/txt files and generate voice.object
-- Attempts to extract wav and text from files, generate voice.object, and replace files
-- @param files table List of file objects to process (will be modified if successful)
-- @param state table Drop state with metadata
-- @param config table Configuration table
-- @return boolean True if processing was attempted (whether successful or not), false if not applicable
function M.process(files, state, config)
	-- Try to extract wav and text from files based on config
	local wav_path, txt_content, trigger = M.extract_wav_and_txt(files, state, config)
	if not wav_path then
		-- No conditions matched, not applicable to this handler
		return false
	end

	dbg("PSDToolKit: Processing wav=%s, txt=%s, trigger=%s", wav_path, txt_content or "(none)", trigger)

	local template = require("PSDToolKitHandler.template")
	local object_content = template.generate_voice_object(wav_path, txt_content)
	if not object_content then
		dbg("PSDToolKit: Failed to generate voice.object content")
		return true -- Processing was attempted but failed
	end

	local temp_filepath = gcmz.create_temp_file("voice.object")
	if not temp_filepath then
		dbg("PSDToolKit: Failed to create temp file")
		return true
	end
	local f = io.open(temp_filepath, "wb")
	if not f then
		dbg("PSDToolKit: Failed to open temp file for writing: %s", temp_filepath)
		return true
	end
	f:write(object_content)
	f:close()

	dbg("PSDToolKit: Created temp file: %s", temp_filepath)

	-- Replace files with the generated object file
	-- Clear existing files and add the new one
	for i = #files, 1, -1 do
		files[i] = nil
	end
	files[1] = {
		filepath = temp_filepath,
		mimetype = "",
		temporary = true,
	}

	return true
end

return M
