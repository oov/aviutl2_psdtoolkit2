--@目パチ
--information:目パチ@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@anm1:開き~ptkl,""
--value@anm2:ほぼ開き~ptkl,""
--value@anm3:半開き~ptkl,""
--value@anm4:ほぼ閉じ~ptkl,""
--value@anm5:閉じ~ptkl,""
--track@interval:間隔(秒),0,60,4,0.01
--track@speed:速さ,1,100,1,1
--track@offset:オフセット,0,10000,0,1
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").add_blinker({
		["開き~ptkl"] = anm1,
		["ほぼ開き~ptkl"] = anm2,
		["半開き~ptkl"] = anm3,
		["ほぼ閉じ~ptkl"] = anm4,
		["閉じ~ptkl"] = anm5,
		["間隔(秒)"] = tostring(interval),
		["速さ"] = tostring(speed),
		["オフセット"] = tostring(offset),
	}, obj)
end)
