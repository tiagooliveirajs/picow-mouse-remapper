# Gate 02 — Classic HID integration

## Objective

Incorporate the proven `poc_bkb3g_classic_hid` solution into the main firmware without coupling TinyUSB/GUI code to a specific Bluetooth transport.

## Implemented

- Ported the Classic HID Host flow into `ClassicHidHost.c/.h`.
- Preserved the POC target names `Bluetooth keyboard 3.0` and `BKB-3G` for Gate 02.
- Added BR/EDR inquiry and remote-name resolution.
- Added Classic HID connection/incoming-connection handling.
- Added legacy PIN fallback (`0000`) and SSP handling.
- Added in-memory pairing passkey/state accessors for a future LCD GUI.
- Added retrieval and exposure of the Classic HID report descriptor.
- Added forwarding of Classic HID input reports into the existing multicore HID queue.
- Preserved USB re-enumeration when the remote HID descriptor becomes available or disappears.
- Added `BluetoothHost.c/.h` as a transport-neutral facade between USB/application code and Bluetooth implementations.
- Kept the existing BLE HOG source compiled for later transport selection.
- Switched the production target to the Pico 2 W / RP2350 hardware validated by the POC.
- Updated the CI build to `pico2_w` with Pico SDK 2.2.0.

## Architecture after G02

```text
Core 1
  BluetoothHost
      |
      +-- ClassicHidHost (selected in G02)
      |
      +-- BLE HOG host (retained for later selection)
      |
      v
  Common HID queue
      |
      v
Core 0
  TinyUSB composite device
      +-- HID
      +-- CDC diagnostics
      |
      v
  USB host
```

The USB descriptor code no longer knows whether the descriptor came from BLE or Bluetooth Classic. It queries `BluetoothHost` for readiness, descriptor data and descriptor length.

## Automated acceptance criteria

- [ ] CMake configures for `pico2_w` with Pico SDK 2.2.0.
- [ ] Main firmware compiles with both Classic HID and the retained BLE HOG module.
- [ ] UF2 output is generated.
- [ ] CI artifact upload succeeds.
- [ ] No BTstack/TinyUSB `hid_report_type_t` collision is introduced.
- [ ] The original multicore HID queue remains linked into the production target.
- [ ] Classic pairing passkey/state can be queried without exposing BTstack types to UI code.

## Physical validation policy

Per project workflow, no terminal-driven manual tests are required. Physical pairing and state validation will be performed through the LCD/HAT GUI once that interface is implemented. Gate 02 therefore finishes with automated build/integration validation and leaves physical interaction validation deferred to the GUI gate.

## Deferred to Gate 03+

- Generic device discovery and selection rather than fixed BKB-3G names.
- User-controlled pairing workflow.
- Persistent paired-device selection.
- LCD presentation of discovery, passkey and connection state.
- Runtime selection between Classic HID and BLE HOG transports.
