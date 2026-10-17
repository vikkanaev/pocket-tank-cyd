/* battery_port_adc.c — (this fork, 2026-10-17) the battery_port.h contract on
 * the Waveshare ESP32-S3-Touch-LCD-1.69 (CONFIG_POCKET_TANK_WST169), which has
 * no PMIC: VBAT comes through a 1/3 divider into ADC1_CH0 (GPIO 1), the PWR
 * key is a plain level on GPIO 40, and power-off is the SYS_EN latch
 * (board_power.c). The numbers and the press detector live in pwr_logic.c,
 * checked on the host (host_test/pwr_host.c).
 *
 * VBAT: 16 one-shot samples at 12 dB, averaged, curve-fitting calibrated,
 * x 3; cached ~1 s like the AXP2101's port. "On the cable" has no line of
 * its own on this board (the spec, clarification 2): the USB host is seen
 * (usb_serial_jtag_is_connected), or VBAT rose >= 15 mV over a 3-minute
 * window of the cached reads. A wall charger therefore shows "charging"
 * only after a few minutes, and a cell resting at full on a wall charger
 * (no host, no longer rising) reads as on battery at 100 %. */
#include "battery_port.h"
#include "pwr_logic.h"
#include "board_pins.h"
#include "board_power.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "battery"
#define BAT_ADC_CHAN      ADC_CHANNEL_0      /* GPIO 1 = PIN_BAT_ADC */
#define BAT_SAMPLES       16
#define BAT_DIVIDER       3                  /* VBAT = ADC mV x 3 */
#define BAT_CACHE_US      900000             /* the same ~1 s cache as the AXP2101's port */
#define RISE_BUCKET_US    10000000LL         /* the rising window: 10 s buckets of the 1 s reads ... */
#define RISE_BUCKETS      19                 /* ... the newest against the one 3 minutes older */
#define RISE_MV           15
#define KEY_PERIOD_US     10000              /* the PWR key sampler: every 10 ms */

_Static_assert(PIN_BAT_ADC == 1, "ADC1_CH0 is GPIO 1 on the ESP32-S3");

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;               /* NULL = the linear estimate */
static SemaphoreHandle_t s_lock;               /* the ADC and the cache: the tank task and the console both read */
static int64_t s_last_us = -1;
static int s_mv, s_raw, s_state = BAT_ON_BATTERY;
static bool s_valid, s_usb, s_rising;
/* the rising window: each bucket is the mean of the reads that fell in it */
static int32_t s_bkt_sum[RISE_BUCKETS]; static int16_t s_bkt_n[RISE_BUCKETS];
static int s_bkt_head = -1, s_bkt_count; static int64_t s_bkt_start;

static int sample_mv(int *raw_out) {
    int32_t sum = 0; int n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) == ESP_OK) { sum += raw; n++; }
    }
    if (!n) { *raw_out = -1; return 0; }
    int raw = (int)(sum / n), mv;
    *raw_out = raw;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK)
        mv = raw * 3100 / 4095;                /* 12 dB spans roughly 0..3.1 V: an estimate, said so at init */
    return mv * BAT_DIVIDER;
}

static bool rising_after(int64_t now, int mv) {
    if (s_bkt_head < 0 || now - s_bkt_start >= RISE_BUCKET_US) {
        s_bkt_head = (s_bkt_head + 1) % RISE_BUCKETS;
        s_bkt_sum[s_bkt_head] = 0; s_bkt_n[s_bkt_head] = 0; s_bkt_start = now;
        if (s_bkt_count < RISE_BUCKETS) s_bkt_count++;
    }
    s_bkt_sum[s_bkt_head] += mv; s_bkt_n[s_bkt_head]++;
    if (s_bkt_count < RISE_BUCKETS) return false;          /* under 3 minutes of history: not yet */
    int old = (s_bkt_head + 1) % RISE_BUCKETS;             /* the oldest bucket */
    return s_bkt_sum[s_bkt_head] / s_bkt_n[s_bkt_head] - s_bkt_sum[old] / s_bkt_n[old] >= RISE_MV;
}

