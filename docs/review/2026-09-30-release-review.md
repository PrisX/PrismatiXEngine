# PrismatiXEngine 發行前 Review（以 Sakura-no-Uta 移植為樣本）

日期：2026-09-30　範圍：Engine `785b9f3`、Sakura-no-Uta `65c603d`

Sakura-no-Uta 是目前最大的真實使用案例：352 個場景、6.5 萬句台詞、3.1 萬個語音、46,334 個資源、15 個自訂 TSX 畫面。本文件把從這個專案看得到的 SDK 使用摩擦，和直接讀 Engine 原始碼找到的問題，整理成可以直接排程的任務。

優先度：**P0** 發行前必修 · **P1** 發行後第一輪 · **P2** 改善體驗。工作量：**S**（一天內）· **M**（數天）· **L**（一週以上）。

## 處理進度（2026-10-01 更新）

每個項目的細節寫在對應的 commit message（`Addresses <task>` 那一行）。

| 狀態 | 任務 |
| --- | --- |
| 已完成 | T1、T2、T3、T4、T7、T20 · U1–U8、U10 · S1–S6、S8 · W1–W7 · A1（IO 抽象、生命週期）、A2 的觸控操作 · Sakura 專案清理 |
| 部分完成 | **T5**：QA journey、設定解析、UI action 已拆出，`PlayerApp.cpp` 從 6,270 行降到 4,134 行；action 的表格式分派還沒做。<br>**T6**：移除了寫死的曲名清單；從 schema 產生設定解析器還沒做。<br>**T8**：`distribution.linkMedia` 避免第二份素材副本；素材重新編碼還沒做。<br>**U9**：工具列判定區改由 HUD 的 `DockArea` 控制項決定；選項列的位置仍在 `player.choiceLayout`，因為那是 Player 依選項數量計算的。<br>**S7**：`[credits]` 已完成；`[input]` 需要 Player 文字輸入畫面，還沒做。 |
| 已寫好、尚未驗證 | **A0**：Android 的 CMake preset、vcpkg triplet、Gradle 範本，以及 `docs/android.md`。需要 NDK 與實機。 |
| 未開始 | **A3**：Play Asset Delivery、ASTC 紋理、TalkBack。<br>**W8**：官方匯入器套件。<br>`prismatix dev` 經由 IPC 驅動的測試介面（T20 的後半）。 |

順手修掉的既有問題：master 上原本失敗的原生測試、CLI 測試和 clean-install gate；CommercialVN sample 無法編譯、也無法在 Preview 啟動；內建 Settings 畫面的 sibling order 重複；一筆跨場景錯誤被報兩次。

---

## 0. 總評

- **核心架構是健康的。** Story → Runtime IR → 原生 VM 的分層清楚，契約有 JSON Schema，也有穩定的存檔 ID，以及 source map 和 PDX 簽章／patch 鏈。Sakura 全專案 `validate` 只要 **8.8 秒**（352 場景、46k 資源）。Engine 本體沒有寫死 Sakura 的遊戲邏輯，Artemis 相關的只有註解和通用語意（`[shake]`、`[flash]`、SE 頻道）。
- **最大的發行風險不在 VN 功能，而在「出貨的 Player」和「發行流程」**：
  1. 存檔相容性綁在「上一版輸出資料夾」上，換資料夾打包、所有玩家存檔就讀不了。
  2. 出貨的 Player 仍會讀 `PRISMATIX_E2E_*` 環境變數這類 QA 後門。
  3. `validate` 會通過，但 Player 一啟動就 fatal 的設定錯誤。
- **架構上最需要處理的是 `PlayerApp.cpp`**：6,212 行，`Init` 就有 1,497 行，`HandleUIAction` 583 行，`DriveBenchJourney`（測試腳本）941 行。另外 `prismatix.json` 的 `settings` 已經長出 21 個 `player.*` 鍵，驗證邏輯分散在 Schema、SDK、C++ 三個地方。
- **作者體驗最大的落差在 UI（TSX）**：SDK 其實已經有 `skin`、`Slider`、`activateSoundPath` 這些型別化 prop，但文件和範本沒有示範，所以 Sakura 仍然用 `runtimeProperties` 和手寫四張狀態圖。Toggle、Radio、Tooltip、「Slider 綁定選項」目前都只能靠拼湊。
- **Android：可行，而且底子不錯**（SDL3 + SDL_main + SDL_GPU/SPIR-V + FFmpeg + QuickJS，觸控事件也已經處理）。真正的工作量在 IO 層（APK 內的資源無法用 `std::ifstream` 讀）、App 生命週期、觸控 UX、大型資源的發佈方式和打包工具鏈，詳見第 6 節。

