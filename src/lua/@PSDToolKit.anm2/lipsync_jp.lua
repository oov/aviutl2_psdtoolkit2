--@口パク あいうえお
--information:口パク あいうえお@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@anm_a:あ~ptkl,""
--value@anm_i:い~ptkl,""
--value@anm_u:う~ptkl,""
--value@anm_e:え~ptkl,""
--value@anm_o:お~ptkl,""
--value@anm_n:ん~ptkl,""
--select@mode:子音処理=1,閉じる=0,前を継続=1,前後で補間=2,調音位置=3
--check@enabled:発声がなくても有効,1
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").add_lipsync_lab({
		["あ~ptkl"] = anm_a,
		["い~ptkl"] = anm_i,
		["う~ptkl"] = anm_u,
		["え~ptkl"] = anm_e,
		["お~ptkl"] = anm_o,
		["ん~ptkl"] = anm_n,
		["子音処理"] = tostring(mode),
		["発声がなくても有効"] = enabled and "1" or "0",
	}, obj)
end)
