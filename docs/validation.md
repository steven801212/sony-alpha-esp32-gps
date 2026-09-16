# End-to-end validation

This project has been validated on real hardware through the entire path from ESP32-C6 BLE pairing and transmission to GPS EXIF embedded by the camera in a Sony ARW RAW file.

## Tested camera

```text
Sony ILCE-7RM3 / A7R III
Camera firmware: 3.01
```

## v6 baseline

The first successful first-time pairing was observed after the v6 A/B diagnostic switched to the SC-capable profile:

```text
ESP_LE_AUTH_REQ_SC_BOND
MITM off
NoInputNoOutput
key size 16
ENC + ID
SC-only enforcement disabled
GATTS enabled
```

The v6 end-to-end path was verified as:

```text
ESP32-C6
  -> Sony BLE pairing/bonding
  -> MTU 158
  -> DD00 / DD11 / DD21
  -> 95-byte Sony location packet
  -> A7R III
  -> ARW GPS EXIF
```

## v7 fixed-profile validation

v7 removes the A/B alternation and always uses the known-good SC-capable bond configuration from v6.

### Fresh pairing from an empty local bond database

With the ESP32 local bond database empty, v7 reported:

```text
[PAIR] local bond before request=0
```

The first SMP attempt returned:

```text
fail_reason=0x56 (REPEATED_ATTEMPT)
```

v7 disconnected and retried using the **same fixed security profile**. The second attempt exchanged keys and completed pairing successfully:

```text
[PAIR] Key exchanged, type=1
[PAIR] Key exchanged, type=2
[PAIR] Key exchanged, type=16
[PAIR] Key exchanged, type=32
[PAIR] Authentication complete: success=1 ... -> BONDED/ENCRYPTED
[PAIR] Bond/encryption established. local bond now=1
```

Post-bond setup then succeeded:

```text
[GATT] MTU exchange: status=0 MTU=158
[GATT] DD11=FOUND
[GATT] DD21=FOUND
[GATT] DD30=missing DD31=missing CC13=missing
[GEO] DD21 config: 06 10 00 9c 02 00 00
[GEO] DD21 timezone/DST flag=1 -> packet=95 bytes.
[TX] lat=25.0339687 lon=121.5644687 packet=95 bytes status=0x0 -> OK
```

This confirms that v7 can establish a new bond without A/B profile alternation. The transient first-attempt `REPEATED_ATTEMPT` did not require changing security settings; the automatic reconnect using the same profile succeeded.

### Persistent bond and cold reconnect

After the new bond was created, the device/camera were disconnected and later reconnected without entering camera pairing mode. v7 reported:

```text
[PAIR] local bond before request=1
[PAIR] Authentication complete: success=1 ... -> BONDED/ENCRYPTED
[PAIR] Bond/encryption established. local bond now=1
[GATT] MTU exchange: status=0 MTU=158
[GEO] DD21 timezone/DST flag=1 -> packet=95 bytes.
[TX] lat=25.0339687 lon=121.5644687 packet=95 bytes status=0x0 -> OK
```

This confirms persistent bond storage in ESP32 NVS and automatic restoration of the Sony location path after reconnect.

An observed reconnect advertisement used `mode22=0xb8`. During the fresh-pair sequence, `0xa8` and `0xe8` were also observed. These values are recorded as empirical observations only; no single `mode22` bit is treated as a definitive public protocol definition.

## DD21 / optional Sony features

The tested A7R III returned:

```text
DD21 = 06 10 00 9c 02 00 00
```

v7 parsed byte 4 bit `0x02` as requiring timezone/DST fields and therefore used the 95-byte DD11 packet.

Service discovery found:

```text
DD11 = FOUND
DD21 = FOUND
DD30 = missing
DD31 = missing
CC13 = missing
```

The tested A7R III therefore does not expose DD30/DD31 or CC13; v7 correctly treats them as optional.

## v7 ARW EXIF validation

The v7 static E7 test coordinate was:

```text
Latitude:  25.0339687
Longitude: 121.5644687
```

A new ARW captured while v7 was transmitting was parsed at the TIFF/EXIF GPS IFD level. The camera stored:

```text
GPSLatitudeRef:  N
GPSLatitude:     25° 2' 2.287"
GPSLongitudeRef: E
GPSLongitude:    121° 33' 52.087"
Decimal:         25.0339686111, 121.5644686111
GPSVersionID:    2.3.0.0
GPSMapDatum:     WGS-84
GPSStatus:       A
GPSMeasureMode:  2
GPSDateStamp:    2026:09:16
GPSTimeStamp:    00:01:44 UTC
GPSDifferential: 0
```

The camera photo metadata also contained:

```text
DateTimeOriginal:      2026:09:16 08:01:45
OffsetTime:            +08:00
OffsetTimeOriginal:    +08:00
OffsetTimeDigitized:   +08:00
```

The requested E7 values are therefore preserved through Sony DD11 with only the camera's final EXIF rational quantization to 0.001 arc-second. In this test the resulting coordinate error is about one centimeter, while the storage resolution is roughly three centimeters per 0.001 arc-second in latitude.

The timezone path is confirmed end-to-end on this camera:

```text
DD21 requests timezone/DST
  -> v7 resolves Asia/Taipei
  -> DD11 standard offset = +480 min, DST = 0
  -> A7R III ARW metadata = +08:00
```

No GPS altitude, DOP, speed or track fields were present in this ARW, consistent with the currently implemented/observed DD11 layout where bytes 26–90 are zero padding.

## Hardware-validated v7 scope

For the tested A7R III 3.01, the following are now hardware-validated:

```text
fresh pairing from empty local bond DB
-> automatic retry after transient SMP repeated-attempt
-> bond creation and NVS persistence
-> reconnect without re-pairing
-> MTU 158
-> DD00 / DD11 / DD21
-> DD21-driven 95-byte packet
-> repeated DD11 writes
-> E7 coordinate propagation into ARW
-> Taiwan UTC+8 timezone metadata
```

## What remains unvalidated

The current tests do **not** yet validate:

- real MAX-M10S UART / UBX-NAV-PVT input
- GNSS Power Save Mode / cyclic tracking
- battery runtime
- global timezone-boundary accuracy outside the explicit compact resolver regions
- CC13 camera-clock synchronization on a Sony body that actually exposes CC13
- compatibility with other Sony Alpha bodies
- the 91-byte DD11 path on a camera/configuration that requests it

## Privacy

The original validation RAW files and the test camera's Bluetooth MAC address are intentionally not published in this repository. Public documentation contains only synthetic/public test coordinates and redacted logs.
