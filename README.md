# Sony Alpha ESP32 GPS

[🇹🇼 繁體中文](README.zh-TW.md)

Open-source, reverse-engineered **BLE GPS / geotagging adapter for Sony Alpha cameras** using the **Seeed XIAO ESP32-C6**.

> **AI-assisted development:** this project was developed with substantial technical and documentation assistance from **OpenAI ChatGPT (GPT-5.6 Sol)**. See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md).

> **Current release: v7 (experimental).** The v6 Sony BLE path was verified end-to-end on a **Sony A7R III (ILCE-7RM3, firmware 3.01)**, including persistent bonding and GPS data written into ARW EXIF. v7 keeps that known-good BLE foundation, removes the A/B pairing experiment, adds DD21-aware 91/95-byte packet generation, optional DD30/DD31 support, a compact offline timezone/DST resolver, E7 coordinate preservation, and optional CC13 camera-clock synchronization. The new v7 additions still require real-camera re-validation.

This is a community interoperability project and is **not affiliated with, sponsored by, or endorsed by Sony or u-blox**.

## v7 highlights

- **Merged v6 A/B pairing experiment** into the known-good security profile:
  - `ESP_LE_AUTH_REQ_SC_BOND`
  - MITM off
  - `NoInputNoOutput`
  - key size 16
  - ENC + ID key distribution
  - SC-only enforcement disabled
  - GATTS enabled
- Reads `DD21` and selects the Sony **91-byte or 95-byte** location packet automatically.
- Preserves latitude/longitude as signed **E7 integers (1e-7 degree)** in the packet path.
- Detects optional `DD30` / `DD31` and uses them only when the camera exposes them.
- Discovers Sony `CC00` / `CC13` and includes an **opt-in camera local-time sync** implementation.
- Adds a **compact no-external-flash timezone/DST resolver** for common travel regions, with an approximate longitude fallback.
- Uses a public 7-decimal test point near Taipei 101 and a 5-second update interval for v7 testing.

## Verified baseline vs v7 additions

| Feature | Status |
|---|---|
| ESP32-C6 scan/connect to A7R III | ✅ Verified in v6 |
| SMP bond/encryption | ✅ Verified in v6 |
| Bond persistence after full power removal | ✅ Verified in v6 |
| MTU request 158 | ✅ Verified in v6 |
| DD00 / DD11 / DD21 path | ✅ Verified in v6 |
| 95-byte location write | ✅ Verified in v6 |
| ARW GPS EXIF validation | ✅ Verified in v6 |
| Fixed SC-capable security profile | 🧪 v7, derived from v6 successful profile |
| DD21-driven 91/95-byte selection | 🧪 v7, needs camera re-test |
| E7 7-decimal preservation test | 🧪 v7, needs new ARW capture |
| Optional DD30/DD31 sequence | 🧪 v7, not exposed on tested A7R III |
| Offline timezone/DST resolver | 🧪 v7 software-tested; boundary resolver is intentionally compact |
| CC13 camera local-time sync | 🧪 v7 implemented, **disabled by default** until real GNSS drives timezone |
| Real MAX-M10S GNSS input | 🚧 Next |

## Sony BLE flow

v7 uses:

```text
scan
  -> connect
  -> bond/encrypt
  -> MTU 158
  -> discover DD00 + optional CC00
  -> read DD21 when present
  -> optional DD30 -> DD31 when present
  -> optional CC13 time sync
  -> DD11 location writes
```

The tested A7R III did **not** expose `DD30` or `DD31`, so they remain optional.

## Timezone behavior in v7

The standalone ESP32 cannot simply ask a phone for its system timezone like Alpha-GPS does. v7 therefore includes a compact coordinate-to-timezone resolver in firmware. It explicitly covers several common travel regions (including Taiwan, Hong Kong, Japan/Korea, China, Southeast Asia, India/Nepal, Australia, New Zealand/McMurdo, much of Europe, and the United States) and implements common DST rules. Other locations fall back to a longitude-derived standard UTC offset with no DST.

This is deliberately **not a full IANA timezone-boundary database**. Border areas and unusual regional rules can be approximate. A future compact polygon/grid database can improve global precision without requiring external SPI flash.

`DD11` can include standard timezone offset and a separate DST offset when `DD21` requests them. `CC13` local-clock sync is implemented but currently defaults to:

```cpp
constexpr bool ENABLE_CAMERA_TIME_SYNC = false;
```

This prevents a static test coordinate from unexpectedly changing a user's camera clock. It can be enabled after live GNSS/timezone input is integrated and re-tested.

## Current static v7 test

The firmware currently uses a public synthetic test fix near Taipei 101 with non-zero seventh decimal digits:

```text
25.0339687, 121.5644687
```

This is intentional: a new ARW capture can verify whether the complete path preserves E7 resolution.

## Build

PlatformIO + ESP-IDF:

```ini
platform = espressif32@6.12.0
board = seeed_xiao_esp32c6
framework = espidf
```

Open in VS Code/PlatformIO, then **Build**, **Upload**, and **Monitor** at `115200` baud. After changing Bluetooth Kconfig options, perform a clean rebuild; on Windows use `CLEAN_REBUILD_WINDOWS.bat`.

> **Build validation:** GitHub Actions now performs a clean PlatformIO/ESP-IDF build for `seeed_xiao_esp32c6`, and the first v7 CI build completed successfully. Real-hardware upload, fresh-pair/reconnect, DD21 91/95-byte behavior, E7 ARW verification, and CC13 camera-time behavior still require A7R III testing.

## Planned portable hardware

```text
1S LiPo -> XIAO ESP32-C6 BAT+/BAT-
             -> USB-C onboard charging
             -> 3V3 -> GNSS VCC (only if the exact GNSS board supports 3.3 V)

MAX-M10S TX -> XIAO D7 / GPIO17 / RX
MAX-M10S RX <- XIAO D6 / GPIO16 / TX
GND         -> GND
```

The final PCB is expected to integrate a MAX-M10S/MAX-M10N-class GNSS module directly. External SPI NOR flash is **optional**: automatic timezone/DST does not require it; external flash mainly increases track-log capacity.

## Documentation

- [v7 release notes](docs/v7-notes.md)
- [Protocol notes](docs/protocol-notes.md)
- [Hardware prototype](docs/hardware-prototype.md)
- [Validation notes](docs/validation.md)
- [Roadmap](docs/roadmap.md)
- [Acknowledgements / AI collaboration](ACKNOWLEDGEMENTS.md)
- [Third-party references](THIRD_PARTY_NOTICES.md)
- [Changelog](CHANGELOG.md)

## Acknowledgements

A substantial part of the Sony BLE protocol analysis, ESP-IDF / Bluedroid debugging, firmware iteration, RAW/EXIF validation planning, documentation, and hardware architecture planning was carried out collaboratively with **OpenAI ChatGPT (GPT-5.6 Sol)**. Physical hardware work and real-camera validation were performed by the repository owner.

## Privacy

The public repository does not contain the test camera's Bluetooth MAC address, private photos, private locations, or the validation RAW file. Test coordinates are public/synthetic examples only.

## License

Project-authored source is released under the [MIT License](LICENSE). Public upstream research remains under its own licenses. In particular, **Saschl/Alpha-GPS is GPL-3.0**; v7 uses independently written code based on observed interoperability behavior and does not copy GPL source. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