---

## 1. Bug 與發行阻斷（P0）

### T1 存檔相容性不應依賴「上一版輸出資料夾」【P0 · M】

- 現象：同樣的 `contentVersion`/`saveVersion`，只要 package fingerprint 變了，就會回 `PXSAVE6111`（[PlayerApp.cpp:2768](../../PrismatiXEngine/Applications/Player/PlayerApp.cpp)）。相容的 fingerprint 清單只從 `--output` 資料夾或 `--previous-package` 找（[Packager.cpp:1223](../../PrismatiXEngine/Engine/SDK/Packager.cpp) `DiscoverPreviousPersistence`）。所以換新資料夾、在乾淨的 CI 機器上打包，或舊版的 dist 被刪掉，所有玩家的存檔都會讀不了，而且打包時完全沒有警告。Sakura QA 已經實際踩到這個問題。
- 建議：
  1. 把 lineage 放進專案本身，例如由 CLI 維護、納入 Git 的 `.prismatix/release-lineage.json`，或 `prismatix.json` 裡的 `persistence.compatibleFingerprints`，不要依賴輸出資料夾。
  2. `pack` 時找不到前一版 lineage，而且版本號也沒有提升時，要發出**警告**，說明舊存檔會被拒絕。
  3. 中期方向：存檔已經用穩定的 operation ID，可以在載入時檢查「存檔位置在新 IR 裡是否存在、變數型別是否相容」，用這個取代「fingerprint 必須完全相同」。
  4. `PXSAVE6111` 目前同時代表「不同遊戲」和「fingerprint 改變」兩件事，應該拆成兩個錯誤碼（[PlayerApp.cpp:2754](../../PrismatiXEngine/Applications/Player/PlayerApp.cpp)）。

### T2 出貨的 Player 仍接受 QA 環境變數後門【P0 · S～M】

- 現象：沒有任何編譯旗標把這些關掉：
  - `PRISMATIX_E2E_START_LABEL`（[PlayerApp.cpp:2652](../../PrismatiXEngine/Applications/Player/PlayerApp.cpp)）可以直接跳到任何劇本標籤，繞過解鎖。
  - `PRISMATIX_E2E_SAVE_DIR`（:826）可以改寫存檔位置。
  - `PRISMATIX_E2E_JOURNEY`／`CAPTURE_DIR`（:4527、:4614）會把截圖寫到任意路徑。
  - `PlayerApp.h` 裡有 37 個 `*E2E*` 成員，`.cpp` 裡有 309 處 E2E 相關程式碼。
- 建議：加上 `PRISMATIX_QA_HOOKS` CMake 選項，Release/發行版預設 OFF。或者至少要求 dev-signed 封包才讀這些變數。長期則照 T20 把 journey 移出 Player。

### T3 `validate` 通過但 Player 啟動失敗／靜默失效的設定【P0 · S】

- `player.musicTracks` 只要有一條路徑不存在，Player 啟動時就是 **Fatal**（PXPLAYER5025，[PlayerApp.cpp:1064](../../PrismatiXEngine/Applications/Player/PlayerApp.cpp)），但 SDK 的 `project.ts` 沒有檢查這些路徑。
- `player.uiFont`、`player.fonts`、`player.titleBgmAfter.path` 路徑錯了，Player 會**靜默略過**（改用預設字型或預設 BGM），SDK 也不會報錯。
- `player.saveLayout` 不合法時 Player 直接 `return false`，**沒有任何診斷訊息**（:1084）。
- 建議：把這些路徑都加進 `project.ts` 的 `addResourceRef`（目前只檢查 titleBgm、ui*Sound、titleVoiceVariants、splashes），並規定 Player 端每一個 `return false` 都要帶診斷訊息。

### T4 UI 節點重名、缺少宿主必需節點不會被偵測【P0 · S】

