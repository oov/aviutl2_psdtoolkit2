local M = {}

--- Get file extension (lowercase)
-- @param filepath string File path
-- @return string File extension in lowercase
function M.get_extension(filepath)
	local ext = filepath:match("%.([^%.]+)$")
	return ext and ext:lower() or ""
end

--- Get file path without extension
-- @param filepath string File path
-- @return string File path without extension
function M.get_basename_without_ext(filepath)
	return filepath:match("^(.+)%.[^%.]+$") or filepath
end

--- Get filename only (without directory path)
-- @param filepath string File path
-- @return string Filename without directory path
function M.get_filename(filepath)
	return filepath:match("[/\\]([^/\\]+)$") or filepath
end

--- Read text content from a file
-- @param filepath string Path to file to read
-- @return string|nil File content, or nil if file cannot be read
function M.read_file(filepath)
	local f = io.open(filepath, "rb")
	if not f then
		return nil
	end
	local content = f:read("*a")
	f:close()
	return content
end

--- Extract character ID from filename
-- Supports formats:
-- - "[0-9]+_(charactor)_(text).wav"
-- - "[0-9]+_[0-9]+_(charactor)_(text).wav"
-- - "[0-9]+-[0-9]+_(charactor)_(text).wav"
-- @param filepath string File path to extract character ID from
-- @return string Character ID, or empty string if not found
-- @example
-- extract_chara_id("251119_081002_ずんだもん_こんにちは.wav") -- returns "ずんだもん"
function M.extract_chara_id(filepath)
	local filename = M.get_filename(filepath)
	-- Remove extension
	local basename = filename:match("^(.+)%.[^%.]+$") or filename

	-- Split by underscore
	local parts = {}
	for part in basename:gmatch("[^_]+") do
		table.insert(parts, part)
	end

	-- Need at least 3 parts: number(s), chara_id, text
	if #parts < 3 then
		return ""
	end

	-- Check if first part is digits only (or digits-digits format)
	local first = parts[1]
	if first:match("^%d+%-%d+$") then
		-- Pattern: "[0-9]+-[0-9]+_(charactor)_(text).wav"
		return parts[2]
	elseif first:match("^%d+$") then
		-- Check if second part is also digits only
		local second = parts[2]
		if second:match("^%d+$") then
			-- Pattern: "[0-9]+_[0-9]+_(charactor)_(text).wav"
			if #parts >= 4 then
				return parts[3]
			end
		else
			-- Pattern: "[0-9]+_(charactor)_(text).wav"
			return parts[2]
		end
	end

	return ""
end

--- Get plugin directory from script directory
-- Goes up one level from GCMZScript to Plugin
-- @return string Plugin directory path, or nil if not found
function M.get_plugin_dir()
	local script_dir = gcmz.get_script_directory()
	-- Remove trailing slash if present
	script_dir = script_dir:gsub("[/\\]$", "")
	-- Go up one level (from GCMZScript to Plugin)
	return script_dir:match("^(.+)[/\\][^/\\]+$")
end

--- Encoding type constants
-- @field ENCODING_UNKNOWN Unknown encoding (not valid UTF-8, no BOM)
-- @field ENCODING_UTF8 UTF-8 (with or without BOM)
-- @field ENCODING_UTF16LE UTF-16 Little Endian (BOM detected)
-- @field ENCODING_UTF16BE UTF-16 Big Endian (BOM detected)
M.ENCODING_UNKNOWN = 0
M.ENCODING_UTF8 = 1
M.ENCODING_UTF16LE = 2
M.ENCODING_UTF16BE = 3

--- Read text file and convert to UTF-8
-- Automatically detects encoding (UTF-8, UTF-16LE, UTF-16BE, or ACP) and converts to UTF-8.
-- BOM is detected and stripped if present.
-- @param filepath string Path to file to read
-- @return string|nil File content as UTF-8, or nil if file cannot be read
function M.read_text_file_utf8(filepath)
	local content = M.read_file(filepath)
	if not content then
		return nil
	end

	local ptk = gcmz.get_script_module("PSDToolKit")
	if not ptk then
		-- Fallback: return as-is if PSDToolKit module not available
		return content
	end

	local encoding = ptk.detect_encoding(content)

	if encoding == M.ENCODING_UTF8 then
		-- Strip UTF-8 BOM if present
		if content:sub(1, 3) == "\xEF\xBB\xBF" then
			content = content:sub(4)
		end
		return content
	elseif encoding == M.ENCODING_UTF16LE then
		return gcmz.convert_encoding(content, "utf16le", "utf8")
	elseif encoding == M.ENCODING_UTF16BE then
		return gcmz.convert_encoding(content, "utf16be", "utf8")
	else
		-- Unknown encoding (not valid UTF-8, no BOM) -> try ACP
		return gcmz.convert_encoding(content, "ansi", "utf8")
	end
end

return M
