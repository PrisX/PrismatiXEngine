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

同一份未加密基底有 16 個 PDX 分片、31,756 筆資源；其中 28,330 筆、6,002,475,350 raw bytes 是未壓縮 entry。基底驗證現在於同一輪讀取中核對這些 entry 的 CRC 與內容索引 SHA-256，並以最多四個 worker 驗證彼此獨立的分片。進度回報仍在呼叫執行緒、保持單調；失敗仍按分片順序回報。`PRISMATIX_PACKAGE_VERIFY_WORKERS=1..8` 可供受限設備或比較使用，未加密基底預設最多四工；加密基底固定單工，避免舊式整筆加密壓縮 entry 的記憶體用量相乘。公開的普通 `Archive::OpenStream` 仍會在交出未壓縮 stream 前核對 CRC；只有完整內容驗證使用合併讀取。

| Release Packager，同機暖快取 | 耗時 | Packager 峰值 working set |
| --- | ---: | ---: |
| 修改前、串行 `verify-package` | 56.187 秒 | 60,854,272 bytes |
| 新版單工，第 1／2 次 | 56.047／54.953 秒 | 約 61.6 MB |
| 新版四工，第 1／2 次 | 17.203／17.250 秒 | 約 77.3 MB |
| 後續重編後四工驗證 | 19.125／18.031 秒 | 約 75–78 MB |
| 完整 `create-overlay --check`，修改前／後 | 92.672／54.266 秒 | 後者原生 Packager 約 591 MB |

單工與四工依「單工→四工→四工→單工」順序比較，全部驗證成功；最後一次大型 `verify-package` 與 Release Packager Contract／Runtime Resources targeted 測試也通過。`--check` 前後都回報 2 筆變更、0 刪除、268,435,588 raw bytes。測試新增「原始內容 SHA 相同但 PDX 內 CRC 欄位被改動」及「未壓縮 entry 的 stored size 被改動」拒絕案例，避免合併讀取削弱完整性檢查。

這些數字只適用於這台機器和這份暖快取資料；並行縮短時間但沒有略過任何資源。artifact 的全量雜湊、基底的全量驗證，以及實際 patch 寫入仍是 O(素材總量) 工作。

## Artifact 與基底驗證重疊

在同一份 D: NTFS 隔離資料上，先以兩個唯讀行程同時量測完整 artifact 預檢與完整基底驗證：前者 27.650 秒、後者 20.176 秒，整輪由較慢的前者決定，約 28.3 秒。這確認兩個獨立讀驗階段有可重疊的工作；它們仍各自讀取並核對完整內容。

正式 `create-overlay --artifact` 路徑現於 Windows 同時執行 AllInputs 來源雜湊／規劃與基底 PDX 驗證。Packager 在驗證前對所有基底分片取得只允許其他讀者的 Windows handle，直到預檢結束或補丁發布才釋放；中途寫入與替換會被拒。基底 manifest／index 及分片大小／修改時間也在驗證前後、使用時核對。規劃失敗會請求取消尚未完成的基底驗證。非 Windows 平台維持串行順序，仍核對分片狀態，避免擴大寫入競態視窗。

| Release `create-overlay --check`，相同輸入 | 耗時 | 變更／刪除 | Node＋Packager 取樣 RSS |
| --- | ---: | ---: | ---: |
| 基底驗證並行化後、兩個階段仍串行 | 54.266 秒 | 2／0 | 698,748,928 bytes |
| Artifact 與基底驗證重疊 | 40.782 秒 | 2／0 | 715,341,824 bytes |
| 同上，另持有 Windows 基底唯讀 handle | 40.250 秒 | 2／0 | 705,150,976 bytes |

三次均回報 268,435,588 changed raw bytes，沒有產生 PDX；最後兩次使用同一份 artifact 和基底，僅 Native Packager 版本不同。原生契約另外驗證並行預檢與既有規劃計數相同、正式簽章 PDX 可獨立驗證、artifact 來源改動被拒，且 Windows 分片在驗證回呼中無法被寫入。Release Packager Contract／Runtime Resources targeted CTest 2/2 通過。不同時間的單次暖快取量測不構成一般機器 SLA；此改動沒有建立可跳過完整讀驗的 cache。

2026-09-28 續測同一份 D: NTFS artifact／base：先以更新前 Release Packager 重跑 `create-overlay --check`，總耗時 38.890 秒；後段最後一筆 `archive` 進度事件在 32.312 秒。發現 AllInputs 規劃已對每個 artifact 來源做 canonicalization，overlay 寫入只會打開變更項，卻在規劃後對 31,756 筆來源再做一輪 canonicalization。將這道後段檢查限於變更來源後，重編 Release Packager，同一命令耗時 33.140 秒，最後一筆 `archive` 事件在 32.125 秒。兩次的 `scan-artifact` 起迄約 0.36–2.3／2.5 秒、`validate-hash` 結束約 19.5／19.3 秒、`verify-base` 結束約 22.2／22.0 秒，改善集中在規劃後段；新跑的 CLI exit 0。Release Packager Contract 1/1 通過，新增測試確保變更來源改指 artifact 外的同內容檔時仍拒絕。這是兩次相鄰的暖快取樣本，未量新跑的 RSS，不能外推一般 SLA；每次仍完整雜湊 artifact 並驗證基底所有 PDX。
