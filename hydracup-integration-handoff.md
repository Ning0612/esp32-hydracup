# HydraCup 端整合交接文件

**這份文件的交付對象是 [esp32-hydracup](https://github.com/Ning0612/esp32-hydracup) 專案**，說明該韌體需要新增哪些程式碼才能與 `epaper-display` 完成 MQTT 整合。本次交付**只涵蓋規格與落地建議，不包含實際程式碼**——`esp32-hydracup` 是獨立 repo，需要在該專案內自行實作。

協議本體（topic、payload schema、QoS、發布時機）定義在 [hydracup-mqtt-protocol.md](hydracup-mqtt-protocol.md)，本文件不重複列出，只講「怎麼落地到 HydraCup 現有的程式碼結構」。

## 背景

`epaper-display`（Raspberry Pi e-Paper 儀表板）要新增一張卡片，顯示 HydraCup 的「今日喝水量/目標量/完成比例」。HydraCup 目前已有：
- WiFi 連線（`lib/WiFiManager`）
- 本機 REST API（`lib/DashboardServer`，`GET /api/status`、`/api/weight` 等）
- Discord webhook 通知（`lib/DiscordNotifier`，含背景任務、重試/backoff 邏輯）
- 喝水事件偵測狀態機（`lib/DrinkDetector`，`DRINK_CONFIRMED` / `REFILL_DETECTED` 狀態轉換）
- 每日目標值 `dailyGoalMl`（NVS `water_config` namespace，透過 `lib/ConfigManager` 管理，`/api/config` 曝露）

但**目前完全沒有 MQTT client**。本次整合需要新增一個 MQTT publisher 模組，讓 HydraCup 在偵測到喝水事件與定期心跳時，把資料推送到 Pi 上的 Mosquitto broker。

## 建議依賴套件

`platformio.ini` 的 `lib_deps` 新增一個 Arduino MQTT client。建議 `knolleary/PubSubClient`：輕量、與現有 `lib_deps`（`bogde/HX711`、`bblanchon/ArduinoJson` 等）風格一致，社群案例多。

**注意事項**：`PubSubClient` 預設封包大小上限僅 128～256 bytes（視版本而定）。本協議的 `hydracup/status` payload 實際大小約 100～150 bytes（視 `event`/`device_time` 欄位長度），理論上落在預設值內，但建議仍在 `platformio.ini` 的 `build_flags` 明確加大以留安全餘裕：

```ini
build_flags =
  -D MQTT_MAX_PACKET_SIZE=512
```

## 建議新模組：`lib/MqttPublisher`

比照現有 `lib/DiscordNotifier` 的獨立模組慣例（`.h`/`.cpp` 配對，自成一個單一職責模組），建議介面：

```cpp
// lib/MqttPublisher/MqttPublisher.h
class MqttPublisher {
public:
    void begin(const MqttConfig& config);   // 建立 WiFiClient + PubSubClient，設定 LWT，發起連線
    void loop();                             // 非阻塞，需在 src/main.cpp 主迴圈每次呼叫（處理重連、心跳計時）
    void publishStatus(uint32_t currentMl, uint32_t goalMl, const char* event);
    bool isConnected() const;
};
```

**`begin()` 內部要做的事**：
- 用 `WiFiClient` + `PubSubClient` 建立連線物件，設定 broker host/port（`MqttConfig`，見下方新增設定欄位）。
- 若 `username` 非空，呼叫對應的帳密登入 API（`PubSubClient::connect()` 的 overload 支援 `willTopic`/`willQos`/`willRetain`/`willMessage` 參數，一次把 LWT 也設好）：
  ```cpp
  mqttClient.connect(clientId, username, password,
                      "hydracup/availability", 1, true, "{\"online\":false}");
  ```
- 連線成功後立即發布一次 `{"online": true}`（retained）到 `hydracup/availability`，覆蓋上一次的 LWT 離線訊息（見協議文件的「發布時機」表）。

**`loop()` 內部要做的事**：
- 呼叫 `mqttClient.loop()` 維持連線（`PubSubClient` 需要定期呼叫才能處理 keep-alive 與重連）。
- 若距離上次發布超過 `mqttHeartbeatSec` 秒，呼叫 `publishStatus(currentMl, goalMl, "heartbeat")`。
- 斷線重連建議加入簡單的最小重試間隔（例如至少間隔 5 秒才重試一次 `connect()`），避免 WiFi 不穩時無限快速重試佔用 CPU。

**`publishStatus()` 內部要做的事**：
- 用 `ArduinoJson`（專案已依賴）組裝符合協議規格的 JSON payload（見 [hydracup-mqtt-protocol.md](hydracup-mqtt-protocol.md#hydracupstatus) 的欄位表）。
- `pct` 欄位可選擇性帶上（`currentMl / (float)goalMl`，`goalMl` 為 0 時省略此欄位讓 epaper-display 端自行判斷）。
- 以 `qos=1, retain=true` 發布到 `hydracup/status`。

## 需新增的設定欄位

掛在既有 `AppConfig`（`include/app_types.h`）與 `ConfigManager`（NVS `water_config` namespace），比照現有 `dailyGoalMl` 的持久化方式，並透過既有 `/api/config` REST 端點（GET/POST）與 `data/settings.html` 網頁曝露供使用者設定：

| 欄位 | 類型 | 預設值 | 說明 |
|------|------|--------|------|
| `mqttEnabled` | bool | `false` | 總開關，關閉時完全不建立 MQTT 連線 |
| `mqttBrokerHost` | string | `""` | Pi 的區網位址或 IP |
| `mqttBrokerPort` | uint16 | `1883` | Broker port |
| `mqttUsername` | string | `""` | Broker 登入帳號 |
| `mqttPassword` | string | `""` | Broker 登入密碼 |
| `mqttClientId` | string | `"hydracup-device"` | 建議含裝置序號以利多裝置除錯 |
| `mqttHeartbeatSec` | uint16 | `60` | 心跳間隔（秒），需與 epaper-display 端的 `heartbeat_timeout_sec`（預設 180 = 心跳的 3 倍）搭配設定，避免誤判過期 |

## 掛載點（Hook Points）

- **`lib/DrinkDetector`** 狀態機：在 `DRINK_CONFIRMED` 與 `REFILL_DETECTED` 這兩個狀態轉換發生時，呼叫 `mqttPublisher.publishStatus(currentMl, dailyGoalMl, event)`（`event` 對應傳入 `"drink"` 或 `"refill"`）。確切呼叫位置比照現有 `DiscordNotifier` 在相同狀態轉換點的呼叫方式（兩者是同一組事件來源，只是通知管道不同）。
- **`src/main.cpp`** 主迴圈（`loop()` 函式）：新增 `mqttPublisher.loop()` 呼叫，與現有 `wifiManager`/`dashboardServer` 等模組的呼叫並列。`src/main.cpp` 依專案既有設計原則只做編排（thin orchestrator），不寫業務邏輯，此次新增呼叫需維持此慣例。
- **`lib/DisplayManager`**（OLED，選配）：若希望在裝置本機 OLED 上也顯示 MQTT 連線狀態（例如一個小圖示），可參考現有 WiFi 連線狀態圖示的呈現方式；此項為選配，不影響 epaper-display 端功能。

## 測試建議

實作完成後，可用 `mosquitto_sub` 在另一台機器上獨立驗證（不需要 epaper-display 端上線）：

```bash
mosquitto_sub -h <Pi-IP> -p 1883 -u hydracup-device -P <password> -t 'hydracup/#' -v
```

觸發一次真實喝水/加水動作，確認：
1. `hydracup/status` 有收到對應 `event` 的訊息，欄位符合協議 schema。
2. 斷開裝置 WiFi（模擬異常斷線），確認 `hydracup/availability` 在數秒內收到 `{"online": false}`（驗證 LWT 生效）。
3. 重新連線後，`hydracup/availability` 立即收到 `{"online": true}`。
4. 靜置超過 `mqttHeartbeatSec` 秒但不觸發喝水事件，確認仍會收到 `event: "heartbeat"` 的 `hydracup/status`。

完整協議欄位定義與 QoS/retained 規則請見 [hydracup-mqtt-protocol.md](hydracup-mqtt-protocol.md)。
