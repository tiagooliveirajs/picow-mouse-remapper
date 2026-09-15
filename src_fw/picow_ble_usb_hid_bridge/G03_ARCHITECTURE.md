# G03 architecture boundary

Gate 03 keeps all BTstack calls on Core 1 and gives the future LCD/HAT UI a BTstack-free contract on Core 0.

```text
LCD/HAT driver (future gate)
        |
        | BT_UI_GetSnapshot()
        | BT_UI_HandleAction()
        v
BluetoothUiController
        |
        v
BluetoothHost
        |
        +--> ClassicHidHost (G03 default)
        |
        +--> BLE HOG host (retained for later transport selection)
```

Device-list reads and UI actions never include BTstack headers. Commands that require BTstack are posted to `ClassicHidHost` and executed from the Core 1 run loop.

The physical HAT/display driver is intentionally not implemented here because no ST7789/HAT GPIO layer exists in the current firmware tree. G03 supplies the state model and actions that layer will consume.
