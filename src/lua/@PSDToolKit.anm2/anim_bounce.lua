--@sub-ぼよん
--information:sub-ぼよん@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@id:キャラクターID,""
--track@duration:長さ(秒),0,5,0.30,0.01
--track@interval:間隔(秒),0,1,0.03,0.01
--track@speed:速さ(秒),0.01,1,0.1,0.01
--track@size:サイズ,0.01,50,3,0.01
--select@vert:縦位置=2,上=0,中=1,下=2
--select@horz:横位置=1,左=0,中=1,右=2
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").bounce({
		id = id,
		duration = duration,
		interval = interval,
		speed = speed,
		size = size,
		vert = vert,
		horz = horz,
	}, obj)
end)
