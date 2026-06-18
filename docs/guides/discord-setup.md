# Discord Webhook Setup

設定後，HydraCup 將自動發送三種通知至指定的 Discord 頻道：上線通知、飲水通知、每日摘要。

---

## 步驟 1：建立 Discord Webhook

1. 在 Discord 中開啟目標**伺服器**與**頻道**（例如 `#water-tracker`）
2. 點擊頻道右鍵 → **「編輯頻道」**
3. 左側選單選擇 **「整合」**
4. 點擊 **「Webhooks」** → **「建立 Webhook」**
5. 設定 Webhook 名稱（例如 `HydraCup`）
6. 點擊 **「複製 Webhook 網址」**
7. URL 格式如下：

```
https://discord.com/api/webhooks/1234567890/abcdefghijklmnopqrstuvwxyz0123456789
```

---

## 步驟 2：輸入 Webhook URL

### 透過 Web UI

1. 前往 `http://<裝置 IP>/settings`
2. 找到「Discord」區塊
3. 貼上複製的 Webhook URL
4. 點擊「Save」

### 透過 API

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"discordWebhookUrl": "https://discord.com/api/webhooks/..."}' \
  http://192.168.1.100/api/config
```

---

## 步驟 3：確認運作

1. 儲存設定後，`/api/status` 的 `webhook_configured` 欄位應為 `true`
2. 重啟裝置（`POST /api/reboot`），約 10 秒後 Discord 頻道應收到上線通知
3. 進行一次飲水動作，確認收到飲水通知

---

## URL 安全遮罩

`/api/config`（GET）回應中，Webhook URL 末尾 token 以 `****` 遮罩：

```
https://discord.com/api/webhooks/1234567890/****
```

`POST /api/config` 時，若 URL 含 `****` 則**跳過更新**，防止意外清除 token。

---

## 通知格式

### 上線通知

```
🟢 HydraCup Online
IP: 192.168.1.100
```

### 飲水通知

```
💧 Drink Detected
Amount: 230 ml
Today Total: 920 ml / 2000 ml (46%)
```

### 每日摘要（午夜 00:00）

```
📊 Daily Summary
Total: 1850 ml / 2000 ml (92.5%)
Drinks: 8 times
```

---

## 常見問題

| 問題 | 解決方法 |
|------|---------|
| `webhook_last_ok` 為 false | 確認 URL 正確、裝置可存取外網（ping discord.com） |
| 通知延遲 | Discord API 偶發限速，屬正常現象，裝置會自動重試 |
| Webhook URL 已遺失 | 回到 Discord 頻道「整合 → Webhooks」重新複製，或建立新 Webhook |
| 想關閉通知 | 將 `discordWebhookUrl` 清空並儲存，或刪除 Discord 端的 Webhook |
