--@PSDファイル
--information:PSDファイル@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--file@psd:PSDファイル
--check@safe_guard:セーフガード,true
--value@tag:タグ,""
--value@scene_id:シーンID,0
--value@character_id:キャラクターID,""
--value@layer:レイヤー,"L.0"
--track@scale:縮小率,0.001,100,100,0.001
--track@x:オフセットX,-10000,100000,0,1
--track@y:オフセットY,-10000,100000,0,1
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").init_psd({
		file = psd,
		scene = scene_id,
		tag = tag ~= "" and tonumber(tag) or nil,
		character_id = character_id ~= "" and character_id or nil,
		layer = layer,
		scale = scale / 100.0,
		offsetx = x,
		offsety = y,
	}, obj)
	obj.clearbuffer("object", 1, 1)
end)
