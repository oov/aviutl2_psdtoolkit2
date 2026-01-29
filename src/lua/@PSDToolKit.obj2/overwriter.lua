--@パーツ上書き
--info:パーツ上書き@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@id:キャラクターID,""
--track@p1:パーツ1,0,200,0,1
--track@p2:パーツ2,0,200,0,1
--track@p3:パーツ3,0,200,0,1
--track@p4:パーツ4,0,200,0,1
--track@p5:パーツ5,0,200,0,1
--track@p6:パーツ6,0,200,0,1
--track@p7:パーツ7,0,200,0,1
--track@p8:パーツ8,0,200,0,1
--track@p9:パーツ9,0,200,0,1
--track@p10:パーツ10,0,200,0,1
--track@p11:パーツ11,0,200,0,1
--track@p12:パーツ12,0,200,0,1
--track@p13:パーツ13,0,200,0,1
--track@p14:パーツ14,0,200,0,1
--track@p15:パーツ15,0,200,0,1
--track@p16:パーツ16,0,200,0,1
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").set_layer_selector_overwriter(id ~= "" and id or nil, {
		p1 = p1 ~= 0 and p1 or nil,
		p2 = p2 ~= 0 and p2 or nil,
		p3 = p3 ~= 0 and p3 or nil,
		p4 = p4 ~= 0 and p4 or nil,
		p5 = p5 ~= 0 and p5 or nil,
		p6 = p6 ~= 0 and p6 or nil,
		p7 = p7 ~= 0 and p7 or nil,
		p8 = p8 ~= 0 and p8 or nil,
		p9 = p9 ~= 0 and p9 or nil,
		p10 = p10 ~= 0 and p10 or nil,
		p11 = p11 ~= 0 and p11 or nil,
		p12 = p12 ~= 0 and p12 or nil,
		p13 = p13 ~= 0 and p13 or nil,
		p14 = p14 ~= 0 and p14 or nil,
		p15 = p15 ~= 0 and p15 or nil,
		p16 = p16 ~= 0 and p16 or nil,
	}, obj)
end)
