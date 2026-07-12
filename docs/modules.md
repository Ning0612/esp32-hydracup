# Modules

HydraCup 共有 18 個模組，位於 `lib/` 目錄下。每個模組對應一個 `.h` 標頭與 `.cpp` 實作。

---

## AppState

**職責**：保留控制 domain 內部狀態與少量 atomic worker 狀態。跨 task 的 Dashboard
讀取改由 `RuntimeCoordinator` 發布一致快照，不直接把 `AppState` 當同步機制。

**位置**：`lib/AppState/AppState.h`

| 欄位 | 型別 | 預設 | 說明 |
|------|------|------|------|
| `mode` | `AppMode` | `BOOT` | 運行模式（BOOT / AP_MODE / NORMAL） |
| `fsOk` | bool | false | webfs LittleFS 可用 |
| `logFsOk` | bool | false | logfs LittleFS 可用 |
| `oledOk` | bool | false | OLED 初始化成功 |
| `hx711Ok` | bool | false | HX711 初始化成功 |
| `buzzerOk` | bool | false | 蜂鳴器初始化成功 |
| `wifiConnected` | bool | false | WiFi 連線狀態 |
| `ntpSynced` | bool | false | NTP 同步狀態 |
| `weightGrams` | float | 0.0 | 目前秤重（g） |
| `cupState` | `CupState` | `NO_CUP` | 杯子狀態 |
| `todayTotalMl` | float | 0.0 | 今日累計飲水量（ml） |
| `lastDrinkMl` | float | 0.0 | 上次飲水量（ml） |
| `drinkCountToday` | uint32 | 0 | 今日飲水次數 |
| `nextReminderSec` | uint32 | 0 | 下次提醒倒數（秒） |
| `webhookConfigured` | bool | false | Discord Webhook URL 已設定 |
| `webhookLastOk` | atomic\<bool\> | false | 最後一次 Webhook 發送成功 |
| `ipAddress` | String | "" | 目前 IP 位址 |

---

## ConfigManager

**職責**：以 ESP32 Preferences（NVS）持久化 `AppConfig` 的所有欄位。命名空間：`water_config`。

**位置**：`lib/ConfigManager/`

| 方法 | 說明 |
|------|------|
| `load(AppConfig& cfg)` | 從 NVS 讀取設定，未設定欄位保持預設值 |
| `save(const AppConfig& cfg)` | 將所有欄位寫入 NVS |
| `saveCalibration(float factor, long offset)` | 僅更新校正係數與 tare 偏移 |
| `saveWifi(const String& ssid, const String& password)` | 僅更新 WiFi 憑證 |
| `clear()` | 清除整個命名空間 |

