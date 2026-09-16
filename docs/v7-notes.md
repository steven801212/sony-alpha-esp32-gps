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

- DD11 packet construction uses latitude, longitude, UTC, and optional timezone/DST fields.
- Coordinates are E7 signed integers in big-endian form.
- The 65 bytes between UTC and timezone/DST are zero padding in its implementation.
- DD21 determines whether timezone/DST bytes are appended, producing 91- or 95-byte location packets.
- DD30/DD31 are optional and used only when exposed by a camera.
- CC13 is used as a separate local camera-time synchronization characteristic.

Alpha-GPS is GPL-3.0. v7 does not copy its source code; these protocol behaviors were independently implemented in C++ and attributed in `THIRD_PARTY_NOTICES.md`.

## v7 timezone design

Unlike a phone app, the ESP32 has no operating-system timezone supplied by the user. v7 therefore adds a compact coordinate-based resolver directly in firmware. It includes explicit common travel regions and common US/EU/NZ/Australia DST rules, then falls back to the nearest nominal 15-degree UTC offset with no DST.

This first resolver is intentionally small. It is suitable for proving the Sony timezone/DD11 path, but it is not equivalent to a full IANA timezone-boundary database.

## CC13 safety default

CC13 local-time synchronization is implemented but disabled by default while v7 still uses a static test position:

```cpp
constexpr bool ENABLE_CAMERA_TIME_SYNC = false;
```

The tested A7R III 3.01 does not expose CC13, so this path cannot be validated on that body. It remains available for later testing on Sony models that expose the characteristic.

## Hardware validation result

v7 has now been tested on a Sony A7R III / ILCE-7RM3 firmware 3.01.

### Fresh pairing

Starting with an empty local ESP32 bond database:

```text
local bond before request=0
```

The first SMP attempt returned `REPEATED_ATTEMPT`. v7 disconnected and retried using the same fixed security profile. The second attempt exchanged keys and succeeded:

```text
Key exchanged: 1 / 2 / 16 / 32
Authentication complete: success=1
local bond now=1
```

### Persistent reconnect

After the new bond was stored, a later reconnect without entering camera pairing mode reported:

```text
local bond before request=1
Authentication complete: success=1
MTU=158
DD21 -> 95 bytes
DD11 TX -> OK
```

This verifies persistent bond storage and automatic restoration of the Sony location path.

### DD21 and DD11

The tested A7R III returned:

```text
DD21 = 06 10 00 9c 02 00 00
```

Byte 4 bit `0x02` correctly selects the 95-byte packet. DD30, DD31 and CC13 were absent and safely skipped.

### ARW validation

The test coordinate:

```text
25.0339687, 121.5644687
```

was stored by the A7R III as:

```text
25.0339686111, 121.5644686111
```

The small difference is final EXIF GPS rational quantization to 0.001 arc-second. The same ARW contained timezone metadata `+08:00`, confirming the Taiwan `+480 min / DST 0` DD11 path.

## v7 validation checklist

1. ✅ PlatformIO clean build on XIAO ESP32-C6.
2. ✅ Fixed-profile fresh pairing from empty local bond DB.
3. ✅ Automatic retry after transient SMP `REPEATED_ATTEMPT` using the same profile.
4. ✅ Bond persistence and cold reconnect without re-pairing.
5. ✅ MTU 158.
6. ✅ DD21 `06 10 00 9c 02 00 00` selects the 95-byte packet.
7. ✅ DD30/DD31 safely skipped on A7R III 3.01 because they are absent.
8. ✅ DD11 accepts the E7 test coordinate repeatedly.
9. ✅ ARW preserves the E7 coordinate to the camera's EXIF rational precision.
10. ✅ ARW timezone metadata is `+08:00` for the Taiwan test coordinate.
11. ⏳ Real MAX-M10S / UBX-NAV-PVT input.
12. ⏳ 91-byte DD11 path on a camera/configuration that requests it.
13. ⏳ CC13 on a Sony body that actually exposes CC13.
