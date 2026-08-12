# REST API Reference

## 共通規則

- 所有端點回應 JSON
- 成功：`{"ok": true, ...}`
- 失敗：`{"ok": false, "error": "<message>"}`
- Normal Mode 的頁面與資料 API 需要登入；未登入頁面回到 `/login`，未登入 API 回 `401`
- 狀態變更的 `POST` API 需要 `X-CSRF-Token`；登入前使用每次開機的 token，登入後使用 session 綁定 token
- session 是裝置端單一有效 session，新登入會取代舊 session；idle timeout 30 分鐘、absolute timeout 24 小時
- 管理密碼以 PBKDF2-HMAC-SHA256 雜湊儲存，不會由 API 回傳
- 目前 WebUI 使用 HTTP；同網段主動攻擊者仍可能攔截並重放密碼、session 或 CSRF token，建議限制在信任的隔離 LAN
- 埠號：80

---

## DashboardServer（Normal Mode）

Normal Mode 下運行，ESP32 連上 WiFi 後提供。

### 靜態頁面

| 方法 | 路徑 | 說明 |
|------|------|------|
| GET | `/` | 儀表板首頁（`index.html`） |
| GET | `/settings` | 設定頁面（`settings.html`） |
| GET | `/history` | 飲水歷史頁面（`history.html`） |
| GET | `/login` | 登入／首次設定管理密碼 |
| GET | `/style.css` | 共用樣式表 |
| GET | `/ui.js` | 共用主題與 session 輔助腳本 |
| GET | `/favicon.svg` | HydraCup favicon |
| GET | `/calibration` | 重定向至 `/settings#calibration` |

### 認證端點

#### `GET /api/auth/csrf`

取得登入頁或已登入 session 使用的 CSRF token。回應包含 `configured` 與
`authenticated` 狀態；token 僅應由同源前端放入 `X-CSRF-Token` header。

#### `POST /api/auth/login`

首次尚未設定管理密碼時，body 必須包含相同的 `password`／`confirm`，且密碼至少 8 個字元；
已設定密碼時只驗證 `password`。成功後回傳 `session` HttpOnly cookie。

#### `POST /api/auth/logout`

需要已登入 session 與 CSRF token；伺服器端會撤銷唯一有效 session。

---

### `GET /api/weight`

取得目前秤重與杯子狀態。

**回應**

```json
{
  "ok": true,
  "weight_g": 342.5,
  "cup_state": 2
}
```

| 欄位 | 型別 | 說明 |
|------|------|------|
| `weight_g` | float | 目前重量（公克） |
| `cup_state` | int | 杯子狀態代碼（見下表） |

**cup_state 代碼**

| 值 | 名稱 | 說明 |
|----|------|------|
| 0 | `no_cup` | 無杯子 |
| 1 | `unstable` | 不穩定 |
| 2 | `stable` | 穩定 |
| 3 | `possible_drink` | 可能正在喝 |
| 4 | `drink_confirmed` | 飲水確認 |
| 5 | `refill_detected` | 補水確認 |

---

### `GET /api/status`

取得完整系統狀態快照。由 `index.html` 每 2 秒輪詢。

**回應**

```json
{
  "ok": true,
  "mode": "normal",
  "wifi_connected": true,
  "ip": "192.168.1.100",
  "ntp_synced": true,
  "weight_g": 342.5,
  "cup_state": 2,
  "cup_state_name": "stable",
  "today_total_ml": 1250.0,
  "daily_goal_ml": 2000,
  "drink_count_today": 5,
  "last_drink_ml": 230.0,
  "next_reminder_sec": 1800,
  "cloud_history_backfill_state": "complete",
  "cloud_history_backfill_running": false,
  "cloud_history_backfill_uploaded_days": 45,
  "cloud_history_backfill_http_status": 200,
  "webhook_configured": true,
  "webhook_last_ok": true,
  "discord_worker_ready": true,
  "discord_queue_drops": 0,
  "hw_hx711": true,
  "hw_oled": true,
  "hw_fs": true,
  "hw_logfs": true,
  "rtos": true,
  "rtos_healthy": true,
  "rtos_sequence": 8124,
  "rtos_command_drops": 0,
  "rtos_result_drops": 0,
  "log_queue_drops": 0
}
```

