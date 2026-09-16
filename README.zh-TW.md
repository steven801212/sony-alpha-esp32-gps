# Sony Alpha ESP32 GPS 🇹🇼

[English README](README.md)

這是一個以 **Seeed XIAO ESP32-C6** 製作的開源 Sony Alpha 相機 **BLE GPS／照片地理標記（geotagging）轉接器**。

> **AI 協作開發：** 本專案在 Sony BLE protocol 分析、ESP-IDF / Bluedroid 除錯、韌體迭代、RAW/EXIF 驗證規劃、文件與硬體架構規劃上，大量使用 **OpenAI ChatGPT（GPT-5.6 Sol）** 協作。完整說明見 [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md)。

> **目前版本：v7。** v7 核心路徑已在 **Sony A7R III（ILCE-7RM3，韌體 3.01）** 完成實機驗證：空白本機 Bond DB 的 fresh pairing、SMP `REPEATED_ATTEMPT` 後自動用相同 profile 重試、Bond 持久化、冷啟動自動重連、MTU 158、DD21 自動選擇 95-byte 封包、連續 DD11 寫入、E7 七位小數座標寫入 ARW，以及台灣測試座標對應的 `+08:00` timezone metadata。

本專案是社群 reverse-engineering / interoperability 專案，**與 Sony、u-blox 無官方關係，也未獲其贊助或背書**。

## v7 主要更新

- 將 v6 A/B pairing 測試**合併成已知成功的單一設定**：
  - `ESP_LE_AUTH_REQ_SC_BOND`
  - MITM 關閉
  - `NoInputNoOutput`
  - key size 16
  - ENC + ID key distribution
  - 不強制 SC-only
  - GATTS enabled
- 讀取 `DD21`，依相機要求自動傳 **91 bytes 或 95 bytes** Sony location packet。
- 經緯度改以 signed **E7 integer（1e-7 degree）**保留。
- 若相機有 `DD30` / `DD31` 才執行對應 enable sequence。
- 掃描 Sony `CC00` / `CC13`，加入**可選的相機當地時間同步**。
- 加入**不需要外接 Flash 的內建 timezone / DST resolver**。
- v7 測試座標使用台北 101 附近的公開 7 位小數測試點，更新週期 5 秒。

## 已驗證狀態

| 項目 | 狀態 |
|---|---|
| ESP32-C6 掃描／連線 A7R III | ✅ 已驗證 |
| 空白本機 Bond DB 的 fresh pair | ✅ v7 已驗證 |
| SMP `REPEATED_ATTEMPT` 後自動重試 | ✅ v7 已驗證 |
| SMP bond/encryption | ✅ 已驗證 |
| 完全斷電後 Bond persistence | ✅ v7 已驗證 |
| 不重新配對的 cold reconnect | ✅ v7 已驗證 |
| MTU request 158 | ✅ 已驗證 |
| DD00 / DD11 / DD21 | ✅ 已驗證 |
| v7 固定單一 SC-capable security profile | ✅ v7 已驗證 |
| DD21 自動選擇 95-byte | ✅ v7 已驗證 |
| 連續 95-byte DD11 write | ✅ v7 已驗證 |
| E7 七位小數寫入 ARW | ✅ v7 已驗證 |
| 台灣 timezone metadata `+08:00` | ✅ v7 已驗證 |
| DD30/DD31 optional flow | 🧪 本機 A7R III 未提供 |
| 內建 timezone / DST | ✅ 台灣路徑已驗證；全球邊界仍是簡化版 |
| CC13 相機時間同步 | 🧪 已實作、預設關閉；本機 A7R III 未提供 CC13 |
| 真實 MAX-M10S GNSS | 🚧 下一步 |

## v7 Sony BLE 流程

```text
scan
  -> connect
  -> bond/encrypt
  -> MTU 158
  -> discover DD00 + optional CC00
  -> DD21 config read
  -> optional DD30 -> DD31
  -> optional CC13 time sync
  -> DD11 location writes
```

這台 A7R III 3.01 實測沒有 `DD30`、`DD31`、`CC13`，因此 v7 會正確略過這些 optional path。

### Fresh pair 與冷啟動重連

ESP32 本機 Bond DB 為空時，v7 顯示：

```text
local bond before request=0
```

第一次 SMP 嘗試回報 `REPEATED_ATTEMPT`；v7 斷線後使用**同一套固定 security profile** 自動重試。第二次成功交換 key 並建立 Bond：

```text
Key exchanged: 1 / 2 / 16 / 32
Authentication complete: success=1
local bond now=1
```

之後完成斷電／重新連線，且沒有再次進入相機配對流程時，v7 顯示：

```text
local bond before request=1
Authentication complete: success=1
MTU=158
DD21 -> 95 bytes
DD11 TX -> OK
```

因此已證明 Bond 可持久化保存，並在重新連線後自動恢復 Sony GPS 傳輸路徑。

