# Logitech Lift Forward -> Left Drag POC

This branch is intentionally a narrow proof of concept for the Logitech Lift.

## Goal

Use the Pico 2 W as a transparent BLE-to-USB mouse bridge while changing only the physical **Forward** button into a real **left mouse button press/hold/release**.

Expected behavior:

- tap Forward -> normal left click;
- hold Forward -> USB left button remains held;
- move the Lift while holding Forward -> drag;
- release Forward -> USB left button releases / drop;
- no host-side driver, daemon, Logitech Options, LogiOps, or Input Remapper is required.

## Why HID++ is used

The validated Lift setup in `infra-planner/docs/modules/rpi5.md` identified:

- physical Forward CID: `0x0056`;
- HID++ feature: `REPROG_CONTROLS_V4` (`0x1B04`);
- host-side LogiOps diversion as the mechanism that preserved press/hold/release semantics.

The ordinary mouse event path did not provide the required held-button behavior. This POC reproduces the essential part of that mechanism in firmware.

## What the firmware does

After the BLE HIDS connection is ready, the firmware:

1. sends HID++ `Root.GetFeature(0x1B04)` over long report `0x11`;
2. obtains the runtime feature index for `REPROG_CONTROLS_V4`;
3. sends `SetControlReporting` for CID `0x0056` with temporary diversion enabled;
4. consumes `DivertedButtonEvent` reports;
5. synthesizes HID Button 1 state in the ordinary mouse report sent over USB;
6. suppresses native Forward from the USB host.

A fallback also maps standard HID Button 5 to Button 1 if the mouse exposes proper held semantics there.

## Pairing for this POC

No display UI is added yet. The existing firmware already scans for BLE HID devices when it has no bonded target and stores the bonded device for reconnection.

For the first test:

1. flash the Pico 2 W;
2. connect its USB port to the test computer;
3. put the Lift into Bluetooth pairing mode;
4. let the Pico pair and bond automatically;
5. move the mouse once, then test Forward click and Forward-drag.

The on-board LED behavior from the original bridge remains available as the current connection-status indication.

A display/button pairing screen should only be added after the remap itself is proven on the physical Lift, and after the exact display/HAT model and GPIO mapping are known.

## UART diagnostics

Useful expected messages include:

```text
[POC] new HIDS connection ...
[POC] HID++ query sent: feature 0x1B04
[POC] HID++ feature 0x1B04 index=...
[POC] HID++ divert request sent: CID 0x0056
[POC] Lift Forward CID 0x0056 diverted; drag remap ACTIVE
[POC] Forward DOWN -> USB Left DOWN
[POC] Forward UP -> USB Left UP
```

If `drag remap ACTIVE` is never printed, capture the UART output before changing the protocol assumptions.

## Build target

The POC branch defaults to:

```cmake
PICO_BOARD=pico2_w
```

The source remains based on Pico SDK 2.2.0 and the existing BTstack/TinyUSB bridge.

## Validation status

Code is committed for the POC, but the HID++ transaction and final drag behavior still require validation on the physical Logitech Lift + Pico 2 W. Do not treat the POC as validated hardware behavior until that test passes.
