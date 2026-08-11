# Configuration

所有設定均可透過登入後的 `http://<ip>/settings` 頁面或帶有效 session／CSRF token 的
`POST /api/config` 修改，並持久化至 NVS。首次進入 Normal Mode 時，`/login` 會要求設定
至少 8 個字元的管理密碼；密碼只以 PBKDF2-HMAC-SHA256 雜湊儲存。

Normal Mode 使用伺服器端單一 session slot：新登入會取代舊 session，閒置 30 分鐘或登入後
24 小時自動失效；裝置重開機也會讓 session 失效。管理介面目前以 HTTP 提供，因此請將它
限制在信任的隔離 LAN，避免同網段攻擊者攔截並重放 token。

設定頁第一層只顯示日常會調整的「飲水與提醒」及「蜂鳴器」。感測器、WiFi、NTP、Cloud
sync 與 MQTT 收在「網路、感測器與整合」進階區；秤重校正仍是獨立區塊，避免誤把校正操作
當成一般設定。

---

## 飲水偵測

| 設定 | API 欄位 | 預設值 | 有效範圍 | 說明 |
|------|---------|-------|---------|------|
| 杯子偵測閾值 | `cupPresentThresholdGram` | 80.0 g | 10–500 g | 低於此重量視為無杯子 |
| 穩定誤差 | `stableToleranceGram` | 3.0 g | 0.5–20 g | 重量允許波動範圍 |
| 穩定持續時間 | `stableDurationMs` | 3000 ms | 500–10000 ms | 判定穩定需持續的時間 |
| 最小飲水量 | `minDrinkDeltaMl` | 20.0 ml | 5–100 ml | 低於此量不記錄為飲水 |
| 最大飲水量 | `maxDrinkDeltaMl` | 500.0 ml | 50–1000 ml | 超過此量不記錄（防誤判） |

**調整建議**：
- 若常出現「沒喝卻觸發」→ 調高 `stableToleranceGram` 或調高 `minDrinkDeltaMl`
- 若喝水後未觸發 → 調低 `minDrinkDeltaMl`
- 若穩定等待太久 → 調低 `stableDurationMs`（建議不低於 1500 ms）

### 為什麼預設是這些值？

HX711 讀值先經過 10 筆 moving average；系統每 100 ms 取樣一次，因此窗口約涵蓋
1 秒。這能降低單次讀值抖動，又不會讓拿起或放回杯子的反應延遲太久。

`stableToleranceGram` 預設 3 g，用來容許秤台與 load cell 的微幅噪聲；
`stableDurationMs` 預設 3000 ms，要求重量在容許範圍內持續 3 秒，避免短暫碰撞或
放杯回彈被誤判為穩定。實際安裝若振動較大，可提高容許誤差或延長時間；若需要更
快的反應，可反向調整，但必須以實機流程確認誤判率。

---

## 每日目標與提醒

| 設定 | API 欄位 | 預設值 | 有效範圍 | 說明 |
|------|---------|-------|---------|------|
| 每日飲水目標 | `dailyGoalMl` | 2000 ml | 100–9999 ml | 顯示於儀表板進度條 |
| 啟用提醒 | `reminderEnabled` | true | — | 開啟/關閉提醒蜂鳴器 |
| 提醒間隔 | `reminderIntervalMin` | 60 min | 1–1440 min | 多久沒喝水就提醒 |
| 提醒持續時間 | `reminderAlertTimeoutSec` | 60 sec | 5–3600 sec | 提醒蜂鳴最長持續時間 |

---

## 蜂鳴器

| 設定 | API 欄位 | 預設值 | 有效範圍 | 說明 |
|------|---------|-------|---------|------|
| 啟用蜂鳴器 | `buzzerEnabled` | true | — | 開啟/關閉所有蜂鳴 |
| 頻率 | `buzzerFrequencyHz` | 2000 Hz | 500–5000 Hz | 蜂鳴音調（無源蜂鳴器） |
| 時長 | `buzzerDurationMs` | 150 ms | 50–2000 ms | 單次蜂鳴時長 |
| 音量 | `buzzerVolumePercent` | 50% | 0–100% | LEDC PWM 占空比控制音量 |

---

## NTP 時間同步

| 設定 | API 欄位 | 預設值 | 說明 |
|------|---------|-------|------|
| 啟用 NTP | `ntpEnabled` | true | 關閉後時間戳將無效 |
| NTP 伺服器 1 | `ntpServer1` | `"pool.ntp.org"` | 主要 NTP 伺服器 |
| NTP 伺服器 2 | `ntpServer2` | `"time.google.com"` | 備用 NTP 伺服器 |
| 時區標籤 | `timezone` | `"Asia/Taipei"` | 顯示與儲存用的說明文字 |

**時區設定**：`timezone` 欄位僅作為說明性標籤儲存於 NVS，**不會自動影響時區偏移**。
實際的 UTC 偏移由 `timezoneOffsetSec`（預設 +28800，即 UTC+8）與
`daylightOffsetSec`（預設 0）決定，`TimeManager` 會透過 ESP-IDF `esp_sntp_*` API
同步時間，並以 `TZ` 環境設定產生本地時間。若 NTP 未同步，飲水記錄的時間戳會以
`"boot+<ms>ms"` 格式儲存，日誌存在 `drink-unsynced.jsonl`。

---

## WiFi

| 設定 | API 欄位 | 說明 |
|------|---------|------|
| SSID | `wifiSsid` | 家庭 WiFi 名稱 |
| 密碼 | `wifiPassword` | WiFi 密碼（API 回應中以 `****` 顯示） |

