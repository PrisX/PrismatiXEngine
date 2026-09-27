# 大型媒體簽章 overlay 驗收（2026-09-27）

這次只使用《千戀＊萬花》現有 artifact 作為 SDK 壓力測試語料；沒有修改移植專案或既有發行包。測試在 D: NTFS 上建立隔離基底和 artifact 副本。副本的未變檔案以 hard link 建立，再用正式 `augmentArtifact` 發布一個有效的 48 kHz、雙聲道 PCM WAV 替換。原檔為 65,291,564 bytes；替換檔為 268,435,456 bytes，尾端增加靜音。augmentation 記錄也進入更新清單。

基底由 Release Packager 和目前 Release Player 封出，含固定的測試發行者公鑰、16 個 PDX 分片、31,756 筆資源；驗證進度回報的原始資源總量為 8,596,468,270 bytes。測試金鑰只在隔離目錄使用，未進入原始專案或 Git。

| 驗收項目 | 結果 |
| --- | --- |
| 唯讀 `create-overlay --check` | 92.672 秒；2 筆變更、0 筆刪除、268,435,588 raw bytes；未寫 PDX |
| 原先未壓縮 overlay | 273,818,866 bytes；產製 96.281 秒 |
| 串流 zstd overlay | 67,162,678 bytes；產製 95.813 秒；比原先小約 75.5% |
| 串流版產製峰值 | 每 250 ms 取樣，Node＋Packager 合計 RSS 707,473,408 bytes；Packager 的 Windows peak working set 593,117,184 bytes |
| 串流版磁碟 | D: 可用空間從 283,791,364,096 bytes 降至取樣最低 283,723,665,408 bytes，約 67.7 MB 差額；完成時 283,724,136,448 bytes |
| 獨立驗簽 | `verify-overlay --require-signature` 通過，仍回報 2 筆變更／0 筆刪除 |
| 已安裝 Player | `Package/updates/patch.pdx` 被認作 `signed overlay=true count=1`；`ui-routes` 完成 22 route、23 張畫面擷取、0 個 error，程序 exit 0；Player 取樣 RSS 峰值 1,164,656,640 bytes |
| 回歸測試 | Release CTest `PrismatiX.SDK.PackagerContract`、`PrismatiX.Integration.Runtime.Resources` 2/2 通過 |

`ArchiveStreamWriter` 現可對檔案及 PDX stream 來源逐段壓縮，仍逐段核對來源大小、SHA-256 和 CRC。overlay 對至少 64 KiB 的檔案來源啟用壓縮，影片串流副檔名維持原樣；使用 zstd level 3、8 MiB window。Player 的未加密 zstd entry reader 仍限制最大 128 MiB window，並在讀到尾端核對長度與 CRC。沒有把整筆 256 MiB 內容載入 writer 記憶體。

這是同機、暖快取的兩次產製，時間不能作為一般機器的 SLA 或壓縮後必然加速的證據。D: 可用空間是整個 volume 的取樣，會受其他程序影響；不能視為精確的程序暫存位元組。Player 的 RSS 包含完整遊戲與 22 route 演出，不能歸因於這個 patch。每次產製仍全量讀驗 artifact 與基底；本次驗收沒有解決依賴級或 block-level 增量建置，也沒有測真實跨版存檔 migration。
