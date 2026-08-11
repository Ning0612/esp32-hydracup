# Getting Started

首次使用 HydraCup 的完整設定流程。

---

## 前置需求

| 工具 | 安裝方式 |
|------|---------|
| [PlatformIO CLI](https://platformio.org/install/cli) | `pip install platformio` |
| Python 3.x | [python.org](https://www.python.org) |
| USB-UART 驅動 | CP2102：[Silicon Labs](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers)；CH340：[WCH](http://www.wch.cn/downloads/CH341SER_EXE.html) |
| Git | [git-scm.com](https://git-scm.com) |

---

## 步驟 1：取得程式碼

```powershell
git clone https://github.com/<your-user>/esp32-hydracup.git
cd esp32-hydracup
```

---

## 步驟 2：接線

依照 [hardware.md](../hardware.md) 將 HX711、OLED、蜂鳴器接至 ESP32 對應 GPIO。  
通電前確認：

- HX711 VCC → 3V3（不要接 5V，防止 OLED 損毀）
- 稱重感測器已正確固定在杯托底座
- USB-UART 線接妥

---

## 步驟 3：首次燒錄

```powershell
# 建置 ESP-IDF 韌體
pio run -e esp32dev

# 燒錄韌體、bootloader 與分割表
pio run -e esp32dev --target upload

# 燒錄 Web UI 靜態資源至 webfs
pio run -e esp32dev --target uploadfs
```

驗證 `uploadfs` 目標位址正確：

```bash
pio run -e esp32dev -v -t uploadfs | Select-String address
# 預期輸出：address 0x290000, size 0x60000
```

---

## 步驟 4：設定 WiFi

1. ESP32 上電後，若沒有可用 WiFi 設定或 STA 連線逾時，蜂鳴器播放 `AP_MODE` 音效
2. 在手機或電腦的 WiFi 列表中找到 `WaterCupTracker-Setup`
3. 輸入 AP 密碼 `12345678` 連線；這是設定 AP 的 WiFi 密碼，不是 admin 密碼
4. 開啟瀏覽器前往 `http://192.168.4.1`
5. 在設定頁掃描或輸入目標 WiFi 的 SSID 與密碼，點擊「儲存並重新啟動」
6. 裝置自動重啟並嘗試連線 WiFi
7. 由 OLED 或路由器查到裝置在區域網路的 IP，開啟 `http://<裝置 IP>/login`
8. 初次使用沒有預設 admin 密碼；在 `/login` 輸入一組至少 8 個字元的管理密碼並確認，完成後即可登入 WebUI

---

## 步驟 5：秤重歸零（Tare）

1. 開啟瀏覽器，前往 `http://<裝置 IP>`（IP 顯示於 OLED 頁 0）
2. 點選「Settings」→「Calibration」
3. **移除杯子**，確認秤台淨空
4. 點擊「Tare」，等待確認訊息
5. 放上一個已知重量（例如 200g 砝碼），點擊「Calibrate」並輸入精確重量

詳細校正流程見 [calibration.md](calibration.md)。

---

## 步驟 6：連接 LINE／LIFF 雲端服務（選用）

這個步驟需要 HydraCup firmware `v0.5.0` 以上、已部署的獨立 `hydracup-service`、已設定的裝置 allowlist，以及可用的
LINE Login／Messaging API channel。完整的使用與配對流程見
[HydraCup Service 使用指南](https://github.com/Ning0612/hydracup-service/blob/main/docs/user-guide.md)。

1. 登入 `http://<裝置 IP>/settings`
2. 在「Cloud sync」勾選「啟用 HTTPS 同步」
3. Service URL 只填 Pages origin，例如 `https://hydracup-service.pages.dev`；不要附加
   `/api/v1/device/sync`，也不要在前後留下空白
4. 確認裝置時區為 `UTC+8`，並核對唯讀的 Device ID 與 Device token SHA-256 已和 Cloudflare production allowlist 一致
5. 儲存設定並依提示重新啟動裝置
6. 回到裝置首頁「03 裝置狀態」，等待 Cloud sync 顯示 `正常 · 待送 0`，HTTP 顯示 `200`
7. 同一區的「WebUI 配對碼」會顯示 8 碼、10 分鐘有效的配對碼；完成 LINE 配對後該碼會消失
8. 若 Local 歷史早於啟用 Cloud sync，回到設定頁「06 Cloud sync」按「開始補傳」，等待
   顯示完成後重新開啟 LIFF「趨勢」

配對碼只顯示於登入後的本機 Device Console，不會顯示在 OLED。請勿分享管理密碼、
raw device token 或 Device token SHA-256。

---

## 步驟 7：確認運作

1. OLED 頁 0 顯示 IP 位址與 WiFi 狀態
2. 開啟 `http://<ip>`，儀表板顯示目前重量與今日進度
3. 放上裝水的杯子，等待 3 秒後「Cup State」顯示 `stable`
4. 拿起杯子喝水後放回，數秒後應看到「Drink Confirmed」並更新今日飲水量
5. 歷史頁面（`/history`）查看月份記錄與年度熱力圖

首頁「今日進度」會依完成比例改變水面、氣泡與滿杯效果；超過 100% 時以溢水呈現，
但 150% 以上不再增加動畫強度。有效飲水事件會短暫顯示落水、漣漪與本次增加的 ml；
點擊或用鍵盤啟用水杯只會播放晃水動畫並顯示進度摘要，不會新增或修改飲水紀錄。
作業系統啟用「減少動態效果」時，頁面會保留靜態狀態而停用主要動畫。

Local History 與 LIFF「趨勢」的年度熱力圖都預設顯示最近 365 天，並可用左右箭頭切換
指定完整年份；平年為 365 格、閏年為 366 格。Local 直接讀取裝置 LittleFS 月檔，LIFF
只顯示已同步的雲端事件與完成歷史補傳的每日摘要，因此兩邊在補傳完成前可能暫時不同。

---

## 常見問題

| 問題 | 排查方式 |
|------|---------|
| OLED 無顯示 | 確認 GPIO 21/22 接線與 I2C 位址（0x3C） |
| 重量一直跳動 | 調高 `stableToleranceGram` 或固定稱重平台 |
| 飲水未觸發 | 確認拿起杯子後放回的動作；調低 `minDrinkDeltaMl` |
| Discord 通知失敗 | 確認 Webhook URL 正確；確認 WiFi 可存取外網 |
| 找不到裝置 IP | 查看路由器 DHCP 清單，或觀察 OLED 頁 0 顯示 |
| NTP 未同步 | 確認 NTP 伺服器可達；先讓裝置連上有 DNS 的網路 |
| Cloud sync 顯示 `HTTP --` | 確認 Service URL 前後沒有空白且只填 HTTPS origin；儲存後重新啟動，等待至少 15 秒 |
| Cloud sync 顯示 HTTP 401 | 核對 Cloudflare production 的 `DEVICE_ID` 與 `DEVICE_TOKEN_HASH` 是否和 Device Console 完全一致 |
| 配對碼一直顯示等待產生 | 先確認 Cloud sync HTTP 200；配對碼只會在裝置尚未綁定且成功同步後出現 |
| 歷史補傳停在暫時失敗 | 會每 15 秒自動重試；先確認 Cloud sync HTTP 200，400 時需確認 service 已部署相容版本與 D1 migration |
