#ifndef BOOT_DEBUG_H
#define BOOT_DEBUG_H

#include <stdbool.h>

void boot_debug_init(void);
void boot_debug_logf(const char *format, ...);
void boot_debug_task(void);
void boot_debug_wait_for_terminal(void);
bool boot_debug_terminal_connected(void);

#endif