/* the cache: refreshed at most once a second, under the lock */
static void refresh(bool force) {
    if (!s_adc) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    if (force || s_last_us < 0 || now - s_last_us > BAT_CACHE_US) {
        s_last_us = now;
        int raw, mv = sample_mv(&raw);
        s_raw = raw; s_mv = mv;
        s_valid = raw >= 0 && pwr_battery_present(mv);
        s_usb = usb_serial_jtag_is_connected();
        s_rising = s_valid && rising_after(now, mv);
        s_state = s_valid ? pwr_state(s_usb, s_rising, mv) : BAT_ON_BATTERY;
    }
    xSemaphoreGive(s_lock);
}

bool battery_port_init(i2c_master_bus_handle_t bus) {
    (void)bus;                                 /* no PMIC on the bus */
    if (s_adc) return true;
    s_lock = xSemaphoreCreateMutex();
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (!s_lock || adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) { s_adc = NULL; ESP_LOGW(TAG, "VBAT ADC (ADC1) did not start: meter hidden"); return false; }
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &ccfg) != ESP_OK) {
        adc_oneshot_del_unit(s_adc); s_adc = NULL;
        ESP_LOGW(TAG, "VBAT ADC channel (GPIO %d) did not configure: meter hidden", PIN_BAT_ADC);
        return false;
    }
    adc_cali_curve_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .chan = BAT_ADC_CHAN, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    esp_err_t ce = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (ce != ESP_OK) { s_cali = NULL; ESP_LOGW(TAG, "VBAT ADC calibration failed (%s): using a LINEAR raw->mV estimate, VBAT may be off by tens of mV", esp_err_to_name(ce)); }
    refresh(false);
    ESP_LOGI(TAG, "VBAT on ADC1_CH0 (GPIO %d, x%d divider, %s): %d mV (raw %d)%s", PIN_BAT_ADC, BAT_DIVIDER,
             s_cali ? "curve-fitting calibrated" : "UNCALIBRATED estimate", s_mv, s_raw, s_valid ? "" : " - no battery, meter hidden");
    return true;
}

bool battery_port_read(float *frac, bool *charging) {
    refresh(false);
    if (!s_valid) return false;
    *frac = pwr_pct(s_mv); *charging = s_state == BAT_CHARGING;
    return true;
}

int battery_port_state(void) { return s_valid ? s_state : BAT_ON_BATTERY; }   /* from the last read, as the contract says */

int battery_port_vbat_mv(void) { refresh(false); return s_valid ? s_mv : 0; }

bool battery_port_poweroff(void) {
    board_power_release();                     /* SYS_EN low: on battery the board drops here; on USB it stays up */
    return true;
}

/* ---- the PWR key: a 10 ms esp_timer samples GPIO 40 into one pwr_key_t,
 * read by key_poll from the tank task and the console. The struct is several
 * fields changed together (sample: down / since / long_sent / pending; take:
 * read + clear; swallow: all of them), so a portMUX spinlock guards it
 * rather than per-field atomics: the section is a few dozen instructions and
 * holds across both cores (the timer task and its callers may sit on either).
 * esp_timer keeps its schedule across light sleep: the first callback after
 * a wake samples the key, and the timestamps (esp_timer_get_time) include
 * the time asleep. */
static pwr_key_t s_key;
static portMUX_TYPE s_key_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_key_timer;

static bool key_down(void) { return gpio_get_level(PIN_PWR_KEY) == PWR_KEY_ACTIVE_LEVEL; }

static void key_tick(void *arg) {
    (void)arg;
    bool d = key_down(); int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_key_mux);
    pwr_key_sample(&s_key, d, now);
    portEXIT_CRITICAL(&s_key_mux);
}

