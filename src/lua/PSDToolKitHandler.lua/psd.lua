-- PSD file processing for PSDToolKit handler
local M = {}

local util = require("PSDToolKitHandler.util")
local debug = require("PSDToolKitHandler.debug")
local dbg = debug.dbg

--- Get directory part of a file path
-- @param filepath string File path
-- @return string Directory path (including trailing separator)
local function get_directory(filepath)
	local filename = util.get_filename(filepath)
	return filepath:sub(1, #filepath - #filename)
end

--- Find first *.psd or *.psb file and its index in files
-- Also searches for a matching *.pfv file in the same directory and concatenates it
-- @param files table List of file objects (may be modified to remove pfv file)
-- @return number|nil, string|nil Index and PSD/PSB file path (with pfv if found) if found, nil otherwise
function M.find_psd(files)
	for i, file in ipairs(files) do
		local ext = util.get_extension(file.filepath)
		if ext == "psd" or ext == "psb" then
			local psd_path = file.filepath
			local psd_dir = get_directory(psd_path)

			-- Search for pfv file in the same directory
			for j, pfv_file in ipairs(files) do
				if util.get_extension(pfv_file.filepath) == "pfv" then
					local pfv_path = pfv_file.filepath
					local pfv_dir = get_directory(pfv_path)
					if psd_dir == pfv_dir then
						-- Found pfv in the same directory, concatenate with "|"
						local pfv_filename = util.get_filename(pfv_path)
						psd_path = psd_path .. "|" .. pfv_filename
						-- Remove pfv from files list
						table.remove(files, j)
						-- Adjust index if pfv was before psd
						if j < i then
							i = i - 1
						end
						break
					end
				end
			end

			return i, psd_path
		end
	end
	return nil, nil
end

--- Create placeholder file for drag_enter
-- Creates a temporary empty file to allow the drop operation
-- @param files table List of file objects to process (will be modified if successful)
-- @return boolean True if placeholder was created, false if not applicable
function M.create_placeholder(files)
	-- Find first PSD file in the list
	local psd_index, psd_path = M.find_psd(files)
	if not psd_path then
		-- No PSD file found, not applicable
		return false
	end

	local temp_filepath = gcmz.create_temp_file("psd.txt")
	if not temp_filepath then
		dbg("PSDToolKit: Failed to create placeholder temp file")
		return false
	end

	-- Replace the PSD file with the placeholder
	files[psd_index] = {
		filepath = temp_filepath,
		mimetype = "text/plain; charset=utf-8",
		temporary = true,
	}

	return true
end

--- Process psd file and generate psd.object
-- Attempts to process PSD file, generate psd.object, and replace the PSD file in the list
-- Also registers the PSD file with IPC for state management
-- @param files table List of file objects to process (will be modified if successful)
-- @param state table Drop state with metadata
-- @param config table Configuration table
-- @return boolean True if processing was attempted (whether successful or not), false if not applicable
function M.process(files, state, config)
	-- Find first PSD file in the list
	local psd_index, psd_path = M.find_psd(files)
	if not psd_path then
		-- No PSD file found, not applicable to this handler
		return false
	end

	-- Check if shift key requirement is enabled and not met
	if config.manual_shift_psd ~= 0 and not state.shift then
		dbg("PSDToolKit: PSD processing skipped (Shift key not pressed)")
		return false
	end

	dbg("PSDToolKit: Processing psd=%s", psd_path)

	local ptk = gcmz.get_script_module("PSDToolKit")
	if not ptk then
		error("PSDToolKit script module is not available")
	end
	local tag = ptk.generate_tag()

	-- Register PSD file with PSDToolKit window
	if not ptk.add_psd_file(psd_path, tag) then
		dbg("PSDToolKit: Failed to register PSD file, continuing anyway")
	end

	local template = require("PSDToolKitHandler.template")
	local object_content = template.generate_psd_object(psd_path, tag, "L.0")
	if not object_content then
		dbg("PSDToolKit: Failed to generate psd.object content")
		return true -- Processing was attempted but failed
	end

	local temp_filepath = gcmz.create_temp_file("psd.object")
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
	files[psd_index] = {
		filepath = temp_filepath,
		mimetype = "",
		temporary = true,
	}

	return true
end

return M
