# REST API Reference

## 共通規則

- 所有端點回應 JSON
- 成功：`{"ok": true, ...}`
- 失敗：`{"ok": false, "error": "<message>"}`
- 無認證機制（局域網內使用）
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
| GET | `/style.css` | 共用樣式表 |
| GET | `/calibration` | 重定向至 `/settings#calibration` |

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

歸零由 TaskControl 收集 10 筆 tare 樣本，再等待 10 筆 warm-up 樣本。HTTP handler
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

## ConfigPortal（AP Mode）

AP Mode 下（WiFi 未設定或連線失敗）運行，固定位址 `192.168.4.1`。

### `GET /`

提供 WiFi 設定頁面 HTML。

---

### `GET /api/status`

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

掃描附近 WiFi 網路（格式同 DashboardServer）。

---

### `POST /api/reboot`

重啟裝置（格式同 DashboardServer）。

---

## curl 範例

```bash
# 查看系統狀態
curl http://192.168.1.100/api/status

# 秤重歸零
curl -X POST http://192.168.1.100/api/tare

# 以 200g 物品校正
curl -X POST -H "Content-Type: application/json" \
  -d '{"known_weight_g": 200.0}' \
  http://192.168.1.100/api/calibrate

# 更新每日目標
curl -X POST -H "Content-Type: application/json" \
  -d '{"dailyGoalMl": 2500}' \
  http://192.168.1.100/api/config

# 查看本月記錄
curl "http://192.168.1.100/api/logs?month=2025-02"
```
