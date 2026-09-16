# Changelog

## v7 — 2026-09-16

Experimental software release built on the hardware-verified v6 BLE baseline.

### Added
- Compact offline coordinate -> timezone/DST resolver that does not require external flash.
- Sony DD21 timezone/DST flag parsing.
- Automatic 91-byte vs 95-byte DD11 packet selection.
- E7 integer coordinate path and 7-decimal public test point.
- Optional DD30/DD31 enable sequence when characteristics exist.
- Sony CC00/CC13 discovery and an opt-in 13-byte camera local-time sync packet.
- 5-second static location update interval.
- GitHub Actions PlatformIO CI for `seeed_xiao_esp32c6`.

### Changed
- Removed the v6 A/B security-profile alternation.
- Fixed pairing configuration to the v6 profile that succeeded on the real A7R III: `ESP_LE_AUTH_REQ_SC_BOND`, IO NONE, key size 16, ENC+ID, SC-only enforcement disabled, GATTS enabled.
- Updated public documentation to distinguish v6 hardware-verified behavior from v7 experimental additions.

### Validation status
- Standalone timezone/DST math was compiled and exercised on the host for Taipei, Tokyo, Hong Kong, McMurdo, London, Paris, New York, Los Angeles and Sydney.
- GitHub Actions completed a clean PlatformIO/ESP-IDF build successfully for the XIAO ESP32-C6 target.
- A7R III real-hardware upload/re-test, CC13 verification and new ARW E7 verification remain pending.
