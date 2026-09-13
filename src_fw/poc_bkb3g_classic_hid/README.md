# BKB-3G Bluetooth Classic HID Host POC

This POC validates that a Raspberry Pi Pico W can act as a Bluetooth Classic HID Host for the `BKB-3G` keyboard.

It is intentionally independent from the existing BLE/HOGP bridge and from the HAT/display code.

## Scope

Current stage:

1. Initialize Pico W + CYW43 + BTstack.
2. Run Bluetooth Classic inquiry (BR/EDR).
3. Find `BKB-3G` by EIR name, or resolve the remote name if it is not present in EIR.
4. Connect using BTstack Classic HID Host.
5. Handle SSP pairing and a simple legacy-PIN fallback.
6. Retrieve the HID descriptor.
7. Print raw HID reports over UART.
8. Parse non-zero HID fields using the received report descriptor.

Not implemented yet:

- TinyUSB HID output to the computer.
- Report remapping.
- Persistent device selection/UI on the HAT.
- Production-quality pairing UX.

## Build

Use the same Pico SDK/toolchain used by `src_fw/picow_ble_usb_hid_bridge`.

From this directory:

```sh
mkdir build
cd build
cmake -DPICO_BOARD=pico_w ..
cmake --build . -j
```

The generated UF2 should be:

```text
build/bkb3g_classic_hid.uf2
```

## Test procedure

1. Flash `bkb3g_classic_hid.uf2` to the Pico W.
2. Connect to the UART console.
3. Put the keyboard into pairing mode using `FN+1`, `FN+2`, or `FN+3` until the white LED blinks.
4. The Pico W repeatedly scans for Classic Bluetooth devices.
5. When `BKB-3G` is found, the POC opens a Classic HID connection.
6. If the UART prints a 6-digit pairing passkey, type that passkey on the BKB-3G and press Enter.
7. Press normal keys and special/function keys and capture the UART output.

Expected successful milestone:

```text
Target 'BKB-3G' found ...
Opening Classic HID connection ...
HID HOST CONNECTED ...
HID descriptor available (... bytes).
POC READY - press keys on the BKB-3G.

HID REPORT (... bytes):
...
  field: page=0x0007 usage=0x.... value=...
```

## Pairing notes

The BKB-3G manual describes selecting one of its three Bluetooth memories with `FN+1`, `FN+2`, or `FN+3`, then selecting `BKB-3G` from the host Bluetooth settings. It does not document a fixed PIN.

For Secure Simple Pairing, this POC exposes the Pico W as a display-capable host through the UART. If BTstack generates a passkey, enter it on the keyboard.

For legacy PIN pairing, the POC currently replies with `0000` as a diagnostic fallback. This can be changed after observing the actual pairing events from the keyboard.

## Next milestone

Once raw keyboard reports are confirmed, connect this Classic HID input path to the existing queue/TinyUSB architecture used by the BLE bridge:

```text
BKB-3G (Classic HID)
        |
        v
BTstack HID Host
        |
        v
report normalization / remapping
        |
        v
TinyUSB HID Device
        |
        v
USB host
```
