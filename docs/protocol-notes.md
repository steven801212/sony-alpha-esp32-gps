# Sony BLE protocol notes

These notes separate **hardware-verified behavior** from **v7 experimental/learned behavior**.

## Verified target

- Camera: Sony A7R III / `ILCE-7RM3`
- Camera firmware: `3.01`
- ESP32 board: Seeed XIAO ESP32-C6
- Framework: ESP-IDF / Bluedroid
- Tested local MTU request: `158`

## Advertisement observations

The A7R III manufacturer payload was observed with Sony company ID `0x012d`, camera type `0x0003`, protocol version `0x64`, and model value `0x3145`.

The firmware logs Sony manufacturer byte `mode22` and bit `0x40` as raw values. Do **not** treat bit `0x40` alone as proof of a completed local bond. Observed values during development included `0xa8`, `0xe8`, and `0xb8` under different camera/bond/location states.

## Services and characteristics

### Location service

```text
8000dd00-dd00-ffff-ffff-ffffffffffff
```

Known characteristics:

```text
DD11  location data write                         verified on A7R III 3.01
DD21  location configuration read                verified on A7R III 3.01
DD30  optional GPS enable/unlock                 not exposed on tested A7R III
DD31  optional GPS enable/lock                   not exposed on tested A7R III
```

### Control service / local time sync

```text
8000cc00-cc00-ffff-ffff-ffffffffffff
```

v7 also searches for:

```text
CC13  camera local-time synchronization          learned from public implementations; pending local validation
```

## Hardware-verified v6 sequence

```text
scan
  -> connect
  -> SMP bond/encrypt
  -> MTU exchange (request 158)
  -> discover DD00
  -> resolve DD11 and DD21
  -> optional DD21 read
  -> write 95-byte location packets to DD11
```

Negative findings on the tested A7R III:

- No EE00/EE01 pairing command was required for the successful GPS path.
- No GATT operation was required before SMP bonding.
- DD30 and DD31 were absent and therefore not required.
- DD11 was the required location write characteristic.

## Pairing breakthrough and v7 merge

Earlier NimBLE `secureConnection()` attempts failed. ESP-IDF Bluedroid progressed farther, and the first successful first-time pairing occurred with:

```text
GATTS enabled
ESP_LE_AUTH_REQ_SC_BOND
MITM disabled
IO capability: NoInputNoOutput
Key size: 16
Initiator key distribution: ENC + ID
Responder key distribution: ENC + ID
ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH disabled
```

v6 alternated:

```text
A: ESP_LE_AUTH_BOND
B: ESP_LE_AUTH_REQ_SC_BOND
```

Profile B was active when pairing succeeded. Because the A/B experiment does not isolate a single root cause, v7 keeps the **entire successful combination** and removes alternation.

## Bond persistence

Bluedroid bond information is stored in NVS. A full ESP32-C6 power removal/reconnect test confirmed bond persistence. On reconnect, Bluedroid may complete link encryption without a new first-pair `AUTH_CMPL`, so firmware also uses the encryption-complete callback plus the local bond database.

## DD11 location packet

Public implementations and our v6 packet agree on this layout.

### 95-byte form

| Offset | Length | Meaning |
|---:|---:|---|
| 0 | 1 | `0x00` |
| 1 | 1 | payload length `0x5D` |
| 2..4 | 3 | fixed `08 02 FC` |
| 5 | 1 | `0x03` when timezone/DST included |
| 6..10 | 5 | fixed `00 00 10 10 10` |
| 11..14 | 4 | latitude, signed E7, big endian |
| 15..18 | 4 | longitude, signed E7, big endian |
| 19..20 | 2 | UTC year, big endian |
| 21..25 | 5 | month/day/hour/minute/second |
| 26..90 | 65 | zero padding in known public implementations |
| 91..92 | 2 | standard UTC offset in minutes, signed big endian |
| 93..94 | 2 | DST offset in minutes, signed big endian |

### 91-byte form

When DD21 does not request timezone/DST:

```text
byte 1 = 0x59
byte 5 = 0x00
packet length = 91
```

The timezone/DST tail is omitted.

## DD21

A tested A7R III returned:

```text
06 10 00 9c 02 00 00
```

Public Alpha-GPS code interprets byte index 4 bit `0x02` as the flag requiring timezone/DST fields. v7 independently implements the same protocol behavior:

```text
(value[4] & 0x02) != 0  -> 95 bytes
otherwise               -> 91 bytes
```

This interpretation is **new in v7 and still needs local camera re-validation**.

## E7 coordinate precision

v7 stores the static test coordinate as signed integer E7 values instead of using a lower-precision decimal constant in the packet path. The public v7 test point is:

```text
25.0339687, 121.5644687
```

A new RAW capture is needed to confirm the A7R III preserves all seven decimal digits through DD11 into ARW GPS EXIF.

## CC13 local-time sync

v7 independently implements the observed 13-byte CC13 packet shape:

```text
0      packet payload length marker (12)
1..2   zero
3..4   local year
5      local month
6      local day
7      local hour
8      local minute
9      local second
10     DST active flag
11     signed standard UTC offset hours
12     absolute remaining offset minutes
```

The local date/time is computed from UTC + standard offset + DST. Because v7 still uses a static test coordinate, CC13 sync is **disabled by default** pending live GNSS integration and real-camera validation.

## 65-byte zero region

The Windows metadata UI exposes generic GPS fields such as altitude, DOP, speed, and track, but this alone does not prove Sony DD11 transports those fields. Current Alpha-GPS source explicitly writes `ByteArray(65)` (all zero) into this region, and public ILCE7M3 protocol research also documents it as zeros. Therefore v7 treats bytes 26..90 as reserved/zero padding rather than inventing unsupported fields.

## Privacy

Development logs contained the test camera's BLE MAC address. That address and the private validation RAW are intentionally omitted from the public repository.
