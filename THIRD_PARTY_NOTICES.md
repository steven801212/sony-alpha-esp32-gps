# Third-party references

This project is independently implemented after studying public Sony BLE reverse-engineering and open-source camera projects.

## Saschl/Alpha-GPS

- Repository: `Saschl/alpha-gps`
- License: **GNU GPL v3**
- Used as an interoperability/protocol behavior reference only.

For v7, Alpha-GPS was particularly useful for confirming observable Sony behaviors such as:

- DD11 latitude/longitude encoded as signed E7 big-endian integers
- a 65-byte zero-padding region in the known location packet
- DD21-driven selection of 91- vs 95-byte location packets
- optional DD30/DD31 handling when characteristics exist
- a separate CC13 camera local-time synchronization path
- 5-second location resend cadence in the current app implementation

**No Alpha-GPS source code is copied into this repository.** The ESP32-C6 implementation is independently written in C++/ESP-IDF. The MIT license in this repository does not relicense Alpha-GPS or any other GPL-covered source.

## Other references

- **anoulis/sony_camera_bluetooth_external_gps** — public Python/pygatt Sony external GPS implementation and working connect/bond/MTU/DD11 sequence.
- **whc2001/ILCE7M3ExternalGps** — public Sony ILCE-7M3 Bluetooth GPS protocol research and packet layout documentation.
- **gkoh/furble / freemote-related Sony BLE research** — Sony BLE interoperability context.
- **OpenClick Lite** — useful comparison for Sony remote-mode BLE pairing behavior on ESP32-C6.
- **Espressif ESP-IDF examples and API documentation** — Bluedroid GATT/SMP implementation reference.

No third-party source file is bundled verbatim. UUIDs, packet layouts and interoperability observations are protocol facts derived from public reverse-engineering plus independent hardware testing.

Each upstream project remains subject to its own license and notices. This repository's MIT license applies only to code and documentation authored for this project.
