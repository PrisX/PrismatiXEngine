# SDK 優化交接（給下一位 agent）

更新：2026-09-27。此文件以當前兩個本機 repo 和實際測試為準；不是「全部完成」聲明。較完整的逐項發現與歷史量測在 `C:\Users\owenl\C\Projects\SenrenBankaPSX\docs\SDK_FEEDBACK.md`。該檔按時間追加，早段的「尚未實作」有些已被後段取代，閱讀時以最新段落與程式碼為準。

### 2026-09-27 後續進度

- `Input::RemoveKeyBinding` 和 Runtime SDK `ctx.input.removeKeyBinding` 現可只移除一個鍵，保留同動作其餘按鍵順序及 gamepad／wheel；原生與 JS 測試涵蓋移除後的保存／載入。`KeyBindingButton` 的逐鍵選擇介面、衝突確認仍未完成。
- PDX 串流讀寫的 CRC-32 改為分段查表計算；Packager 的寫入／輸入驗證 CRC 亦改用查表。CRC 標準向量與原有封裝／VFS contract 通過。這是演算法路徑改善，尚未以同條件大型素材量測吞吐，不能聲稱某個百分比加速。
- `CreatePackageOverlay` 現在比較完整驗證前、後與發布前的兩份 manifest／content index 原始 bytes；驗證回呼期間改寫 target manifest 的測試確認拒絕發布。這降低 metadata TOCTOU 風險，但不能保證同時可寫的 PDX／素材不變，也不是發行者認證。
- Native Debug 指定的 Packager、Runtime Resources、Player BootSplash CTest 3/3 通過；`npm run cli:build` 與 `npm run cli:test` 104/104 通過。Senren `npm test` 21/21 通過，`npm run validate` 四語 368 scene／31,339 resources 通過。本機 `dist` 目前僅 `senren-banka-psx-authoring-v29` 具有 `Package/content-index.json`；因此還沒有兩份可用於 8 GB `create-overlay` 對照的完整發行版，沒有大型 overlay 時間／記憶體／磁碟量測。
- 新版 Release Packager 對 Senren `senren-banka-psx-authoring-v29` 執行 `--verify-package` 成功，檢查 1 個 PDX、31,756 筆、8,596,468,270 raw bytes。第二次暖快取（Windows、此機器 C:、Release、同一發行包）耗時 57.27 秒，程序峰值工作集約 63.4 MiB；首次冷一些的讀取依輸出檔時間約 111 秒，但量測腳本結尾格式錯誤，無可信峰值，不能作前後加速比較。這仍不是兩版本 overlay 建立或 Player 驗收。
- 使用者已選擇 Player 自動載入更新採「基底發行包固定發行者公鑰、驗簽且 exact-base 才掛載」。unsigned `patch.pdx` 仍不可自動套用。
- 後續已加入 `pack`／發行版 `build --publisher-key <file>`：公鑰寫入 package manifest 與 package fingerprint，overlay 建立／驗證拒絕換鑰；Native Packager contract 與 CLI 104/104 通過。
- 再後續加入 P-256 overlay keygen、revision 2 簽章與 `--signing-key`／`--require-signature`；Player 在 `Package/updates` 尋找唯一 signed exact-base 更新鏈，依依賴順序掛載 `patch.pdx`、`patch2.pdx` 等，檔名不決定順序；失敗回基底，基底 archiveKey 不被目標 manifest 取代。再加入 revision 3 `--optional-id adult` 簽章選裝包：預設關閉，使用者以 `PrismatiXPlayer --optional-pack adult on|off` 寫入 per-user 設定並重啟生效，且只對目前主更新版本適用。暫時一次只支援一個啟用中的選裝包、尚無遊戲內開關。
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
| 輸入與設定 | Native keyboard／wheel／具名 action、Player 設定持久化、按鍵捕捉、`KeyBindingButton`、追加鍵、逐鍵移除 API、Delete 清除鍵盤鍵、衝突拒絕、全螢幕動作。Senren 原作 25 項快捷鍵接通 13 項。 | 其餘 12 項仍是占位；`KeyBindingButton` 無逐鍵選擇 UI、原版衝突確認流程、F4 原版尺寸切換；尚未在最新 8 GB 封裝實按或驗舊設定遷移。 |
| UI／轉場／呈現 | Route/splash/動態 UI 圖片預載、無動畫 route 的 presentation hold、文字描邊排版修補、`.pxmotion` rev 1–11 的圖層、keyframe、atlas frame、音效 marker、基本混合、alpha mask、局部時間、曲線、tint、tickRate 等及 TSX `<Motion>`。部分舊版 Player 做過 22 route 截圖 E2E。 | 無原版 M2 等效轉換；原 `.mtn` 特殊曲線／粒子／複合語意未完整對齊，沒有現在版以原資產做逐幀 frame-time 與視覺一致性驗收。不能再把「沒有 alpha mask」照抄為現況。 |
| 大型封裝 | 正式 PDX 分片、未變分片重用、壓縮 entry cache、`pack --check`／`--verify-media`、`verify-package`、provenance 安全保留成品／僅替換 runtime、階段進度與 CLI 發布驗證平行化。Senren 曾完整封裝 16 PDX（約 7.50 GiB）並逐筆驗證 31,756 資源／8,594,731,766 原始 bytes。 | 仍沒有完整 dependency-aware／block-level 增量構建；改一個小檔仍可能讀驗大量舊輸入／PDX。以上大容量驗證不涵蓋後續每次改動。 |
| 更新 | 已有明示執行的目錄補丁；單檔 PDX overlay 可建／驗；revision 2 以固定發行者公鑰驗簽，Player 在 `Package/updates` 載入唯一 exact-base 更新鏈且失敗回基底；revision 3 選裝包可簽出並以 per-user 選擇啟停。 | 一次只支援一個選裝包且無遊戲內開關；舊存檔更新實測與大型雙版本量測未做。 |

