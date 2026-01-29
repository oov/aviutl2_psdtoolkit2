--@口パク 開閉のみ
--information:口パク 開閉のみ@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@anm1:開き~ptkl,""
--value@anm2:ほぼ開き~ptkl,""
--value@anm3:半開き~ptkl,""
--value@anm4:ほぼ閉じ~ptkl,""
--value@anm5:閉じ~ptkl,""
--track@locut:ローカット,0,2000,100,1
--track@hicut:ハイカット,0,8000,1000,1
--track@threshold:しきい値,0,100,20,1
--track@sensitivity:感度,1,100,1,1
--check@enabled:発声がなくても有効,1
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").add_lipsync({
		["開き~ptkl"] = anm1,
		["ほぼ開き~ptkl"] = anm2,
		["半開き~ptkl"] = anm3,
		["ほぼ閉じ~ptkl"] = anm4,
		["閉じ~ptkl"] = anm5,
		["ローカット"] = tostring(locut),
		["ハイカット"] = tostring(hicut),
		["しきい値"] = tostring(threshold),
		["感度"] = tostring(sensitivity),
		["発声がなくても有効"] = enabled and "1" or "0",
	}, obj)
end)