void battery_port_key_init(void) {
    if (s_key_timer) return;
    gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_PWR_KEY, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE };
    gpio_config(&in);                          /* no pulls: the board's own pull sets the rest level (director `pwrkey`) */
    bool d = key_down();
    portENTER_CRITICAL(&s_key_mux);
    pwr_key_init(&s_key, d);                   /* the power-on press, if still held, is not a press */
    portEXIT_CRITICAL(&s_key_mux);
    const esp_timer_create_args_t ta = { .callback = key_tick, .name = "pwrkey" };
    if (esp_timer_create(&ta, &s_key_timer) != ESP_OK || esp_timer_start_periodic(s_key_timer, KEY_PERIOD_US) != ESP_OK) {
        ESP_LOGW(TAG, "PWR key sampler did not start: the key is dead");
        return;
    }
    ESP_LOGI(TAG, "PWR key on GPIO %d (active %d, %s at boot): sampled every 10 ms; short press = sleep, 1.5 s = power-off",
             PIN_PWR_KEY, PWR_KEY_ACTIVE_LEVEL, d ? "held" : "up");
}

int battery_port_key_poll(void) {
    if (!s_key_timer) return 0;
    portENTER_CRITICAL(&s_key_mux);
    int e = pwr_key_take(&s_key);
    portEXIT_CRITICAL(&s_key_mux);
    return e;
}

void battery_port_key_swallow(void) {
    if (!s_key_timer) return;
    bool d = key_down();
    portENTER_CRITICAL(&s_key_mux);
    pwr_key_swallow(&s_key, d);
    portEXIT_CRITICAL(&s_key_mux);
}

/* bench (director `keytime N`): each press's length for N s, read straight
 * off the pin; the presses are swallowed at the end, so nothing sleeps */
void battery_port_key_trace(int seconds) {
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000, down = 0, last_up = 0; int n = 0;
    bool was = key_down();
    ESP_LOGI(TAG, "key trace: press the PWR key a few times, naturally, in the next %d s (GPIO %d, active %d; the tank holds still; nothing sleeps)",
             seconds, PIN_PWR_KEY, PWR_KEY_ACTIVE_LEVEL);
    if (was) down = esp_timer_get_time();
    while (esp_timer_get_time() < end) {
        bool d = key_down(); int64_t now = esp_timer_get_time();
        if (d && !was) down = now;
        if (!d && was) {
            n++;
            int held = (int)((now - down) / 1000);
            ESP_LOGI(TAG, "key trace: press %d held %d ms%s | gap since the last release %lld ms", n, held,
                     held >= PWR_LONG_US / 1000 ? " (long)" : "", last_up ? (now - last_up) / 1000 : 0LL);
            last_up = now;
        }
        was = d;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    battery_port_key_swallow();
    ESP_LOGI(TAG, "key trace over: %d presses", n);
}

/* ---- diagnostics */
void battery_port_dump(void) {
    if (!s_adc) { ESP_LOGW(TAG, "no VBAT ADC"); return; }
    refresh(true);                             /* a fresh read for the dump */
    static const char *ST[4] = { "on battery", "charging", "full", "plugged" };
    ESP_LOGI(TAG, "VBAT %d mV (ADC raw %d, %s, x%d) | %s, %.0f%% | USB host %s, rising %s -> %s | no PMIC: no rails, no charger registers",
             s_mv, s_raw, s_cali ? "calibrated" : "UNCALIBRATED", BAT_DIVIDER, s_valid ? "battery present" : "NO BATTERY (outside 2.5-4.35 V)",
             s_valid ? pwr_pct(s_mv) * 100.0 : 0.0, s_usb ? "yes" : "no", s_rising ? "yes" : "no", ST[s_state & 3]);
}
void battery_port_charge_report(void) {
    battery_port_dump();
    ESP_LOGI(TAG, "charge: no PMIC - the board's charger is fixed in hardware; the state is a guess (USB host seen, or VBAT up >= %d mV in 3 min)", RISE_MV);
}
int  battery_port_charge_ma(void) { return 0; }
bool battery_port_set_charge_ma(int ma) { ESP_LOGW(TAG, "charge %d mA: no PMIC, nothing to set", ma); return false; }
bool battery_port_set_rail(const char *name, bool on) { ESP_LOGW(TAG, "rail %s %s: no PMIC", name, on ? "on" : "off"); return false; }
void battery_port_pin_rail(const char *name) { ESP_LOGI(TAG, "pin rail %s: no PMIC", name); }
void battery_port_trim_rails(void) { ESP_LOGI(TAG, "trim rails: no PMIC, nothing to trim"); }
