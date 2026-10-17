/* (this fork, 2026-10-17) See board_power.h. */
#include "board_power.h"

#if CONFIG_POCKET_TANK_WST169
#include "driver/gpio.h"

void board_power_latch(void) {
    /* GPIO41's IO_MUX function 0 is JTAG MTDI: gpio_config switches the pad
     * to the GPIO function, gpio_set_direction alone would not */
    gpio_config_t en = { .pin_bit_mask = 1ULL << PIN_SYS_EN, .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    gpio_set_level(PIN_SYS_EN, 1);  /* the level first: the pad comes up driving high, no low glitch */
    gpio_config(&en);
    gpio_hold_en(PIN_SYS_EN);   /* a digital pad's hold (DIG_PAD_HOLD) survives a software reset; unlike
                                   gpio_deep_sleep_hold_dis() in app_main, which only clears the sleep-time autohold */
    gpio_config_t bz = { .pin_bit_mask = 1ULL << PIN_BUZZER, .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    gpio_set_level(PIN_BUZZER, 0);  /* likewise: never a high blip into the buzzer */
    gpio_config(&bz);
}

void board_power_release(void) {
    gpio_hold_dis(PIN_SYS_EN);  /* a held pad ignores the level: unfreeze first, then drive low */
    gpio_set_level(PIN_SYS_EN, 0);
}
#endif
