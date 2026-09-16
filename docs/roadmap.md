# Roadmap

## Stage 0 — Sony BLE protocol PoC ✅

Hardware-verified on Sony A7R III / ILCE-7RM3 firmware 3.01:

- BLE discovery and connection
- Bluedroid SMP pairing
- persistent bond storage in NVS
- reconnect after full power removal
- MTU request 158
- DD00 discovery
- DD11 95-byte location write
- DD21 read
- RAW ARW GPS EXIF verification

## Stage 0.5 — v7 protocol cleanup and timezone foundation 🧪

Implemented in v7:

- remove A/B pairing alternation and keep the v6 successful SC-capable profile
- parse DD21 timezone/DST requirement
- switch between 91-byte and 95-byte DD11 packets
- preserve signed E7 coordinate integers
- optional DD30/DD31 sequence only when exposed
- discover CC00/CC13
- compact coordinate -> timezone/DST resolver without external flash
- optional CC13 local-camera-time packet, disabled by default

Pending validation:

- PlatformIO clean build on physical XIAO ESP32-C6
- A7R III fresh-pair and reconnect tests
- new ARW E7 precision capture
- CC13 behavior on the A7R III

## Stage 1 — Real GNSS input 🚧

Target: XIAO ESP32-C6 + MAX-M10S/MAX-M10N-class GNSS.

- UART: D7/GPIO17 RX and D6/GPIO16 TX
- identify/validate receiver with UBX where supported
- consume native E7 latitude/longitude directly from GNSS
- source UTC from GNSS instead of a synthetic epoch
- parse altitude, hAcc/vAcc, speed, heading, pDOP, satellites, fix type
- use accuracy/fix quality to gate what is sent to Sony
- feed live coordinate/UTC into timezone resolver and DD11
- enable CC13 only after live timezone is trustworthy

## Stage 2 — Better global timezone mapping

v7's resolver is intentionally compact and heuristic. Next options, still possible inside the ESP32-C6's internal 4 MB flash:

- compressed coarse grid + boundary polygons
- compact numeric timezone IDs instead of IANA strings
- expanded historical/current DST rule table
- explicit confidence flag near timezone boundaries

External SPI flash is **not required** for timezone support.

## Stage 3 — Low-power portable operation

- 1S LiPo on XIAO BAT+/BAT-
- XIAO USB-C charging
- GNSS 5 s / 10 s cyclic or power-save tracking
- avoid hard power-cycling GNSS between fixes
- measure acquisition/tracking/BLE/sleep current
- define stale-position behavior

## Stage 4 — Track logger

- binary circular log
- begin with remaining internal flash for development
- optional W25Q128/W25Q256 on final PCB for multi-week logging
- RAM batching and wear-aware sector rotation
- GPX / CSV export
- UTC as canonical log time

## Stage 5 — Hardware integration

- custom PCB
- MAX-M10S / MAX-M10N-compatible footprint where practical
- GNSS RF layout / antenna keepout
- optional U.FL test/backup antenna path
- ESP32-C6 BLE antenna separation
- LiPo charge/power-path design
- battery measurement and status LED
- compact enclosure

## Stage 6 — Camera compatibility matrix

Test other Sony Alpha bodies only after the A7R III v7/v8 path is stable. Compatibility must be based on real-camera testing rather than UUID presence alone.
