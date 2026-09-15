# Sony BLE protocol notes

These notes separate **observed hardware behavior** from hypotheses. They document the A7R III path that was actually tested.

## Verified target

- Camera: Sony A7R III / `ILCE-7RM3`
- Camera firmware: `3.01`
- ESP32 board: Seeed XIAO ESP32-C6
- Framework: ESP-IDF / Bluedroid
- Tested local MTU request: `158`

## Advertisement observations

The A7R III manufacturer payload was observed with Sony company ID `0x012d`, camera type `0x0003`, protocol version `0x64`, and model value `0x3145`.

The firmware intentionally logs Sony manufacturer byte `mode22` and bit `0x40` as raw values. Do **not** assume bit `0x40` universally means "paired"; behavior appears mode/camera-generation dependent.

Observed values during development included `0xa8`, `0xe8`, and `0xb8` under different camera/bond/location states. These are useful diagnostic observations, not a complete semantic decode.

## Location service

Verified service UUID:

```text
8000dd00-dd00-ffff-ffff-ffffffffffff
```

Verified characteristics on the tested A7R III:

```text
DD11  location data write
DD21  optional configuration/diagnostic read
```

On the tested camera, `DD30` and `DD31` were not exposed and were not needed.

## Working sequence

The successful path was:

```text
scan
  -> connect
  -> SMP bond/encrypt
  -> MTU exchange (request 158)
  -> discover DD00 service
  -> resolve DD11 and DD21
  -> optional DD21 read
  -> write 95-byte location packets to DD11
```

Important negative findings:

- No Sony EE00/EE01 pairing command was needed for the successful A7R III GPS path.
- No GATT operation was required before SMP bonding.
- DD21 is optional for the location stream.
- DD11 is the required location write characteristic in the tested path.

## Pairing breakthrough

Earlier attempts using NimBLE `secureConnection()` consistently failed on the A7R III. Moving to ESP-IDF Bluedroid allowed the protocol to proceed, but initial Bluedroid attempts still hit SMP `RSP_TIMEOUT (0x63)`.

The first successful pairing occurred with:

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

The v6 diagnostic alternates two security profiles after a failed attempt:

```text
A: ESP_LE_AUTH_BOND
B: ESP_LE_AUTH_REQ_SC_BOND
```

Profile B was the profile present when first-time pairing succeeded. Because profile A's observed failure mode during that run was `REPEATED_ATTEMPT`, not the earlier `RSP_TIMEOUT`, the experiment does **not** prove whether GATTS support, SC capability, or their combination was the sole root cause. Production code should therefore retain the known-good combination rather than over-interpreting the A/B experiment.

## Bond persistence

Bluedroid bond information is stored in NVS. A full ESP32-C6 power removal/reconnect test confirmed that the existing bond survives reboot and the camera can return to encrypted operation without repeating the full first-pair ceremony.

On an existing bond, Bluedroid can report encryption through the GATT encryption-complete callback without a fresh first-time pairing exchange. The firmware therefore checks the local bond database before advancing to post-bond GATT setup.

## 95-byte location packet

The tested Sony location packet is exactly `95` bytes. The current PoC uses a static coordinate and a monotonically advancing UTC timestamp to isolate the Sony BLE path from GNSS parsing.

Latitude and longitude are encoded as signed 32-bit integer values scaled by `1e7`, then written in big-endian form in the packet.

The current PoC uses UTC offsets of zero. Real GNSS integration will source UTC from the receiver and can later add local timezone/DST handling separately.

## DD21 observation

On the tested camera, an example DD21 diagnostic read returned:

```text
06 10 00 9c 02 00 00
```

No interpretation is claimed here beyond recording the observed bytes.

## Privacy

Development logs contained the test camera's BLE MAC address. That address is intentionally omitted from this public repository.
