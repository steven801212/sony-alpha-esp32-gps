# End-to-end validation

This project has been validated on real hardware through the entire path from ESP32-C6 BLE transmission to GPS EXIF embedded by the camera in a Sony ARW RAW file.

## Tested camera

```text
Sony ILCE-7RM3 / A7R III
Camera firmware: 3.01
```

## Successful BLE log milestones

The first successful first-time pairing was observed after the v6 A/B diagnostic switched to the SC-capable profile:

```text
[PAIR] Security profile applied: B:GATTS+SC_CAPABLE_BOND ...
[PAIR] Key exchanged, type=1
[PAIR] Key exchanged, type=2
[PAIR] Key exchanged, type=16
[PAIR] Key exchanged, type=32
[PAIR] Authentication complete: success=1 ... -> BONDED/ENCRYPTED
```

Post-bond setup then succeeded:

```text
[GATT] MTU exchange: status=0 MTU=158
[GATT] Sony Location service DD00 FOUND
[GATT] DD11=FOUND ... DD21=FOUND ...
[GEO] Sony Location path READY
[TX] ... packet=95 bytes status=0x0 -> OK
```

## Power-cycle persistence test

The XIAO ESP32-C6 was unplugged completely and later powered again. On reconnect, the local bond database still contained the camera and encryption was restored successfully before the location service was reopened.

This verifies that Bluedroid bond data survives a full power removal through NVS storage.

## RAW EXIF validation

The PoC used the static public test coordinate:

```text
Latitude:  -77.841900
Longitude: 166.686300
```

A photo captured by the A7R III while the ESP32-C6 was streaming the 95-byte Sony location packet was inspected at the TIFF/EXIF GPS IFD level.

The ARW contained:

```text
GPSLatitudeRef:  S
GPSLatitude:     77° 50' 30.840"
GPSLongitudeRef: E
GPSLongitude:    166° 41' 10.680"
Decimal:         -77.841900, 166.686300
GPSVersionID:    2.3.0.0
GPSMapDatum:     WGS-84
GPSStatus:       A
GPSMeasureMode:  2
```

The recorded date/time corresponded to the UTC timestamp supplied by the PoC packet.

Therefore the following path is confirmed:

```text
ESP32-C6
  -> Sony BLE pairing/bonding
  -> DD11 Sony location packet
  -> A7R III
  -> ARW GPS EXIF
```

## What this validation does not yet prove

The test does **not** yet validate:

- MAX-M10S UART parsing
- GNSS Power Save Mode / cyclic tracking
- battery runtime
- automatic timezone or DST handling
- compatibility with other Sony Alpha bodies

Those are intentionally tracked separately so the verified BLE protocol result is not mixed with planned features.

## Privacy

The original validation RAW and the test camera's Bluetooth MAC address are intentionally not published in this repository.
