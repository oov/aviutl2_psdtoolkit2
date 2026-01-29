--@sub-ずいっ
--information:sub-ずいっ@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@id:キャラクターID,""
--select@ein:登場=15,なし=0,Linear=1,SineIn=2,SineOut=3,SineInOut=4,QuadIn=5,QuadOut=6,QuadInOut=7,CubicIn=8,CubicOut=9,CubicInOut=10,QuartIn=11,QuartOut=12,QuartInOut=13,QuintIn=14,QuintOut=15,QuintInOut=16,ExpoIn=17,ExpoOut=18,ExpoInOut=19,BackIn=20,BackOut=21,BackInOut=22,ElasticIn=23,ElasticOut=24,ElasticInOut=25,BounceIn=26,BounceOut=27,BounceInOut=28
--select@eout:退場=14,なし=0,Linear=1,SineIn=2,SineOut=3,SineInOut=4,QuadIn=5,QuadOut=6,QuadInOut=7,CubicIn=8,CubicOut=9,CubicInOut=10,QuartIn=11,QuartOut=12,QuartInOut=13,QuintIn=14,QuintOut=15,QuintInOut=16,ExpoIn=17,ExpoOut=18,ExpoInOut=19,BackIn=20,BackOut=21,BackInOut=22,ElasticIn=23,ElasticOut=24,ElasticInOut=25,BounceIn=26,BounceOut=27,BounceInOut=28
--select@vert:縦位置=1,上=0,中=1,下=2
--select@horz:横位置=1,左=0,中=1,右=2
--track@speed:速さ(秒),0,1,0.5,0.01
--track@interval:間隔(秒),0,1,0.03,0.01
--track@alpha:透明度,0,100,50,0.01
--track@x:X,-1000,1000,-20,0.01
--track@y:Y,-1000,1000,0,0.01
--track@z:Z,-1000,1000,0,0.01
--track@rx:X軸回転,-360,360,0,0.01
--track@ry:Y軸回転,-360,360,0,0.01
--track@rz:Z軸回転,-360,360,0,0.01
--track@sx:拡大率X,0,10000,100,0.01
--track@sy:拡大率Y,0,10000,100,0.01
--check@outside:範囲外も処理する,1
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").enter_exit({
		id = id,
		speed = speed,
		interval = interval,
		alpha = alpha,
		ein = ein,
		eout = eout,
		outside = outside,
		x = x,
		y = y,
		z = z,
		sx = sx,
		sy = sy,
		vert = vert,
		horz = horz,
		rx = rx,
		ry = ry,
		rz = rz,
	}, obj)
end)