## v7 ARW 實測結果

測試座標：

```text
25.0339687, 121.5644687
```

A7R III 最後寫入：

```text
25° 2' 2.287" N
121° 33' 52.087" E
= 25.0339686111, 121.5644686111
```

與輸入 E7 座標的差異來自相機最後將 GPS rational quantize 到 **0.001 arc-second**；這次誤差約 1 公分，儲存解析度約為緯度方向每 0.001 arc-second 約 3 公分。

同一張 ARW 也包含：

```text
GPSDateStamp:          2026:09:16
GPSTimeStamp:          00:01:44 UTC
DateTimeOriginal:      2026:09:16 08:01:45
OffsetTime:            +08:00
OffsetTimeOriginal:    +08:00
OffsetTimeDigitized:   +08:00
```

因此這條路徑已實機證明：

```text
DD21 要求 timezone/DST
      ↓
v7 解析台灣 = standard +480 min / DST 0
      ↓
DD11 95-byte
      ↓
A7R III ARW = +08:00
```

## 自動 timezone / DST

Alpha-GPS 可以直接使用手機系統提供的 timezone；我們的 ESP32 是獨立裝置，因此 v7 自己從經緯度推導 timezone。

目前採用**小型內建 resolver**，直接存在 firmware code 裡，不需要 W25Q128 或其他外接 Flash。明確涵蓋多個常見旅行區域與常用 DST 規則；其他地區則以經度推估標準 UTC offset 並不套 DST。

這不是完整 IANA timezone polygon database，所以**國界／時區邊界與特殊行政區仍可能有誤差**。後續可在 ESP32-C6 內建 4 MB Flash 加入壓縮 grid/polygon database，不一定需要外接 Flash。

### CC13 相機時間同步

v7 已加入 CC13 packet builder，但目前預設：

```cpp
constexpr bool ENABLE_CAMERA_TIME_SYNC = false;
```

本次實機 service discovery 顯示這台 A7R III 3.01 **沒有 CC13**，所以這個 body 無法用目前已知 CC13 characteristic 驗證相機時鐘同步；此功能需在有提供 CC13 的 Sony 機身上另行測試。

## Build

使用 PlatformIO + ESP-IDF：

```ini
platform = espressif32@6.12.0
board = seeed_xiao_esp32c6
framework = espidf
```

VS Code / PlatformIO 執行 **Build → Upload → Monitor**，序列埠 `115200`。變更 Bluetooth Kconfig 後請 clean rebuild；Windows 可直接執行 `CLEAN_REBUILD_WINDOWS.bat`。

> **Build / 實機驗證：** GitHub Actions 已完成 XIAO ESP32-C6 的乾淨 PlatformIO / ESP-IDF build；v7 的 fresh pair、Bond 持久化、cold reconnect、DD21 95-byte、自動 timezone、DD11 傳送與 ARW E7 寫入也已在 A7R III 實機驗證。

## 預計硬體

```text
1S LiPo -> XIAO ESP32-C6 BAT+/BAT-
             -> XIAO USB-C 充電
             -> 3V3 -> GNSS VCC（僅限該 GNSS board 明確支援 3.3 V）

MAX-M10S TX -> XIAO D7 / GPIO17 / RX
MAX-M10S RX <- XIAO D6 / GPIO16 / TX
GND         -> GND
```

正式 PCB 會優先考慮直接整合 MAX-M10S / MAX-M10N 類 GNSS module。**自動 timezone 不需要外接 Flash**；W25Q128 主要價值是增加 GPS track log 容量。

## 文件

- [v7 release notes](docs/v7-notes.md)
- [Protocol notes](docs/protocol-notes.md)
- [Hardware prototype](docs/hardware-prototype.md)
- [Validation notes](docs/validation.md)
- [Roadmap](docs/roadmap.md)
- [致謝／AI 協作](ACKNOWLEDGEMENTS.md)
- [Third-party references](THIRD_PARTY_NOTICES.md)
- [Changelog](CHANGELOG.md)

## 致謝／AI 協作

本專案相當一部分的 Sony BLE protocol 分析、ESP-IDF / Bluedroid 除錯、韌體迭代、RAW/EXIF 驗證規劃、文件整理與後續硬體架構規劃，都是與 **OpenAI ChatGPT（GPT-5.6 Sol）** 共同完成。實體硬體組裝、Sony 相機操作與實機驗證則由 repository owner 執行。

## 隱私

公開 repo 不放實測相機 Bluetooth MAC、私人照片、私人定位資訊或驗證用 RAW。測試座標均為公開／合成測試資料。

## 授權

本專案自行撰寫的程式碼採 [MIT License](LICENSE)。第三方研究仍受各自授權條款約束。特別是 **Saschl/Alpha-GPS 採 GPL-3.0**；v7 只參考公開可觀察的 protocol 行為並重新獨立實作，沒有複製其 GPL 原始碼。詳見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。