上表僅列主線。詳細的成因、SDK 改動、原版差距與每次測試條件，請查 `SenrenBankaPSX/docs/SDK_FEEDBACK.md`；尤其不要把其中舊段落的「未有分片／未有 overlay 產製」照抄成現況。

### 尚未收尾的 SDK／採用項目（避免被「已有 API」掩蓋）

| 項目 | 目前做到哪裡 | 仍缺什麼／驗收條件 |
| --- | --- | --- |
| Story API／大型匯入 | `compileStoryProject` 解析跨 scene；單檔誤用會報 `PXSTORY1215`；368 份劇本以 sidecar 保存 531,812 個執行錨點，`ids --sync` 可預覽差異。 | 文件要更清楚區分兩層 API；通用增量／串流 importer、精確 source-span 診斷與移除／重排錨點的安全遷移策略仍缺。PSB→Story 目前是 Senren 私有轉換器。 |
| 多語姓名與存檔 | `.pxcharacter.displayNames`、`@alias`／`@"顯示姓名"` 已可產出對應 speaker。 | Senren 為保留舊 operation kind 仍以 narration/name-prefix 為主，未整批採用；native／JS `say` 的姓名亦未自動本地化。必須先做舊存檔 migration 實測，才可改劇本操作種類。 |
| 立繪 | `.pxportrait` rev 2 有 base／expression 標記、`transition=auto`、逐層淡化與前瞻／還原圖層預載；Senren 已產生 2,941 descriptor／761 共用圖層並通過 validate。 | UI `TextureRect` 不能直接呈現 descriptor；descriptor 內動態 dress／pose／face 編輯、所有角色 placement profile、原圖逐句畫面／首次出現 frame-time、舊存檔恢復均未完成正式驗收。最新大型發行包未因 rev 2 重建。 |
| UI contract／素材依賴 | typed props、原生 TypeRegistry、Action 參數、canonical UiValue、UI 動態圖片預載、`ItemsPlaced`／`Modal` 已補。 | 執行期 ViewModel 路徑與自訂 formatter 的語意無完整靜態驗證；任意自訂繪圖、Story 中途素材與音效的預載無通用保證。18 個設定開關雖已模板化，動作仍是 `settings.noop`。 |
| 文字與路由演出 | 已修文字 outline 雙層換行差異、未變文字每幀重排，以及 route crossfade 暗谷／預載閃白。 | 正式單次 layout/mask 的文字效果管線、依事件 author 的白幕／shader／遮罩轉場與通用 route motion graph 缺；最新大型封裝與原版逐幀對照未做。 |
| `.pxmotion`／Story 動畫 | 格式已演進至 rev 11，TSX UI／splash 可播；Story `[timeline]` 已可控 `.pxanim`／`.pxtimeline` 的 await/speed。 | `.pxmotion` **仍不能直接由 Story／Stage 播放**，需設計呈現、預載、等待及存檔還原契約。M2 `.mtn` 轉換器／逐幀驗證暫停；不要重新把它列為優先工作。 |
| 輸入／設定 | `.pxgame.inputActions` 有 context、key、wheel、route／`uiAction`；捕捉／追加／逐鍵移除／整組清除可保存。 | 逐鍵選擇 UI、使用者改 wheel、衝突確認 UI、非 Player host 保存責任尚未收尾；需 E2E 核對舊設定。原作剩餘 12 項與多數 settings.noop 須先判別可攜 SDK 能力，不能一律當作 SDK 缺陷。 |
| 更新與持久化 | 可顯式安裝目錄補丁；簽章更新鏈可建／驗／由 Player 載入或回基底；選裝包可獨立啟停，且存檔身分保留主更新 fingerprint；Player 資料已移至 per-user 位置以免污染 distribution。 | 選裝包內舊存檔、舊版 save key／save migration 的新版 Player 實測仍缺；不要把 read-only migration 錨點檢查當存讀檔相容驗證。 |
| 發行效能 | 16 分片、Release Packager、未變片重用、preflight／media 驗證、runtime-only refresh 已有真實大型量測。 | 日常素材變更仍做 O(全量素材) 的多輪核驗；缺依賴級／block-level 安全增量、同條件長期吞吐與磁碟峰值量測、最新 overlay 的 8 GB 壓力測試。 |
| 平台／原作特有功能 | rule/shake/影片、主要 UI 與基本 flowchart 可運行。 | 老闆鍵／置頂／語音分流／Steam 等是否抽成可攜 SDK API 尚需取捨；不要為了 Senren 外觀宣稱這些設定已生效。 |