- 宿主用名稱找控制項，例如 `Dialogue`、`Speaker`、`Slots`、`DialogYes`、`VoiceProfile_*`，全部約 40 個名稱。但 SDK 不檢查同一畫面裡有沒有重名。
- 實例：Sakura 的 [HUD.tsx:92](../../../Sakura-no-Uta/Content/UI/HUD.tsx) 有一個沒有 action 的 `HudButton name="DockLock"`，它會渲染成一張無障礙標籤為「ツールバー固定（未実装）」的靜態圖，壓在第 95 行真正的 `DockLock`/`DockLockOff` 切換鈕底下，而且一直可見。這在 Sakura 端要刪掉。
- 建議：
  - 同一 route 的名稱重複時報 error。
  - HUD 沒有 `Dialogue` 這類必需節點時報 warning。
  - `player-ui-contract.ts` 的名稱表目前是手動維護，應該跟 `UiTypeRegistry` 一樣從 C++ 產生，避免兩邊漂移。

---

## 2. 架構與程式碼品質

### T5 拆解 `PlayerApp`【P1 · L】

`PlayerApp.cpp` 有 6,212 行，最長的幾個函式是 `Init` 1,497 行、`DriveBenchJourney` 941 行、`HandleUIAction` 583 行、`GameFrame` 313 行。建議拆成：

- `PlayerSettings`：由 Schema 產生的型別化結構和解析器（見 T6）。
- `UiActionDispatcher`：表格式註冊 `action id → handler + 參數型別`，可以同時輸出 action contract 給 SDK 做型別檢查。
- `ConfirmationService`、`SaveLoadController`、`AudioSettingsController`。
- `QaJourneys`：獨立 target，只連結進 QA 版（見 T2、T20）。

### T6 `player.*` 設定的單一真實來源與分組【P1 · M】

- 目前同一份設定要在三個地方分別驗證：`project.schema.json`、`project.ts`、`PlayerApp.cpp`。三邊的嚴重度不一致，有的 Fatal、有的 Warn、有的靜默。
- 建議：以 Schema 為準，產生 C++ 解析器和 TS 型別；Player 不應再自己寫一套寬鬆的解析。
- 21 個平鋪的 `player.*` 鍵建議依用途分組，例如 `player.audio`、`player.saves`、`player.title`、`player.confirmations`。
- 一些只為單一遊戲設計的形狀應該一般化：
  - `titleBgmAfter` 只支援一個旗標，應改成規則陣列 `titleBgm: [{when: "g_clear_6", path}, {path}]`。
  - `choiceLayout`、`dockArea` 是畫面呈現的東西，應該移到 HUD TSX（見 U9）。

### T7 非同步讀取統一成有上限的 job system【P2 · M】

`AudioEngine`、`Stage`（立繪描述檔）、`AssetCache`、`PlayerApp`（設定和 profile 寫入）各自用 `std::async(std::launch::async)`。Audio 的預讀有 16 個上限，但被淘汰、仍在執行中的 `m_retiredPrefetches` 沒有上限，快轉時可能累積很多執行緒。建議統一成固定大小的工作池。這對 Android 的執行緒和記憶體預算也是前置工作。

### T8 建置產物大小與迭代速度【P2 · M】

Sakura 的 `.prismatix/build` 約 9.3 GB，QA 封包約 12 GiB，C 槽空間不夠只能改放 D 槽。建議：

- 量測 artifact 是否全部用 hardlink；Windows 的 `du` 會重複計算硬連結，所以要用實際可用空間判斷。
- 增加可選的資源轉碼管線：PNG 轉 WebP/KTX2，大型 BGM 重新編碼。這同時是 Android 的前置工作（ASTC）。

---

## 3. Story 語法：更短、更少樣板

### S1 專案層級共用 macro【P1 · S】

`[macro]` 目前只在單一檔案內有效（`story-macros.ts`）。Sakura 每次換場都重複同一段七行的指令：`[image.hide all]` → `[hud visible=false]` → `[bg black rule=… ]` → `[wait]` → `[hide all transition=false]` → `[bg X transition=false]` → `[transition …]`。建議支援 `prismatix.json` 的 `storyMacros: ["Story/_macros.pxstory"]`，讓專案可以寫 `[scene_change bg006a rule=083r]`。

### S2 流程控制補齊【P1 · M】

- `[elseif]`（目前只有 `if/else/endif`）。
- 條件選項：`[choice text="…" goto=x if="affection > 3"]`，並支援「已選過」的灰化狀態。
- 區塊式選項語法，範本可以用這種寫法：
  ```pxstory
  [choice]
  - 一起看看星星 -> stars
  - 留在這裡說故事 -> story
  [endchoice]
  ```

