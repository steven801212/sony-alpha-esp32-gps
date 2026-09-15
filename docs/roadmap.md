# Roadmap

The project is being developed in stages so protocol validation remains separate from GNSS, power, storage, and enclosure work.

## Stage 0 — Sony BLE protocol PoC ✅

Completed on Sony A7R III / ILCE-7RM3 firmware 3.01:

- BLE discovery and connection
- Bluedroid SMP pairing
- persistent bond storage in NVS
- reconnect after full power removal
- MTU request 158
- DD00 Location service discovery
- DD11 95-byte location write
- optional DD21 diagnostic read
- RAW ARW GPS EXIF verification

## Stage 1 — Real GNSS input 🚧

Target hardware: XIAO ESP32-C6 + MAX-M10S generation GNSS breakout.

Planned work:

- UART on D7/GPIO17 RX and D6/GPIO16 TX
- auto-detect / validate receiver identity with UBX where supported
- parse UTC, latitude, longitude, altitude, speed and fix validity
- reject invalid/stale fixes
- map live GNSS fields into the already verified Sony 95-byte packet

## Stage 2 — Low-power portable operation

- 1S LiPo on XIAO BAT+/BAT-
- XIAO USB-C charging
- GNSS 5 s / 10 s cyclic or power-save tracking
- avoid hard power-cycling GNSS between every update
- measure real system current in acquisition, tracking, BLE-connected and sleep states
- define stale-position behavior when GNSS temporarily loses fix

## Stage 3 — GPS track logger

- binary circular log format
- start with internal flash for development
- optional 16/32 MB SPI NOR flash on final PCB
- RAM batching before flash writes
- GPX and CSV export tooling
- retain UTC as canonical log time

## Stage 4 — Timezone support

- offline coordinate -> IANA timezone mapping
- DST rule support
- do not alter canonical GNSS UTC timestamps
- investigate whether the Sony Location Information Link protocol can also safely correct camera local clock/timezone on A7R III

## Stage 5 — Hardware integration

- compact custom PCB
- MAX-M10S or successor GNSS selection based on measured RF/power results
- antenna keepout and enclosure RF review
- physical power switch / load-switch design that still permits USB charging while the device is logically off
- battery voltage measurement
- status LED behavior
- 3D printed prototype enclosure

## Stage 6 — Camera compatibility matrix

Test additional Sony bodies only after the A7R III implementation is stable.

Potential targets include other Alpha bodies that expose Sony's Location Information Link BLE service. Compatibility should be marked only after real-camera testing; service presence alone is not enough to claim support.
