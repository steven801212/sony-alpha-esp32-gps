# Sony Alpha ESP32 GPS

[🇹🇼 繁體中文](README.zh-TW.md)

Open-source, reverse-engineered **BLE GPS / geotagging adapter for Sony Alpha cameras** using the **Seeed XIAO ESP32-C6**.

> **AI-assisted development:** this project was developed with substantial technical and documentation assistance from **OpenAI ChatGPT (GPT-5.6 Sol)**. See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) for a transparent breakdown of the collaboration.

> **Current status:** the Sony BLE protocol proof-of-concept has been verified end-to-end on a **Sony A7R III (ILCE-7RM3, firmware 3.01)**. The firmware in this repository is the known-good **static-location v6 PoC** used for that validation. Real GNSS input (MAX-M10S), track logging, timezone handling, and low-power operation are the next development stage.

This is a community interoperability project and is **not affiliated with, sponsored by, or endorsed by Sony or u-blox**.

## What has been verified

- ESP32-C6 discovers and connects to the A7R III over BLE.
- Bluedroid SMP bonding succeeds and the bond persists in NVS across a full power removal/reboot.
- Post-bond MTU exchange works at **158 bytes**.
- Sony Location service `8000dd00-dd00-ffff-ffff-ffffffffffff` is discovered.
- `DD11` accepts the Sony **95-byte location packet**.
- `DD21` can be read as an optional diagnostic/configuration characteristic.
- `EE01`, `DD30`, and `DD31` are **not required** for the verified A7R III location path.
- A Sony ARW file was verified to contain the exact test coordinates and UTC timestamp written by the ESP32-C6.

Verified flow:

```text
scan -> connect -> bond/encrypt -> MTU 158 -> DD00 service
     -> optional DD21 read -> DD11 95-byte location writes -> camera RAW EXIF
```

## Current firmware

The root firmware is intentionally kept close to the exact **v6 A/B diagnostic build that succeeded on real hardware**. It sends a public test location near McMurdo Station, Antarctica once per second.

The initial successful pairing occurred with the SC-capable security profile:

```text
ESP_LE_AUTH_REQ_SC_BOND
MITM: off
IO capability: NoInputNoOutput
Key size: 16
Key distribution: ENC + ID
SC-only enforcement: disabled
GATTS: enabled
```

The v6 source still contains the A/B pairing diagnostic because that is the configuration that was physically tested. A later production firmware will remove the A/B logic and use the known-good security configuration directly.

## Camera setup tested

On the A7R III:

```text
Bluetooth Function = On
Bluetooth Rmt Ctrl = Off
Bluetooth Settings -> Pairing
```

Boot/reset the ESP32-C6 while the camera is in the pairing screen. If the camera shows the ESP32 peer name, confirm pairing on the camera.

## Build

This project uses **PlatformIO + ESP-IDF**.

```ini
platform = espressif32@6.12.0
board = seeed_xiao_esp32c6
framework = espidf
```

Open the repository in VS Code/PlatformIO, then run **Build**, **Upload**, and **Monitor** at `115200` baud.

After changing Bluetooth Kconfig options, perform a clean rebuild. On Windows, `CLEAN_REBUILD_WINDOWS.bat` removes cached PlatformIO/ESP-IDF configuration.

## Planned portable hardware

The next prototype stage is:

```text
1S LiPo
  -> XIAO ESP32-C6 BAT+/BAT-
      -> USB-C onboard charging
      -> 3V3 -> GNSS VCC

MAX-M10S TX -> XIAO D7 / GPIO17 / RX
MAX-M10S RX <- XIAO D6 / GPIO16 / TX
GND         -> GND
```

Use the `3V3` rail only with a GNSS module/breakout that is specified to accept 3.3 V. For battery-powered use, do not assume the XIAO `5V/VBUS` pin remains powered when USB is disconnected.

See [docs/hardware-prototype.md](docs/hardware-prototype.md).

## Project status

| Item | Status |
|---|---|
| Sony BLE scan/connect | ✅ Verified |
| SMP bond/encryption | ✅ Verified |
| Bond persistence after power removal | ✅ Verified |
| DD00 / DD11 / DD21 path | ✅ Verified |
| 95-byte Sony location write | ✅ Verified |
| ARW GPS EXIF validation | ✅ Verified |
| Real GNSS UART input | 🚧 Next |
| 5–10 s GNSS low-power tracking | 🚧 Planned |
| Flash track logging / GPX export | 🚧 Planned |
| Automatic timezone/DST database | 🚧 Planned |
| Compact PCB/enclosure | 🚧 Planned |

## Acknowledgements

A substantial part of the protocol analysis, BLE debugging, firmware iteration, documentation, and hardware architecture planning was carried out collaboratively with **OpenAI ChatGPT (GPT-5.6 Sol)**. Physical hardware work and real-camera validation were performed by the repository owner.

See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) for details.

## Documentation

- [Acknowledgements / AI collaboration](ACKNOWLEDGEMENTS.md)
- [Protocol notes](docs/protocol-notes.md)
- [Hardware prototype](docs/hardware-prototype.md)
- [Validation notes](docs/validation.md)
- [Roadmap](docs/roadmap.md)
- [Third-party references](THIRD_PARTY_NOTICES.md)

## Privacy

The repository does not contain the test camera's Bluetooth MAC address, private photos, private locations, or the uploaded validation RAW file. Example coordinates are public test coordinates only.

## License

Project source is released under the [MIT License](LICENSE). Public protocol research and upstream projects remain subject to their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
