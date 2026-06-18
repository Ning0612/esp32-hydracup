# Data Formats

## JSONL 飲水日誌

### 檔案位置

```
logfs partition (0x2F0000, 1 MB)
└── /logs/
    ├── drink-2025-01.jsonl
    ├── drink-2025-02.jsonl
    ├── drink-unsynced.jsonl    ← NTP 未同步期間
    └── ...
```

路徑規則：`/logs/drink-<YYYY-MM>.jsonl`，月份由 `TimeManager.getYearMonth()` 取得。  
若 NTP 未同步，所有記錄寫入 `drink-unsynced.jsonl`。

### 格式

每筆飲水事件佔一行（JSON Lines，`\n` 分隔），UTF-8 無 BOM。

```json
{"ts":"2025-02-18T14:30:45+08:00","ml":250.0,"total":1500.0}
```

| 欄位 | 型別 | 說明 |
|------|------|------|
| `ts` | string | ISO-8601 時間戳，含時區偏移（NTP 同步後）；未同步時為 `"boot+<ms>ms"` 格式（例：`"boot+12345ms"`） |
| `ml` | number | 本次飲水量（公克，約等於毫升）；格式為無小數整數（`%.0f`） |
| `total` | number | 當日累計飲水量（ml）；格式為無小數整數（`%.0f`） |

### 範例

```jsonl
{"ts":"2025-02-18T08:10:22+08:00","ml":185.0,"total":185.0}
{"ts":"2025-02-18T10:33:05+08:00","ml":230.0,"total":415.0}
{"ts":"2025-02-18T12:58:47+08:00","ml":310.0,"total":725.0}
{"ts":"2025-02-18T15:20:11+08:00","ml":200.0,"total":925.0}
```

### 注意事項

- 追加寫入（`FILE_APPEND`），不覆寫
- `logfs` 分割區首次開機自動格式化（不需手動操作）
- `uploadfs` 指令只觸及 `webfs`，不影響日誌資料
- `/api/logs?month=YYYY-MM` 可透過 Web API 讀取全月記錄
- `skipped` 欄位回報無法解析的損毀行數

---

## NVS Schema

ESP32 Preferences 儲存於 NVS flash 分割區（0x9000，20 KB）。

### 命名空間：`water_config`

由 `ConfigManager` 管理，對應 `AppConfig` 結構體的所有欄位。

| NVS Key | 型別 | 預設值 | 對應 AppConfig 欄位 |
|---------|------|-------|-------------------|
| `wifi_ssid` | String | `""` | `wifiSsid` |
| `wifi_pass` | String | `""` | `wifiPassword` |
| `webhook_url` | String | `""` | `discordWebhookUrl` |
| `rem_en` | bool | `true` | `reminderEnabled` |
| `rem_interval` | UInt | `60` | `reminderIntervalMin` |
| `rem_alert_to` | UInt | `60` | `reminderAlertTimeoutSec` |
| `daily_goal` | UInt | `2000` | `dailyGoalMl` |
| `buz_en` | bool | `true` | `buzzerEnabled` |
| `buz_freq` | UInt | `2000` | `buzzerFrequencyHz` |
| `buz_dur` | UInt | `150` | `buzzerDurationMs` |
| `buz_vol` | UInt | `50` | `buzzerVolumePercent` |
| `ntp_en` | bool | `true` | `ntpEnabled` |
| `ntp_srv1` | String | `"pool.ntp.org"` | `ntpServer1` |
| `ntp_srv2` | String | `"time.google.com"` | `ntpServer2` |
| `tz` | String | `"Asia/Taipei"` | `timezone` |
| `tz_off` | Int | `28800` | `timezoneOffsetSec` |
| `dst_off` | Int | `0` | `daylightOffsetSec` |
| `cal_factor` | Float | `1.0` | `calibrationFactor` |
| `tare_offset` | Long | `0` | `tareOffset` |
| `cup_thresh` | Float | `80.0` | `cupPresentThresholdGram` |
| `stable_tol` | Float | `3.0` | `stableToleranceGram` |
| `stable_dur` | UInt | `3000` | `stableDurationMs` |
| `min_drink` | Float | `20.0` | `minDrinkDeltaMl` |
| `max_drink` | Float | `500.0` | `maxDrinkDeltaMl` |
| `ap_ssid` | String | `"WaterCupTracker-Setup"` | `apSsid` |
| `ap_pass` | String | `"12345678"` | `apPassword` |
| `admin_hash` | String | `""` | `adminPasswordHash` |

### 命名空間：`drink_ctr`

由 `DrinkDetector` 管理，用於跨重啟持久化每日計數器。

| NVS Key | 型別 | 說明 |
|---------|------|------|
| `period` | String | 目前計數的日期（`YYYY-MM-DD`） |
| `total_ml` | Float | 當日累計飲水量（ml） |
| `count` | UInt | 當日飲水次數 |
| `last_ml` | Float | 上次飲水量（ml） |

**日期重置機制**：`DrinkDetector.init()` 時比對 `period` 欄位與目前日期。若日期不同，先將舊數據傳給 `DailySummaryManager` 聚合處理，再重置所有欄位並以新日期寫入 NVS。

---

## Flash 分割表

來源：`partitions.csv`

| 名稱 | 標籤 | 型別 | 偏移 | 大小 | 說明 |
|------|------|------|------|------|------|
| nvs | nvs | data | 0x9000 | 20 KB | WiFi 憑證 + AppConfig（NVS） |
| otadata | ota | data | 0xe000 | 8 KB | OTA 狀態 |
| app0 | ota_0 | app | 0x10000 | 1.3 MB | 韌體 A（主要） |
| app1 | ota_1 | app | 0x150000 | 1.3 MB | 韌體 B（OTA 備份） |
| **webfs** | spiffs | data | 0x290000 | 384 KB | 靜態 Web 資源（`data/`） |
| **logfs** | — | data | 0x2F0000 | 1 MB | 飲水事件 JSONL 日誌 |

`uploadfs` 目標只燒錄 `webfs`（0x290000）；`logfs` 不受影響。