### S3 變數宣告不需要手寫 `.pxgame` JSON【P1 · M】

目前新增一個變數必須編輯 `.pxgame` JSON（cookbook 也是這樣示範）。建議提供以下其中一種：

- Story 內宣告：`[var affection=0 scope=session]`。
- 型別化的 `Content/game.ts`：`defineGame({variables: {affection: session.int(0)}})`，建置時產生 `.pxgame`。
- `prismatix var add affection --int --session`。

Sakura 文件（`script-coverage-and-authoring.md` 末段）也提出同樣的需求。

### S4 行內文字指令【P1 · M】

目前行內標記只有 `b / i / color / size / font / ruby / br`。VN 常用、移植時也常遇到的還缺：

- `[w 500ms]`：句中停頓。
- `[speed 50%]…[/speed]`：局部文字速度。
- `[nowait]`：瞬間顯示。
- 句中觸發效果，例如 `[shake]`。

### S5 分層立繪只換表情【P2 · S】

Sakura 的表情別名是「body＋face」組合名（例如 `z1e0700_e0022`），換表情必須寫出完整組合。`.pxportrait` 本身已經是分層結構，可以提供 `@藍[face=e0023]` 或 `[face 藍 e0023]` 只換臉、保留身體和姿勢。

### S6 一致性與範本示範最短寫法【P2 · S】

- `[hide]`（角色）和 `[image.hide]`（圖層）命名方式不一致。可以考慮讓 `[hide image=cg1]` 也能用，或在文件裡用一張表說明。
- `[wait 1000ms]` 已經支援位置參數，但 Sakura 匯入器仍然輸出 `[wait duration="1000ms"]`，而且連續的 wait 沒有合併。
- 範本的 `main.pxstory` 還在用 `[jump target=finish]`、`[bg Assets/window.png]` 這類長寫法，應該示範 `[jump finish]` 和 `storyAssets` 短名稱。

### S7 片尾名單與輸入指令【P2 · M】

- `[staffroll]`／`[credits]`：捲動片尾，可以接 TSX 或圖片序列。Sakura 還有 5 處沒有移植。
- `[input var=name]`：輸入主角名字。

### S8 `.pxids.json` 側檔瘦身【P2 · S】

Sakura 的側檔是劇本本身的 2.2 倍（83 KB 對 38 KB），352 個檔案會讓 Git diff 很吵。建議改成緊湊格式（一行一筆、不縮排），或改成專案單一的 ID 資料庫。

---

## 4. UI（TSX）：從「拼湊」到「元件」

Sakura 15 個畫面統計：`runtimeProperties` 43 處、手寫 `normalPath` 31 處、`activateSoundPath` 29 處、`formatter: "Not"` 8 處、`Control runtimeType="Slider"` 6 處。

### U1 文件與範本示範既有的型別化 prop，並提供 lint 和 codemod【P0 · S】

- `TextureButton skin=`、`<Slider>`、`activateSoundPath=` 都已經存在（commit `10aa6f4`），但 `docs/`、`Samples/`、範本裡 `skin=` 出現 0 次。
- 預設取消音已經會自動套用到 `overlay.close`、`*.cancel`（[PlayerApp.cpp:955](../../PrismatiXEngine/Applications/Player/PlayerApp.cpp)）。Sakura 只要設定 `player.uiCancelSound`，就可以刪掉全部 29 處 `activateSoundPath`。
- 建議：`validate` 對「`runtimeProperties` 裡有對應型別化 prop 的鍵」發出 warning，並由 `prismatix migrate` 自動改寫。

### U2 Toggle／Checkbox／RadioGroup 元件【P1 · M】

目前的做法是「兩顆疊在一起的按鈕，各自用 `visible` 綁定正反狀態」（`formatter: "Not"`），再加上 `options.dock.is1` 這種魔術後綴。建議改成：

```tsx
<Toggle option="dock" on={{skin: "mw/bt_lock_on"}} off={{skin: "mw/bt_lock_off"}} />
<RadioGroup option="font">{[1,2,3].map(v => <Radio value={v} skin="conf/radio34" />)}</RadioGroup>
```

### U3 Slider 直接綁定選項【P1 · S】