| 欄位 | 型別 | 說明 |
|------|------|------|
| `mode` | string | `"normal"` 或 `"ap"` |
| `wifi_connected` | bool | WiFi 連線狀態 |
| `ip` | string | 目前 IP 位址 |
| `ntp_synced` | bool | NTP 時間同步狀態 |
| `weight_g` | float | 目前重量（g） |
| `cup_state` | int | 杯子狀態代碼 |
| `cup_state_name` | string | 杯子狀態名稱 |
| `today_total_ml` | float | 今日累計飲水量（ml） |
| `daily_goal_ml` | uint32 | 每日目標飲水量（ml） |
| `drink_count_today` | uint32 | 今日飲水次數 |
| `last_drink_ml` | float | 上次飲水量（ml） |
| `next_reminder_sec` | uint32 | 下次提醒倒數（秒） |
| `cloud_history_backfill_state` | string | `idle`、`queued`、`uploading`、`retrying` 或 `complete` |
| `cloud_history_backfill_running` | bool | 歷史補傳背景工作是否仍會繼續 |
| `cloud_history_backfill_uploaded_days` | uint32 | 本次開機／工作已由 service 確認的日期數 |
| `cloud_history_backfill_http_status` | int | 最近一次歷史補傳 HTTP status；尚未送出為 0 |
| `webhook_configured` | bool | Webhook URL 已設定 |
| `webhook_last_ok` | bool | 最後一次 Webhook 是否成功 |
| `discord_worker_ready` | bool | 持久 Discord worker 已建立 |
| `discord_queue_drops` | uint32 | Discord Queue 滿或不可用的累計丟棄數 |
| `hw_hx711` | bool | HX711 初始化成功 |
| `hw_oled` | bool | OLED 初始化成功 |
| `hw_fs` | bool | webfs 初始化成功 |
| `hw_logfs` | bool | logfs 初始化成功 |
| `rtos` | bool | control task 已啟動 |
| `rtos_healthy` | bool | 最近 2 秒內收到 control heartbeat |
| `rtos_sequence` | uint32 | runtime snapshot 發布序號 |
| `rtos_command_drops` | uint32 | control command Queue 滿的累計次數 |
| `rtos_result_drops` | uint32 | command result Queue 滿的累計次數 |
| `log_queue_drops` | uint32 | 非同步飲水 log 無法排入/寫入的累計次數 |

---

### `GET /api/config`

取得所有設定值。密碼與 Webhook URL 已遮罩。

**回應**

```json
{
  "ok": true,
  "wifiSsid": "MyWiFi",
  "wifiPassword": "****",
  "wifiPasswordSet": true,
  "discordWebhookUrl": "https://discord.com/api/webhooks/123/****",
  "reminderEnabled": true,
  "reminderIntervalMin": 60,
  "reminderAlertTimeoutSec": 60,
  "dailyGoalMl": 2000,
  "buzzerEnabled": true,
  "buzzerFrequencyHz": 2000,
  "buzzerDurationMs": 150,
  "buzzerVolumePercent": 50,
  "ntpEnabled": true,
  "ntpServer1": "pool.ntp.org",
  "ntpServer2": "time.google.com",
  "timezone": "Asia/Taipei",
  "calibrationFactor": 427.5,
  "cupPresentThresholdGram": 80.0,
  "stableToleranceGram": 3.0,
  "stableDurationMs": 3000,
  "minDrinkDeltaMl": 20.0,
  "maxDrinkDeltaMl": 500.0
}
```

---

### `POST /api/config`

儲存設定。若 `wifiPassword` 或 `discordWebhookUrl` 包含 `"****"` 則略過更新（防誤覆蓋）。

**請求 Body（JSON，所有欄位可選）**

```json
{
  "dailyGoalMl": 2500,
  "reminderEnabled": true,
  "reminderIntervalMin": 45,
  "reminderAlertTimeoutSec": 30,
  "buzzerEnabled": true,
  "buzzerFrequencyHz": 2000,
  "buzzerDurationMs": 150,
  "buzzerVolumePercent": 70,
  "ntpEnabled": true,
  "ntpServer1": "pool.ntp.org",
  "ntpServer2": "time.google.com",
  "timezone": "Asia/Taipei",
  "wifiSsid": "MyWiFi",
  "wifiPassword": "newpassword",
  "discordWebhookUrl": "https://discord.com/api/webhooks/.../token",
  "cupPresentThresholdGram": 80.0,
  "stableToleranceGram": 3.0,
  "stableDurationMs": 3000,
  "minDrinkDeltaMl": 20.0,
  "maxDrinkDeltaMl": 500.0
}
```

