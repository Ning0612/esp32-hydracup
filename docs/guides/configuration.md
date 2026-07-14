# Configuration

所有設定均可透過登入後的 `http://<ip>/settings` 頁面或帶有效 session／CSRF token 的
`POST /api/config` 修改，並持久化至 NVS。首次進入 Normal Mode 時，`/login` 會要求設定
至少 8 個字元的管理密碼；密碼只以 PBKDF2-HMAC-SHA256 雜湊儲存。

Normal Mode 使用伺服器端單一 session slot：新登入會取代舊 session，閒置 30 分鐘或登入後
24 小時自動失效；裝置重開機也會讓 session 失效。管理介面目前以 HTTP 提供，因此請將它
限制在信任的隔離 LAN，避免同網段攻擊者攔截並重放 token。

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
| 時區 | `timezone` | `"Asia/Taipei"` | POSIX 時區字串 |

**時區設定**：`timezone` 欄位僅作為說明性標籤儲存於 NVS，**不會自動影響時區偏移**。實際的 UTC 偏移由 `timezoneOffsetSec`（預設 +28800，即 UTC+8）與 `daylightOffsetSec`（預設 0）決定，傳入 `configTime()` 生效。若需調整時區，請同步修改 `timezoneOffsetSec`。若 NTP 未同步，飲水記錄的時間戳會以 `"boot+<ms>ms"` 格式儲存，日誌存在 `drink-unsynced.jsonl`。

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
