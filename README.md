# HydraCup

[![PlatformIO](https://img.shields.io/badge/PlatformIO-espressif32%406.10-orange?logo=platformio)](https://platformio.org)
[![ESP32](https://img.shields.io/badge/Board-ESP32-red?logo=espressif)](https://www.espressif.com/en/products/socs/esp32)
[![ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF-blue?logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)
[![Version](https://img.shields.io/badge/Version-0.3.0-brightgreen)](include/version.h)
[![CI](https://github.com/Ning0612/esp32-hydracup/actions/workflows/ci.yml/badge.svg)](https://github.com/Ning0612/esp32-hydracup/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

ESP32-based smart water cup tracker. Measures cup weight via HX711, detects drink events, sends Discord Webhook notifications, and provides a local web dashboard.

The firmware runs on native ESP-IDF/FreeRTOS through PlatformIO. Hardware access uses
ESP-IDF drivers; the web server, MQTT, Discord HTTPS client, NVS, and LittleFS are
all initialized from `app_main()` without the Arduino framework.

## Project Status

Personal side project for daily hydration tracking. It may continue to receive maintenance when new feature ideas or practical usage needs come up.

Real hardware:

<div align="center">
  <table align="center">
    <tr>
      <td align="center" valign="top">
        <strong>HydraCup real setup</strong><br>
        <img src="docs/demo/real-setup.jpg" alt="HydraCup real hardware setup" width="420">
      </td>
      <td align="center" valign="top">
        <strong>OLED status</strong><br>
        <img src="docs/demo/oled-status.jpg" alt="HydraCup OLED status on real hardware" width="420">
      </td>
    </tr>
  </table>
</div>

## Demo

<p align="center">
  <img src="docs/demo/demo-preview.gif" alt="HydraCup drink detection preview" width="560">
</p>

The GIF above is a compressed full-length preview; use the MP4 link below for the full-quality demo.

Full demo video:

- [Drink detection demo (MP4)](docs/demo/drink-detection-demo.mp4)

WebUI screenshots:

The local dashboard is served by the ESP32 at `http://<device-ip>`. The screenshots below cover the live dashboard, history analytics, and device configuration workflows.

<div align="center">
  <table align="center">
    <tr>
      <td colspan="2" align="center" valign="top">
        <strong>Dashboard overview</strong><br>
        <img src="docs/demo/webui/dashboard-overview.png" alt="HydraCup WebUI dashboard overview" width="640">
      </td>
    </tr>
    <tr>
      <td align="center" valign="top">
        <strong>History overview</strong><br>
        <img src="docs/demo/webui/history-overview.jpeg" alt="HydraCup WebUI history overview" width="360">
      </td>
      <td align="center" valign="top">
        <strong>History statistics</strong><br>
        <img src="docs/demo/webui/history-statistics.png" alt="HydraCup WebUI history statistics" width="360">
      </td>
    </tr>
    <tr>
      <td align="center" valign="top">
        <strong>Device settings</strong><br>
        <img src="docs/demo/webui/settings-overview.jpeg" alt="HydraCup WebUI device settings" width="440">
      </td>
      <td align="center" valign="top">
        <strong>Scale calibration</strong><br>
        <img src="docs/demo/webui/settings-calibration.png" alt="HydraCup WebUI scale calibration" width="640">
      </td>
    </tr>
  </table>
</div>

---

## Features

- **Automatic drink detection** — 6-state machine triggered only via cup-lift path, avoids false positives
- **Web dashboard** — real-time weight, daily progress, and drink history at `http://<device-ip>`
- **Discord Webhook** — online notification, per-drink notification, daily summary at midnight
- **JSONL event log** — monthly log files stored on a dedicated LittleFS partition (`/logs/`)
- **OLED display** — 2-page rotating status display with auto-sleep
- **Configurable reminders** — interval-based buzzer reminder when no drink detected
- **Captive config portal** — WiFi setup at `192.168.4.1` on first boot (no app needed)
- **FreeRTOS control isolation** — scale/detection/reminder/display run in one high-priority control task; Web, MQTT and Discord cannot stall sampling
- **Non-blocking tare and workers** — tare is sample-driven; Discord, MQTT and drink logging use persistent/background workers

---

## Hardware Requirements

| Component | Spec | GPIO |
|-----------|------|------|
| ESP32 dev board | 30-pin or 38-pin | — |
| HX711 load cell amplifier | 10 Hz or 80 Hz | DOUT→4, SCK→5 |
| Load cell / strain gauge | 1–5 kg range | via HX711 |
| SSD1306 OLED | 128×32, I2C | SDA→21, SCL→22 |
| Passive buzzer | 3.3 V compatible | PWM→18 |

See [docs/hardware.md](docs/hardware.md) for wiring details and BOM.

---

## Quick Start

**Prerequisites**: [PlatformIO](https://platformio.org/install/cli), Python 3.x, CP210x / CH340 USB driver

```bash
# 1. Clone
git clone https://github.com/<your-user>/esp32-hydracup.git
cd esp32-hydracup

# 2. Build firmware (ESP-IDF environment)
pio run -e esp32dev

# 3. Flash firmware + web assets (first time)
pio run -e esp32dev --target upload
pio run -e esp32dev --target uploadfs

# 4. If WiFi is not configured or the STA connection times out, the device
#    broadcasts "WaterCupTracker-Setup" AP. Connect and open http://192.168.4.1
#    Enter your WiFi credentials and save

# 5. After reboot, find the device IP and open http://<ip>
```

See [docs/guides/getting-started.md](docs/guides/getting-started.md) for the complete first-time setup walkthrough.

---

## Architecture Overview

```
┌─────────────────── Normal Mode ───────────────────────┐
│  hydracup_control (Core 1, 10 ms)                     │
│  ├─ ScaleManager → DrinkDetector                      │
│  ├─ Reminder / Buzzer / Display / Time                │
│  └─ RuntimeSnapshot + ControlCommand Queue             │
│                                                       │
│  hydracup_service (50 ms) → WiFi / MQTT / health      │
│  esp_http_server task → Dashboard / REST API           │
│  workers → Discord / MQTT / Log / counter NVS         │
└───────────────────────────────────────────────────────┘

┌────── AP Mode (no WiFi configured) ──────────────────┐
│  ConfigPortal  (HTTP at 192.168.4.1)                 │
└──────────────────────────────────────────────────────┘
```

---

## API Quick Reference

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Full system state snapshot |
| GET | `/api/weight` | Current weight + cup state |
| GET | `/api/config` | All settings |
| POST | `/api/config` | Update settings |
| POST | `/api/tare` | Tare scale |
| POST | `/api/calibrate` | Calibrate with known weight |
| GET | `/api/logs?month=YYYY-MM` | Drink history |
| GET | `/api/wifi/scan` | Scan nearby networks |
| POST | `/api/reboot` | Reboot device |

All responses: `{"ok": true, ...}` or `{"ok": false, "error": "..."}`.  
Full API reference: [docs/api.md](docs/api.md)

---

## Documentation

| Document | Description |
|----------|-------------|
| [docs/hardware.md](docs/hardware.md) | GPIO wiring and bill of materials |
| [docs/architecture.md](docs/architecture.md) | System architecture, boot flow, drink detection state machine |
| [docs/api.md](docs/api.md) | Complete REST API reference |
| [docs/modules.md](docs/modules.md) | Firmware module descriptions and public APIs |
| [docs/data-formats.md](docs/data-formats.md) | JSONL log format and NVS storage schema |
| [docs/guides/getting-started.md](docs/guides/getting-started.md) | First-time setup |
| [docs/guides/build-flash.md](docs/guides/build-flash.md) | Build and flash commands |
| [docs/guides/calibration.md](docs/guides/calibration.md) | Scale calibration |
| [docs/guides/configuration.md](docs/guides/configuration.md) | All settings explained |
| [docs/guides/discord-setup.md](docs/guides/discord-setup.md) | Discord Webhook setup |

## License

This public version is released under the MIT License. See [LICENSE](LICENSE).