WiFi、NTP、MQTT 或進階感測器設定有異動時，回應會要求重新啟動。

**回應**

```json
{
  "ok": true,
  "reboot_required": true,
  "control_applied": true
}
```

`reboot_required` 為 `true` 表示至少一項設定需重啟後完整套用；`control_applied`
表示 daily goal、reminder 與 buzzer 的即時 control commands 都已成功處理。

---

### `POST /api/tare`

歸零由 `hydracup_control` 收集 10 筆 tare 樣本，再等待 10 筆 warm-up 樣本。HTTP handler
最多等待 4.5 秒；
執行期間再次要求會回傳 `409`，尚未暖機回傳 `503`。

秤重歸零（Tare）。將目前重量設為 0 基準點並儲存偏移至 NVS。

**請求 Body**：無

**回應**

```json
{"ok": true}
```

---

### `POST /api/calibrate`

以已知重量校正感測器。校正係數（`calibrationFactor`）儲存至 NVS。

**請求 Body**

```json
{"known_weight_g": 200.0}
```

**回應**

```json
{
  "ok": true,
  "calibration_factor": 427.53,
  "current_weight_g": 200.1
}
```

---

### `GET /api/wifi/scan`

掃描附近 WiFi 網路。

**回應**

```json
{
  "ok": true,
  "networks": [
    {"ssid": "MyWiFi", "rssi": -45, "secure": true},
    {"ssid": "GuestNet", "rssi": -72, "secure": false}
  ]
}
```

---

### `GET /api/logs`

取得指定月份的飲水記錄。

**查詢參數**

| 參數 | 說明 |
|------|------|
| `month` | 格式 `YYYY-MM`（例：`2025-02`）；或 `"unsynced"`（NTP 未同步期間的記錄） |

**回應**

```json
{
  "ok": true,
  "month": "2025-02",
  "entries": [
    {"ts": "2025-02-18T14:30:45+08:00", "ml": 250.0, "total": 1500.0},
    {"ts": "2025-02-18T15:45:20+08:00", "ml": 180.0, "total": 1680.0}
  ],
  "skipped": 0
}
```

| 欄位 | 說明 |
|------|------|
| `entries[].ts` | ISO-8601 時間戳（含時區偏移） |
| `entries[].ml` | 本次飲水量（ml） |
| `entries[].total` | 當日累計飲水量（ml） |
| `skipped` | 無法解析的行數（JSONL 損毀行） |

---

### `POST /api/reboot`

重啟裝置。

**回應**

```json
{"ok": true}
```

---

### `POST /api/cloud/history-backfill`

需要登入與 CSRF token。建立可跨重啟續傳的背景工作，逐月讀取 `/logfs/logs`，只把今天
以前的每日累計量、飲水次數與最後飲水時間送到已設定的 HydraCup Service。重複呼叫進行中
的工作不會重設游標；完成後再次呼叫會從頭安全重送。

成功排入回 `202`：

```json
{"ok": true, "state": "queued"}
```

logfs 不可用回 `503 logfs_unavailable`；Cloud sync 未完整設定回
`409 cloud_not_configured`。執行狀態由 `GET /api/status` 查詢。

---

### `POST /api/logs/clear`

需要登入與 CSRF token，**並在 body 再次提供管理密碼**。session 本身不足以授權這個操作，
因為它無法復原。

```json
{"password": "..."}
```

**這個端點不會當場清除任何東西。** 它只把要求記在 NVS，回應後重新啟動；真正的清除發生在
下次開機、掛載檔案系統之前。回應為 `{"ok": true, "state": "restarting"}`。

清除內容：格式化整個 `logfs`（飲水月檔 `/logfs/logs/drink-*.jsonl` 與尚未上傳的同步佇列
`/logfs/cloud/outbox.jsonl`）、NVS `drink_ctr` 的今日累計與飲水次數、以及 `cloud_sync` 的
`evt_overflow` 溢位暫存（留著會把剛刪掉的事件放回佇列）。

