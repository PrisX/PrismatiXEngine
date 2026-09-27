# SDK 優化交接（給下一位 agent）

更新：2026-09-27。此文件以當前兩個本機 repo 和實際測試為準；不是「全部完成」聲明。較完整的逐項發現與歷史量測在 `C:\Users\owenl\C\Projects\SenrenBankaPSX\docs\SDK_FEEDBACK.md`。該檔按時間追加，早段的「尚未實作」有些已被後段取代，閱讀時以最新段落與程式碼為準。

### 2026-09-27 後續進度

- `create-overlay --base ... --artifact ... --check` 現走正式 AllInputs 規劃、完整基底 PDX 驗證、Player／runtime 相符檢查和（提供私鑰時）發行者簽章金鑰檢查，回報變更／刪除數與變更 raw bytes，不建 staging／PDX；`--updates-dir`／`--optional-id` 亦可預檢。正式產製會重新驗來源與發布邊界，預檢不是事後不可變保證。Native Packager Contract 1/1、CLI mock 路由和小型真實 signed `patch → patch2` E2E 通過，預檢與正式產製計數一致。以 Release Packager、基底內原版 Debug Player、`sdk-signed-v1` 加 `artifact-v29` 實行唯讀 8 GB 級預檢成功：31,756 筆資源、3,321 筆變更、0 刪除、457,900,951 changed raw bytes，約 88.93 秒；沒有產出大型測試補丁。這兩份資料差異過大，仍不適合單一小變更或大型壓縮媒體 patch 實測；也沒有記錄最終 RSS／磁碟峰值。
- Release Packager 的 AllInputs 來源驗證現在以最多四個有界 worker 雜湊各自獨立的檔案，仍核對每筆宣告 SHA-256，錯誤按請求順序報出，並提供 `validate-hash` 進度與取消；`PRISMATIX_PACKAGE_HASH_WORKERS=1..8` 可供設備量測，預設最多四工。於 D: NTFS 外接 SSD、`artifact-v29` 31,756 筆、約 8.60 GB 輸入，以 `pack --artifact ... --check --verify-media` 唯讀暖快取比較：單工 48.43 秒→四工 25.18 秒；反轉執行順序後四工 25.11 秒→單工 48.29 秒，四次皆通過、deferredMediaCount=0。先前未固定快取的單次全量檢查為 160.04 秒，不可拿來計算改動收益。另 `sdk-signed-v1` 的 Release `verify-package` 唯讀全量基底驗證約 53.99 秒；這是另一份輸入與條件，不可直接加總成直接 overlay 耗時。Native Packager Contract 1/1 含多筆同尺寸改動、決定性錯誤順序與 AllInputs 取消。未測 8 GB 直接 overlay 產出、最終 RSS／磁碟峰值；並行只減少可重疊的讀驗時間，沒有安全驗證 cache 或 block-level 增量。
- `story.pxindex` 的 scene ID 重複、缺 locale source、來源檔不存在、entryScene 無法載入、required label 不存在，現在會在有原始 JSON 文字時指向索引宣告的精確行列；CLI 使用載入時已保存的同一份文字，格式或內容與編譯輸入不符時只回報路徑，不猜位置。正常無錯編譯不掃描索引位置。Authoring SDK 167/167、CLI 110/110（另 1 項 native E2E 依環境跳過）通過；Story 指令本身的 source span 與此索引診斷互補，其他動態素材依賴的來源位置仍待改善。
- `create-overlay --base <完整發行版> --updates-dir <前段簽章更新目錄> --artifact <新版 artifact> --output <patch2.pdx> --signing-key <私鑰>` 現可從已驗簽主更新鏈的最終 manifest/index 規劃下一段，不需完整 v2。`verify-overlay` 同樣接受 `--updates-dir` 驗下一段，並強制簽章；待驗檔已安裝在該目錄時會先排除自身。原生測試涵蓋 `patch → patch2 → adult`、完整基底與兩段簽章解析、前段損壞及寫入中遭改動時拒絕；小型真實 CLI 流程已建立且驗簽 patch2，含安裝後重驗。產出前重新解析整條鏈並核對目標文件與路徑；選裝包仍只掛在最新主更新之後。尚無 8 GB 鏈式產製及 Player 在這條新產製路徑的實機驗收。
- 大型輸入的唯讀配對檢查：`D:\PrismatiXBuilds\SenrenBankaPSX` 目前兩份有 ownership marker 的 `artifact-v29` 與 `artifact-sdk-20260924` 相對 `sdk-signed-v1` 的 content index 都有 3,321 筆指紋不同，這些舊資源大小合計約 462 MB；相對 `sdk-signed-v2` 為 3,323 筆、約 462 MB。這些不是乾淨的單一變更基線，故沒有用它們產出可能達數百 MB 的測試 PDX，也沒有改寫 D:。後續大型效能驗收應先確認相符的 artifact／base，再量測單一媒體變更。
- `create-overlay --base <完整發行版> --artifact <已發佈 artifact> --output <patch.pdx>` 現可不先建第二份完整發行版：原生 Packager 共用正式封裝的身分／manifest／content-index 規劃器；測試逐 byte 比對規劃文件與完整封裝輸出。此路徑對 artifact 做 AllInputs 雜湊、完整驗證基底發行版、固定規劃時的基底 manifest 並確認 Player 與 runtime library bytes 等於基底，然後只串流變更資源並用既有 verifier 驗簽與 exact-base；來源在規劃後改動、基底 manifest 不符、runtime 不符、未全量驗證的規劃均拒絕且不發布。原生 Packager Contract 1/1、當時 CLI 109/109，另以小型真實 CLI 流程完成 `build artifact → pack signed base → augment/update artifact → create-overlay --artifact → verify-overlay --require-signature`。目前沒有 8 GB 專案的直接 overlay 時間／RSS／磁碟峰值證據；完整 artifact 來源與基底仍須各讀驗一次。
- Player 啟動時現會核對 `authoring.uiContract` revision 1 與實際註冊的 ViewModel roots、當前值（含非空陣列 item 欄位）和 formatter 簽名；`GalgameUI` 提供自訂 formatter 註冊與其他 host 可呼叫的契約檢查。新增 `UpdateUiData`：已通過的契約會保留，host 晚些更新宣告 root（含原本為空的 item array）時，先在無訂閱者的候選 ViewModel 驗欄位，再通知正式 UI；無效更新保留舊值。宣告 root 現須為真正註冊的 property，不能只靠巢狀路徑假冒。原生 UI adapter 1/1 涵蓋空陣列晚更新、錯型／缺欄位原子拒絕及已掛載 UI 的合法刷新；Player Catalog Native Acceptance 先前 1/1 用帶有合約的封裝啟動通過。其他 host 需明確呼叫契約檢查與 `UpdateUiData`，任意直接寫入的自訂 ViewModel 不會自動受此 API 管理。

