# Gate 01 validation

This gate validates the firmware base before Bluetooth Classic integration begins.

## Automated build

The GitHub Actions workflow `.github/workflows/firmware-build.yml` must complete successfully and produce:

- `picow_ble_usb_hid_bridge.uf2`
- `picow_ble_usb_hid_bridge.elf`
- `picow_ble_usb_hid_bridge.map`

## Expected USB topology

After flashing the Gate 01 firmware, the Pico W must enumerate as a composite USB device exposing:

- one HID interface;
- one CDC ACM interface for diagnostics.

On Linux, the CDC interface should normally appear as `/dev/ttyACM0` when no other ACM devices are connected.

## Manual physical validation

### T01 - Cold boot and USB enumeration

1. Disconnect the Pico W from USB.
2. Reconnect it normally, without BOOTSEL.
3. Run `dmesg -w` in another terminal while reconnecting.
4. Confirm that the USB device enumerates without repeated disconnect/reconnect loops.
5. Confirm that a `/dev/ttyACM*` device appears.

Pass criteria: the Pico W remains enumerated and a CDC ACM device exists.

### T02 - CDC diagnostic banner

1. Identify the ACM device with `ls -l /dev/ttyACM*`.
2. Open it with `picocom -b 115200 /dev/ttyACM0` (adjust the device number if needed).
3. Confirm that opening the port prints lines containing:
   - `CDC console opened`
   - `Pico W HID Remapper firmware 0.1.0-g01`
   - `Gate 01 base firmware diagnostics are active`
   - `USB interfaces: HID + CDC diagnostics; UART fallback enabled`

Pass criteria: all four diagnostic messages are readable and the firmware remains running.

### T03 - Status LED base behavior

1. Boot without a paired BLE HID device.
2. Observe the onboard Pico W LED.

Pass criteria: while the BLE application is not READY, the LED blinks at approximately 200 ms intervals.

### T04 - USB reconnect robustness

1. Keep `dmesg -w` running.
2. Disconnect and reconnect the Pico W USB cable five times.
3. After each reconnect, reopen `/dev/ttyACM*` with picocom.

Pass criteria: every cycle enumerates successfully, the CDC banner is printed, and there is no boot loop or permanently missing ACM interface.

### T05 - BOOTSEL reflash regression check

1. Disconnect the Pico W.
2. Hold BOOTSEL and reconnect USB.
3. Confirm that the `RPI-RP2` mass-storage device appears.
4. Copy the Gate 01 UF2 to the device.
5. Wait for the Pico W to reboot.
6. Confirm that `/dev/ttyACM*` returns and T02 still passes.

Pass criteria: firmware can still be reflashed through the standard BOOTSEL workflow and boots normally afterward.

## Notes

The Bluetooth host still writes its legacy `printf()` diagnostics to UART. Gate 01 intentionally keeps the new TinyUSB CDC logger confined to Core 0 to avoid cross-core access to the TinyUSB device stack. Cross-core diagnostic routing can be added later without changing the USB descriptor again.