**不會**刪除已同步到雲端的資料，也不動 `cloud_sync` 的 sequence 狀態、校正值、WiFi 或其他設定。

| 狀態 | 錯誤 | 條件 |
|---|---|---|
| 401 | `invalid_credentials` | 密碼錯誤、未設定或超過 128 bytes。失敗計入與登入相同的鎖定機制 |
| 429 | `rate_limited` | 密碼嘗試次數過多 |
| 409 | `history_backfill_in_progress` | 歷史補傳進行中——它會在回應與重開機之間繼續上傳並推進 cursor，而那些歷史即將消失 |
| 500 | `request_not_stored` | 要求無法寫入 NVS，**沒有排定任何清除** |

#### 為什麼在開機時清除

在系統運行中清除，等於要和每一個正在寫入這些資料的來源賽跑：EventLogger 佇列中的事件
（包含已出佇列、正等待檔案系統鎖的那一筆）、計數器儲存的佇列與重試路徑、開機時仍在讀取
舊值的 NVS restore，以及 Cloud sync 在鎖內讀出、鎖外送出的批次。每一項都需要各自的屏障，
而檔案清除與計數清除之間仍然無法對「剛好在此時發生的一次飲水」保持原子性。

開機時這些都還不存在——沒有任何 task 被建立、分割區也尚未掛載，清除因此是一條沒有併發的
直線。`/logfs/logs` 與 `/logfs/cloud` 由 `EventLogger` 與 `CloudSyncClient` 在同一次開機的
初始化中重建，不需要額外處理。

執行順序為 **NVS 計數先、logfs 格式化後**，讓不可逆的一步排在最後：計數抹除失敗就直接中止，
此時一個位元組都還沒被刪；格式化失敗則計數已歸零但月檔仍在。兩種情況下要求旗標都會保留，
下次開機重跑一次。旗標**最後才清除**，因此中途斷電也只是再跑一次，不會回報一個沒做到的成功。

已知取捨：旗標清除失敗（NVS 持續性故障）時每次開機都會重新格式化一次 logfs。這是更深層的
硬體問題，且格式化是冪等的、log 會明確指出，因此不另設重試上限。

---

### `GET /api/ota/status`

需要登入。回傳韌體更新的完整狀態快照。只在 Normal Mode 下註冊；AP Mode 沒有外網，
`OtaUpdater` 不會建立，此時回 `503 ota_unavailable`。

```json
{
  "ok": true,
  "check_state": "update_available",
  "update_state": "idle",
  "running_version": "0.6.0",
  "latest_version": "0.6.1",
  "running_partition": "app0",
  "progress_percent": 0,
  "image_size": 0,
  "bytes_read": 0,
  "last_http_status": 200,
  "message": "有新版本 0.6.1 可更新",
  "pending_verify": false
}
```

| 欄位 | 說明 |
|---|---|
| `check_state` | `unknown` / `checking` / `up_to_date` / `update_available` / `check_failed` |
| `update_state` | `idle` / `downloading` / `writing` / `ready_pending_reboot` / `failed` |
| `stage` | 更新期間指出正在處理哪一段：`firmware` 或 `webfs` |
| `last_outcome` | 最後一次更新的結果，存於 NVS 因此**跨重開機保留**。更新以重開機收尾，`message` 只存在約 2.5 秒，沒有這個欄位就查不到究竟發生了什麼 |
| `message` | 面向使用者的中文說明，**由韌體產生**。裝置可能處於「UI 比韌體舊」的狀態（webfs 寫入被跳過或失敗、或以 USB 燒入舊映像），舊版 UI 必須能顯示新韌體的狀態與錯誤，因此前端只做渲染、不自行組字串 |
| `pending_verify` | 目前執行的映像尚未確認。控制任務持續心跳滿 30 秒後自動確認；在此之前重新開機會回滾到前一個 slot |

此端點的 JSON 欄位**只增不改**——移除或改名會讓尚未以 USB 更新 web 資產的裝置顯示錯誤。

---

### `POST /api/ota/check`

需要登入與 CSRF token。向 GitHub Releases 查詢最新版本（`version.txt` 固定資產），
與韌體內的 `APP_VERSION` 做 SemVer 比對，只有**嚴格大於**才視為有更新。