- 共用 identity namespace 的兩個 Story operation ID 撞名時，`PXSTORY1308` 現指出後一處的 source-map 行列，details 帶前一處 scene 路徑與行號；原先只有 scene 檔名。針對性 Story project 測試 10/10 通過。Story index 宣告與動態素材依賴等其他來源位置仍須個別改善。

- 動態 UI authoring 合約新增 `settings["authoring.uiContract"]` revision 1：host 明列真正註冊的 ViewModel roots、各路徑／陣列 item 欄位的 Variant 型別及自訂 formatter 輸入／輸出；SDK 在專案編譯時檢查宣告 root 下的 binding path、itemSource array、stable key、placement 數值欄位、formatter 型別與原生 TypeRegistry target。宣告根以完整路徑比對，例如 `settings.items` 不會誤涵蓋 `settings.other`；initial `player.uiData` 與 contract 矛盾也會拒絕。`docs/placed-item-authoring.md` 有完整範例。這是 authoring 端可驗的 host 承諾，不會自行註冊實際 ViewModel／formatter，也不保證執行期提供的值遵守宣告。另修 `dev` watcher 在一次編輯的事件串流尚未安靜時就建新版，避免重複 revision。Contract bundle check、SDK 166/166、CLI 108/108、dev 針對性連跑 3 次及 Senren 四語 368 scene／31,339 資源唯讀 validate 通過；全套 CLI 曾因 watcher 重複事件出現 107/108，修正後重跑 108/108。
- 真實大型 signed overlay 已完成正向驗收：用 9/25 的完整 artifact 封出 `sdk-signed-v1`，用目前 Senren 專案及 `--previous-package` 封出 `sdk-signed-v2`；兩版同一 Debug Player bytes、同一測試 P-256 發行者公鑰、31,756 筆及各約 7.488 GiB，fingerprint 不同，v2 保留 v1 的同版存檔 lineage。Release Packager 封 v1 約 5.28 分鐘、CLI 從來源完整建置／封 v2 約 9.03 分鐘。CLI `create-overlay --signing-key` 用約 1.83 分鐘產出 5,668,933 bytes（5.41 MiB）的 signed PDX，內容變更 3 筆／382,396 raw bytes、無刪除；`verify-overlay --require-signature` 獨立通過。將它安裝於 v1 的 `Package/updates/patch.pdx` 後，封裝 Player 記錄 `signed overlay=true count=1` 並掛載基底 31,756 筆與更新 7 entries；Windows D3D11 的 `ui-routes` 旅程完成 22 個真實 route、擷取 23 張畫面、0 個 error，程序以 0 結束。變更內容為 `Content/UI/SettingsPage5.pxui`、`Content/UI/SettingsPage7.pxui` 與 `project.pxproject`，前兩頁也包含在旅程中。建立時抽樣觀察原生 Packager `PeakWorkingSet64` 至少 77.4 MiB；未持續取樣到程序退出，不能當最終峰值。尚缺同規模大壓縮媒體 entry 修改與磁碟使用峰值量測，以及真實跨版存檔 migration。
- 真實大型舊存檔驗收：先發現 `out/build/x64-release` 的 Packager 編譯檔早於目前原始碼；用該舊工具做出的 `sdk-acceptance-v30` 雖可解密 v29 存檔，卻沒有傳遞 `compatibleSaveFingerprints`，不可當作載入成功。重編 Release Packager 後，以目前 Senren 專案和 `--previous-package` 指向 v29 重建 `D:\PrismatiXBuilds\SenrenBankaPSX\sdk-acceptance-v31`：31,756 筆、`Content.pdx` 7.488 GiB、整個 CLI `pack` 約 8.13 分鐘；新版 manifest 列出 v29 fingerprint `bff820a078ce41188d58a2fd64f04e162a7f50093e49c1fea1a5386bfcbcb4fa`。Player 新增僅由 `PRISMATIX_E2E_JOURNEY=save-load` 啟用的通用 slot 2 載入旅程；在隔離的 `PRISMATIX_PLAYER_DATA_ROOT` 放入 v29 原始 `autosave.pxsav` 複本，Windows D3D11 Player 真正執行 `LoadSlot(2)`、記錄 `Loaded slot 2` 並以 0 結束。這證實 v29 同 content/save version 存檔可在新獨立包恢復；不證明 v28 lineage、跨版 migration、簽章 overlay 或畫面演出。v29 沒有固定公鑰且 Player bytes 與目前版不同，不能直接用它當大型簽章 overlay 基底。
- `compileProject` 現對明確宣告的 `settings["player.uiData"]` 初始 ViewModel 值，提前驗 UI binding 與 `itemSource`：來源陣列、每筆唯一且非空的穩定 key、placement 數值、`item.<field>` 是否存在、已知內建 formatter 的輸入型別，及提供原生 UI TypeRegistry 時的目標型別。它只檢查宣告過的資料根，不猜測 host／腳本執行期註冊的路徑，也沒有自訂 formatter 宣告來源。Authoring SDK 163/163、CLI 108/108 與 Senren 四語 368 scene／31,339 資源唯讀 validate 通過；該 UI 合約已進入 v31 大型封裝，但沒有逐項畫面驗收。
- CLI `build`／`dev` 現把逐 scene Story 編譯結果保存為每專案單一 `.prismatix/cache/story-scenes-v1.json.gz`，跨 CLI 行程可重用；改一個 scene 時其他未變 scene 仍可跳過解析與編譯，改 sidecar／跨 scene label／共用編譯輸入時使用既有精確快照失效。快取另以實際 CLI／Authoring SDK 編譯後 JS 與 Node 版本做身份綁定，損壞檔回退重編；檔案上限 128 MiB 壓縮／512 MiB 解壓，沒有新結果時不重寫。SDK 160/160、CLI 108/108 通過，跨獨立行程的重用、單 scene 修改與損壞回退已驗；**尚未在四語大型專案量測 cache 大小、CPU、記憶體，也未使媒體封裝驗證增量化**。原 `validate --runtime` 仍為唯讀全編譯；此快取不跳過發布前來源與成品驗證。
- CLI `build`／`dev` 現在保存載入時的作者文字快照（project/JSON 文件、Story、外部 ID sidecar、locale、shader 及本地 TSX module graph），重複讀到不同文字也立即拒絕，並在正式 artifact 發布前重讀比對；原本不存在的 project manifest 或 Story sidecar 若中途出現亦拒絕。已存在 artifact 的「未變直接保留」路徑同樣在鎖內檢查，變更時保留原成品。CLI 全套 107/107 通過；新增整合測試於發布中改 Story、加入 sidecar、及未變 artifact 驗證中改 Story，三者都拒絕且原 ownership marker／Runtime IR 原樣保留；最後的重複讀取補強後 targeted 2/2 與四語 368 scene／31,339 資源的 `validate --runtime` 通過。這只核對實際載入的文字，媒體仍走既有複製／指紋驗證；執行中 TSX 自行讀取的任意外部檔案無法由靜態 module graph 保證。此安全失效邊界也適用於上列新快取。
- 新一輪四語 368 scene／31,339 資源的唯讀 `validate --runtime` 通過，未開 CPU profile 的單次耗時約 8.96 秒；帶 profile 的前一次約 9.97 秒，兩者條件不同，不能據此宣稱加速比例。Profile 顯示 `canonicalJson`、穩定 ID FNV、Story 編譯仍是主要 CPU 熱點。曾試驗不複製物件的串流 canonical JSON 原型，10 萬個合成 operation 約 579 ms，慢於原實作約 121 ms，故未採用。穩定 ID 改為對 ASCII 前綴直接處理同一批 UTF-8 byte，Unicode 後綴維持 `TextEncoder`；舊 BigInt 演算法對照、Authoring SDK 159/159、CLI 106/106、四語 `validate --runtime` 均通過，10 萬筆合成 ASCII ID 暖機後約 45 ms → 40 ms，單次量測不代表大型專案固定收益。Story 跨場景不存在 scene／label 的診斷現帶作者原始命令的 source span，CLI 可顯示行列；缺少來源位置的 index/required-label 診斷仍待擴充。這輪效能修正本身未減少發行版的全量安全核驗。
- 原生角色資源載入器現會驗證並保留 `.pxcharacter.displayNames`，GameCatalog 可依 exact locale 選擇姓名，缺少該語系時回退至 `displayName`；穩定 UUID／alias 查找不受翻譯姓名影響。VM 的 `[say char=...]` 在未明寫 `speaker` 時使用目前語系，Player 與原生／Wasm Preview 均同步該語系，明寫的 speaker 仍優先。CharacterResources Contract、Runtime IR Runtime Adapter、Preview Localization Contract、Player Catalog Native Acceptance 均通過。JavaScript runtime SDK 目前只有唯讀 dialogue model，沒有獨立的 JS `say` 作者 API；先前表格的「JS say」描述應以此為準。Senren 既有 narration/name-prefix 未變，改 Story operation kind 前仍須真實舊存檔 migration 驗收。
- 非 Player 原生 host 現可用 `GameSettings::LoadWithInputBindings`／`SaveWithInputBindings` 同時處理鍵盤與滾輪設定，stock Player 也改用同一路徑。`Input::RestoreUserBindings` 在暫存副本上先重設作者預設並核對兩張保存表，任一表無效時不會只套用另一張；設定載入失敗亦保留原物件與原檔。`docs/input-actions.md` 補非 Player host 的 `UIContext::SetInputBindingService` 保存範例。Native SaveLoad integration 1/1 驗正常讀寫、晚註冊 action、舊檔缺滾輪及無效鍵碼／滾輪的原子拒絕；Player Catalog Native Acceptance 1/1 通過。此 API 不會自行挑選 host 的 per-user 路徑或加密金鑰。
- Node Story 來源 loader 與 CLI authored text 載入改用共用 `readBoundedUtf8File`：從已開啟的檔案逐段讀取，檔案在初次 stat 後增長也不能越過 16 MiB 上限；錯誤 UTF-8 直接拒絕，不再被 Node 的字串解碼替換成 U+FFFD；BOM 仍保留供既有契約明確檢查。Authoring SDK targeted 4/4 與全套 159/159、CLI 原有 105/105 加新增壞 UTF-8 targeted 1/1 通過；四語 368 scene／31,339 資源 `validate --runtime` 通過。這是大型來源匯入的界限與資料正確性補強；後來的跨次快取仍以這項有界讀取取得新來源。
- 正式 `RunPackager` 現在不再於 preflight 對每個非 `.ts` 輸入先做一輪全檔 SHA-256；會被結構驗證讀取的文件在該次讀取時核對宣告 fingerprint，小型 JSON identity／effect 來源在再次讀取時核對，其餘輸入由 archive writer 或整片重用驗證。獨立 `ValidatePackageRequest`／`pack --check` 仍保留原先 eager 非媒體核對；`.ts` 也維持 eager。新增合約測試涵蓋未解析非媒體來源延後至 writer 拒絕、結構文件在 preflight 後讀取前變動即拒絕，且都保留既有輸出。Native Packager Contract 1/1、Player Catalog Native Acceptance 1/1 通過；這只省下正式封裝一次重複來源讀取，該輪尚無 8 GB 兩版 overlay；後續已補 signed 雙版本實測，見本節首段。
- Packager 在驗證請求後再次讀取小型 JSON 來計算 package identity、或再次讀取自訂 effect manifest／HLSL 來編譯時，現立即核對該次讀到的 bytes 是否仍符合宣告的 SHA-256；否則拒絕發布並保留既有發行版。原生 contract 以進度回呼在兩個階段改動同大小來源，均驗證拒絕；Native Packager Contract 1/1 與 Player Catalog Native Acceptance 1/1 通過。這修的是讀檔一致性，不代表全量素材雜湊或舊 PDX 驗證可略過，也未解決大型封裝的多輪 I/O。
- 多語 Story 對齊的每個 operation 現直接比較非本地化字串參數的鍵與值，避免反覆排序並序列化 canonical JSON；`customNode` 的資源參數仍先移除並正規化 JSON，再檢查其餘參數。新增非資源參數變更的拒絕測試；Authoring SDK 157/157、CLI 105/105、四語 368 scene／31,339 資源 `validate --runtime` 通過。同機單次驗證修改前約 10.57 秒、修改後兩次約 9.74／10.12 秒，快取／負載未固定，不能當穩定加速率；Story 編譯與最終 artifact 序列化仍待另行量測。
- Player `settings.reset` 現會一併重設滾輪綁定；`.pxgame.inputActions` 的作者滾輪方向／門檻會獨立保留，不被使用者覆寫取代，並於重設時還原；未註冊動作的延後覆寫也會清除。Input 原生 contract 已驗作者預設、內建預設與延後動作；正式封裝實按仍待驗。
- 穩定 ID 的 FNV-1a 64-bit 仍保留原字節與兩個 32-bit limb 運算；新版用固定 4 KiB `TextEncoder.encodeInto` 緩衝區避免每個 ID 配置一份 UTF-8 陣列，舊環境仍退回 `encode`。舊 BigInt 演算法 1,000 個混合 Unicode 樣本、10 萬字長輸入、緩衝邊界後的 surrogate pair 逐值相等；Authoring SDK 157/157、四語 31,339 資源 `validate --runtime` 通過。接在目錄式存在檢查改善後，同機單次驗證約 9.78 秒（前一次約 10.73 秒），快取／負載條件未固定，不能當固定加速率。
- 大型四語專案的 `validate --runtime` 重新量測：原版 31,339 資源約 13.00 秒，CPU profile 有約 1,282 個 `existsSync` self samples／8,647 總 samples。CLI 現在每次同步編譯對資源檢查按父目錄建立檔名集合，大小寫不一致、符號連結與讀目錄失敗仍回退逐檔 `existsSync`；每次編譯重建集合，實際 artifact 發布仍重驗檔案 bytes。修改後同機一次約 10.73 秒；再跑 profile 有 1 個 `existsSync` self sample／7,377 總 samples，`npm run cli:test` 105/105 與四語 `validate --runtime` 通過。快取及系統負載可能不同，不能把兩個單次耗時宣稱為穩定加速率；canonical JSON 和 Story 編譯仍是下一層熱點。
- 單一大型變更資源的 overlay 路徑新增可調整大小的原生壓力測試：先產生 32 MiB 的 MP4 型資源，僅改中央 1 byte，再封兩個完整小型發行包、產單檔 PDX、獨立驗證與 seek 讀回改動位元；以 `PRISMATIX_PACKAGER_RSS_BYTES=268435456` 改跑 256 MiB 也通過 Native Packager Contract 1/1（73.29 秒），既有測試的程序峰值工作集增量 64 MiB 上限亦通過。暫存測試資料由 fixture 清理；這是合成單資源，不等於 8 GB／31,000 資源的實際發行版時間、磁碟峰值或壓縮大 entry 驗收。
- 跨目錄獨立發行版新增 CLI `build`／`pack`／`run --previous-package <舊發行目錄>`：驗證舊包及同遊戲／發行者後，將其作為原生 Packager 的讀取來源，繼承相容舊 fingerprint 與舊金鑰 reader；舊目錄不改動，新輸出須為新建或空目錄，打包期間舊 manifest 改動會阻止發布。CLI 105/105 通過，新增測試涵蓋驗證順序、來源傳遞、錯誤遊戲／公鑰、佔用輸出及來源改動。隨後 Native Packager Contract 1/1 驗獨立新版讀取舊加密 slot 且舊包不變，Player Catalog Native Acceptance 1/1 驗另目錄完整新版 Player 載入舊 fingerprint／舊金鑰 slot，並完整走完 UI 旅程；皆為合成資料，真實舊版玩家資料仍待驗。
- 舊加密 slot 在新版 Player 的兩段簽章更新鏈中已做原生 E2E：先以舊 fingerprint 金鑰及舊 package fingerprint 建檔，同內容版本的替換發行包在 `persistence.compatibleSaveFingerprints` 明確記錄舊身分；Player 合併基底／已驗簽主鏈的舊金鑰與相容 fingerprint，不讓選裝包單獨擴張保存身分。載入後再保存同一 slot 時，原始加密 bytes 留在 `.legacy.bak`，新 slot 使用穩定金鑰；選裝包啟用後重試舊 slot 亦通過，第三段 contentVersion 更新亦可對原始舊 slot 執行明示 migration。CLI 式 staging Packager 現從 `cacheSourceDir` 繼承舊金鑰與同版 lineage，而非只看空白 staging output。Native Packager Contract 1/1、Player Catalog Native Acceptance 1/1、當時 CLI 104/104 通過。測試使用合成舊版資料；真實玩家資料仍待驗。
- `CreatePackageOverlay` 現直接從目標 PDX 的資源串流寫出 changed entries，寫入期間逐筆核對預期大小與 SHA-256，省去先解出全部變動資源的暫存檔及再次讀取。輸出仍先在同磁碟 staging 驗證、再不可覆寫發布；不能據此宣稱 8 GB 大包吞吐或峰值已驗。Native Packager Contract 1/1、Runtime Resources Integration 1/1 通過，含串流複製與錯誤 SHA 拒絕。
- 舊 revision-2 `config.dat` 只有鍵盤改鍵、沒有 `wheelBindings` 的原生整合驗收已補：沿用 Input 預設滾輪、之後改滾輪可保存並於重開後還原。Player Catalog Native Acceptance 也以加密的無滾輪設定從封裝匯入，驗證旅程後再存檔帶有正確滾輪預設。設定檔 `format`／`schemaRevision` 型別損壞時，`GameSettings::LoadWithFallback` 現會安全回報失敗，`SavePreservingExisting` 保留原 bytes，不再讓未捕捉的 JSON 型別例外中斷啟動。Native SaveLoad integration 1/1、Player Catalog Native Acceptance 1/1 通過；人工實按與實際舊版玩家資料仍待驗。
- 選裝包 `--optional-pack <id> on|off` 的 per-user `optional-pack.txt` 現使用 SDK 共用的同磁碟原子寫檔，避免直接截斷舊偏好；仍維持原有單 ID 格式。Native SaveLoad integration 1/1、Player Catalog Native Acceptance 1/1（含兩段主更新、成人選裝包開關與存檔）通過。多個選裝包同時掛載不能只改偏好格式：每個簽章 pack 都含完整目標 manifest/index，需先設計可驗證的組合與衝突合約，故本輪沒有冒然改變限制。
- 針對 368 scene／四語的 `validate --runtime` 做 CPU profile：總耗時約 15.5 秒，全部 Story 與 sidecar 約 91 MB 的單獨讀取量測僅約 95 ms，因此暫不新增以 mtime 判斷的來源快取。熱點是穩定 ID 的逐 byte BigInt FNV-1a、資源 `existsSync` 與 canonical JSON。SDK 現用兩個 32-bit limb 計算**同一個** FNV-1a 64-bit，並讓 Story／sidecar／專案正規化共用；1,000 個決定性混合 Unicode 樣本及長字串逐值對照舊演算法，Authoring SDK 157/157、Senren 四語 `validate --runtime` 通過。改後單次同機量測約 12.9 秒；快取條件不同，不能宣稱固定加速百分比。資源存在檢查與全量 canonical 序列化仍是待優化熱點。
- UI `TextureRect` 現可直接以 `path` 或資源 UUID 使用 `.pxportrait`，與 Stage 共用有界 descriptor 解析；靜態 UI 依賴預載會展開成圖層圖片，Runtime 換圖在新圖層載妥前保留舊圖。同時修正 `runtimeType=TextureRect` 原本忽略 `assetId` 的問題。Native UI adapter／Splash／Stage＋UI 像素整合 targeted 3/3 通過；正式 Player 封裝尚未實測。
- Authoring／CLI 使用原生 UI TypeRegistry 驗證 `runtimeProperties` 與 Visual State Group override 時，除名稱／型別外，現在也會拒絕不在 `enumChoices` 的字串，例如無效的 `bindingDevice` 或 `visibility`；不把編輯器的 numeric `range` 一律當 Runtime 硬限制。原生明確要求 0–100 的 `wheelThreshold` 則另外在 UI scene／component 編譯時拒絕錯誤值。Authoring SDK 156/156、列舉值改動後 CLI 104/104，以及 wheel 門檻補驗後的 CLI targeted 1/1 通過。
- Player 更新鏈尋找現在先讀各 PDX 的小型 manifest，以 `basePackageFingerprint` 過濾與目前版本無關的候選檔；只有匹配者才做完整簽章、exact-base 文件及 payload 驗證，避免多段更新反覆全量驗證舊 PDX。Native Packager contract 1/1 通過，含逆檔名順序的兩段更新、選裝包及相同 base hint 的壞簽章拒絕；Player Catalog Native Acceptance 1/1 通過。這是驗證路徑減少，尚無 8 GB Player 啟動量測。
- `WheelBindingButton` 現可在 TSX 設定頁擷取實體滾輪方向、顯示目前綁定、設定動作門檻與方向文字；可選 Delete 清除，Escape／再次點擊取消，成功時觸發 Player 的設定保存。擷取中的滾輪事件不再觸發舊動作或穿透到下層捲動。Authoring SDK 156/156 與 Native UI/Accessibility targeted 3/3 通過；正式封裝實按、舊設定 E2E 仍未做。
- Windows `CreatePackageOverlay` 的最後發布改為同磁碟不可覆寫原子移動，讓不支援 hard link 的磁碟也能發布已驗證 PDX；驗證後才出現的同名輸出仍須保留。Native Packager contract 1/1 通過，含競爭輸出測試。其他平台仍使用 hard link；當時正式大型 overlay 尚未實測；後續已完成 signed 雙版本正向驗收。
- `dev` 的 Story 編譯快取當時已可在單一 scene 修改後重用其他未變 scene 的 label 摘要、編譯結果、source map 與資源依賴；普通文字修改不再解析其餘 scene，cross-scene label、sidecar 或共用編譯輸入變更時仍會安全重解析／重編。快取不保留 AST，且編譯使用進度回呼前擷取的來源快照。Authoring SDK 155/155、CLI 104/104（最後來源快照修正後另跑開發模式 targeted），及 Senren 四語 368 scene／31,339 resources 的 `validate --runtime` 通過。大型專案的 `dev` 單 scene 熱編譯耗時／記憶體尚未量測。當時只在同一 CLI 行程內有效；現已加上本節首項所述的跨行程保存，但仍會重新讀取所有 scene，並非串流 importer。
- Wheel 使用者設定已補 `Input::CaptureWheelBindings`／`RestoreWheelBindings`、Runtime SDK `ctx.input.wheelBinding`／`setWheelBinding` 與 Player per-user `config.dat` 保存／還原；舊 revision-2 設定缺少欄位時沿用作者預設。Native 輸入／設定／JS Host targeted 測試及 Runtime SDK 測試通過；非 Player host 仍須自行觸發持久化，正式封裝實按未驗。
- Story 現可用 `[motion asset=...pxmotion await=true speed=...]`／`[motion.stop]` 在 Stage 播放全畫面 `.pxmotion`；Stage 預檢 descriptor、圖片與音效資源，圖片預載完成後才推進時間，音效 marker 依 bus 發聲，等待非循環動畫結束後續行。進度、速度與等待狀態寫入存檔並於還原後接續；Authoring Story／Project、Runtime IR、原生動畫／存讀檔 targeted 測試通過。尚未在正式大型封裝實機驗證畫面。
- `Input::RemoveKeyBinding` 和 Runtime SDK `ctx.input.removeKeyBinding` 現可只移除一個鍵，保留同動作其餘按鍵順序及 gamepad／wheel；原生與 JS 測試涵蓋移除後的保存／載入。後續 `Input::ReplaceKeyBinding` 可原位替換單鍵，`KeyBindingButton` 可選取、刪除／替換單鍵，並可啟用衝突二次確認；原生與 Authoring SDK targeted 測試通過。
- PDX 串流讀寫的 CRC-32 改為分段查表計算；Packager 的寫入／輸入驗證 CRC 亦改用查表。CRC 標準向量與原有封裝／VFS contract 通過。這是演算法路徑改善，尚未以同條件大型素材量測吞吐，不能聲稱某個百分比加速。
- `CreatePackageOverlay` 現在比較完整驗證前、後與發布前的兩份 manifest／content index 原始 bytes；驗證回呼期間改寫 target manifest 的測試確認拒絕發布。這降低 metadata TOCTOU 風險，但不能保證同時可寫的 PDX／素材不變，也不是發行者認證。
- Native Debug 指定的 Packager、Runtime Resources、Player BootSplash CTest 3/3 通過；`npm run cli:build` 與 `npm run cli:test` 104/104 通過。Senren `npm test` 21/21 通過，`npm run validate` 四語 368 scene／31,339 resources 通過。本機 `dist` 當時僅 `senren-banka-psx-authoring-v29` 具有 `Package/content-index.json`；其後另在 D: 建成 v31，但兩版 Player 不同且都未固定公鑰，後續已完成大型 signed overlay 的時間與 patch 大小量測；記憶體／磁碟峰值尚未完整量到。
- 新版 Release Packager 對 Senren `senren-banka-psx-authoring-v29` 執行 `--verify-package` 成功，檢查 1 個 PDX、31,756 筆、8,596,468,270 raw bytes。第二次暖快取（Windows、此機器 C:、Release、同一發行包）耗時 57.27 秒，程序峰值工作集約 63.4 MiB；首次冷一些的讀取依輸出檔時間約 111 秒，但量測腳本結尾格式錯誤，無可信峰值，不能作前後加速比較。該次單版驗證本身不是兩版本 overlay 或 Player 驗收；後續另有正向實測。
- 使用者已選擇 Player 自動載入更新採「基底發行包固定發行者公鑰、驗簽且 exact-base 才掛載」。unsigned `patch.pdx` 仍不可自動套用。
- 後續已加入 `pack`／發行版 `build --publisher-key <file>`：公鑰寫入 package manifest 與 package fingerprint，overlay 建立／驗證拒絕換鑰；Native Packager contract 與 CLI 104/104 通過。
- 再後續加入 P-256 overlay keygen、revision 2 簽章與 `--signing-key`／`--require-signature`；Player 在 `Package/updates` 尋找唯一 signed exact-base 更新鏈，依依賴順序掛載 `patch.pdx`、`patch2.pdx` 等，檔名不決定順序；失敗回基底，基底 archiveKey 不被目標 manifest 取代。再加入 revision 3 `--optional-id adult` 簽章選裝包：預設關閉，使用者以 `PrismatiXPlayer --optional-pack adult on|off` 寫入 per-user 設定並重啟生效，且只對目前主更新版本適用。暫時一次只支援一個啟用中的選裝包、尚無遊戲內開關。
- 先前針對性驗收：Native `PrismatiX.SDK.PackagerContract` 與 `PrismatiX.Player.CatalogNativeAcceptance` 2/2 通過，後者包含兩段主更新、選裝包預設關閉／指令啟用／停用、unsigned 拒絕；`npm run cli:build` 和 `npm run cli:test` 104/104 通過。停用的選裝包現在只讀小型中繼資料，不驗全量 payload；損壞簽章的啟用選裝包會被略過，主更新繼續運作。
- 隨後把同版舊存檔限制在**已驗簽主更新鏈的祖先 fingerprint**，再檢查 Runtime 文件與執行錨點；不接受任意同版 fingerprint。擴充 Player 原生 E2E：從基底版產生真實加密 slot，兩段同版更新後載入、選裝包啟用後再載入；第三段更新提高 contentVersion 並帶明示 save migration，載入基底 slot，同時以 tombstone 遮蔽前段更新新增的資源。最新 Native Packager／Player 2/2 通過。這是小型 Player 驗收，尚非 Senren 舊 key／8 GB 發行版驗收。
- Senren `.prismatix` 的舊驗證 artifact 約 8.01 GiB、現有無 ownership marker 的 `artifact` 約 6.67 GiB，以及舊 v28 截圖約 0.05 GiB，均已確認可重建／無腳本引用（現有 `artifact` 為 `pack --artifact` 不可直接使用）。此環境的自動審核仍阻擋對精確路徑的遞迴刪除，**尚未清理任何目錄**。`system-ui-extract`、`motion-source`、ID backup 和舊玩家資料應保留。

