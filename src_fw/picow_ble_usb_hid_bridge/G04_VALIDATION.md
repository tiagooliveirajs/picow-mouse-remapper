# Gate 04 validation — persistent pairing and auto-reconnect

## Scope

G04 persists the identity of the last successfully initialized Bluetooth Classic
HID device and reuses the Pico SDK/BTstack TLV-backed Classic link-key database.
No application-owned copy of pairing keys is created.

The application record uses the `RHID` TLV tag and stores only:

- Bluetooth address;
- resolved device name (when available);
- Class of Device;
- classified HID device kind.

A remembered target is committed only after `HID_SUBEVENT_DESCRIPTOR_AVAILABLE`
succeeds and the bridge reaches `READY`. Reconnects that reproduce the same
record do not write flash again.

## Reconnect policy

1. On BTstack `HCI_STATE_WORKING`, load the remembered target.
2. If none exists, start normal discovery from G03.
3. If a target exists, connect immediately (`RECONNECTING`, attempt 1/4).
4. On failure, retry after 1 s, 2 s, then 4 s.
5. After attempt 4 fails, retain the remembered target but fall back to normal
   discovery/selection.
6. If a remembered device disconnects after a successful session, start the
   same reconnect cycle again.
7. `CANCEL`/`RESCAN` can abandon a reconnect cycle and return to discovery.
8. `FORGET` deletes only the application `RHID` target. Link-key lifecycle stays
   owned by BTstack's link-key database.

## Automated validation

The firmware build must pass for `pico2_w` with Pico SDK 2.2.0, including:

- `BluetoothPersistence.c` against the SDK-provided BTstack TLV API;
- Classic HID host/reconnect state machine;
- UI snapshot/action contract;
- USB HID bridge and retained BLE HOG source;
- UF2 generation and artifact upload.

## Deferred physical scenarios

The current firmware tree still has no ST7789/HAT driver, so pairing and device
selection cannot be exercised through the intended product UI yet. Do not use a
terminal-only substitute for acceptance testing.

When the GUI/HAT gate is available, validate these scenarios from the physical
buttons/display:

1. Pair/select a HID device, reach `READY`, power-cycle the Pico 2 W, and confirm
   automatic reconnect without returning to `SELECT DEVICE`.
2. Power-cycle with the remembered device off; confirm four reconnect attempts
   are represented in the UI and the firmware then falls back to scanning.
3. While `READY`, turn the HID device off and back on; confirm automatic recovery
   and resumed USB HID operation.
4. Use `CANCEL` or `RESCAN` during `RECONNECTING`; confirm the firmware stops the
   retry cycle and enters discovery.
5. Use `FORGET`, reboot, and confirm the firmware starts discovery instead of
   attempting the previously remembered target.
6. Select a different HID device, reach `READY`, reboot, and confirm the new
   device replaces the previous remembered target.