現在要寫 `<Control runtimeType="Slider" runtimeProperties={{optionKey: "mspeed", …}}>`，建議改成 `<Slider option="mspeed" />` 或 `bind={{value: "options.mspeed"}}`，並支援顯示用的數值格式（百分比）。

### U4 Tooltip／hover 狀態不需要事先宣告 `player.uiData`【P1 · S】

Sakura 在 `player.uiData` 手動宣告了 12 個 `hover.*` 鍵，才能用 `bind={{visible: "hover.Save"}}` 顯示提示圖。建議提供 `tooltip={<Image …/>}`，或讓 `hover.<name>` 自動成為可綁定的資料。

### U5 型別化 action 建構器與統一的 action 命名【P1 · M】

現有 action 超過 100 個，命名方式混雜：`set.fullscreen.on/off/toggle/value`、`settings.duckOn`、`option.set` 加 `"dock=0"` 字串參數、`voiceProfile.test`。建議：

- 統一成 `setting.set {key, value}`、`setting.toggle {key}`、`setting.step {key, delta}`，舊名稱保留為 alias。
- SDK 輸出型別化建構器，例如 `actions.save.slot("$key")`、`actions.setting.set("dock", 0)`，編譯期就能擋掉參數型別錯誤，避免 `save.slot` 收到字串 key 時只在執行期報 `PXUIACTION2013`。

### U6 條件 prop 一級化【P2 · S】

提供 `visibleWhen`、`enabledWhen`，並讓 `bind` 支援簡單運算式（`not`、`==`）。Sakura 的 [Title.tsx:29-30](../../../Sakura-no-Uta/Content/UI/Title.tsx) 用兩次 spread 寫入 `bind`，兩個條件同時給時，前一個會被覆蓋。目前沒有觸發，但這是潛在的 bug。

### U7 精靈圖集裁切【P2 · M】

提供 `<Image path="conf/char.png" region={[x, y, w, h]} />` 和 `skin` 的 atlas 版本。這樣可以取代 Sakura 的兩種繞法：Config 角色頭像用的「clip Group＋負座標」，以及 `generate_ui_assets.py` 的預先切圖。

### U8 `prismatix eject ui <route>`【P1 · M】

把內建的 HUD、Save/Load、Config、Backlog 輸出成可編輯的 TSX（包含所有宿主必需名稱和綁定）。這是「從預設 UI 改成自己的 UI」最省事的起點，目前只能從零開始寫。

### U9 呈現設定回歸 TSX【P2 · M】

`player.choiceLayout`（每種選項數量的列偏移）和 `player.dockArea`（游標靠近判定區）都是畫面配置，應該由 HUD TSX 宣告，例如 `<ItemsPlaced layout=…>`、`<Group revealOnHover area=…>`，不要放在 manifest 裡。

### U10 TSX 文字在地化【P2 · S】

TSX 裡的文字目前是寫死的，例如 SaveLoadView 的錯誤訊息。建議提供 `t("save.incompatible")`，直接引用 `Content/Localization/*.json` 的 `strings`。

---

## 5. 從建立專案到發行的流程

### W1 一個指令完成發行【P1 · M】

Sakura 的 README 要手動走 8 步，而且要自己帶 `--packager`、`--player`、`--archive-shards`、`--compression`。建議：

- `prismatix.json` 增加 `distribution.output`、`shards`、`compression` 設定。
- `prismatix release` 一次完成 `validate → build → pack → verify`，並檢查 lineage（T1）。

### W2 不需要手動指定 `--ui-registry`【P1 · S】

Sakura 的 `package.json` 把 registry 路徑寫死成 Engine 的 build 目錄。實跑 `npm run validate` 還出現 `PXCLI1043`（沒有 InputKeyRegistry）。SDK 應該從打包好的 runtime 自動找到 registry；只有 Engine 開發者需要覆寫時才用 `--runtime`。

### W3 Preview「從這裡開始」並保留位置【P1 · M】

- 目前編輯後會重啟場景，而且 Preview 從標題畫面開始（`getting-started.md`）。想改第 5,000 行，就得從頭玩到那裡。
- Sakura QA 只能借用 `PRISMATIX_E2E_START_LABEL` 來跳。
- 建議：`prismatix dev --at <scene>:<label|line>`，編輯後回到同一個 operation ID 繼續。

### W4 `.pxstory` 編輯器支援（VS Code 擴充／LSP）【P1 · L】

