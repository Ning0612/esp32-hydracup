# Architecture

## 系統架構

HydraCup 在 ESP32 Arduino/FreeRTOS 上運行，分為兩個互斥模式。Arduino `loopTask`
負責 Web 與健康監督；固定在 Arduino core 的高優先 `hydracup_control` task 負責感測與
即時控制：

```
┌────────────────────────────────────────────────────────┐
│                    BOOT                                │
│  Serial → LittleFS → NVS/Config → OLED → HX711        │
│  → Buzzer → WiFi configured?                          │
│        No  → AP_MODE (ConfigPortal)                   │
│        Yes → STA connect → OK? → NORMAL : AP_MODE     │
└────────────────────────────────────────────────────────┘

┌────────── AP Mode ─────────────────────────────────────┐
│  ConfigPortal  HTTP server @ 192.168.4.1               │
│  → WiFi config saved → reboot → NORMAL                │
└────────────────────────────────────────────────────────┘

┌────────── Normal Mode ─────────────────────────────────┐
│  TaskControl (10 ms, Core 1)                           │
│  ScaleManager → DrinkDetector                          │
│                     ├─→ EventLogger                    │
│                     ├─→ DiscordNotifier                │
│                     ├─→ ReminderManager                │
│                     ├─→ BuzzerController               │
│                     └─→ AppState                       │
│                                                        │
│  ├─ ReminderManager / BuzzerController                 │
│  ├─ DisplayManager / TimeManager                       │
│  └─ DailySummaryManager                                │
│                                                        │
│  loopTask: DashboardServer + WiFi health               │
│  workers: MQTT / Discord / EventLogger / counter NVS   │
└────────────────────────────────────────────────────────┘
```

### 設計約束

- `hydracup_control` 是 Scale、Detector、Reminder、Buzzer、Display、Time 的單一 owner
- `loopTask` 不直接存取上述模組的 mutable state；Dashboard 透過 snapshot / command 溝通
- 絕對禁止在 `loop()` 或任何 `update()` 中使用 `delay()`
- 所有計時使用 `millis()`
- Discord 與 MQTT 各使用持久 FreeRTOS worker；兩者不共用阻塞 task
- tare 是 10 筆、每 100 ms 一筆的非阻塞狀態機；期間暫停飲水偵測
- NVS 操作透過共用 mutex 序列化；LogFS 讀寫也有獨立 mutex
- counter restore worker 只載入 POD，由 TaskControl 套用；跨日資料即使非午夜開機也會立即結算
- Daily settlement 等待 counter save ack，再由 storage worker 寫 settled marker，控制 task 不等待 flash

---

## 模組職責表

| 模組 | 職責 |
|------|------|
| `AppState` | 共用執行時狀態結構（模式、杯子狀態、今日飲水量等） |
| `ConfigManager` | NVS 讀寫 `AppConfig`；命名空間 `water_config` |
| `WiFiManager` | STA 連線、AP 啟動、連線狀態管理 |
| `ConfigPortal` | AP Mode HTTP 伺服器（192.168.4.1），WiFi 設定 |
| `DashboardServer` | Normal Mode HTTP 伺服器，儀表板 + REST API |
| `ScaleManager` | HX711 取樣、移動平均濾波、秤台校正 |
| `DrinkDetectorCore` | 不依賴 Arduino 的 6 態飲水偵測核心，輸出飲水/補水事件 |
| `DrinkDetector` | ESP32 adapter：對接 ScaleManager、AppState、每日計數器與通知管道 |
| `ReminderManager` | millis 計時提醒，飲水後重置 |
| `BuzzerController` | LEDC PWM 非阻塞蜂鳴佇列（7 種模式） |
| `DisplayManager` | SSD1306 OLED 輪播（2 頁，各 4 秒），自動睡眠 |
| `DiscordNotifier` | 非同步 HTTPS POST 至 Discord Webhook |
| `EventLogger` | LittleFS JSONL 飲水事件日誌（依月份分檔） |
| `TimeManager` | NTP 時間同步；提供 ISO-8601 時戳 |
| `DailySummaryManager` | 每日午夜聚合統計並送出 Discord 摘要 |
| `RuntimeCoordinator` | 一致 runtime snapshot、control command/result Queue |
| `StorageLock` | 跨 task 序列化 Preferences/NVS 操作 |

---

## 開機流程

