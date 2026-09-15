# Sony Alpha ESP32 GPS 🇹🇼

[English README](README.md)

這是一個以 **Seeed XIAO ESP32-C6** 製作的開源 Sony Alpha 相機 **BLE GPS／照片地理標記（geotagging）轉接器**。

> **目前狀態：Sony BLE 通訊已完成端到端 PoC 驗證。** 已在 **Sony A7R III（ILCE-7RM3，韌體 3.01）** 實機成功完成 BLE 掃描、配對、Bond 持久化、Location service 寫入，並確認相機拍攝的 ARW RAW 檔真的寫入了 ESP32 傳送的 GPS 座標與 UTC 時間。

目前 repo 的韌體是實際成功的 **v6 靜態座標 PoC**。真正的 MAX-M10S GNSS 輸入、5–10 秒低功耗更新、軌跡記錄、時區資料庫與小型 PCB 是下一階段。

本專案是社群 reverse-engineering / interoperability 專案，**與 Sony、u-blox 無官方關係，也未獲其贊助或背書**。

## 已實機驗證

- ESP32-C6 可掃描並連線 A7R III。
- ESP-IDF Bluedroid SMP 配對成功。
- Bond key 可存於 NVS，整機斷電再上電仍可直接恢復加密連線。
- Bond 後 MTU 交換成功，使用 **158 bytes**。
- 找到 Sony Location service：`8000dd00-dd00-ffff-ffff-ffffffffffff`。
- `DD11` 可接受 Sony **95-byte location packet**。
- `DD21` 可作為選配 diagnostic/config read。
- A7R III 這條已驗證路徑 **不需要 EE01、DD30、DD31**。
- Sony ARW 已確認含有 ESP32 傳送的測試座標與 UTC timestamp。

已驗證流程：

```text
scan -> connect -> bond/encrypt -> MTU 158 -> DD00
     -> optional DD21 -> DD11 95-byte location write -> RAW EXIF
```

## 相機設定

A7R III 實測設定：

```text
Bluetooth Function = On
Bluetooth Rmt Ctrl = Off
Bluetooth Settings -> Pairing
```

相機停留在 Pairing 畫面後，再啟動或 reset ESP32-C6。若相機顯示 `SonyGPS-C6-v6`，在相機上確認配對。

## 已知可工作的 BLE security 組合

首次成功配對發生在：

```text
ESP_LE_AUTH_REQ_SC_BOND
MITM = off
IO capability = NoInputNoOutput
Key size = 16
Key distribution = ENC + ID
SC-only enforcement = disabled
GATTS = enabled
```

目前 v6 原始碼仍保留 A/B pairing diagnostic，因為這就是實機跑通的版本；後續 production firmware 才會移除 A/B 輪替，只保留已知成功的 security profile。

## 預計可攜式硬體

```text
1S LiPo
  -> XIAO ESP32-C6 背面 BAT+ / BAT-
      -> XIAO USB-C 充電
      -> XIAO 3V3 -> GNSS VCC

MAX-M10S TX -> XIAO D7 / GPIO17 / RX
MAX-M10S RX <- XIAO D6 / GPIO16 / TX
GND         -> GND
```

注意：只有在你的 GNSS breakout 明確支援 3.3 V 輸入時，才把 XIAO `3V3` 接到 GNSS VCC。電池模式下不要假設 XIAO 的 `5V/VBUS` 會持續有輸出。

詳細見 [docs/hardware-prototype.md](docs/hardware-prototype.md)。

## 開發狀態

| 項目 | 狀態 |
|---|---|
| Sony BLE scan/connect | ✅ 已驗證 |
| SMP bond/encryption | ✅ 已驗證 |
| 完全斷電後 Bond persistence | ✅ 已驗證 |
| DD00 / DD11 / DD21 | ✅ 已驗證 |
| Sony 95-byte location packet | ✅ 已驗證 |
| ARW GPS EXIF | ✅ 已驗證 |
| 真實 GNSS UART | 🚧 下一步 |
| 5–10 秒低功耗 GNSS | 🚧 規劃中 |
| 外部 Flash 軌跡記錄 / GPX | 🚧 規劃中 |
| 自動時區 / DST | 🚧 規劃中 |
| 小型 PCB / 外殼 | 🚧 規劃中 |

## 隱私

公開 repo 不放實測相機 Bluetooth MAC、私人照片、私人定位資訊或驗證用 RAW。測試程式只使用公開的示範座標。

## 授權

專案原始碼採 [MIT License](LICENSE)。第三方公開研究與參考專案仍受各自授權條款約束，詳見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