SDK 已經有精確的 source span 和診斷資訊，可以做成 LSP：

- 即時錯誤。
- 角色、表情、`storyAssets` 短名、標籤的自動完成。
- 跳到標籤定義。
- hover 預覽圖片。
- 「從此行預覽」（接 W3）。

這是降低寫作門檻 CP 值最高的一項。

### W5 腳手架指令【P2 · M】

- `prismatix add character kei --from Assets/Portraits/kei/`：從一個資料夾的表情圖產生 `.pxcharacter`。
- `prismatix add scene`、`prismatix add screen settings`（接 U8）。

### W6 範本【P2 · M】

- 除了 `minimal`，再提供一個「完整 ADV」範本，包含自訂的 HUD、Save、Config TSX，並用上 `skin`、`Slider`、Toggle。
- `minimal` 目前用手寫 UUID 的 `assets` 陣列登錄資源，應改成示範 `resourceIncludes` 加 `storyAssets`，讓作者不必接觸 UUID。

### W7 每個 CLI 子指令的 `--help`【P2 · S】

目前 `usage()` 只印一行總表。每個子指令應該列出自己的旗標，並附一個範例。

### W8 官方匯入器套件【P2 · L】

Artemis 轉換器現在放在 Sakura 的 `tools/*.py`。可以把共用部分抽成 `prismatix import artemis|kirikiri|renpy`，或做成匯入器 SDK。這對「移植既有作品」很有宣傳價值。

---

## 6. QA 基礎設施

### T20 把 E2E journey 移出 Player，改成可腳本化的測試介面【P1 · L】

Player 內建了 journey（`DriveBenchJourney` 941 行，還有 settings-sound、incompatible-save 等），而且直接引用 Sakura 的控制項名稱，例如 `VoiceProfile_`、`SaveLoadError`。Sakura 也只好自己寫 `capture_player.py`、`click_player.py` 來驅動。

建議：QA 版 Player 開一個 debug IPC，提供點擊控制項、讀 UI 資料、截圖、跳標籤等操作；再由 `prismatix test journeys/*.ts` 驅動。這樣出貨版就不包含任何 journey 程式碼（接 T2）。

---

## 7. Android Export 評估

**結論：可行，而且 Engine 的選型對 Android 很友善。** 但 Android 不是加一個 preset 就好。建議分成四個階段，每一階段都可以獨立交付。iOS 能共用大部分的工作。

已經具備的條件：

- SDL3 加 `SDL_main`（[PlayerMain.cpp](../../PrismatiXEngine/Applications/PlayerMain.cpp)）。
- SDL_GPU 的 Vulkan 後端，搭配已經離線編譯的 SPIR-V。
- 觸控事件已經處理（[Input.cpp:1038](../../PrismatiXEngine/Engine/Platform/Input.cpp)）。
- 存檔位置用 `SDL_GetPrefPath`。
- 依賴都是可以交叉編譯的函式庫：FFmpeg、QuickJS-NG、mbedtls、zstd，sdl3-* 系列都有 vcpkg 的 `arm64-android` triplet 可以試。