## 最近完成的單檔 overlay：交接重點

Engine `docs/package-overlay.md` 是格式規格和安全邊界。這一輪已新增：

- `PrismatiXEngine/Engine/IO/Archive.h/.cpp`：`ArchiveStreamWriter`，檔案來源逐段寫入未加密、未壓縮 PDX5；核對來源預期大小、CRC／SHA-256，支援 metadata、進度。它不把整個新 PDX 放進記憶體，但來源壓縮 entry 的讀取仍可能 buffer 大筆資料。
- `PrismatiXEngine/Engine/IO/PackageVerifier.h/.cpp`：`CreatePackageOverlay(base,target,output,progress)`。先完整驗證兩份發行版、確認同遊戲／Engine、不同 package fingerprint、拒絕 Player／非內容 runtime 檔案變更；只抽新增／修改資源，寫入 PDX metadata／target manifest／target index，產後再獨立 `VerifyPackageOverlay`。不覆寫現有輸出。暫存磁碟需約額外變更資源量；輸出發布目前依賴 hard link（非支援檔案系統的 fallback 尚未做）。
- `PrismatiXEngine/Applications/PackagerMain.cpp`：`--create-overlay`／`--verify-overlay`。
- `packages/cli/src/cli.ts`：`prismatix create-overlay --base ... --target ... --output ...` 和 `verify-overlay --base ... --patch ...`；`packages/cli/test/overlay-command.test.ts` 測 CLI→native 參數與輸出。
- Native contract 以兩個真正打包版本（含加密舊包）測新增、覆蓋、刪除-only、進度、既有輸出保護、runtime 變更拒絕；VFS contract 測 streaming writer。文件已更新 `docs/package-overlay.md`、`docs/getting-started.md`、移植案 `docs/SDK_FEEDBACK.md`。

