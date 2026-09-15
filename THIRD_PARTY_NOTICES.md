# Third-party references

This proof-of-concept was independently implemented after studying public Sony BLE reverse-engineering and open-source camera projects.

Key references:

- **Saschl/alpha-gps** — Sony camera geotagging application. Its documentation credits the external-GPS implementation below as a protocol basis.
- **anoulis/sony_camera_bluetooth_external_gps** — public Python/pygatt Sony external GPS implementation. The observed working sequence is connect, bond, MTU exchange, then 95-byte DD11 location writes.
- **whc2001/ILCE7M3ExternalGps** — public Sony ILCE-7M3 Bluetooth GPS protocol research.
- **gkoh/furble / freemote-related Sony BLE research** — useful context for Sony BLE behavior and interoperability.
- **OpenClick Lite** — useful comparison for Sony remote-mode BLE pairing behavior on ESP32-C6.
- **Espressif ESP-IDF examples and API documentation** — Bluedroid GATT/SMP implementation reference.

No third-party source file is bundled verbatim in this repository. UUIDs, packet layouts and interoperability observations are protocol facts derived from public reverse-engineering and independent hardware testing.

Each upstream project remains subject to its own license and notices. This repository's MIT license applies only to code and documentation authored for this project.
