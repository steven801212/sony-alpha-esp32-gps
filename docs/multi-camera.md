# Multi-camera development branch

This branch is an experimental refactor of the hardware-validated v7 firmware so one XIAO ESP32-C6 can maintain up to two Sony camera sessions while sharing one location/time source.

## Architecture

```text
shared GNSS / test fix
        |
   ESP32-C6
   |       |
Camera 1  Camera 2
DD21      DD21
DD11      DD11
```

Each camera has an independent session containing its Bluetooth address, connection ID, state machine, Sony service handles, DD21 result, optional DD30/DD31/CC13 handles, retry state, and transmit state. A camera disconnect or pairing failure therefore does not reset the other camera session.

Connection setup is intentionally serialized: the firmware discovers and fully prepares one camera, then resumes scanning for the second while keeping the first link alive. DD11 location writes are also serialized and separated by 30 ms so both cameras receive the same shared fix without overlapping write-with-response transactions.

## Current status

- `MAX_CAMERAS = 2`
- Bluedroid ACL connection requirement is explicitly set to 2 in `sdkconfig.defaults`.
- Existing v7 Sony protocol behavior is preserved per session: bond/encryption, MTU 158, DD00/CC00 discovery, DD21 91/95-byte selection, optional DD30/DD31/CC13, and DD11 writes.
- The static v7 Taipei E7 test fix remains in place for regression testing.
- The branch is **not yet dual-camera hardware validated**.

## Validation order

1. Build in CI.
2. Regression-test with one A7R III to ensure v7 behavior is unchanged.
3. Pair camera A and confirm `[C1] ... READY` and repeated `[C1][TX] ... OK`.
4. Leave camera A connected, put camera B into pairing mode, and confirm a second connection ID appears as `[C2]`.
5. Confirm both sessions independently read DD21 and reach READY.
6. Confirm alternating `[C1][TX]` and `[C2][TX]` writes succeed for several minutes.
7. Power-cycle and verify both stored bonds reconnect without re-pairing.
8. Turn either camera off and confirm the remaining camera keeps receiving DD11 updates.

Do not merge this branch into the v7 release baseline until steps 2–8 are completed on real hardware.
