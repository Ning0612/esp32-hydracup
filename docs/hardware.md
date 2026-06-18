# Hardware

## GPIO 腳位

所有腳位定義在 `include/pins.h`，原始碼中不得使用裸數字。

| 腳位名稱 | GPIO | 方向 | 說明 |
|---------|------|------|------|
| `PIN_HX711_DOUT` | 4 | 輸入 | HX711 資料輸出（Data Out） |
| `PIN_HX711_SCK` | 5 | 輸出 | HX711 時脈（Serial Clock） |
| `PIN_OLED_SDA` | 21 | 雙向 | I2C 資料線（SSD1306 OLED） |
| `PIN_OLED_SCL` | 22 | 輸出 | I2C 時脈線（SSD1306 OLED） |
| `PIN_BUZZER` | 18 | 輸出 | LEDC PWM 蜂鳴器輸出 |

## 接線說明

### HX711 ↔ ESP32

```
HX711       ESP32
------      ------
VCC    →    3V3
GND    →    GND
DOUT   →    GPIO 4
SCK    →    GPIO 5
```

HX711 的 E+ / E- / A+ / A- 接稱重感測器（4線橋式）。  
使用 80 Hz 輸出速率時，將 RATE 腳位接高電位（3.3V）；使用 10 Hz 則接低或懸空。

### SSD1306 OLED ↔ ESP32

```
OLED        ESP32
------      ------
VCC    →    3V3
GND    →    GND
SDA    →    GPIO 21
SCL    →    GPIO 22
```

I2C 位址：`0x3C`，解析度：128×32。

### 蜂鳴器 ↔ ESP32

```
Buzzer      ESP32
------      ------
+      →    GPIO 18  (via 100Ω resistor recommended)
-      →    GND
```

使用無源蜂鳴器（Passive Buzzer）；有源蜂鳴器無法由 LEDC PWM 控制音調。

## 電源需求

| 供電 | 說明 |
|------|------|
| 5V USB | ESP32 板子 micro-USB 供電 |
| 3.3V | HX711、OLED、蜂鳴器（由 ESP32 板子 3V3 引腳供電） |

若使用超過 5 kg 的感測器或 HX711 增益 128 以外的設定，請確認感測器 E+ 供電穩定。

---

## 元件清單（BOM）

| 元件 | 規格 | 備注 |
|------|------|------|
| ESP32 Dev Board | 38-pin，帶 USB-UART（CP2102 或 CH340） | 任何標準 esp32dev 相容板皆可 |
| HX711 模組 | 10/80 Hz，24-bit ADC | 搭配 4線橋式稱重感測器 |
| 稱重感測器 | 1–5 kg，橋式應變計 | 建議搭配杯架固定 |
| SSD1306 OLED | 128×32，I2C，0.91 吋 | 0x3C 位址 |
| 無源蜂鳴器 | 3.3V，2 kHz | 不可用有源蜂鳴器 |
| 杯托 / 底座 | 平面壓力均勻分布 | 可 3D 列印 |
| 跳線 | 公對公 / 公對母 | 接麵包板或焊接 |
| 麵包板 / PCB | 830孔麵包板 或 洞洞板 | 原型用途 |

### 台灣採購建議

- ESP32 / HX711 / SSD1306：台灣各大網路電子材料商（露天、蝦皮、飆機器人）
- 稱重感測器：「應變計稱重」or「load cell 台灣」搜尋
