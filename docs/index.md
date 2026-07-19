# HydraCup Documentation

ESP32 智慧水杯追蹤器完整文件索引。

---

## 硬體與設定

- [hardware.md](hardware.md) — GPIO 接線圖、元件清單（BOM）

## 系統架構

- [architecture.md](architecture.md) — 模組架構、資料流、開機流程、飲水偵測狀態機

## API 參考

- [api.md](api.md) — 完整 REST API 端點（DashboardServer + ConfigPortal）

## 模組文件

- [modules.md](modules.md) — 18 個模組的責任、依賴與公開方法

## 資料格式

- [data-formats.md](data-formats.md) — JSONL 日誌格式、NVS 儲存 Schema

## 操作指南

- [guides/getting-started.md](guides/getting-started.md) — 首次使用設定流程
- [guides/build-flash.md](guides/build-flash.md) — 建置與燒錄指令
- [guides/calibration.md](guides/calibration.md) — 秤重校正步驟
- [guides/configuration.md](guides/configuration.md) — 所有設定說明
- [guides/discord-setup.md](guides/discord-setup.md) — Discord Webhook 設定

## 版本紀錄

- [releases/v0.3.0.md](releases/v0.3.0.md) — WebUI 風格重設計與展示素材更新
- [releases/v0.2.0.md](releases/v0.2.0.md) — FreeRTOS runtime 與背景 worker 更新
