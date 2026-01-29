--@セリフ準備
--information:セリフ準備@PSDToolKit %VERSION% by oov
--label:PSDToolKit
--value@id:キャラクターID,""
--text@text:テキスト,こんにちは
--file@audio:音声ファイル
require("PSDToolKit").psdcall(function()
	require("PSDToolKit").set_voice(id, text, audio, obj)
end)