交接基準：Engine `1948fac`（`feat: create verified single-file PDX overlays`）；SenrenBankaPSX `61337a8`（`docs: record verified overlay creation status`）。本文件另外提交；若接手時 HEAD 已不同，先核對變更而不是假設這兩個 SHA 仍是最新。

## 使用者要求與範圍

- 原始《千戀＊萬花》完整 1:1 移植目標，使用者已明確要求暫停；不要把遊戲畫面尚未還原當成 SDK 優化已完成，也不要擅自重啟完整移植。
- 當前 SDK 目標是：把實作過程中發現的不友善、繁瑣、難用之處盡可能優化，並使用 `feat:`／`fix:`／`chore:`／`docs:` 等慣用格式提交。此目標仍有未完成工作；本交接不宣稱完成。
- 使用者最近明確說「先別繼續搞 M2，趕快把其他 SDK 優化做完」。不要把 M2 Cheeseware 原 `.mtn` 還原或逐幀比對列為目前工作主線。`.pxmotion` 的基本能力已存在，但 M2 原動畫仍不等價。
- 2026-09-27 再確認：當前目標是收尾這次移植過程暴露的**可重用 SDK 問題**，不是把《千戀＊萬花》完整移植。遊戲特有畫面、素材、25 項原作快捷鍵的逐一映射與 1:1 視覺對照只用於辨識 SDK 缺口，不計入 SDK 完成條件。

