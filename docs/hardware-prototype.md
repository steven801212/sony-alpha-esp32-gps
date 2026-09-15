# Portable hardware prototype

This document describes the **planned next-stage portable prototype**. The Sony BLE path is already verified; the MAX-M10S GNSS integration described here is the next implementation step.

## Controller

- Seeed XIAO ESP32-C6
- 4 MB flash
- USB-C programming and LiPo charging
- ESP-IDF / Bluedroid

## GNSS target

Preferred first portable prototype:

- u-blox **MAX-M10S** generation GNSS breakout
- Prefer an **integrated-antenna breakout** for the first carry-around prototype, to avoid loose coax/IPEX wiring and RF-layout work.
- The exact breakout supply voltage must be verified from the seller/manufacturer documentation. A raw MAX-M10S device is not a 5 V IC; some breakout boards add their own regulator.

## UART wiring

XIAO ESP32-C6 official UART pins:

```text
D6 = GPIO16 = TX
D7 = GPIO17 = RX
```

Connect UART cross-over:

```text
MAX-M10S TX  -> XIAO D7 / GPIO17 / RX
MAX-M10S RX  <- XIAO D6 / GPIO16 / TX
MAX-M10S GND -> XIAO GND
```

Both UART directions are recommended. RX-only would be enough to read NMEA, but the project intends to configure the GNSS receiver using UBX commands for update rate and low-power behavior.

## GNSS power

For a breakout explicitly specified to accept **3.3 V input**:

```text
XIAO 3V3 -> GNSS VCC
XIAO GND -> GNSS GND
```

Do **not** connect an unknown breakout to 5 V just because it carries a MAX-M10S module. Verify the breakout input stage first.

Also note that on the XIAO ESP32-C6, battery-powered operation should not rely on the `5V/VBUS` pin remaining powered when USB is disconnected. For the planned battery prototype, a 3.3 V-compatible GNSS breakout is therefore convenient.

## Battery and charging

The XIAO ESP32-C6 has battery pads on the **back** of the board. They are separate from the D0-D10 side pins.

```text
1S LiPo red   -> BAT+
1S LiPo black -> BAT-
```

The XIAO USB-C port can then be used for charging the attached 1S LiPo.

For the first prototype, no separate TP4056 or external charger is required.

A 500–1000 mAh 1S LiPo is a reasonable prototype range. Final runtime must be measured on real hardware rather than inferred from module headline specifications.

## Physical layout

For an integrated ceramic-patch GNSS board:

```text
+-----------------------------------+
| GNSS patch antenna   -> sky side  |
|                                   |
|        battery (below/away)       |
|                                   |
|                 XIAO ESP32-C6     |
|                 BLE antenna end   |
+-----------------------------------+
```

Guidelines:

- Use a plastic enclosure.
- Keep the GNSS patch antenna facing outward/upward.
- Avoid placing LiPo foil, a large ground plane, or metal directly over the patch antenna's sky side.
- Put the XIAO BLE antenna and GNSS antenna toward opposite ends of the enclosure where practical.
- Aim for roughly 20–30 mm or more separation in the first prototype if enclosure size allows.

## Update rate and power strategy

The camera does not need a 1 Hz fresh GNSS fix for normal photo geotagging. The target is approximately **5 or 10 seconds per fresh GNSS position**.

Do not hard-power-cycle the GNSS receiver every 5–10 seconds. That can repeatedly pay acquisition/TTFF cost and may consume more average energy than a supported cyclic/power-save tracking mode.

The intended firmware architecture is:

```text
MAX-M10S low-power/cyclic tracking
        -> valid GNSS fix + UTC
        -> ESP32-C6
            -> Sony DD11 location stream
            -> optional track log
```

## Optional external SPI flash

The XIAO's internal flash is enough for early logging experiments. A later PCB may add SPI NOR flash such as 16 MB or 32 MB for circular track storage.

Possible SPI assignment:

```text
D8  / GPIO19 / SCK  -> Flash CLK
D10 / GPIO18 / MOSI -> Flash DI
D9  / GPIO20 / MISO <- Flash DO
D3  / GPIO21        -> Flash CS
3V3                 -> Flash VCC
GND                 -> Flash GND
```

This does not conflict with the GNSS UART on D6/D7.

For flash endurance and power efficiency, the firmware should buffer multiple binary track records in RAM and write them in batches rather than performing an erase/write cycle for every individual fix.

## Time and timezone

GNSS supplies UTC directly. Sony GPS EXIF timestamps should remain UTC.

A later feature can map latitude/longitude to an offline IANA timezone identifier and apply DST rules for local-time display or camera clock assistance. This is separate from the core GPS EXIF timestamp path.
