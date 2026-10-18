/* pwr_logic.c — (this fork, 2026-10-17) the WST169's power logic; see
 * pwr_logic.h. Pure C, checked on the host by host_test/pwr_host.c. */
#include "pwr_logic.h"

void pwr_key_init(pwr_key_t *k, bool down_now) {
    *k = (pwr_key_t){ .armed = !down_now };   /* the power-on press is not a press: wait for its release */
}

void pwr_key_sample(pwr_key_t *k, bool down, int64_t now_us) {
    if (!k->armed) {                          /* disarmed (power-on press, swallowed press): only a release re-arms */
        if (!down) { k->armed = true; k->down = false; }
        return;
    }
    if (down && !k->down) { k->down = true; k->since_us = now_us; k->long_sent = false; return; }
    if (down) {
        if (!k->long_sent && now_us - k->since_us >= PWR_LONG_US) { k->pending = 2; k->long_sent = true; }
        return;
    }
    if (k->down) {                            /* the release */
        k->down = false;
        int64_t held = now_us - k->since_us;
        if (k->long_sent || held < PWR_MIN_PRESS_US) return;   /* after a long press: nothing; a blip: bounce */
        /* a hold of 1.5 s that was never sampled at 1.5 s (the sampler paused
           in a light sleep) is still a long press, at its release */
        int e = held >= PWR_LONG_US ? 2 : 1;
        if (e > k->pending) k->pending = e;   /* a long press waiting is never downgraded by a short one */
    }
}

int pwr_key_take(pwr_key_t *k) { int e = k->pending; k->pending = 0; return e; }

void pwr_key_swallow(pwr_key_t *k, bool down_now) {
    k->pending = 0;
    k->down = false; k->long_sent = false;
    k->armed = !down_now;                     /* a press still held is not counted at its release */
}

/* a Li-ion cell's resting curve, 3.30 V empty to 4.15 V full (the spec's
   ends), linear between the points */
static const struct { int mv; float f; } CURVE[] = {
    { 3300, 0.00f }, { 3450, 0.05f }, { 3600, 0.15f }, { 3700, 0.32f }, { 3750, 0.43f },
    { 3800, 0.55f }, { 3900, 0.70f }, { 4000, 0.82f }, { 4100, 0.94f }, { 4150, 1.00f },
};
float pwr_pct(int vbat_mv) {
    const int n = (int)(sizeof CURVE / sizeof CURVE[0]);
    if (vbat_mv <= CURVE[0].mv) return 0.0f;
    if (vbat_mv >= CURVE[n - 1].mv) return 1.0f;
    for (int i = 1; i < n; i++)
        if (vbat_mv <= CURVE[i].mv)
            return CURVE[i - 1].f + (CURVE[i].f - CURVE[i - 1].f) * (float)(vbat_mv - CURVE[i - 1].mv) / (float)(CURVE[i].mv - CURVE[i - 1].mv);
    return 1.0f;
}

bool pwr_battery_present(int vbat_mv) { return vbat_mv >= PWR_BAT_MIN_MV && vbat_mv <= PWR_BAT_MAX_MV; }

/* the board has no VBUS / charger line (the spec, clarification 2): "on the
   cable" = the USB host is seen, or VBAT is rising */
int pwr_state(bool usb_host, bool rising, int vbat_mv) {
    if (!usb_host && !rising) return BAT_ON_BATTERY;
    return vbat_mv >= PWR_FULL_MV ? BAT_FULL : BAT_CHARGING;
}

bool pwr_cutoff_due(pwr_cutoff_t *c, bool on_cable, int vbat_mv, int64_t now_us) {
    if (on_cable || !pwr_battery_present(vbat_mv) || vbat_mv >= PWR_EMPTY_MV) { c->low_since_us = 0; return false; }
    if (!c->low_since_us) { c->low_since_us = now_us + 1; return false; }   /* kept as start + 1 us: 0 means "not low", even for a start at t = 0 */
    return now_us - (c->low_since_us - 1) >= PWR_CUTOFF_US;
}

bool pwr_boot_too_low(bool on_cable, int vbat_mv) {
    return !on_cable && pwr_battery_present(vbat_mv) && vbat_mv < PWR_BOOT_LOW_MV;
}

void pwr_off_begin(pwr_off_t *o) { o->held_since_us = 0; o->up_since_us = 0; }

int pwr_off_sample(pwr_off_t *o, bool key_down, int64_t now_us) {
    if (key_down) {
        o->up_since_us = 0;                                  /* a bounce or a new press: the settle starts over */
        if (!o->held_since_us) o->held_since_us = now_us + 1;
        return now_us - (o->held_since_us - 1) >= PWR_OFF_STUCK_US ? PWR_OFF_STUCK : PWR_OFF_WAIT;
    }
    o->held_since_us = 0;
    if (!o->up_since_us) { o->up_since_us = now_us + 1; return PWR_OFF_WAIT; }
    return now_us - (o->up_since_us - 1) >= PWR_OFF_SETTLE_US ? PWR_OFF_STAYED_UP : PWR_OFF_WAIT;
}
