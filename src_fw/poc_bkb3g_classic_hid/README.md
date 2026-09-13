# BKB-3G Bluetooth Classic HID Host POC

This POC validates that a Raspberry Pi Pico 2 W (RP2350) can act as a Bluetooth Classic HID Host for the `BKB-3G` keyboard.

The target hardware was confirmed from BOOTSEL `INFO_UF2.TXT` as:

```text
UF2 Bootloader v1.0
Model: Raspberry Pi RP2350
Board-ID: RP2350
```

The project therefore targets `pico2_w`, not `pico_w`.

It is intentionally independent from the existing BLE/HOGP bridge and from the HAT/display code.

## Scope

Current stage:

1. Initialize Pico 2 W + CYW43 + BTstack.
2. Run Bluetooth Classic inquiry (BR/EDR).
3. Find `BKB-3G` by EIR name, or resolve the remote name if it is not present in EIR.
4. Connect using BTstack Classic HID Host.
5. Handle SSP pairing and a simple legacy-PIN fallback.
6. Retrieve the HID descriptor.
7. Print logs and raw HID reports over USB CDC (and UART as fallback).
8. Parse non-zero HID fields using the received report descriptor.

Not implemented yet:

- TinyUSB HID output to the computer.
- Report remapping.
- Persistent device selection/UI on the HAT.
- Production-quality pairing UX.

## Build

Use Pico SDK 2.2.0 or a compatible version with Pico 2 W support.

From this directory, always remove an old build directory when changing board targets:

```sh
rm -rf build
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
cmake --build . -j
```

The CMake file intentionally rejects other board targets so this POC cannot accidentally be built for RP2040/Pico W.

The generated UF2 should be:

```text
build/bkb3g_classic_hid.uf2
```

Before flashing, verify the build target:

```sh
picotool info -a bkb3g_classic_hid.elf
```

Expected key information includes:

```text
name:              bkb3g_classic_hid
pico_board:        pico2_w
```

The output should identify an RP2350/Pico 2 W build rather than `pico_w`.

## Flash

1. Hold BOOTSEL while connecting the Pico 2 W over USB.
2. The bootloader volume should appear as `RP2350`.
3. Copy `bkb3g_classic_hid.uf2` to that volume.
4. After a valid RP2350 UF2 is accepted, the BOOTSEL volume should disappear and the application should start.

`INDEX.HTM` and `INFO_UF2.TXT` on the BOOTSEL volume are virtual bootloader files. Previous UF2 applications are not stored there as normal files and do not need to be deleted.

## USB log console

This POC enables USB CDC stdio, so an external USB-UART adapter is not required.

After flashing and normal application boot, check for the serial device:

```sh
ls /dev/ttyACM* 2>/dev/null
```

Usually it will be:

```text
/dev/ttyACM0
```

Open it with:

```sh
picocom -b 115200 /dev/ttyACM0
```

If permission is denied, add the user to `dialout`, log out, and log back in:

```sh
sudo usermod -aG dialout "$USER"
```

UART0 on GPIO 0/1 remains enabled only as a fallback debugging path.

## Test procedure

1. Flash `bkb3g_classic_hid.uf2` to the Pico 2 W.
2. Open `/dev/ttyACM0` with `picocom`.
3. Put the keyboard into pairing mode using `FN+1`, `FN+2`, or `FN+3` until the white LED blinks.
4. The Pico 2 W repeatedly scans for Classic Bluetooth devices.
5. When `BKB-3G` is found, the POC opens a Classic HID connection.
6. If the console prints a 6-digit pairing passkey, type that passkey on the BKB-3G and press Enter.
7. Press normal keys and special/function keys and capture the console output.

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

For Secure Simple Pairing, this POC exposes the Pico 2 W as a display-capable host through the console. If BTstack generates a passkey, enter it on the keyboard.

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