工作目錄：

- Engine／SDK：`C:\Users\owenl\C\Projects\PrismatiXEngine`
- 實際移植驗證專案：`C:\Users\owenl\C\Projects\SenrenBankaPSX`
- 原始抽取素材：`C:\Users\owenl\C\Extracted`（來源，不要清理或改寫）
- Engine `docs/architecture/PRISMATIX_ENGINE_AUDIT.md` 是另一份未追蹤、使用者所有的舊審查文件；**保留，不要順手 stage／提交／刪除**。它的基準是 2026-09-06 的 `0d65047`，其發現須與現在程式碼重核，不能直接當最新狀態。

## 已完成、但有明確邊界的能力

| 領域 | 已有能力／證據 | 不可宣稱的事 |
| --- | --- | --- |
| Authoring | Story 分檔／跨場景 `compileStoryProject`、外部穩定 ID sidecar、TSX UI、具名 UI 元件、`ItemsGrid`／`ItemsList`、`Modal`、分層立繪、`SpriteText`、typed props／Action／binding 驗證、resource includes 與正式 `augment`。Senren 92 scene × 四語的 `validate --runtime` 曾通過，約 31,339 個資源。 | 並非原 KAG/PSB 到 Authoring 的通用 importer；所有特殊 UI／動效也未完整 1:1。 |
| 輸入與設定 | Native keyboard／wheel／具名 action、Player 設定持久化、按鍵捕捉、`KeyBindingButton`／`WheelBindingButton`、追加／逐鍵移除／原位替換 API、逐鍵選取 UI、可選的共用鍵二次確認、Delete 清除綁定、衝突拒絕、全螢幕動作。Senren 原作 25 項快捷鍵接通 13 項。 | 其餘 12 項仍是占位；原版衝突對話框外觀與 F4 原版尺寸切換未還原；尚未在最新 8 GB 封裝實按或驗舊設定遷移。 |
| UI／轉場／呈現 | Route/splash/動態 UI 圖片預載、無動畫 route 的 presentation hold、文字描邊排版修補、`.pxmotion` rev 1–11 的圖層、keyframe、atlas frame、音效 marker、基本混合、alpha mask、局部時間、曲線、tint、tickRate 等及 TSX `<Motion>`。新版 signed overlay Player 也完成 22 route／23 張截圖 E2E。 | 無原版 M2 等效轉換；原 `.mtn` 特殊曲線／粒子／複合語意未完整對齊，沒有現在版以原資產做逐幀 frame-time 與視覺一致性驗收。不能再把「沒有 alpha mask」照抄為現況。 |
| 大型封裝 | 正式 PDX 分片、未變分片重用、壓縮 entry cache、`pack --check`／`--verify-media`、`verify-package`、provenance 安全保留成品／僅替換 runtime、階段進度與 CLI 發布驗證平行化。可從 published artifact 直接規劃 signed overlay，並沿已驗簽主鏈續產 patch2／選裝包，不需逐版完整目標發行版；目前僅小型真實 CLI E2E。Senren 曾完整封裝 16 PDX（約 7.50 GiB）並逐筆驗證 31,756 資源／8,594,731,766 原始 bytes。 | 仍沒有完整 dependency-aware／block-level 增量構建；直接 overlay 仍全量讀驗 artifact 與基底，8 GB 同規模速度／峰值尚未實測。 |
| 更新 | 已有明示執行的目錄補丁；單檔 PDX overlay 可建／驗；revision 2 以固定發行者公鑰驗簽，Player 在 `Package/updates` 載入唯一 exact-base 更新鏈且失敗回基底；revision 3 選裝包可簽出並以 per-user 選擇啟停。小型 Player E2E 已測同版存檔、版本 migration 與跨更新 tombstone。 | 一次只支援一個選裝包且無遊戲內開關；v29 同版存檔恢復及 8 GB signed 雙版本主更新已驗；真實跨版 migration／大媒體 patch 未驗。 |

