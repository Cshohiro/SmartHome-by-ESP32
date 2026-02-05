# API 仕様

## 共通

- Base URL: `http://192.168.1.240`
- Method: GET
- 文字コード: UTF-8

---

## AC (3F)

### GET /api/ac3
エアコン操作API（クエリで指定）

#### パラメータ

| パラメータ | 説明 |
|-----------|------|
| mode | 運転モード切替（heat=暖房, cool=冷房） |
| tempStep | 温度を ±0.5℃ 変更 |
| fan | 風量設定（auto, quiet, 1〜5） |
| power | `0` で停止 |
| timerMin | 指定分後に自動停止 |
| timerCancel | オフタイマー取消 |

レスポンス：`text/plain`

---

### GET /api/ac3_state
現在の状態を取得

レスポンス：`application/json`

#### レスポンス項目

| 項目 | 説明 |
|------|------|
| power | 運転状態（true=運転中） |
| mode | 運転モード |
| temp | 設定温度（℃） |
| fan | 風量表示 |
| timer_left | オフタイマー残り時間 |

#### レスポンス例

```json
{
  "power": true,
  "mode": "heat",
  "temp": "27.5",
  "fan": "自動",
  "timer_left": "120s"
}
```

## Light
### GET /api/light?cmd=on|night|off

レスポンス： text/plain

---