```
1. Serial.begin(115200)
2. LittleFS.begin(webfs)          → 靜態資源
3. LittleFS.begin(logfs)          → 日誌分割區（首次自動格式化）
4. ConfigManager.load(cfg)        → 從 NVS 讀取設定
5. OLED.init()                    → 失敗則記錄並繼續
6. HX711.init()                   → 失敗則記錄並繼續
7. BuzzerController.init()        → BOOT_OK 蜂鳴
8. WiFiManager.connect(cfg)
   ├─ WiFi SSID 空白 → AP_MODE
   ├─ 連線失敗       → AP_MODE
   └─ 連線成功       → NORMAL
9. 建立 RuntimeCoordinator 與背景 workers
10. 建立 `hydracup_control`；建立失敗時進入 degraded loop control，tare/calibrate API 停用
11. NORMAL: DashboardServer 由 loopTask 服務
    AP_MODE: ConfigPortal 由 loopTask 服務
```

外設（OLED、HX711）初始化失敗時**不中斷開機**，僅記錄錯誤並設 `AppState.oledOk = false` / `hx711Ok = false`。

---

## 飲水偵測狀態機

### 狀態定義

| 狀態 | 值 | 說明 |
|------|----|------|
| `NO_CUP` | 0 | 無杯子（重量低於閾值） |
| `CUP_UNSTABLE` | 1 | 杯子已放上但重量未穩定 |
| `CUP_STABLE` | 2 | 杯子穩定放置 |
| `POSSIBLE_DRINK` | 3 | 重量下降中（可能正在喝） |
| `DRINK_CONFIRMED` | 4 | 飲水確認（瞬間狀態） |
| `REFILL_DETECTED` | 5 | 補水確認（瞬間狀態） |

### 狀態轉換圖

```
                    ┌─────────────────────────────┐
                    │                             │
                    ▼                             │ weight < threshold
              [NO_CUP]                            │ (_cupLifted = true)
                    │                             │
   weight >= threshold                            │
                    │                             │
                    ▼                             │
          [CUP_UNSTABLE] ◄────────────────────────┘
           (_cupLifted?)
                    │
    isStable() && !_cupLifted
                    │
                    ▼
           [CUP_STABLE] ◄──────────────────────────────────────────┐
                    │                                               │
    weight drops    │  weight < threshold          isStable()      │
    (not stable)    │  (_cupLifted = true)             │           │
                    │                                  │           │
                    ▼                                  │           │
        [POSSIBLE_DRINK]                               │           │
                    │                                  │           │
    weight < threshold     isStable()                  │           │
    (_cupLifted = true)        │                       │           │
                    │          └───────────────────────┘           │
                    │                                               │
                    └───────────────────────────────────────────────┘
                              (→ CUP_STABLE after update)

    isStable() && _cupLifted:
      delta in [minDrink, maxDrink]             → [DRINK_CONFIRMED]  → CUP_STABLE
      (newStable - prevRef) >= minDrink         → [REFILL_DETECTED]  → CUP_STABLE
      else                                      → CUP_STABLE
```

### 關鍵機制

**`_cupLifted` 旗標**：只有當狀態為 `CUP_STABLE`（或 `POSSIBLE_DRINK`）且重量低於 `cupPresentThresholdGram` 時才設置。確保**飲水與補水事件只能透過「拿起再放下」路徑觸發**，防止秤台上的重量微變造成誤判。

**`DRINK_LIFT_TIMEOUT_MS = 120,000 ms`**：若杯子被拿起超過 120 秒後放回，重置 `_cupLifted` 為 false，不觸發飲水事件（視為與飲水無關的操作）。

**杯子放置但重量變化（無拿起）**：只更新 `_prevStableWeight`，不觸發任何飲水或補水事件。

### 觸發條件參數

| 參數 | 預設值 | 說明 |
|------|-------|------|
| `cupPresentThresholdGram` | 80.0 g | 低於此值視為無杯子 |
| `stableToleranceGram` | 3.0 g | 穩定判斷允許誤差 |
| `stableDurationMs` | 3000 ms | 必須在誤差內持續此時間才算穩定 |
| `minDrinkDeltaMl` | 20.0 ml | 最小有效飲水量 |
| `maxDrinkDeltaMl` | 500.0 ml | 最大有效飲水量 |

`1 g ≈ 1 ml` 在整個系統中成立。

### 濾波與穩定判定依據

`ScaleManager` 以 10 筆 moving average 平滑 HX711 讀值；取樣間隔為 100 ms，
因此約涵蓋 1 秒的短期輸入。這個窗口用來壓低單次 ADC 抖動，同時保留拿起杯子
與放回杯子的重量變化反應速度。

穩定判定預設允許重量在 ±3 g 內波動，並要求持續 3000 ms。3 g 是用來容許 load
cell 與平台的微幅噪聲；3 秒則避免短暫碰撞或放杯回彈直接進入 `CUP_STABLE`。
這兩個值是初始調校值，應依實際秤台、load cell 與環境振動調整：放寬誤差或縮短
時間會加快反應但提高誤判風險，收緊誤差或延長時間則相反。