上表僅列主線。詳細的成因、SDK 改動、原版差距與每次測試條件，請查 `SenrenBankaPSX/docs/SDK_FEEDBACK.md`；尤其不要把其中舊段落的「未有分片／未有 overlay 產製」照抄成現況。

### 尚未收尾的 SDK／採用項目（避免被「已有 API」掩蓋）

| 項目 | 目前做到哪裡 | 仍缺什麼／驗收條件 |
| --- | --- | --- |
| Story API／大型匯入 | `compileStoryProject` 解析跨 scene；單檔誤用會報 `PXSTORY1215`；368 份劇本以 sidecar 保存 531,812 個執行錨點，`ids --sync` 可預覽差異。`build`／`dev` 可跨行程重用未變 scene 的 label 摘要與編譯結果；跨場景不存在 scene／label 的診斷已有原命令 source span。 | 仍會重讀所有 scene；`validate --runtime` 仍全編譯，通用串流 importer、其餘跨檔錯誤的精確位置與移除／重排錨點的安全遷移策略仍缺。PSB→Story 目前是 Senren 私有轉換器；大型專案跨行程快取效益未量測。 |
| 多語姓名與存檔 | `.pxcharacter.displayNames`、`@alias`／`@"顯示姓名"` 可產出對應 speaker；原生 `say` 未明寫 speaker 時也可按 Player／Preview 當前語系顯示姓名，缺值回退預設。 | Senren 為保留舊 operation kind 仍以 narration/name-prefix 為主，未整批採用；改劇本操作種類前須做真實舊存檔 migration 實測。JavaScript runtime SDK 目前只有唯讀 dialogue model，尚無獨立的 JS `say` 作者 API。 |
| 立繪 | `.pxportrait` rev 2 有 base／expression 標記、`transition=auto`、逐層淡化與前瞻／還原圖層預載；UI `TextureRect` 可直接顯示 descriptor 並預載其圖層；Senren 已產生 2,941 descriptor／761 共用圖層並通過 validate。 | descriptor 內動態 dress／pose／face 編輯、所有角色 placement profile、原圖逐句畫面／首次出現 frame-time、舊存檔恢復均未完成正式驗收。最新大型發行包未因 rev 2 重建。 |
| UI contract／素材依賴 | typed props、原生 TypeRegistry、Action 參數、canonical UiValue、UI 動態圖片預載、`ItemsPlaced`／`Modal` 已補。`player.uiData` 初始值與 `authoring.uiContract` 明列的 host ViewModel roots、item 欄位、placement 及自訂 formatter 輸入／輸出，可在 SDK 階段對 binding 與原生 target 做型別驗證；Player 啟動時也核對已註冊的 roots、現有值與 formatter 簽名。host 透過 `UpdateUiData` 的晚更新會先驗保留的契約，包含空陣列後來出現的 item。 | 非 Player host 須自行註冊值／formatter 並明確使用驗證及更新 API；任意自訂繪圖、Story 中途素材與音效的預載無通用保證。18 個設定開關雖已模板化，動作仍是 `settings.noop`。 |
| 文字與路由演出 | 已修文字 outline 雙層換行差異、未變文字每幀重排，以及 route crossfade 暗谷／預載閃白。 | 正式單次 layout/mask 的文字效果管線、依事件 author 的白幕／shader／遮罩轉場與通用 route motion graph 缺；最新大型封裝與原版逐幀對照未做。 |
| `.pxmotion`／Story 動畫 | 格式已演進至 rev 11，TSX UI／splash 可播；Story `[timeline]` 可控 `.pxanim`／`.pxtimeline` 的 await/speed；新增 `[motion]`／`[motion.stop]` 直接在 Stage 播放 `.pxmotion`，含預載、音效 marker、等待與存檔還原。 | 尚缺以正式大型發行版驗證圖層畫面及音效時序；M2 `.mtn` 轉換器／逐幀驗證暫停。 |
| 輸入／設定 | `.pxgame.inputActions` 有 context、key、wheel、route／`uiAction`；鍵盤捕捉／追加／逐鍵移除／原位替換／整組清除、wheel 方向／門檻可保存；`KeyBindingButton` 可逐鍵選取與衝突二次確認，`WheelBindingButton` 可實體捕捉方向；原生 Player E2E 已驗加密舊設定缺滾輪欄位的匯入與保存。 | 非 Player host 須自行觸發持久化；實際舊版玩家資料與正式封裝人工實按仍待驗。原版衝突對話框屬遊戲 UI，仍未還原。原作剩餘 12 項與多數 settings.noop 須先判別可攜 SDK 能力，不能一律當作 SDK 缺陷。 |
| 更新與持久化 | 可顯式安裝目錄補丁；簽章更新鏈可建／驗／由 Player 載入或回基底；同版舊存檔可沿已驗簽鏈或明確發行 lineage 的舊 fingerprint 載入並檢查 Runtime 文件／錨點；合成舊金鑰 slot 已通過 Player E2E，覆寫時保留原始備份；選裝包可獨立啟停且存檔身分保留主更新 fingerprint；Player 資料已移至 per-user 位置。跨目錄獨立發行版可用 `--previous-package` 驗證並傳遞前版 lineage；Packager 與 Player 原生合成 E2E 已驗。 | 不相容劇本仍需明示 content/save version 與 migration；v29 同版存檔已在 v31 實際載入；v28 lineage 與真實跨版 migration 仍缺。 |
| 發行效能 | 16 分片、Release Packager、未變片重用、preflight／media 驗證、runtime-only refresh 已有真實大型量測；直接 artifact→overlay 與沿已驗簽鏈接續產出不需完整目標發行版。 | 一般完整封裝與直接 overlay 都仍做 O(全量素材) 的讀驗；缺依賴級／block-level 安全增量、同條件長期吞吐與磁碟峰值量測、最新 overlay 的 8 GB 壓力測試。 |
| 平台／原作特有功能 | rule/shake/影片、主要 UI 與基本 flowchart 可運行。 | 老闆鍵／置頂／語音分流／Steam 等是否抽成可攜 SDK API 尚需取捨；不要為了 Senren 外觀宣稱這些設定已生效。 |

