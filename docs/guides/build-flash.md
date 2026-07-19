# Build & Flash

## 開發環境

HydraCup 使用 PlatformIO 的兩個 environment：

- `esp32dev`：ESP32 + native ESP-IDF/FreeRTOS firmware，`framework = espidf`
- `native`：主機端 C++17 + Unity 測試，執行不依賴硬體的 `DrinkDetectorCore` 測試

目前 PlatformIO environment 設定為 `espressif32@~6.10.0`，由 PlatformIO 管理對應的
ESP-IDF toolchain。若 Windows 的 `pio` 不在 PATH，可使用目前機器的完整路徑：
`C:\Users\Ning\.platformio\penv\Scripts\pio.exe`；其他電腦請替換使用者名稱。

## CI

GitHub Actions 會在 push、pull request 與手動 workflow dispatch 時執行
`.github/workflows/ci.yml`：

- 建置 `pio run -e esp32dev`
- 執行 `pio test -e native`

CI 不會燒錄裝置、上傳 Web 資源、連接 HX711/OLED，也不會執行硬體或 Discord/MQTT
整合測試。

## 常用指令

以下指令在 repository 根目錄執行。PowerShell 請逐行執行，不要把多個命令串在同一行。

```powershell
# 建置 ESP-IDF firmware
pio run -e esp32dev

# 執行 native host-side tests
pio test -e native

# 清除 ESP32 建置快取
pio run -e esp32dev --target clean

# 燒錄 firmware、bootloader 與 partition table
pio run -e esp32dev --target upload --upload-port COM5

# 燒錄 data/ 到 webfs 分割區
pio run -e esp32dev --target uploadfs --upload-port COM5

# 開啟 Serial monitor
pio device monitor -e esp32dev --port COM5 --baud 115200
```

`upload` 與 `uploadfs` 是兩個不同步驟：firmware 不會自動包含 `data/` 靜態資源。
若 `pio` 不在 PATH，可將上述命令中的 `pio` 替換為 PlatformIO 虛擬環境內的執行檔。

## 燒錄速率

`platformio.ini` 設定 `upload_speed = 921600`，monitor baud 為 `115200`。若燒錄不穩定，
可暫時將 `upload_speed` 降為 `460800` 或 `115200`。

## uploadfs 位址驗證

`board_build.filesystem = littlefs` 會將 `data/` 寫入第一個符合 PlatformIO filesystem
目標的資料分割區，即 `webfs`（`0x290000`、`0x60000` bytes）。PowerShell 可用：

```powershell
pio run -e esp32dev -v --target uploadfs | Select-String address
```

預期位址為 `0x290000`。若位址不符，先確認 `partitions.csv` 與 `platformio.ini` 沒有被
修改。

## 分割區與資料保護

| 分割區 | 偏移 | 大小 | 用途 |
|-------|------|------|------|
| `nvs` | `0x9000` | 20 KB | WiFi、AppConfig、管理密碼雜湊 |
| `app0` / `app1` | `0x10000` / `0x150000` | 各 1.25 MB | OTA firmware slot |
| `webfs` | `0x290000` | 384 KB | `/webfs` 靜態 Web 資源 |
| `logfs` | `0x2F0000` | 1 MB | `/logfs/logs/` JSONL 飲水日誌 |

- `uploadfs` 只寫入 `webfs`，不會影響 `logfs` 歷史日誌。
- firmware 以 `esp_vfs_littlefs_register()` 掛載 `/webfs` 與 `/logfs`。
- `logfs` 使用 `format_if_mount_failed = false`；mount 失敗時不會自動格式化或清除資料。
- 完整 erase 或變更分割區位置可能使 NVS、Web 資源與日誌失效；執行前應先備份需要保留的資料。

## ESP-IDF 相依元件

專案不再使用 Arduino `lib_deps`。韌體相依元件由 `src/CMakeLists.txt` 與
`src/idf_component.yml` 管理，包含：

- ESP-IDF：`driver`、`esp_event`、`esp_netif`、`esp_wifi`、`esp_http_server`、
  `esp_http_client`、`mqtt`、`nvs_flash`、`json`、`mbedtls`、FreeRTOS 等
- registry component：`joltwallet/littlefs`（`>=1.14.6,<2.0.0`）

不要再安裝或新增 `bogde/HX711`、`ArduinoJson`、`Adafruit SSD1306`、`Adafruit GFX`
等 Arduino library；目前硬體驅動與 JSON 處理使用 ESP-IDF API。
