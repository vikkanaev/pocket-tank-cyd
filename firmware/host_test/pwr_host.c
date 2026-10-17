/* pwr_host.c - (this fork, 2026-10-17) host check of the WST169's power
 * logic (firmware/main/pwr_logic.c). Build + run from firmware/host_test:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../main -I../../common -o pwr_host pwr_host.c ../main/pwr_logic.c && ./pwr_host
 * Checks: a short press is 1 at release, a long hold is 2 at 1.5 s and
 * nothing at release; the power-on press and a swallowed press count for
 * nothing; the percent curve's ends, clamps and order; the battery-present
 * window; the 30 s cutoff and what resets it; the boot cutoff. */
#include <stdio.h>
#include "pwr_logic.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000LL
/* holds the key at `down` from t to t + dur, sampling every 10 ms; returns the
   first event taken (and the time it came), or 0 */
static int64_t t;
static int feed(pwr_key_t *k, bool down, int64_t dur, int64_t *at) {
    int got = 0;
    for (int64_t end = t + dur; t < end; t += 10 * MS) {
        pwr_key_sample(k, down, t);
        int e = pwr_key_take(k);
        if (e && !got) { got = e; if (at) *at = t; }
    }
    return got;
}

int main(void) {
    pwr_key_t k; int64_t at = 0, t0;

    /* a 300 ms press: nothing while down, 1 at the release */
    t = 1000 * MS; pwr_key_init(&k, false);
    feed(&k, false, 100 * MS, NULL);
    CHECK(feed(&k, true, 300 * MS, NULL) == 0, "a 300 ms press gave an event while held");
    t0 = t;
    CHECK(feed(&k, false, 100 * MS, &at) == 1 && at == t0, "a 300 ms press did not give 1 at its release");

    /* a 2 s hold: 2 at 1.5 s, nothing at the release */
    t0 = t;
    CHECK(feed(&k, true, 2000 * MS, &at) == 2, "a 2 s hold gave no long press");
    CHECK(at - t0 >= 1500 * MS && at - t0 < 1520 * MS, "the long press came at %lld ms, not 1.5 s", (long long)((at - t0) / MS));
    CHECK(feed(&k, false, 200 * MS, NULL) == 0, "the release after a long press gave an event");

    /* down at init (the power-on press): its release is nothing, the next press counts */
    t = 5000 * MS; pwr_key_init(&k, true);
    CHECK(feed(&k, true, 400 * MS, NULL) == 0, "the power-on press gave an event while held");
    CHECK(feed(&k, false, 100 * MS, NULL) == 0, "the power-on press's release gave an event");
    feed(&k, true, 200 * MS, NULL);
    CHECK(feed(&k, false, 100 * MS, NULL) == 1, "the press after the power-on press did not count");

    /* swallow while down: the release gives nothing, the next press counts */
    pwr_key_init(&k, false);
    feed(&k, true, 200 * MS, NULL);
    pwr_key_swallow(&k, true);
    CHECK(feed(&k, true, 100 * MS, NULL) == 0, "a swallowed press gave an event while held");
    CHECK(feed(&k, false, 100 * MS, NULL) == 0, "a swallowed press gave an event at its release");
    feed(&k, true, 200 * MS, NULL);
    CHECK(feed(&k, false, 100 * MS, NULL) == 1, "the press after a swallow did not count");
    /* swallow drops what was pending */
    feed(&k, true, 200 * MS, NULL); pwr_key_sample(&k, false, t); t += 10 * MS;
    pwr_key_swallow(&k, false);
    CHECK(pwr_key_take(&k) == 0, "swallow left a pending press");

    /* a 20 ms blip is contact bounce: nothing */
    pwr_key_init(&k, false); t = 20000 * MS;
    feed(&k, false, 50 * MS, NULL);
    feed(&k, true, 20 * MS, NULL);
    CHECK(feed(&k, false, 100 * MS, NULL) == 0, "a 20 ms press gave an event");
    /* a hold never sampled at 1.5 s (the sampler paused in a light sleep):
       2 at its release, and nothing more */
    pwr_key_sample(&k, true, t); CHECK(pwr_key_take(&k) == 0, "a press gave an event at its start");
    t += 2000 * MS;
    pwr_key_sample(&k, false, t);
    CHECK(pwr_key_take(&k) == 2, "a 2 s hold sampled only at its ends did not give 2 at the release");
    t += 10 * MS;
    CHECK(feed(&k, false, 200 * MS, NULL) == 0, "the late long press gave a second event");

    /* the percent curve */
    CHECK(pwr_pct(4150) == 1.0f, "pwr_pct(4150) = %f", (double)pwr_pct(4150));
    CHECK(pwr_pct(3300) == 0.0f, "pwr_pct(3300) = %f", (double)pwr_pct(3300));
    CHECK(pwr_pct(4300) == 1.0f, "pwr_pct(4300) = %f", (double)pwr_pct(4300));
    CHECK(pwr_pct(3000) == 0.0f, "pwr_pct(3000) = %f", (double)pwr_pct(3000));
    CHECK(pwr_pct(3700) > 0.0f && pwr_pct(3700) < 1.0f, "pwr_pct(3700) = %f", (double)pwr_pct(3700));
    { float last = -1; bool mono = true;
      for (int mv = 3300; mv <= 4150; mv++) { float p = pwr_pct(mv); if (p < last) mono = false; last = p; }
      CHECK(mono, "pwr_pct is not monotonic over 3300..4150"); }

    /* the battery-present window */
    CHECK(!pwr_battery_present(0), "0 mV is a battery");
    CHECK(!pwr_battery_present(4400), "4400 mV is a battery");
    CHECK(pwr_battery_present(3800), "3800 mV is not a battery");

    /* the cable guess */
    CHECK(pwr_state(false, false, 3800) == BAT_ON_BATTERY, "no host, not rising: not on battery");
    CHECK(pwr_state(true, false, 3800) == BAT_CHARGING, "the USB host at 3800: not charging");
    CHECK(pwr_state(false, true, 3800) == BAT_CHARGING, "rising at 3800: not charging");
    CHECK(pwr_state(true, false, 4160) == BAT_FULL, "the USB host at 4160: not full");

    /* the cutoff: 30 s below 3300 off the cable */
    { pwr_cutoff_t c = {0}; bool due29 = false, due30 = false;
      for (int s = 0; s <= 30; s++) { bool d = pwr_cutoff_due(&c, false, 3290, (int64_t)s * 1000000); if (s == 29) due29 = d; if (s == 30) due30 = d; }
      CHECK(!due29, "the cutoff was due at 29 s");
      CHECK(due30, "the cutoff was not due at 30 s");
      c = (pwr_cutoff_t){0};
      for (int s = 0; s <= 20; s++) pwr_cutoff_due(&c, false, 3290, (int64_t)s * 1000000);
      pwr_cutoff_due(&c, false, 3310, 21 * 1000000LL);              /* one good read */
      bool d = false;
      for (int s = 22; s <= 40; s++) d = pwr_cutoff_due(&c, false, 3290, (int64_t)s * 1000000) || d;
      CHECK(!d, "a single 3310 read did not reset the cutoff");
      CHECK(pwr_cutoff_due(&c, false, 3290, 52 * 1000000LL), "the cutoff did not restart after the reset");
      c = (pwr_cutoff_t){0}; d = false;
      for (int s = 0; s <= 120; s++) d = pwr_cutoff_due(&c, true, 3000, (int64_t)s * 1000000) || d;
      CHECK(!d, "the cutoff came on the cable");
      c = (pwr_cutoff_t){0}; d = false;
      for (int s = 0; s <= 120; s++) d = pwr_cutoff_due(&c, false, 0, (int64_t)s * 1000000) || d;
      CHECK(!d, "the cutoff came with no battery"); }

    /* the boot cutoff */
    CHECK(pwr_boot_too_low(false, 3150), "3150 off the cable at boot is not too low");
    CHECK(!pwr_boot_too_low(true, 3150), "3150 on the cable at boot is too low");
    CHECK(!pwr_boot_too_low(false, 0), "no battery at boot is too low");
    CHECK(!pwr_boot_too_low(false, 3250), "3250 off the cable at boot is too low");

    if (fails) { printf("pwr_host: %d FAILED\n", fails); return 1; }
    printf("pwr_host: all passed\n");
    return 0;
}