## 最近完成的單檔 overlay：交接重點

2026-09-27 的唯讀封裝 preflight 量測：以 `D:\PrismatiXBuilds\SenrenBankaPSX\artifact-v29` 的 31,756 筆、約 8.2 GiB 輸入，Release Packager 執行 `pack --artifact ... --check --archive-shards 16` 成功，28,330 筆媒體雜湊依設計延至正式寫入。首跑總計 66.74 秒；同機暖快取下，試驗性八工路徑探測為 23.13 秒、單工為 25.21 秒。首跑與後兩次不具相同快取條件，不能宣稱並行帶來約三倍加速；暖快取的差距僅約 2 秒，試驗碼已撤回。此量測只涵蓋 `--check`，未測正式封裝的 source copy、舊 PDX 驗證與最終發佈，不能作整體 build 加速證據。下一步應在同條件量測完整階段，再決定安全增量設計。

Engine `docs/package-overlay.md` 是格式規格和安全邊界。這一輪已新增：

- `PrismatiXEngine/Engine/IO/Archive.h/.cpp`：`ArchiveStreamWriter`，檔案來源逐段寫入未加密、未壓縮 PDX5；核對來源預期大小、CRC／SHA-256，支援 metadata、進度。後續 `Archive::OpenStream` 對未加密 zstd entry 改為有界視窗串流解壓（128 MiB 視窗上限、讀到末尾驗大小／CRC）；舊式整筆加密且壓縮 entry 仍會整筆 buffer，encrypted chunked entry 可串流。
- `PrismatiXEngine/Engine/IO/PackageVerifier.h/.cpp`：`CreatePackageOverlay(base,target,output,progress)`。先完整驗證兩份發行版、確認同遊戲／Engine、不同 package fingerprint、拒絕 Player／非內容 runtime 檔案變更；只串流新增／修改資源，寫入 PDX metadata／target manifest／target index，產後再獨立 `VerifyPackageOverlay`。不覆寫現有輸出。暫存磁碟需容納 staged PDX，但不再額外存放解出的變動資源；Windows 以不可覆寫的同磁碟原子移動發布，其他平台仍依賴 hard link。
- `PrismatiXEngine/Applications/PackagerMain.cpp`：`--create-overlay`／`--verify-overlay`。
- `packages/cli/src/cli.ts`：`prismatix create-overlay --base ... --target ... --output ...` 和 `verify-overlay --base ... --patch ...`；`packages/cli/test/overlay-command.test.ts` 測 CLI→native 參數與輸出。
- Native contract 以兩個真正打包版本（含加密舊包）測新增、覆蓋、刪除-only、進度、既有輸出保護、runtime 變更拒絕；VFS contract 測 streaming writer。文件已更新 `docs/package-overlay.md`、`docs/getting-started.md`、移植案 `docs/SDK_FEEDBACK.md`。

