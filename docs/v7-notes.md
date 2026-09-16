# v7 release notes

v7 converts the successful v6 diagnostic firmware into a cleaner single-profile code path and starts the transition toward a stand-alone GPS accessory.

## What came from the v6 hardware experiment

The first successful A7R III pairing happened while v6 profile B was active:

```text
ESP_LE_AUTH_REQ_SC_BOND
MITM off
NoInputNoOutput
key size 16
ENC + ID
SC-only enforcement disabled
GATTS enabled
```

v7 removes the alternating A/B experiment and keeps this complete known-good combination. This does not claim that SC capability alone was the root cause of success.

## What was learned from Alpha-GPS

Current Saschl/Alpha-GPS source confirms several useful Sony behaviors:

- DD11 packet construction uses only latitude, longitude, current UTC, and optional timezone/DST fields.
- Coordinates are E7 signed integers in big-endian form.
- The 65 bytes between UTC and timezone/DST are zero padding in its implementation.
- DD21 determines whether timezone/DST bytes are appended, producing 91- or 95-byte location packets.
- DD30/DD31 are optional and used only when exposed by a camera.
- CC13 is used as a separate local camera-time synchronization characteristic.

Alpha-GPS is GPL-3.0. v7 does not copy its source code; these protocol behaviors were independently implemented in C++ and attributed in `THIRD_PARTY_NOTICES.md`.

## v7 timezone design

Unlike a phone app, the ESP32 has no operating-system timezone supplied by the user. v7 therefore adds a compact coordinate-based resolver directly in firmware. It includes explicit common travel regions and common US/EU/NZ/Australia DST rules, then falls back to the nearest nominal 15-degree UTC offset with no DST.

This first resolver is intentionally small. It is suitable for proving the Sony timezone/DD11/CC13 path, but it is not equivalent to a full IANA timezone-boundary database.

## CC13 safety default

CC13 local-time synchronization is implemented but disabled by default while v7 still uses a static test position:

```cpp
constexpr bool ENABLE_CAMERA_TIME_SYNC = false;
```

This avoids unexpectedly changing a user's camera clock from sample data. Once MAX-M10S supplies live coordinates and UTC and the A7R III path is re-verified, this can be enabled by policy.

## v7 validation checklist

1. PlatformIO clean build on XIAO ESP32-C6.
2. Existing v6 bond reconnects successfully.
3. Fresh-pair test succeeds without A/B alternation.
4. DD21 `06 10 00 9c 02 00 00` selects the 95-byte packet.
5. DD30/DD31 remain safely skipped on A7R III 3.01 when absent.
6. DD11 accepts the new E7 test coordinate.
7. New ARW preserves `25.0339687, 121.5644687` as expected.
8. Enable CC13 temporarily and verify local camera time behavior before making it a default.
