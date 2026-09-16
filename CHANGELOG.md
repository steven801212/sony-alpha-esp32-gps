# Changelog

## v7 — 2026-09-16

Hardware-validated software release built on the successful v6 BLE baseline.

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
- Updated documentation to distinguish verified A7R III behavior from still-unvalidated optional paths.

### Validation status
- GitHub Actions completed a clean PlatformIO/ESP-IDF build successfully for the XIAO ESP32-C6 target.
- v7 fresh pairing was verified from an empty local ESP32 bond database.
- The first fresh-pair SMP attempt returned `REPEATED_ATTEMPT`; automatic disconnect/retry with the same fixed security profile then exchanged keys and completed pairing successfully.
- The new bond persisted and a later reconnect reported `local bond before request=1`, restored encryption, negotiated MTU 158, reread DD21 and resumed DD11 transmission without re-pairing.
- MTU 158, DD00/DD11/DD21 discovery and repeated DD11 writes were verified on real hardware.
- `DD21 = 06 10 00 9c 02 00 00` correctly selected the 95-byte packet.
- The tested A7R III 3.01 exposed neither DD30/DD31 nor CC13; v7 safely skipped those optional paths.
- A new ARW verified E7 coordinate propagation from `25.0339687, 121.5644687` to `25.0339686111, 121.5644686111`, the difference being final EXIF rational quantization to 0.001 arc-second.
- The same ARW verified timezone metadata `+08:00` from the v7 Taiwan resolver (`+480 min`, DST 0).
- Real MAX-M10S input, the 91-byte DD11 path and CC13 behavior on a camera that exposes CC13 remain pending.