此段原始單檔產製階段的歷史驗證（2026-09-27）：native 三項 targeted CTest 3/3 通過；`npm run cli:test` 104/104 通過。當時尚無 Player 自動套用；現在已有上方所列的 signed Player E2E。目錄 `create-patch`／`apply-patch` 流程仍可使用。

## 未完成工作：下一位 agent 的建議順序

1. **繼續驗收已簽章的單檔更新。** `PlayerApp::LoadBootConfig` 現解析 `Package/updates` 內唯一的 signed exact-base 多版本鏈與 per-user 啟用的 revision 3 選裝包，`Runtime::Init` 以基底 archiveKey 掛原片，再依序掛 overlay/tombstones；失敗回基底。小型 Player E2E 已驗合成舊金鑰／舊 fingerprint slot、明示 migration、跨更新資源刪除及重啟；v29 真實同版存檔已在大型獨立包載入；大型 signed 主更新與 22 route 已驗；下一步須驗真實跨版 migration 與大媒體 entry patch，並決定是否需要遊戲內選裝包開關和同時啟用多個選裝包。不得把 unsigned payload SHA-256 當發行者認證。
2. **擴充大型 overlay 壓力證據**：兩個真實完整發行版的 signed `create-overlay`／`verify-overlay`、小變更 patch 大小與 Player 22 route 已驗。仍需在同級 8 GB 專案修改大型壓縮媒體 entry，記錄程序最終峰值記憶體與暫存／最終磁碟峰值；小型 256 MiB fixture 不能取代此項。
3. **真正的增量 authoring/build/packaging**：現已有 16 片重用、Release Packager、免目標發行版的 artifact→overlay，以及 AllInputs 來源雜湊的有界並行；但兩種發佈路徑仍須全量驗 artifact／基底，約 31,000 個檔與約 8.2 GiB 素材仍是成本。來源完整雜湊、基底 PDX 驗證和兩者合併的直接 overlay 唯讀預檢已量測；後續仍須在相符的大型 artifact／base 上量測包含 patch 寫入的完整流程、RSS／磁碟峰值，再決定有失效條件的依賴／驗證 cache，保留 TOCTOU 與 ownership 安全性。歷史量測：Release Packager 同版 16/16 分片重封約 96.30 秒、14/16 小變更約 110.58 秒；不同快取與磁碟條件，不能當 SLA。僅更新 Player 的 `refresh-runtime` 曾約 23.19 秒。詳見 `SDK_FEEDBACK.md` 當日段落。
4. **跨層 Authoring 合約與大型 importer**：Story 指令與索引宣告的主要 source-location diagnostics 已補；仍須針對 UI/Action/runtime property、動態 ViewModel、依賴圖漏資源及其他素材來源位置建 regression。現有 `build`／`dev` 逐 scene 解析／編譯快取已可跨行程，但 92 scene × 四語仍重讀來源且未量測快取效益，後續需判斷安全增量或串流 importer 的實際瓶頸。清楚區分 `compileStory` 與 `compileStoryProject`，先從真實移植 importer 的重複 workaround 提取通用 API，不要把專案特例硬塞 stock Player。
5. **輸入設定與 UI 作者體驗尾巴**：逐鍵選擇／可選衝突二次確認與 wheel 使用者改鍵已加至 stock button；仍需設定資料遷移、已接項目的封裝實按與非 Player host 的保存範例。原作其餘快捷鍵需先確認哪些是可攜 Engine 動作、哪些只是遊戲特有功能。文字效果長期應共用一次布局／遮罩，減少雙重排版與逐字 texture；`.pxmotion` 的一般可重用資源／演出契約可繼續，但依使用者要求暫不追 M2 原動畫。
6. **回到移植案做接受測試**：先用最新 SDK 重新 `validate --runtime`、跑專案測試，再挑一個小的發行版 E2E 旅程驗證 UI、按鍵、存讀檔和音效；若要大容量重封，記錄實際版本、構建參數與磁碟空間。原作 1:1 差距應留在 `SDK_FEEDBACK.md`，不要把新 SDK API 存在誤寫成遊戲畫面已經實裝。

