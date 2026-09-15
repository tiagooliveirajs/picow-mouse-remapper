# Gate 03 validation

Gate 03 replaces the fixed BKB-3G target from G02 with a generic Bluetooth Classic HID discovery, selection and pairing backend.

## Implemented

- Generic Bluetooth Classic inquiry instead of matching a fixed device name.
- Up to 20 discovered devices retained for presentation by the future LCD GUI.
- Device metadata exposed without BTstack types: name, address, RSSI, Class of Device, HID-candidate flag and keyboard/mouse classification.
- HID candidates identified from Bluetooth Peripheral Class of Device and common HID name hints.
- Remote-name resolution before entering the device-selection state.
- Explicit selected-device index; no automatic connection to BKB-3G.
- Selection APIs for previous/next/direct selection and confirmation.
- Core-safe command handoff: UI calls can originate on Core 0 while all BTstack calls remain on Core 1.
- Pairing models for legacy PIN, numeric confirmation, displayed passkey and passkey input.
- Numeric confirmation is no longer silently auto-accepted.
- Legacy PIN `0000` is exposed to the UI and only sent after confirmation.
- Pairing accept/reject and passkey submission APIs.
- `BluetoothUiController` maps future HAT actions (`UP`, `DOWN`, `CONFIRM`, `CANCEL`, `RESCAN`) to the Bluetooth backend.
- `BluetoothUiController` exposes one snapshot containing state text, discovered devices, selected device and pairing information for LCD rendering.
- USB HID descriptor forwarding and multicore HID report queue from G02 remain unchanged.

## Current UI boundary

The repository does not yet contain the ST7789/HAT GPIO/display driver. G03 therefore implements the complete UI-facing contract but does not invent temporary GPIO mappings or terminal commands. The later LCD/HAT gate only needs to render `BT_UI_GetSnapshot()` and translate physical button events to `BT_UI_HandleAction()`.

## Automated acceptance

G03 passes when CI builds the main `pico2_w` firmware with Pico SDK 2.2.0, produces the UF2 artifact, and the G03 modules compile together with both the Classic HID and retained BLE HOG implementations.

## Physical validation policy

No terminal-driven physical testing is required. Physical validation is intentionally deferred until the LCD/HAT GUI is available.

When that GUI exists, the G03 LCD-only scenarios are:

1. Boot shows a scanning state.
2. After discovery, the LCD shows a selectable list of likely HID devices.
3. Up/down HAT actions change the selected HID device.
4. Confirm starts connection to the selected device only.
5. Numeric confirmation is displayed and requires an explicit confirm/cancel action.
6. Legacy PIN flow displays `0000` before confirmation.
7. Display-passkey flow shows the six-digit passkey for entry on the remote keyboard.
8. Pairing/connection progress transitions through CONNECTING, PAIRING, CONNECTED and READY.
9. On connection error the LCD shows ERROR and offers rescan.
10. On disconnect, G03 returns to discovery; persistent reconnection belongs to G04.
