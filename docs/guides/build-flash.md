# Build & Flash

## PlatformIO 指令

> **Windows Bash 工具注意**：`pio` 未在 Bash PATH 中，請使用完整路徑：  
> `~/.platformio/penv/Scripts/pio.exe`（PowerShell：`$env:USERPROFILE\.platformio\penv\Scripts\pio.exe`）

## CI

GitHub Actions 會在 push、pull request 與手動 workflow dispatch 時執行
`.github/workflows/ci.yml`：

- 安裝 PlatformIO
- 執行 `pio run -e esp32dev`

CI 只驗證韌體可建置，不會燒錄裝置、上傳 LittleFS、連接 HX711/OLED，
也不會執行硬體或 Discord/MQTT 整合測試。

### 常用指令

```bash
# 建置韌體
pio run

# 燒錄韌體（含分割表）
pio run --target upload

# 燒錄 Web UI 靜態資源（data/ → webfs 分割區）
pio run --target uploadfs

# 開啟串列監控（115200 baud）
pio device monitor --baud 115200

# 建置 + 燒錄 + 監控（一鍵流程）
pio run --target upload && pio device monitor --baud 115200

# 清除建置快取
pio run --target clean
```

---

## 燒錄速率

`platformio.ini` 設定 `upload_speed = 921600`（快速燒錄）。  
若燒錄不穩定，可暫時降為 `460800` 或 `115200`。

---

## uploadfs 位址驗證

確認 `uploadfs` 目標正確寫入 `webfs` 分割區（0x290000）：

```bash
pio run -v -t uploadfs | grep address
# 預期輸出：address 0x290000, size 0x60000
```

若位址不符，請確認 `partitions.csv` 未被修改。

---

## 首次部署後的分割表變更

若分割表（`partitions.csv`）有異動，需完整重刷：

1. `pio run --target upload` — 燒錄韌體 + 新分割表（舊資料會被清除）
2. `pio run --target uploadfs` — 燒錄 Web 資源至 webfs
3. `logfs` 在首次開機時自動格式化，**不需手動操作**

> 分割表變更會清除 NVS 的所有設定（WiFi、AppConfig）。重刷後需重新設定 WiFi 與其他參數。

---

## 雙分割區說明

| 分割區 | 標籤 | 偏移 | 大小 | 用途 |
|-------|------|------|------|------|
| webfs | spiffs | 0x290000 | 384 KB | 靜態 Web 資源（`uploadfs` 目標） |
| logfs | — | 0x2F0000 | 1 MB | 飲水事件 JSONL 日誌（不受 `uploadfs` 影響） |

`uploadfs` 只寫 webfs，不影響 logfs 中的歷史飲水記錄。

---

## 建置旗標

`platformio.ini` 中的關鍵設定：

```ini
build_flags =
    -D CORE_DEBUG_LEVEL=3    # 啟用詳細 Serial 輸出（除錯用）
    -I include               # 讓 lib/ 找到 include/ 下的標頭
```

生產環境可將 `CORE_DEBUG_LEVEL` 降為 `1` 或 `0` 以減少輸出。

---

## 相依函式庫

```ini
lib_deps =
    bogde/HX711                    # 稱重 IC 驅動
    bblanchon/ArduinoJson          # JSON 序列化
    adafruit/Adafruit SSD1306      # OLED 驅動
    adafruit/Adafruit GFX Library  # 繪圖底層庫
```

PlatformIO 會在首次建置時自動下載。若在離線環境使用，需先執行 `pio pkg install`。
