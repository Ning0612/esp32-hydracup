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

## LINE／LIFF 雲端服務

- [HydraCup Service 使用指南](https://github.com/Ning0612/hydracup-service/blob/main/docs/user-guide.md)（`hydracup-service` 為私有 repo，需有存取權限） — Cloud sync、配對、歷史補傳與 LIFF 操作
- [HydraCup Service v1 協定](https://github.com/Ning0612/hydracup-service/blob/main/docs/protocol.md)（`hydracup-service` 為私有 repo，需有存取權限） — 裝置同步、command／ACK 與歷史查詢合約

韌體 `v0.5.0` 與 service protocol v1 是目前驗證組合。ESP32 保留 Local 月檔並作為飲水事件與
提醒狀態的唯一真實來源；LIFF 顯示已同步事件及使用者主動執行的歷史補傳資料。

## 版本紀錄

- [releases/v0.5.0.md](releases/v0.5.0.md) — 杯況感知提醒、統一 WebUI、LINE／LIFF 同步與歷史補傳
- [releases/v0.4.0.md](releases/v0.4.0.md) — 原生 FreeRTOS/ESP-IDF 遷移與首次使用流程更新
- [releases/v0.3.0.md](releases/v0.3.0.md) — WebUI 風格重設計與展示素材更新
- [releases/v0.2.0.md](releases/v0.2.0.md) — FreeRTOS runtime 與背景 worker 更新