> WiFi 設定異動後裝置會自動重啟。

---

## Discord Webhook

| 設定 | API 欄位 | 說明 |
|------|---------|------|
| Webhook URL | `discordWebhookUrl` | 完整 Discord Webhook URL（API 回應中末尾以 `****` 遮罩） |

Discord 通知類型：
- **上線通知**：裝置連上 WiFi 後發送，含 IP 位址
- **飲水通知**：每次 `DRINK_CONFIRMED` 後發送，含飲水量與今日進度
- **每日摘要**：午夜 00:00 發送，含當日總量與達成率

設定步驟見 [discord-setup.md](discord-setup.md)。

---

## Cloud sync

Cloud sync 將 ESP32 已確認的飲水／補水事件透過 HTTPS durable outbox 同步至獨立的
HydraCup Service。ESP32 仍是飲水事件與提醒狀態的唯一真實來源，雲端 WebUI 不提供
手動新增飲水量。目前 service protocol v1 的驗證組合需要 HydraCup firmware `v0.5.0`
以上；若任一端修改 schema、history backfill 或 command／ACK 語意，必須同步升級兩個 repo。

| 設定 | API 欄位 | 說明 |
|------|---------|------|
| 啟用 HTTPS 同步 | `cloudEnabled` | 開啟裝置背景同步；變更後需重新啟動 |
| Service URL | `cloudBaseUrl` | 只填 HTTPS origin，例如 `https://hydracup-service.pages.dev` |
| Device ID | `cloudDeviceId` | 首次啟動由裝置依 MAC 產生；WebUI 唯讀顯示 |
| Device token SHA-256 | `cloudDeviceTokenHash` | 供 Cloudflare allowlist 使用；不是 raw token，WebUI 唯讀顯示 |

Service v1 的日期邊界固定為 Asia/Taipei，因此啟用 Cloud sync 時裝置時區必須是 `UTC+8`；
其他時區會被設定 API 拒絕，既有非 UTC+8 設定也不會開始同步。這可確保即時事件、Local
月檔與 LIFF 熱力圖使用同一天界。

WebUI 與 `POST /api/config` 都會移除 Service URL 前後空白與結尾斜線，並只接受純 HTTPS
origin。不可附加 `/api/v1/device/sync`、其他 path、query、fragment 或帳密；不合法值會在
套用其他設定前回傳 `400`。背景 worker 每 15 秒同步一次；新事件或 command ACK 會要求
提早同步。登入後的裝置首頁「03 裝置狀態」會顯示：

- `正常 · 待送 0`／HTTP `200`：最近一次同步成功，durable outbox 已送完。
- `待送 N`：尚有事件等待 server ACK；短暫離線時屬正常現象。
- HTTP `401`：Device ID 或 token hash 與部署環境 allowlist 不一致。
- HTTP `--`：尚未送出，或 HTTPS 在取得 HTTP status 前失敗；先檢查 URL 空白、DNS、網路與 TLS 時間。
- `儲存失敗 N 筆`：LittleFS outbox 與 NVS emergency overflow 都無法保存事件，需立即排查儲存空間。

裝置未綁定時，第一次成功同步會在同區顯示 8 碼、10 分鐘有效的「WebUI 配對碼」。
配對完成後 server 回傳 `deviceBound=true`，裝置會清除顯示中的配對碼。完整 LINE／LIFF
使用流程見
[HydraCup Service 使用指南](https://github.com/Ning0612/hydracup-service/blob/main/docs/user-guide.md)。

Raw device token 只存在 ESP32 NVS，不會顯示在 WebUI，也不應放入 Cloudflare、repo、文件
或訊息。Device Console 目前使用 LAN HTTP，請只在信任的隔離網路操作。

### 補傳 Local 歷史到 LIFF

Cloud sync 已正常連線後，在設定頁「06 Cloud sync」按「開始補傳」。背景 worker 每批處理
一個月份，把 LittleFS 中今天以前的每日摘要送往 `/api/v1/device/history-backfill`；原始
token 不會交給瀏覽器。service 確認後才持久化 `hist_cursor`，所以離線、5xx 或重開機後會
重送相同月份，不會跳過資料，也不會重複累加。完成後可再次執行，補上前次執行時仍屬今日
的資料。游標會綁定 Service origin 與裝置身分；身分變更後從頭安全重送。月檔以短批次
持有 filesystem lock、在鎖外解析，避免阻塞新的飲水日誌。

狀態為 `retrying` 時裝置每 15 秒自動重試。HTTP 400 通常表示 firmware 與 service 協定
版本不一致；401 是裝置身分不符；410 是雲端帳號刪除後的停用 tombstone。

---

## AP 模式設定

| 設定 | API 欄位 | 預設值 | 說明 |
|------|---------|-------|------|
| AP SSID | `apSsid` | `WaterCupTracker-Setup` | AP Mode 廣播的 WiFi 名稱 |
| AP 密碼 | `apPassword` | `12345678` | AP 連線密碼 |

---

## curl 批次設定範例

以下假設已登入，`cookies.txt` 內有 `session` cookie，並已取得 session CSRF token。

```bash
curl -b cookies.txt -H "X-CSRF-Token: <session-csrf-token>" \
  -X POST -H "Content-Type: application/json" \
  -d '{
    "dailyGoalMl": 2500,
    "reminderEnabled": true,
    "reminderIntervalMin": 45,
    "buzzerVolumePercent": 70,
    "ntpServer1": "pool.ntp.org",
    "timezone": "Asia/Taipei"
  }' \
  http://192.168.1.100/api/config
```