| 階段 | 任務 | 說明 |
| --- | --- | --- |
| A0 · M | 工具鏈 | CMake preset `android-arm64` 加 vcpkg `arm64-android`，Gradle 範本用 SDL 的 `SDLActivity`。FFmpeg 以 `.so` 動態連結（LGPL）。`shadercross` 只是 host 工具，不進 APK。 |
| A1 · L | **IO 抽象層（關鍵路徑）** | `Archive.cpp`、`VFS.cpp` 用 `std::ifstream` 和 `std::filesystem`，**讀不到 APK 或 asset pack 裡的檔案**。需要一個 `io::FileSource` 介面（seek/read/size），實作 `SDL_IOStream`/`AAssetManager` 和本機檔案兩種版本。不建議「首次啟動解壓到內部儲存」，因為會讓容量加倍。 |
| A1 · M | App 生命週期 | 目前程式碼完全沒有處理 `SDL_EVENT_WILL_ENTER_BACKGROUND`、`DID_ENTER_FOREGROUND`、`LOW_MEMORY`、`TERMINATING`。需要做到：切到背景時暫停音訊和影片、自動快存、flush 設定，回前景時重建 swapchain，記憶體不足時清快取。 |
| A2 · M | 觸控 UX | 手機沒有 hover、右鍵、鍵盤。建議手勢：點擊推進、長按或雙指隱藏視窗、上滑看 Backlog、下滑開選單。工具列要常駐，因為 Sakura 的 `dockArea` 靠近判定和 hover 提示在手機上無效。要處理瀏海 safe-area 和 20:9 letterbox。TSX 需要平台條件，例如 `when={{platform: "touch"}}`；Sakura 的 `tabletui` 選項正好可以接上。 |
| A2 · M | 打包 | `prismatix build --target android` 輸出簽章好的 `.aab`/`.apk`。`distribution.android` 放 `applicationId`、keystore、adaptive icon。CLI 要像桌面版一樣附帶預建的 `libPrismatiXPlayer.so`。目前的 Packager 是「複製 host 上的 Player」，沒有「目標平台」的概念，需要重構成支援多目標。 |
| A3 · L | 大型遊戲發佈 | Sakura 約 12 GiB，遠超過 Google Play 對 AAB 和 asset pack 的大小限制。需要 Play Asset Delivery（fast-follow／on-demand）或自架的分段下載（可以沿用現有的 PDX shard 和 overlay 機制）。圖片要轉 ASTC/ETC2 或 WebP（接 T8）。 |
| A3 · M | 效能與記憶體 | 1920×1080 分層立繪的紋理快取需要設上限，要測發熱和低階機。影片可以考慮 MediaCodec 硬體解碼；WMV 走 FFmpeg 軟解在低階機上可能不夠快。執行緒要走 T7 的工作池。 |
| A3 · S | 無障礙和語音 | `SpeechService` 和 `PlatformSemanticAdapter` 沒有 Android 實作（TalkBack），第一版可以先宣告不支援。 |

建議順序：先用 `minimal` 範本走完 A0 和 A1，側載 APK 在實機上跑完一個分歧，再做 A2，最後才處理 Sakura 這種大作品需要的 A3。另外，現有的 WASM Preview 表示瀏覽器平台已經跑得動，「Web 版發行」可能是比 Android 更便宜的第二個平台，可以一起評估。

---

## 8. 建議排程總表

| # | 任務 | 優先 | 量 |
| --- | --- | --- | --- |
| T1 | 存檔 lineage 不依賴輸出資料夾，打包時警告，拆分錯誤碼 | P0 | M |
| T2 | QA 環境變數後門改為編譯期開關 | P0 | S～M |
| T3 | validate 補齊 musicTracks、uiFont、fonts、titleBgmAfter 檢查，Player 不再無聲失敗 | P0 | S |
| T4 | UI 重名與必需節點檢查；契約表從 C++ 產生；修掉 Sakura HUD 的 DockLock | P0 | S |
| U1 | 文件與範本示範 skin、Slider、activateSoundPath，加上 lint 和 codemod | P0 | S |
| W1/W2 | `prismatix release` 一鍵發行，registry 自動探索 | P1 | M |
| T5/T6 | 拆 PlayerApp；player 設定型別化並分組 | P1 | L |
| T20 | E2E 移出 Player，改為可腳本化的測試介面 | P1 | L |
| S1–S4 | 共用 macro、elseif、條件選項、變數宣告、行內指令 | P1 | M |
| U2–U5, U8 | Toggle、Radio、Slider 綁定、Tooltip、型別化 action、eject UI | P1 | M |
| W3/W4 | 從此行預覽、VS Code LSP | P1 | M～L |
| A0–A2 | Android：工具鏈、IO 抽象、生命週期、觸控、打包 | P1～P2 | L |
| 其餘 | S5–S8、U6/U7/U9/U10、W5–W8、T7/T8、A3 | P2 | — |

## 附：Sakura 專案本身可以立刻清掉的項目

- 刪除 [HUD.tsx:92](../../../Sakura-no-Uta/Content/UI/HUD.tsx) 那顆沒有 action 的 `DockLock` HudButton（T4）。
- 設定 `player.uiCancelSound`，然後刪掉 29 處 `activateSoundPath`；四張狀態圖改用 `skin=`（U1）。
- 匯入器輸出 `[wait 1000ms]` 並合併連續的 wait；換場的七行樣板改成 macro（S1、S6）。
- `package.json` 的 `--ui-registry` 改成 `--runtime`，就不會再出現 PXCLI1043（W2）。