成功回 `200 {"ok": true, "state": "checking"}`。實際結果由 `GET /api/ota/status` 查詢。

進行中回 `409 ota_busy`；未連上 WiFi 回 `503 offline`。

---

### `POST /api/ota/update`

需要登入與 CSRF token。單一動作依序處理韌體與網頁資源，完成後約 2.5 秒自動重新開機。
必須先呼叫 `/api/ota/check` 且結果為 `update_available`。

順序為**先韌體、後 webfs**：韌體失敗則整個更新中止且 webfs 未被觸碰；反過來做，失敗時會留下
「網頁比韌體新」的組合，而相容性契約只保證反向。webfs 僅在 `SHA256SUMS` 取得成功、且其雜湊
與 NVS 記錄的已安裝值不同時才會重寫。

**webfs 沒有備援分割區**，寫入是就地覆蓋，中途失敗會使靜態頁面損毀，需以
`pio run -e esp32dev --target uploadfs` 復原。此時 `/api/*` 仍可正常運作。

成功排入回 `202`：

```json
{"ok": true, "state": "queued"}
```

| 狀態 | 錯誤 | 條件 |
|---|---|---|
| 409 | `ota_busy` | 已有檢查或更新進行中 |
| 409 | `no_update_available` | 尚未檢查，或目前已是最新版 |
| 503 | `insufficient_memory` | 內部可用 heap 低於 60 KB。寧可拒絕，也不要在寫入途中 OOM 而留下半寫入的 slot |
| 503 | `offline` | 未連上 WiFi |

更新期間裝置會主動降載：暫停飲水事件判定、Cloud sync、Discord 通知與 MQTT 心跳
（詳見 `docs/architecture.md`）。失敗不影響目前執行的韌體，可直接重試。

---

## ConfigPortal（AP Mode）

AP Mode 下（WiFi 未設定或連線失敗）運行，固定位址 `192.168.4.1`。首次尚未設定管理
密碼時可直接設定 WiFi；已有管理密碼的 recovery AP 必須先登入，才能掃描或修改設定。

### `GET /`

提供 WiFi 設定頁面 HTML。

---

### `GET /api/status`

已有管理密碼時需要有效 session；首次 provisioning 可直接讀取。

**回應**

```json
{
  "ok": true,
  "mode": "ap",
  "ap_ssid": "WaterCupTracker-Setup",
  "ap_ip": "192.168.4.1"
}
```

---

### `POST /api/config`

需要 `X-CSRF-Token`；已有管理密碼時另外需要有效 session。

儲存 WiFi 憑證並觸發重啟。

**請求 Body**

```json
{
  "wifi_ssid": "MyWiFi",
  "wifi_password": "mypassword"
}
```

**回應**

```json
{"ok": true}
```

---

### `GET /api/wifi/scan`

掃描附近 WiFi 網路（格式同 DashboardServer）；已有管理密碼時需要有效 session。

---

### `POST /api/reboot`

重啟裝置（格式同 DashboardServer），需要 `X-CSRF-Token`；已有管理密碼時另外需要有效
session。

---

## curl 範例

以下 Normal Mode 範例假設已登入，`cookies.txt` 內有 `session` cookie，並將
`<session-csrf-token>` 替換成 `GET /api/auth/csrf` 回傳的 session token。

```bash
# 查看系統狀態
curl -b cookies.txt http://192.168.1.100/api/status

# 秤重歸零
curl -b cookies.txt -H "X-CSRF-Token: <session-csrf-token>" \
  -X POST http://192.168.1.100/api/tare

# 以 200g 物品校正
curl -b cookies.txt -H "X-CSRF-Token: <session-csrf-token>" \
  -X POST -H "Content-Type: application/json" \
  -d '{"known_weight_g": 200.0}' \
  http://192.168.1.100/api/calibrate

# 更新每日目標
curl -b cookies.txt -H "X-CSRF-Token: <session-csrf-token>" \
  -X POST -H "Content-Type: application/json" \
  -d '{"dailyGoalMl": 2500}' \
  http://192.168.1.100/api/config

# 查看本月記錄
curl -b cookies.txt "http://192.168.1.100/api/logs?month=2025-02"
```
