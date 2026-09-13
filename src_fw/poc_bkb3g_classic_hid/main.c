#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"

void bkb3g_classic_hid_init(void);

int main(void) {
    stdio_init_all();

    // Give an attached UART terminal a moment to come up after reset.
    sleep_ms(500);

    printf("\n=== Remapper BKB-3G Classic HID Host POC ===\n");
    printf("Target: BKB-3G\n");
    printf("Transport: Bluetooth Classic HID (BR/EDR)\n\n");

    if (cyw43_arch_init() != PICO_OK) {
        panic("cyw43_arch_init failed");
    }

    bkb3g_classic_hid_init();

    btstack_run_loop_execute();

    cyw43_arch_deinit();
    return 0;
}
