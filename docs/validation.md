# End-to-end validation

This project has been validated on real hardware through the entire path from ESP32-C6 BLE transmission to GPS EXIF embedded by the camera in a Sony ARW RAW file.

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

A full power-removal test also confirmed that Bluedroid bond data survives in NVS and can restore encryption on reconnect.

## v7 real-camera validation

v7 removes the A/B alternation and always uses the known-good SC-capable bond configuration from v6. A real A7R III reconnect with an existing bond succeeded immediately:

```text
[PAIR] local bond before request=1
[PAIR] Authentication complete: success=1 ... -> BONDED/ENCRYPTED
[GATT] MTU exchange: status=0 MTU=158
```

Service discovery then found:

```text
DD11 = FOUND
DD21 = FOUND
DD30 = missing
DD31 = missing
CC13 = missing
```

The tested A7R III therefore does not expose DD30/DD31 or CC13; v7 correctly treats them as optional.

The camera returned:

```text
DD21 = 06 10 00 9c 02 00 00
```

v7 parsed byte 4 bit `0x02` as requiring timezone/DST fields:

```text
[GEO] DD21 timezone/DST flag=1 -> packet=95 bytes.
```

Repeated DD11 writes then succeeded:

```text
[TX] lat=25.0339687 lon=121.5644687 packet=95 bytes status=0x0 -> OK
```

This validates the fixed single security profile on reconnect, DD21-driven 95-byte packet selection, optional-feature skipping, and repeated DD11 transmission on the real camera.

### Fresh-pair status

This v7 test used an already stored bond (`local bond before request=1`). A fresh pairing after deleting bond state on both the camera and ESP32 has not yet been repeated with v7. The original first-time pairing itself was proven in v6.

## v7 ARW EXIF validation

The v7 static E7 test coordinate was:

```text
Latitude:  25.0339687
Longitude: 121.5644687
```

A new ARW captured while v7 was transmitting was parsed directly at the TIFF/EXIF GPS IFD level. The camera stored:

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

The timezone path is also confirmed end-to-end on this camera:

```text
DD21 requests timezone/DST
  -> v7 resolves Asia/Taipei
  -> DD11 standard offset = +480 min, DST = 0
  -> A7R III ARW metadata = +08:00
```

No GPS altitude, DOP, speed or track fields were present in this ARW, consistent with the known DD11 format where bytes 26–90 are zero padding in the current implementation.

## What remains unvalidated

The current tests do **not** yet validate:

- v7 fresh pairing after clearing bond state on both sides
- MAX-M10S UART parsing
- GNSS Power Save Mode / cyclic tracking
- battery runtime
- global timezone-boundary accuracy outside the explicit compact resolver regions
- CC13 camera-clock synchronization on a Sony body that actually exposes CC13
- compatibility with other Sony Alpha bodies

## Privacy

The original validation RAW files and the test camera's Bluetooth MAC address are intentionally not published in this repository. Public documentation contains only synthetic/public test coordinates and redacted logs.
