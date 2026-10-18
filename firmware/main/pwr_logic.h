/* pwr_logic.h — (this fork, 2026-10-17) the WST169's power logic, pure C so
 * firmware/host_test/pwr_host.c can check it on the host: the PWR key's
 * press detector (GPIO 40 is a plain level, the board has no PMIC to time
 * it), the VBAT -> percent curve, the cable guess and the low-battery
 * cutoff. battery_port_adc.c feeds it; no ESP-IDF header here. */
#ifndef PWR_LOGIC_H
#define PWR_LOGIC_H
#include <stdbool.h>
#include <stdint.h>
#include "battery.h"            /* BAT_* */

#define PWR_LONG_US        1500000      /* the long press: 1.5 s held */
#define PWR_MIN_PRESS_US   30000        /* shorter is contact bounce, not a press */
#define PWR_FULL_MV        4150         /* 100 % */
#define PWR_EMPTY_MV       3300         /* 0 %, and the cutoff */
#define PWR_BAT_MIN_MV     2500         /* outside 2.5..4.35 V: no battery */
#define PWR_BAT_MAX_MV     4350
#define PWR_CUTOFF_US      30000000     /* below 3.30 V this long off the cable: power off */
#define PWR_BOOT_LOW_MV    3200         /* at boot, below this off the cable: power off at once */

/* the key: one detector, shared by the tank and the dark loop. "short" (1) is
 * queued at the release of a press under 1.5 s, "long" (2) the moment the
 * hold reaches 1.5 s; the release after a long press gives nothing. */
typedef struct { bool down; bool armed; int64_t since_us; bool long_sent; int pending; } pwr_key_t;
void pwr_key_init(pwr_key_t *k, bool down_now);          /* down at boot (the power-on press): not armed until released */
void pwr_key_sample(pwr_key_t *k, bool down, int64_t now_us); /* feeds one sample; queues 1 (short, at release < 1.5 s) or 2 (long, at 1.5 s held) */
int  pwr_key_take(pwr_key_t *k);                         /* 0 / 1 / 2, then clears */
void pwr_key_swallow(pwr_key_t *k, bool down_now);       /* drop pending; if down, disarm until released */

float pwr_pct(int vbat_mv);                              /* 0..1, piecewise Li-ion, 4150 = 1, 3300 = 0, clamped */
bool pwr_battery_present(int vbat_mv);                   /* 2500..4350 */
int  pwr_state(bool usb_host, bool rising, int vbat_mv); /* BAT_ON_BATTERY / BAT_CHARGING / BAT_FULL (>= 4150 on the cable) */

/* low_since_us: when VBAT went below the cutoff, + 1 us; 0 = not below (a zeroed struct is the start) */
typedef struct { int64_t low_since_us; } pwr_cutoff_t;
bool pwr_cutoff_due(pwr_cutoff_t *c, bool on_cable, int vbat_mv, int64_t now_us); /* true after 30 s below 3300 off the cable; on the cable or no battery: never, and the timer resets */
bool pwr_boot_too_low(bool on_cable, int vbat_mv);       /* off the cable and present and < 3200 */

/* (this fork, 2026-10-18) after SYS_EN goes low: the PWR key holds the rails
 * up while it is down (Key2 -> D3 -> Q5), so "still powered, so it is USB"
 * is a verdict only after the key is released and the board has stayed up
 * PWR_OFF_SETTLE_US more. A key held PWR_OFF_STUCK_US is taken as stuck: the
 * caller stops waiting, but must not latch SYS_EN again (on battery the
 * board then drops at the release, as the keeper meant). Sample every few ms
 * from pwr_off_begin on; a held sample restarts the settle wait. */
#define PWR_OFF_SETTLE_US  1000000      /* released this long and still up: on the cable */
#define PWR_OFF_STUCK_US   60000000     /* held this long: a stuck key, stop waiting */
enum { PWR_OFF_WAIT, PWR_OFF_STAYED_UP, PWR_OFF_STUCK };
/* held_since_us / up_since_us: + 1 us, 0 = not in that state (as pwr_cutoff_t) */
typedef struct { int64_t held_since_us; int64_t up_since_us; } pwr_off_t;
void pwr_off_begin(pwr_off_t *o);
int  pwr_off_sample(pwr_off_t *o, bool key_down, int64_t now_us); /* PWR_OFF_WAIT / _STAYED_UP / _STUCK */

#endif
