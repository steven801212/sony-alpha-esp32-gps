# Sony Alpha ESP32 GPS 🇹🇼

[English README](README.md)

這是一個以 **Seeed XIAO ESP32-C6** 製作的開源 Sony Alpha 相機 **BLE GPS／照片地理標記（geotagging）轉接器**。

> **AI 協作開發：** 本專案在 Sony BLE protocol 分析、ESP-IDF / Bluedroid 除錯、韌體迭代、RAW/EXIF 驗證規劃、文件與硬體架構規劃上，大量使用 **OpenAI ChatGPT（GPT-5.6 Sol）** 協作。完整說明見 [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md)。

> **目前版本：v7（experimental）。** v6 已在 **Sony A7R III（ILCE-7RM3，韌體 3.01）** 完成端到端實機驗證，包括持久化 Bond 與 GPS 寫入 ARW EXIF。v7 保留這條已知可工作的 BLE 基礎，移除 A/B pairing 輪替，新增 DD21 自動選擇 91/95-byte 封包、可選 DD30/DD31、內建自動 timezone/DST、E7 座標精度，以及可選 CC13 相機時間同步。**v7 新增功能仍需重新燒錄到實機驗證。**

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
- 經緯度改以 signed **E7 integer（1e-7 degree）**保留，不在封包路徑先降精度。
- 若相機有 `DD30` / `DD31` 才執行對應 enable sequence；A7R III 3.01 實測沒有這兩個 characteristic。
- 掃描 Sony `CC00` / `CC13`，加入**可選的相機當地時間同步**。
- 加入**不需要外接 Flash 的內建 timezone / DST resolver**。
- v7 測試座標使用台北 101 附近的公開 7 位小數測試點，更新週期改為 5 秒。

## 已驗證基礎 vs v7 新增功能

| 項目 | 狀態 |
|---|---|
| ESP32-C6 掃描／連線 A7R III | ✅ v6 已驗證 |
| SMP bond/encryption | ✅ v6 已驗證 |
| 完全斷電後 Bond persistence | ✅ v6 已驗證 |
| MTU request 158 | ✅ v6 已驗證 |
| DD00 / DD11 / DD21 | ✅ v6 已驗證 |
| 95-byte location write | ✅ v6 已驗證 |
| ARW GPS EXIF | ✅ v6 已驗證 |
| 固定 SC-capable security profile | 🧪 v7，來自 v6 成功 profile |
| DD21 自動 91/95-byte | 🧪 v7，待重新實測 |
| E7 七位小數保留 | 🧪 v7，待拍新 ARW 驗證 |
| DD30/DD31 optional flow | 🧪 v7；本機 A7R III 未提供 |
| 內建 timezone / DST | 🧪 v7，host 端邏輯測試完成；邊界為簡化版 |
| CC13 相機時間同步 | 🧪 v7 已實作，**預設關閉** |
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

## 自動 timezone / DST

Alpha-GPS 可以直接使用手機系統提供的 timezone；我們的 ESP32 是獨立裝置，因此 v7 自己從經緯度推導 timezone。

目前採用**小型內建 resolver**，直接存在 firmware code 裡，不需要 W25Q128 或其他外接 Flash。明確涵蓋台灣、香港、日本／韓國、中國、部分東南亞、印度／尼泊爾、澳洲、紐西蘭／McMurdo、歐洲常見區域與美國，並實作常用 DST 規則；其他地區則用經度推估標準 UTC offset，且不套 DST。

因此 v7 已經能做到：

```text
lat/lon + UTC
      ↓
內建 timezone resolver
      ↓
standard UTC offset + DST offset
      ↓
DD11 timezone/DST fields
```

這不是完整 IANA timezone polygon database，所以**國界／時區邊界與特殊行政區仍可能有誤差**。之後可在 ESP32-C6 內建 4 MB Flash 再加入壓縮 grid/polygon database，提高全球精度，不一定需要外接 Flash。

### CC13 相機時間同步

v7 已加入 CC13 packet builder，但目前預設：

```cpp
constexpr bool ENABLE_CAMERA_TIME_SYNC = false;
```

原因是目前還是靜態測試座標。如果預設開啟，任何人燒錄範例韌體都可能被測試座標改掉相機時間。等 MAX-M10S 提供真實座標與 UTC、且 A7R III 實機驗證完成後，再改成正式預設行為比較安全。

## E7 精度測試

v7 測試座標改成公開的台北 101 附近測試點：

```text
25.0339687, 121.5644687
```

故意保留第七位非 0。下一次拍 ARW 後，可以直接解析 GPS IFD，確認：

```text
ESP32 E7 -> Sony DD11 -> A7R III -> ARW EXIF
```

整條鏈是否保留到 `1e-7°`。

## Build

使用 PlatformIO + ESP-IDF：

```ini
platform = espressif32@6.12.0
board = seeed_xiao_esp32c6
framework = espidf
```

VS Code / PlatformIO 執行 **Build → Upload → Monitor**，序列埠 `115200`。變更 Bluetooth Kconfig 後請 clean rebuild；Windows 可直接執行 `CLEAN_REBUILD_WINDOWS.bat`。

> 本次 v7 已做 source review，且 timezone/DST 純 C++ 演算法已在 host 端編譯測試；但目前這個執行環境沒有 ESP-IDF / PlatformIO toolchain，因此尚未在此環境完成完整 firmware build。實際 PlatformIO build 與 A7R III 重測列為 v7 validation。

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
