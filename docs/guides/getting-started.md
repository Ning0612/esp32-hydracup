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

```bash
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

```bash
# 建置韌體
pio run

# 燒錄韌體（含分割表）
pio run --target upload

# 燒錄 Web UI 靜態資源至 webfs
pio run --target uploadfs
```

驗證 `uploadfs` 目標位址正確：

```bash
pio run -v -t uploadfs | grep address
# 預期輸出：address 0x290000, size 0x60000
```

---

## 步驟 4：設定 WiFi

1. ESP32 上電後，蜂鳴器播放 `AP_MODE` 音效
2. 在手機或電腦的 WiFi 列表中找到 `WaterCupTracker-Setup`
3. 輸入密碼 `12345678` 連線
4. 開啟瀏覽器前往 `http://192.168.4.1`
5. 輸入您的 WiFi SSID 與密碼，點擊「Save & Reboot」
6. 裝置自動重啟並嘗試連線 WiFi

---

## 步驟 5：秤重歸零（Tare）

1. 開啟瀏覽器，前往 `http://<裝置 IP>`（IP 顯示於 OLED 頁 0）
2. 點選「Settings」→「Calibration」
3. **移除杯子**，確認秤台淨空
4. 點擊「Tare」，等待確認訊息
5. 放上一個已知重量（例如 200g 砝碼），點擊「Calibrate」並輸入精確重量

詳細校正流程見 [calibration.md](calibration.md)。

---

## 步驟 6：確認運作

1. OLED 頁 0 顯示 IP 位址與 WiFi 狀態
2. 開啟 `http://<ip>`，儀表板顯示目前重量與今日進度
3. 放上裝水的杯子，等待 3 秒後「Cup State」顯示 `stable`
4. 拿起杯子喝水後放回，數秒後應看到「Drink Confirmed」並更新今日飲水量
5. 歷史頁面（`/history`）查看月份記錄與年度熱力圖

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
