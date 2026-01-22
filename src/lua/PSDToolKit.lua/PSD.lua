--- PSD object for PSDToolKit %VERSION% by oov
local debug = require("PSDToolKit.debug")
local dbg = debug.dbg

local PSD = {}
PSD.__index = PSD

--- Generate unique ID from scene and layer.
-- @param scene number: Scene ID (0-9999)
-- @param layer number: Layer number (0-999999)
-- @return number: Unique ID
function PSD.make_id(scene, layer)
	PSD.LAYER_MAX = 1000000
	PSD.SCENE_MAX = 10000
	if scene < 0 or scene >= PSD.SCENE_MAX then
		error(string.format("scene_id must be 0-%d", PSD.SCENE_MAX - 1))
	end
	if layer < 0 or layer >= PSD.LAYER_MAX then
		error(string.format("layer must be 0-%d", PSD.LAYER_MAX - 1))
	end
	return layer + scene * PSD.LAYER_MAX
end

--- Create a new PSD object.
-- @param id number: Unique identifier (scene * LAYER_MAX + layer)
-- @param file string: Path to PSD file
-- @param tag number: Tag for GUI window linkage
-- @param opt table|nil: Optional parameters {layer=string, scale=number, offsetx=number, offsety=number, character_id=string}
-- @return PSD object
function PSD.new(id, file, tag, opt)
	opt = opt or {}
	local self = setmetatable({
		id = id,
		file = file,
		tag = tag or 0,
		layer = {},
		scale = opt.scale or 1,
		offsetx = opt.offsetx or 0,
		offsety = opt.offsety or 0,
		rendered = false,
		character_id = opt.character_id or "",
	}, PSD)

	-- Add initial layer state if provided
	if opt.layer and opt.layer ~= "" then
		table.insert(self.layer, opt.layer)
	end

	return self
end

--- Add layer state to the PSD object.
-- @param layer string|table: Layer state string or table with getstate method
-- @param index number|nil: If provided with a table, select layer[index]
function PSD:add_state(layer, index)
	if layer == nil then
		return
	end

	if index == nil then
		-- Direct add
		if type(layer) == "string" and layer ~= "" then
			table.insert(self.layer, layer)
		elseif type(layer) == "table" then
			-- Objects like Blinker, LipSync that have getstate method
			table.insert(self.layer, layer)
		end
	else
		-- Select from list by index
		if type(layer) == "table" and type(index) == "number" then
			if index >= 1 and index <= #layer then
				table.insert(self.layer, layer[index])
			end
		end
	end
end

--- Build layer state string from accumulated states.
-- @param obj table: The AviUtl object (for dynamic state resolution)
-- @return string: Concatenated layer state
function PSD:buildlayer(obj)
	if #self.layer == 0 then
		return ""
	end

	-- Create context object for state evaluation
	local Context = require("PSDToolKit.Context")
	local ctx = Context.new(self, obj)

	local result = {}
	for _, v in ipairs(self.layer) do
		local t = type(v)
		if t == "string" then
			table.insert(result, v)
		elseif t == "table" and type(v.getstate) == "function" then
			-- Dynamic state from Blinker, LipSync, LayerSelector, etc.
			-- getstate can return string or array of strings
			local state = v:getstate(ctx)
			if state then
				local st = type(state)
				if st == "string" and state ~= "" then
					table.insert(result, state)
				elseif st == "table" then
					-- Array of strings - merge into result
					for _, s in ipairs(state) do
						if s and s ~= "" then
							table.insert(result, s)
						end
					end
				end
			end
		end
	end

	return table.concat(result, " ")
end

--- Render the PSD image.
-- @param obj table: The AviUtl object
-- @return boolean: true on success
function PSD:draw(obj)
	if self.rendered then
		error("already rendered")
	end
	if not self.file then
		error("no file specified")
	end
	self.rendered = true

	local ptk = obj.module("PSDToolKit")
	if not ptk then
		error("PSDToolKit script module is not available")
	end

	-- Build layer state string
	local layer_str = self:buildlayer(obj)

	-- Convert draft_mode (bool) to quality (int: 0=Fast, 1=Beautiful)
	local draft_mode = ptk.get_draft_mode()
	local quality = draft_mode and 0 or 1

	-- Call set_props to get cache key and dimensions
	-- modified: indicates whether properties (layer, scale, offset) changed since last call.
	--           This does NOT indicate cache existence. Even if modified=false, cache may not exist
	--           (e.g., after state changes A->B->A, modified=false but cache for A may be evicted).
	local modified, cachekey_hex, width, height = ptk.set_props(self.id, self.file, {
		tag = self.tag,
		layer = layer_str,
		scale = self.scale,
		offsetx = self.offsetx,
		offsety = self.offsety,
		quality = quality,
	})
	dbg(
		"PSD:draw: id=%s modified=%s cachekey=%s size=%sx%s",
		tostring(self.id),
		tostring(modified),
		tostring(cachekey_hex),
		tostring(width),
		tostring(height)
	)

	local mw, mh = obj.getinfo("image_max")
	if width > mw then
		width = mw
	end
	if height > mh then
		height = mh
	end

	if modified then
		-- Properties changed, need to draw first
		local ok = ptk.draw(self.id, self.file, width, height, cachekey_hex)
		if not ok then
			error("failed to render image")
		end
	end

	-- Try to load from cache
	obj.load("image", cachekey_hex .. ".ptkcache")

	-- If cache miss and not modified, draw and retry
	if (obj.w == 0 or obj.h == 0) and not modified then
		dbg("PSD:draw: cache miss, rendering")
		local ok = ptk.draw(self.id, self.file, width, height, cachekey_hex)
		if not ok then
			error("failed to render image")
		end
		obj.load("image", cachekey_hex .. ".ptkcache")
	end

	if obj.w == 0 or obj.h == 0 then
		error("failed to load cached image")
	end
	return true
end

return PSD
