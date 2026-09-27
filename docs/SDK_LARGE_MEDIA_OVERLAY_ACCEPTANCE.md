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

## 基底 PDX 驗證優化

同一份基底有 16 個 PDX 分片、31,756 筆資源；其中 28,330 筆、6,002,475,350 raw bytes 是未壓縮 entry。基底驗證現在於同一輪讀取中核對這些 entry 的 CRC 與內容索引 SHA-256，並以最多四個 worker 驗證彼此獨立的分片。進度回報仍在呼叫執行緒、保持單調；失敗仍按分片順序回報。`PRISMATIX_PACKAGE_VERIFY_WORKERS=1..8` 可供受限設備或比較使用，預設最多四工。公開的普通 `Archive::OpenStream` 仍會在交出未壓縮 stream 前核對 CRC；只有完整內容驗證使用合併讀取。

| Release Packager，同機暖快取 | 耗時 | Packager 峰值 working set |
| --- | ---: | ---: |
| 修改前、串行 `verify-package` | 56.187 秒 | 60,854,272 bytes |
| 新版單工，第 1／2 次 | 56.047／54.953 秒 | 約 61.6 MB |
| 新版四工，第 1／2 次 | 17.203／17.250 秒 | 約 77.3 MB |
| 後續重編後四工驗證 | 19.125／18.031 秒 | 約 75–78 MB |
| 完整 `create-overlay --check`，修改前／後 | 92.672／54.266 秒 | 後者原生 Packager 約 591 MB |

單工與四工依「單工→四工→四工→單工」順序比較，全部驗證成功；最後一次大型 `verify-package` 與 Release Packager Contract／Runtime Resources targeted 測試也通過。`--check` 前後都回報 2 筆變更、0 刪除、268,435,588 raw bytes。測試新增「原始內容 SHA 相同但 PDX 內 CRC 欄位被改動」及「未壓縮 entry 的 stored size 被改動」拒絕案例，避免合併讀取削弱完整性檢查。

這些數字只適用於這台機器和這份暖快取資料；並行縮短時間但沒有略過任何資源。artifact 的全量雜湊、基底的全量驗證，以及實際 patch 寫入仍是 O(素材總量) 工作。
