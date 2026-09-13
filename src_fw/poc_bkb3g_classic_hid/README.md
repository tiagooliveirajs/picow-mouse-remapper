# BKB-3G Bluetooth Classic HID -> USB HID POC

This POC validates a complete plug-and-play keyboard bridge on a Raspberry Pi Pico 2 W (RP2350):

```text
BKB-3G / Bluetooth keyboard 3.0
            |
            | Bluetooth Classic HID (BR/EDR)
            v
       Pico 2 W / RP2350
            |
            | TinyUSB HID Device
            v
      Windows / Linux / macOS / BIOS
```

The target hardware was confirmed from BOOTSEL `INFO_UF2.TXT` as:

```text
UF2 Bootloader v1.0
Model: Raspberry Pi RP2350
Board-ID: RP2350
```

The project therefore targets `pico2_w`, not `pico_w`.

This POC remains independent from the HAT/display UI. It reuses the proven TinyUSB descriptor and multicore queue infrastructure from `src_fw/picow_ble_usb_hid_bridge`.

## Current scope

Implemented:

1. Initialize Pico 2 W + CYW43 + BTstack.
2. Run Bluetooth Classic inquiry (BR/EDR).
3. Find the keyboard under either observed name: `Bluetooth keyboard 3.0` or `BKB-3G`.
4. Connect using BTstack Classic HID Host.
5. Handle SSP pairing and a legacy-PIN diagnostic fallback.
6. Retrieve the keyboard HID report descriptor.
7. Receive and parse Classic HID input reports.
8. Run BTstack on Core 1 and TinyUSB on Core 0.
9. Re-enumerate the USB device after the Bluetooth HID descriptor becomes available.
10. Expose the exact Bluetooth HID report descriptor to the USB host.
11. Forward Classic HID input reports through the multicore queue to TinyUSB HID.

This means normal keys, modifiers and report IDs produced by the BKB-3G can now be presented to the computer as a standard USB HID device without installing software on the host OS.

Not implemented yet:

- Remapping rules.
- Forwarding USB HID output reports back to Bluetooth (for example keyboard LED state such as Caps Lock).
- Persistent paired-device selection/UI on the HAT.
- Production-quality pairing UX and persistent configuration.

## Architecture

Core 1 owns the Bluetooth side:

```text
BTstack -> Classic HID Host -> raw HID report -> multicore HID queue
```

Core 0 owns the USB side:

```text
multicore HID queue -> TinyUSB HID Device -> USB host
```

When the Classic HID descriptor becomes available, Core 1 requests a USB re-enumeration. Core 0 briefly disconnects TinyUSB, clears reports queued against the old descriptor, and reconnects. The computer then requests the descriptor again and receives the BKB-3G descriptor.

## Build

Use Pico SDK 2.2.0 or a compatible version with Pico 2 W support.

If the SDK path is not present in the current shell:

```sh
export PICO_SDK_PATH="$HOME/pico/pico-sdk"
```

Always use a clean build directory after pulling this stage because the USB architecture and linked libraries changed:

```sh
cd ~/pico/remmaper-picow-ble-hid-gui/src_fw/poc_bkb3g_classic_hid
rm -rf build
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
cmake --build . -j"$(nproc)"
```

The CMake file intentionally rejects other board targets.

Generated UF2:

```text
build/bkb3g_classic_hid.uf2
```

Before flashing, verify the target:

```sh
picotool info -a bkb3g_classic_hid.elf
```

Expected key information:

```text
name:              bkb3g_classic_hid
pico_board:        pico2_w
```

## Flash

1. Hold BOOTSEL while connecting the Pico 2 W over USB.
2. The bootloader volume should appear as `RP2350`.
3. Copy `bkb3g_classic_hid.uf2` to that volume.
4. The BOOTSEL volume should disappear and the application should start.

`INDEX.HTM` and `INFO_UF2.TXT` are virtual bootloader files. Old UF2 applications are not stored there as ordinary files.

## Important change: no USB serial console

USB is now used by TinyUSB as the actual HID device. Therefore this POC deliberately disables Pico SDK USB stdio.

After flashing this version, `/dev/ttyACM0` is **not expected**. This is intentional.

Diagnostic `printf` output remains available on UART0:

- GPIO0 = UART0 TX
- GPIO1 = UART0 RX
- 115200 baud
- 3.3 V TTL

For normal validation, an external UART adapter is not required: test the actual USB keyboard behavior directly on the computer.

## Test procedure

1. Build and flash the new UF2.
2. Let the Pico 2 W boot normally through its USB cable.
3. Put the BKB-3G into pairing mode using `FN+1`, `FN+2`, or `FN+3` until its LED blinks.
4. Wait for the keyboard LED to stop blinking.
5. When the HID descriptor is acquired, the Pico deliberately re-enumerates its USB connection. A brief USB disconnect/reconnect is expected.
6. Open a text editor on the host computer.
7. Type `abc`, space, Enter, Shift combinations and other normal keys on the BKB-3G.
8. The text should now appear through the Pico 2 W as a USB keyboard.

On Linux, USB enumeration can also be inspected with:

```sh
lsusb
```

or, while plugging/re-enumerating the Pico:

```sh
sudo dmesg -w
```

## Reports already confirmed from the BKB-3G

The Classic HID POC observed the standard keyboard Report ID `0x01` and normal keyboard usages, including examples such as:

```text
0x04 = A
0x05 = B
0x28 = Enter
0x2C = Space
0xE1 = Left Shift
```

The current bridge forwards the HID report itself rather than translating individual key codes, so the host receives the device's report structure directly.

## Pairing notes

The keyboard has been observed using the Bluetooth Classic name `Bluetooth keyboard 3.0`. The firmware also continues accepting the alias `BKB-3G`.

For Secure Simple Pairing, if BTstack produces a passkey it must be typed on the Bluetooth keyboard followed by Enter. For legacy PIN pairing the current diagnostic fallback is `0000`.

## Next milestone

Once USB typing is confirmed, the next layer is the actual Remapper transformation pipeline:

```text
Classic HID input
       |
       v
parse / normalize
       |
       v
remapping rules
       |
       v
USB HID output
```

That is where keyboard mappings and, later, the Logitech Lift mouse transformations should be inserted.