NVS key 對應見 [data-formats.md](data-formats.md#nvs-schema)。

所有 Preferences 操作都使用 `StorageLock` 共用 mutex，避免多個 worker 同時進入 NVS。

---

## RuntimeCoordinator

**職責**：發布帶 sequence 的一致 `RuntimeSnapshot`，並以固定大小 Queue 傳遞
`ControlCommand` / `ControlResult`。HTTP timeout 不會留下 caller-owned pointer。
同步 result Queue 明確由單一 `DashboardServer` requester 使用。

**位置**：`lib/RuntimeCoordinator/`

---

## StorageLock

**職責**：提供全專案共用的 NVS mutex；Config、飲水計數器與每日結算使用相同鎖。

**位置**：`lib/StorageLock/`

---

## WiFiManager

**職責**：管理 STA 連線與 AP 啟動，向 `AppState` 回報連線狀態與 IP。

**位置**：`lib/WiFiManager/`

| 方法 | 說明 |
|------|------|
| `connectSTA(const String& ssid, const String& password, uint32_t timeoutMs = 10000)` | 嘗試 STA 連線，帶超時（ms）；回傳是否成功 |
| `startAP(const String& ssid, const String& password)` | 啟動 SoftAP；回傳是否成功 |
| `loop()` | 處理 WiFi 事件（需在 `loop()` 中呼叫） |
| `isConnected()` | 回傳 STA 是否已連線 |
| `getIP()` | 回傳 STA IP 字串 |
| `getAPIP()` | 回傳 AP IP 字串（AP Mode 下使用） |

---

## ConfigPortal

**職責**：AP Mode 下的 HTTP 設定伺服器，提供 WiFi 設定頁面，接收並儲存憑證後觸發重啟。

**位置**：`lib/ConfigPortal/`

**端點**：`/`、`/api/status`、`/api/config`（POST）、`/api/wifi/scan`、`/api/reboot`

---

## DashboardServer

**職責**：Normal Mode 下的 HTTP 伺服器，提供靜態 Web UI 並實作完整 REST API。

**位置**：`lib/DashboardServer/`

| 方法 | 說明 |
|------|------|
| `begin(AppState&, AppConfig&, ...)` | 初始化並掛載所有路由 |
| `loop()` | 處理待辦 HTTP 請求 |

Dashboard 讀取 `RuntimeSnapshot`；tare、calibrate 與立即生效的 reminder/buzzer 設定會送出
typed control command，等待固定大小的 `ControlResult`。

API 完整文件見 [api.md](api.md)。

---

## ScaleManager

**職責**：驅動 HX711，維護 10 樣本的移動平均緩衝區，並判斷重量是否穩定。

**位置**：`lib/ScaleManager/`

| 方法 | 說明 |
|------|------|
| `init(float calibFactor, long tareOffset, float stableToleranceG, uint32_t stableDurationMs)` | 初始化 HX711 與穩定偵測參數 |
| `update()` | 每 100 ms 取樣一次，更新移動平均 |
| `isReady()` | HX711 是否成功初始化 |
| `isSamplesReady()` | 移動平均緩衝區是否已填滿 10 個樣本 |
| `isStable()` | 若重量在 ±`stableToleranceG` 內持續 `stableDurationMs` 則回傳 true |
| `getWeightGrams()` | 取得目前移動平均重量（g） |
| `getStableWeightGrams()` | 取得最後穩定重量（g） |
| `getRawAverage()` | 取得原始 ADC 均值（除錯用） |
| `tare()` / `startTare()` | 啟動非阻塞歸零狀態機 |
| `isTareRunning()` | tare 是否仍在收集樣本 |
| `takeTareResult(long&)` | 取得完成的 tare offset |
| `setCalibrationFactor(float factor)` | 直接設定校正係數 |
| `calibrateWithKnownWeight(float knownGrams)` | 計算並回傳新的 calibrationFactor |
| `getTareOffset()` | 取得目前 tare 偏移 |
| `getCalibrationFactor()` | 取得目前校正係數 |

**常數**：`HX711_SAMPLE_COUNT = 10`，`HX711_READ_INTERVAL_MS = 100`

---

## DrinkDetectorCore

**職責**：不依賴 Arduino 的 6 態飲水偵測核心。輸入重量、穩定狀態與時間，
透過 `DrinkDetectorEventSink` 輸出飲水/補水事件；`DrinkDetectorEventHandler`
則隔離計數器、持久化與通知效果，供 native 測試使用 fake ports。

**位置**：`lib/DrinkDetectorCore/`

## DrinkDetector

**職責**：ESP32 adapter，將 `ScaleManager`、`AppState`、NVS、提醒、蜂鳴器與通知
管道接到 `DrinkDetectorCore`；核心仍僅透過「杯子拿起後放下」路徑觸發飲水或補水事件。

**位置**：`lib/DrinkDetector/`

| 方法 | 說明 |
|------|------|
| `init(ScaleManager& scale, AppState& state, const AppConfig& cfg, ReminderManager& reminder, BuzzerController& buzzer)` | 初始化，注入依賴並從 NVS 還原當日計數 |
| `update()` | 由 TaskControl 呼叫，推進狀態機 |
| `resetScaleBaseline()` | tare 完成後清除舊杯重基準，避免誤判事件 |
| `setDiscordNotifier(DiscordNotifier* dn)` | 注入 DiscordNotifier 依賴 |
| `setEventLogger(EventLogger* el)` | 注入 EventLogger 依賴 |
| `setTimeManager(TimeManager* tm)` | 注入 TimeManager 依賴 |
| `setCounterPersistence(DrinkCounterPersistence* store)` | 替換每日計數器持久化 port；未注入時使用 ESP32 Preferences |
| `getCupState()` | 取得目前 `CupState` |
| `getTodayTotalMl()` | 取得今日累計飲水量（ml） |
| `getLastDrinkMl()` | 取得上次飲水量（ml） |
| `getDrinkCountToday()` | 取得今日飲水次數 |
| `resetDailyCounters()` | 重置計數器（DailySummaryManager 午夜呼叫） |

**持久化**：NVS 命名空間 `drink_ctr`；日期變更時自動重置並聚合統計。狀態機邏輯見 [architecture.md](architecture.md#飲水偵測狀態機)。
一般 counter snapshot 由單一 worker 合併儲存；pending/completed version 提供 DailySummary
使用的非阻塞 persistence barrier。Restore worker 只讀取 POD，最後由 TaskControl 套用。

---

## ReminderManager

**職責**：根據 `reminderIntervalMin` 在無飲水事件超時後觸發蜂鳴提醒，飲水後重置計時器。

**位置**：`lib/ReminderManager/`

| 方法 | 說明 |
|------|------|
| `init(uint32_t intervalMin, bool enabled)` | 設定提醒間隔（分鐘）與啟用狀態 |
| `update()` | 由 TaskControl 呼叫，驅動提醒計時器 |
| `resetTimer()` | 重置計時器（飲水確認後呼叫） |
| `setBuzzer(BuzzerController* buz)` | 注入蜂鳴器依賴 |
| `setAppState(AppState* state)` | 注入 AppState 依賴 |
| `setEnabled(bool en)` | 啟用/停用提醒 |
| `setIntervalMin(uint32_t min)` | 動態更新提醒間隔 |
| `setAlertTimeoutSec(uint32_t sec)` | 動態更新提醒持續時間 |
| `getNextReminderSec()` | 取得下次提醒倒數（秒） |

提醒蜂鳴使用 `BeepPattern::REMINDER`，`_alertTimeoutMs` 後自動停止蜂鳴。`BEEP_CYCLE_GAP_MS = 800` 為蜂鳴循環間隔。

---

## BuzzerController

**職責**：LEDC PWM 非阻塞蜂鳴佇列，支援最多 6 個 BeepStep 的組合序列。

**位置**：`lib/BuzzerController/`

| 方法 | 說明 |
|------|------|
| `init(uint32_t freqHz, uint8_t volumePct)` | 初始化 LEDC，GPIO 18 |
| `play(BeepPattern pattern)` | 播放預設模式 |
| `update()` | 非阻塞推進蜂鳴佇列 |
| `stop()` | 立即停止 |
| `setFrequency(uint32_t hz)` | 設定基礎頻率 |
| `setVolume(uint8_t pct)` | 設定音量（0–100%） |
| `setDuration(uint32_t ms)` | 設定基礎時長 |
| `isPlaying()` | 是否正在播放 |

**BeepPattern 列表**

| 模式 | 說明 |
|------|------|
| `BOOT_OK` | 開機成功音 |
| `AP_MODE` | AP Mode 啟動提示 |
| `WIFI_CONNECTED` | WiFi 連線成功 |
| `DRINK` | 飲水確認音 |
| `REMINDER` | 喝水提醒音 |
| `ERROR_BEEP` | 錯誤提示音 |
| `CALIBRATION_OK` | 校正完成音 |

---

## DisplayManager

**職責**：SSD1306 OLED（128×32）輪播兩個頁面，60 秒無操作後自動睡眠。

**位置**：`lib/DisplayManager/`

| 方法 | 說明 |
|------|------|
| `init()` | 初始化 Adafruit SSD1306；回傳是否成功 |
| `showBootScreen()` | 顯示開機畫面 |
| `showError(const char* msg)` | 顯示錯誤訊息 |
| `showWifiConnecting(const String& ssid)` | 顯示 WiFi 連線中畫面 |
| `showAPMode(const String& apSsid, const String& apPassword, const String& ip)` | 顯示 AP Mode 資訊 |
| `showNormalMode(float weightG, bool stable, float todayMl, uint32_t goalMl, uint32_t drinkCount, float lastDrinkMl, uint32_t nextRemSec, bool wifiOk, const String& ip, bool ntpSynced)` | 更新 Normal Mode 顯示資料 |
| `wake()` | 喚醒螢幕（重置睡眠計時器） |
| `sleep()` | 立即關閉螢幕 |
| `update()` | 由 TaskControl 呼叫，驅動頁面切換與睡眠邏輯 |
| `isAvailable()` | 回傳 OLED 是否正常初始化 |

**頁面內容**

| 頁 | 顯示內容 |
|----|---------|
| 0 | 目前秤重（g）、IP 位址 |
| 1 | 今日飲水進度（ml）、飲水次數、上次飲水量、下次提醒倒數 |

頁面每 4 秒（`PAGE_INTERVAL_MS`）切換一次；60 秒（`SCREEN_ON_DURATION_MS`）無操作後螢幕自動關閉。

---

## DiscordNotifier

**職責**：以單一持久 FreeRTOS worker 非同步發送 HTTPS POST。drink/summary 使用高優先
Queue；online 狀態使用可合併的低優先 Queue，不會為每次 POST 配置新 task stack。

**位置**：`lib/DiscordNotifier/`

| 方法 | 說明 |
|------|------|
| `init(AppState& state, const AppConfig& cfg)` | 初始化，設定 AppState 與 AppConfig 引用，檢查 Webhook URL 是否已設定 |
| `notifyOnline(const String& ipAddress)` | 裝置上線通知（含 IP） |
| `notifyDrink(float amountMl, float totalMl, uint32_t drinkCount)` | 飲水事件通知（含飲水量、今日總量、次數） |
| `notifyDailySummary(float totalMl, uint32_t drinkCount, const String& dateStr)` | 每日摘要通知（回傳是否排程成功） |
| `update()` | 由 loopTask 呼叫（保留給未來擴展） |

**安全**：Web UI 回應中，Webhook URL 末尾以 `****` 遮罩。`POST /api/config` 時若 URL 含 `****` 則略過更新，防止誤覆蓋 token。

---

## EventLogger

**職責**：將每次飲水事件以 JSONL 格式追加至 logfs 分割區，依月份分檔案。

**位置**：`lib/EventLogger/`

| 方法 | 說明 |
|------|------|
| `init(bool fsOk, fs::LittleFSFS& fs)` | 初始化，確認 logfs 可用 |
| `logDrink(const String& ts, float amountMl, float totalMl, TimeManager* tm)` | 追加一筆飲水記錄 |

日誌格式與路徑規則見 [data-formats.md](data-formats.md)。

---

## TimeManager

**職責**：NTP 時間同步與時戳產生，每 12 小時重新同步防止漂移。未同步時以 `uptime_ms` 回退。

**位置**：`lib/TimeManager/`

| 方法 | 說明 |
|------|------|
| `init(const AppConfig& cfg)` | 設定 NTP 伺服器與時區偏移，開始同步 |
| `update()` | 每 12 小時（`RESYNC_INTERVAL_MS`）重新同步 NTP |
| `isSynced()` | 是否已完成 NTP 同步 |
| `getLocalTm(struct tm& t)` | 取得本地時間結構（失敗回傳 false） |
| `getISOTimestamp()` | 取得 ISO-8601 時間戳（含時區偏移）；未同步時回傳 `"boot+<ms>ms"` |
| `getYearMonth()` | 取得 `"YYYY-MM"` 格式字串（用於日誌路徑）；未同步時回傳 `"unsynced"` |
| `getDateString()` | 取得 `"YYYY-MM-DD"` 格式字串；未同步時回傳空字串 |

時區偏移由 `timezoneOffsetSec`（預設 +28800，即 UTC+8）與 `daylightOffsetSec`（預設 0）組合而成。

---

## DailySummaryManager

**職責**：偵測每日午夜（00:00）時刻，呼叫 `DrinkDetector.resetDailyCounters()` 並透過 `DiscordNotifier` 發送當日總結。

**位置**：`lib/DailySummaryManager/`

| 方法 | 說明 |
|------|------|
| `init(DiscordNotifier& discord, DrinkDetector& detector, TimeManager& time, const AppConfig& cfg)` | 初始化四個依賴模組 |
| `update()` | 由 TaskControl 呼叫，以非阻塞 settlement stages 推進每日摘要 |

午夜摘要包含：當日總飲水量、飲水次數、相對每日目標的達成率。
