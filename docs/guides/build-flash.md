# Build & Flash

## 開發環境

HydraCup 使用 PlatformIO 的兩個 environment：

- `esp32dev`：ESP32 + native ESP-IDF/FreeRTOS firmware，`framework = espidf`
- `native`：主機端 C++17 + Unity 測試，執行不依賴硬體的 `DrinkDetectorCore` 與 `ReminderCore` 測試

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

`native` environment 明確設定 `test_build_src = no`，只建置測試引用的純 C++ core；不要把
`src/main.cpp` 或 FreeRTOS／ESP-IDF 硬體模組加入 native test build。

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

## OTA 韌體更新

v0.6.0 起，Normal Mode 可從 GitHub Releases 直接更新韌體（設定頁「08 韌體更新」，
API 見 `docs/api.md`）。

**首次啟用需要一次 USB 燒錄**，之後才能改用 OTA：

```powershell
pio run -e esp32dev --target upload     # bootloader + partitions + otadata + firmware
pio run -e esp32dev --target uploadfs   # webfs（設定頁的韌體更新卡片）
```

`upload` 這一步不可省略成「只寫 app 分割區」：rollback（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）
是 **bootloader** 的行為，只有重燒 bootloader 才會生效。若沒重燒就跑新韌體，開機會把
`ota_state` 設成 PENDING_VERIFY 而舊 bootloader 不認得——不會變磚，但回滾功能會靜默失效。

**OTA 不會更新 `webfs`。** app slot 之外的分割區都不在 OTA 範圍內，所以設定頁本身改版時
仍需 `uploadfs`。為了讓舊版 UI 能顯示新韌體的狀態，`/api/ota/status` 的欄位只增不改，
且所有面向使用者的文字都由韌體回傳。

`sdkconfig.defaults` 中與 OTA 直接相關的設定：

| 設定 | 值 | 原因 |
|---|---|---|
| `CONFIG_LWIP_MAX_SOCKETS` | 16 | GitHub 會 302 轉址到 `objects.githubusercontent.com`，下載期間需要 2–3 條連線；預設 10 不夠 |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` + `_DEFAULT_CMN` | y | OTA 不能釘選根憑證（見 `docs/architecture.md`）。CMN 子集約 18 KB，全量約 67 KB |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | y | 新韌體若在 30 秒內崩潰，bootloader 自動回滾到前一個 slot |

> **改 `sdkconfig.defaults` 後必須刪掉 `sdkconfig.esp32dev` 再建置**，否則新值會被既有檔
> 蓋過而靜默失效。詳見 `CLAUDE.md` 的 Known Environment Constraints。

`[env:esp32dev-debug]` 是同一份韌體以 `-Og` 建置的除錯環境（`scripts/sdkconfig_debug_defaults.py`
會把 `sdkconfig.debug.defaults` 疊在 `sdkconfig.defaults` 之上）。它**不可用於發佈或 OTA**。

## ESP-IDF 相依元件

專案不再使用 Arduino `lib_deps`。韌體相依元件由 `src/CMakeLists.txt` 與
`src/idf_component.yml` 管理，包含：

- ESP-IDF：`driver`、`esp_event`、`esp_netif`、`esp_wifi`、`esp_http_server`、
  `esp_http_client`、`esp_https_ota`、`esp-tls`、`app_update`、`mqtt`、`nvs_flash`、
  `json`、`mbedtls`、FreeRTOS 等
- registry component：`joltwallet/littlefs`（`>=1.14.6,<2.0.0`）

不要再安裝或新增 `bogde/HX711`、`ArduinoJson`、`Adafruit SSD1306`、`Adafruit GFX`
等 Arduino library；目前硬體驅動與 JSON 處理使用 ESP-IDF API。