最近一次驗證（2026-09-27，同一工作樹）：native 三項 targeted CTest 3/3 通過；`npm run cli:test` 104/104 通過。之前同一組改動曾 `npm run cli:build` 通過。**這不是 Player 自動套用的測試**。目前公開且可直接更新發行版的仍是目錄 `create-patch`／`apply-patch` 流程。

## 未完成工作：下一位 agent 的建議順序

1. **繼續驗收已簽章的單檔更新。** `PlayerApp::LoadBootConfig` 現解析 `Package/updates` 內唯一的 signed exact-base 多版本鏈與 per-user 啟用的 revision 3 選裝包，`Runtime::Init` 以基底 archiveKey 掛原片，再依序掛 overlay/tombstones；失敗回基底。下一步須驗收舊版存檔與 migration、刪除資源及重啟覆蓋的 Player E2E，並決定是否需要遊戲內選裝包開關和同時啟用多個選裝包。不得把 unsigned payload SHA-256 當發行者認證。
2. **把新 overlay producer 在實際 Senren 版上驗收**：用兩個實際完整發行版測 `create-overlay`／`verify-overlay`，量測總時間、暫存／最終磁碟、峰值記憶體、patch 大小、壓縮大 entry；小型 contract 不代表 8 GB 封裝可接受。若目前只有同一版成品，先用隔離副本與可重現小變更，勿改寫已交付 `dist`。
3. **真正的增量 authoring/build/packaging**：現已有 16 片重用與 Release Packager 大幅改善，但小改仍要對約 31,000 個檔與約 8.2 GiB 素材做多輪安全核驗。後續應先 profile「source hash／四語結構驗證／舊 PDX 驗證／staging 發布」，設計有失效條件的依賴／驗證 cache，保留 TOCTOU 與 ownership 安全性。歷史量測：Release Packager 同版 16/16 分片重封約 96.30 秒、14/16 小變更約 110.58 秒；不同快取與磁碟條件，不能當 SLA。僅更新 Player 的 `refresh-runtime` 曾約 23.19 秒。詳見 `SDK_FEEDBACK.md` 當日段落。
4. **跨層 Authoring 合約與大型 importer**：針對 UI/Action/runtime property、動態 ViewModel、依賴圖漏資源、source-location diagnostics 建 regression；讓 92 scene × 四語的匯入能增量／串流，清楚區分 `compileStory` 與 `compileStoryProject`。先從真實移植 importer 的重複 workaround 提取通用 API，不要把專案特例硬塞 stock Player。
5. **輸入設定與 UI 作者體驗尾巴**：逐鍵選擇／衝突確認 UI、設定資料遷移與已接項目的封裝實按；原作其餘快捷鍵需先確認哪些是可攜 Engine 動作、哪些只是遊戲特有功能。文字效果長期應共用一次布局／遮罩，減少雙重排版與逐字 texture；`.pxmotion` 的一般可重用資源／演出契約可繼續，但依使用者要求暫不追 M2 原動畫。
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

`npm run build:artifact`／`pack:artifact` 會產生大檔、耗時、占空間；不是每個小改都要盲目重跑。要測 Player 畫面／音效／存檔，必須另有最新封裝的實機證據，不能只憑 `validate` 或 native contract。完整 Engine `npm test`、全量 native CTest、Senren 最新版 8 GB overlay 產製／Player E2E 尚未在本次單檔改動後全部執行。

## 接手時注意

- 先跑 `git status --short`，核對本文件所述工作是否已提交。不要覆蓋使用者或其他 agent 的變更；Engine 那份未追蹤舊 audit 不屬於本輪交付。
- 優先讀 Engine `docs/package-overlay.md`、`docs/getting-started.md` 和移植案 `docs/SDK_FEEDBACK.md` 的最新段落；後者包含早期已過時狀態，不能只讀搜尋結果第一筆。
- 每一項改善要留下可回歸的 contract／integration test；效能宣稱標示 Debug/Release、版本、硬體／磁碟、輸入數與是否暖快取。提交訊息使用 `feat:`、`fix:`、`docs:` 等使用者指定風格。
- SDK 優化目標尚未達「全部優化完成」；不要因為寫了交接或完成單檔產製就把目標標為完成。