## 重現與交付門檻

Engine 工作目錄執行（先檢查乾淨狀態與實際工具路徑；下方 `out/build` 是本機已存在的 build tree）：

```powershell
git status --short
npm run cli:build
npm run cli:test
ctest --test-dir out/build/x64-debug -R 'PrismatiX\.(SDK\.PackagerContract|Integration\.Runtime\.Resources|Integration\.Player\.BootSplash)$' --output-on-failure
```

移植案工作目錄的主要入口：

```powershell
npm test
npm run validate
npm run build:artifact
npm run pack:artifact
```

`npm run build:artifact`／`pack:artifact` 會產生大檔、耗時、占空間；不是每個小改都要盲目重跑。要測 Player 畫面／音效／存檔，必須另有最新封裝的實機證據，不能只憑 `validate` 或 native contract。Senren 8 GB signed overlay 產製／驗簽及 Player 22 route E2E 已完成；完整 Engine `npm test` 與全量 native CTest 仍未在這一輪執行。

## 接手時注意

- 先跑 `git status --short`，核對本文件所述工作是否已提交。不要覆蓋使用者或其他 agent 的變更；Engine 那份未追蹤舊 audit 不屬於本輪交付。
- 優先讀 Engine `docs/package-overlay.md`、`docs/getting-started.md` 和移植案 `docs/SDK_FEEDBACK.md` 的最新段落；後者包含早期已過時狀態，不能只讀搜尋結果第一筆。
- 每一項改善要留下可回歸的 contract／integration test；效能宣稱標示 Debug/Release、版本、硬體／磁碟、輸入數與是否暖快取。提交訊息使用 `feat:`、`fix:`、`docs:` 等使用者指定風格。
- SDK 優化目標尚未達「全部優化完成」；不要因為寫了交接或完成單檔產製就把目標標為完成。
