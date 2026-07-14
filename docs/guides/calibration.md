# Scale Calibration

HX711 需要兩個步驟才能正確轉換 ADC 原始值為公克：**歸零（Tare）** 與 **校正（Calibrate）**。

---

## 原理

HX711 輸出的是原始 24-bit ADC 值，需要兩個參數轉換：

```
weightGrams = (rawADC - tareOffset) / calibrationFactor
```

| 參數 | 說明 | 儲存位置 |
|------|------|---------|
| `tareOffset` | 空秤台的 ADC 基準值 | NVS key `tare_offset` |
| `calibrationFactor` | ADC 值 / 公克 的比值 | NVS key `cal_factor` |

---

## 步驟 1：歸零（Tare）

1. **移除秤台上的所有物品**（確保淨空）
2. 前往 `http://<ip>/settings`，找到「Calibration」區塊
3. 點擊「**Tare**」按鈕
4. 確認頁面顯示「Tare OK」且重量顯示接近 0

```bash
# 或使用 curl（需先登入並取得 session cookie／CSRF token）
curl -b cookies.txt -H "X-CSRF-Token: <session-csrf-token>" \
  -X POST http://192.168.1.100/api/tare
```

---

## 步驟 2：校正（Calibrate）

1. 準備一個**已知精確重量**的物品（建議 100–500 g；砝碼最佳）
2. 將物品放上秤台
3. 在「Calibration」區塊輸入已知重量（例如 `200`）
4. 點擊「**Calibrate**」
5. 確認顯示的重量與已知重量一致（誤差 ±2 g 以內）

```bash
# 或使用 curl（需先登入並取得 session cookie／CSRF token）
curl -b cookies.txt -H "X-CSRF-Token: <session-csrf-token>" \
  -X POST -H "Content-Type: application/json" \
  -d '{"known_weight_g": 200.0}' \
  http://192.168.1.100/api/calibrate
# 回應：{"ok":true,"calibration_factor":427.53}
```

---

## 步驟 3：驗證

移除校正物，放上水杯（加水）後觀察：

1. 重量讀數穩定（跳動 ≤ 3 g）
2. 拿起杯子再放下，`Cup State` 應依序顯示 `no_cup → unstable → stable`
3. 喝水後放回，`DRINK_CONFIRMED` 顯示，今日飲水量增加

若重量持續跳動，可嘗試：

- 將 `stableToleranceGram` 調高（預設 3.0 g，可試 5.0 g）
- 確認秤台固定平穩、無晃動
- 將 HX711 改為 80 Hz 輸出速率（RATE 腳位接高電位）

---

## 重新校正

若感測器更換或讀數長期偏移，重複步驟 1–3 即可。校正係數會立即存入 NVS，重啟後仍有效。

---

## 注意事項

- 歸零與校正時，**感測器必須已完全靜置**（放置 10 秒以上再操作）
- 環境溫度變化可能影響 HX711 讀值，建議在實際使用環境下校正
- `calibrationFactor` 預設值 `1.0`（未校正），此時重量讀值無意義
