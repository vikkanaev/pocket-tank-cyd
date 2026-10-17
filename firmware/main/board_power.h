/* (this fork, 2026-10-17) The Waveshare ESP32-S3-Touch-LCD-1.69's power
 * latch. On battery the board stays on only while SYS_EN (GPIO41) is high;
 * the buzzer (GPIO42) is passive and must idle low. On every other board
 * both calls are empty. */
#pragma once
#include "sdkconfig.h"
#include "board_pins.h"

#if CONFIG_POCKET_TANK_WST169
/* first thing in app_main: SYS_EN output high and held (the hold keeps the
 * level through software resets - panic, WDT, esp_restart), buzzer output low */
void board_power_latch(void);
/* the deliberate power-off: hold released, SYS_EN driven low */
void board_power_release(void);
#else
static inline void board_power_latch(void) {}
static inline void board_power_release(void) {}
#endif
