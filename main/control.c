/**
 * control.c — Irrigation controller.
 *
 * AUTO mode:
 *   moisture < threshold           -> pump + valve ON (if tank not full,
 *                                     min cycle interval respected)
 *   moisture >= threshold + hyst.  -> OFF
 *   pump runs > MAX_RUNTIME        -> OFF (safety)
 *   float switch "tank full"       -> OFF
 *
 * MANUAL mode: relays only move on explicit SMS commands; the safety
 * max-runtime still applies to the pump.
 *
 * SMS commands (case-insensitive):
 *   HELP              list commands
 *   STATUS            full status report
 *   PUMP ON|OFF       manual pump control (switches mode to MANUAL)
 *   VALVE ON|OFF      manual valve control
 *   AUTO / MANUAL     switch mode
 *   THRESH <0-100>    set moisture threshold (persisted)
 *   MASTER <pin>      register sender as the authorized master number
 *   UNMASTER          clear master (must come from current master)
 */
#include "control.h"
#include "board_config.h"
#include "sensors.h"
#include "gsm_modem.h"
#include "config_store.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ctrl";

/* ------------------------------------------------------------------ */
/* Board GPIO implementation (declared in board_config.h)               */
/* ------------------------------------------------------------------ */

static const gpio_num_t s_relay_gpios[RELAY_COUNT] = { RELAY_PUMP_GPIO, RELAY_VALVE_GPIO };
static bool s_relay_state[RELAY_COUNT];

void board_gpio_init(void)
{
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < RELAY_COUNT; i++) {
        out.pin_bit_mask = 1ULL << s_relay_gpios[i];
        gpio_config(&out);
        gpio_set_level(s_relay_gpios[i], !RELAY_ACTIVE_LEVEL);
        s_relay_state[i] = false;
    }
    out.pin_bit_mask = 1ULL << STATUS_LED_GPIO;
    gpio_config(&out);
    gpio_set_level(STATUS_LED_GPIO, !STATUS_LED_ACTIVE);

    /* GPIO 36/39 are input-only without internal pulls (external on board) */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << FLOAT_SWITCH_GPIO) | (1ULL << DIGITAL_IN2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    /* DHT pin idles as input; dht22_read() toggles direction */
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);
}

void board_relay_set(relay_id_t id, bool on)
{
    if (id < 0 || id >= RELAY_COUNT) return;
    gpio_set_level(s_relay_gpios[id], on ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
    s_relay_state[id] = on;
    ESP_LOGI(TAG, "relay %d -> %s", id, on ? "ON" : "OFF");
}

bool board_relay_get(relay_id_t id)
{
    return (id >= 0 && id < RELAY_COUNT) ? s_relay_state[id] : false;
}

void board_led_set(bool on)
{
    gpio_set_level(STATUS_LED_GPIO, on ? STATUS_LED_ACTIVE : !STATUS_LED_ACTIVE);
}

/* ------------------------------------------------------------------ */
/* Irrigation state machine                                             */
/* ------------------------------------------------------------------ */

static int64_t s_pump_started_us;      /* esp_timer time when pump went ON   */
static int64_t s_last_pump_stop_us;    /* last time pump went OFF            */

static void pump_set(bool on)
{
    if (on == board_relay_get(RELAY_PUMP)) return;
    board_relay_set(RELAY_PUMP, on);
    if (on) s_pump_started_us = esp_timer_get_time();
    else    s_last_pump_stop_us = esp_timer_get_time();
}

static void control_task(void *arg)
{
    bool led = false;
    TickType_t last_sample = 0;
    static int moisture = -1;
    static float temp = 0, hum = 0;
    static bool tank_full = false;

    for (;;) {
        /* --- LED heartbeat: 1 Hz when registered, 5 Hz when not ---
         * registration is re-queried at most every 5 s to keep AT
         * traffic low (and avoid starving the SMS poll task). */
        static bool registered = false;
        static TickType_t last_reg_check = 0;
        if ((xTaskGetTickCount() - last_reg_check) >= pdMS_TO_TICKS(5000)) {
            last_reg_check = xTaskGetTickCount();
            registered = gsm_modem_is_ready() && gsm_modem_registered();
        }
        led = !led;
        board_led_set(led);
        vTaskDelay(pdMS_TO_TICKS(registered ? 500 : 100));

        /* --- sample sensors at CONFIG_AGRI_SENSOR_PERIOD_S --- */
        if ((xTaskGetTickCount() - last_sample) >= pdMS_TO_TICKS(CONFIG_AGRI_SENSOR_PERIOD_S * 1000)) {
            last_sample = xTaskGetTickCount();
            moisture = soil_moisture_percent();
            tank_full = float_switch_tank_full();
            if (dht22_read(&temp, &hum) != ESP_OK) { temp = -127; hum = -1; }
            ESP_LOGI(TAG, "moisture=%d%% tank_full=%d T=%.1fC H=%.1f%%",
                     moisture, tank_full, temp, hum);

            agri_config_t cfg;
            config_store_get(&cfg);

            if (cfg.mode == AGRI_MODE_AUTO && moisture >= 0) {
                int on_th  = cfg.moisture_threshold;
                int off_th = cfg.moisture_threshold + CONFIG_AGRI_MOISTURE_HYSTERESIS;

                if (!board_relay_get(RELAY_PUMP)) {
                    int64_t since_stop = esp_timer_get_time() - s_last_pump_stop_us;
                    bool cycle_ok = s_last_pump_stop_us == 0 ||
                                    since_stop >= (int64_t)CONFIG_AGRI_PUMP_MIN_CYCLE_S * 1000000LL;
                    if (moisture < on_th && !tank_full && cycle_ok) {
                        ESP_LOGI(TAG, "AUTO: dry soil -> irrigation ON");
                        board_relay_set(RELAY_VALVE, true);
                        pump_set(true);
                    }
                } else {
                    if (moisture >= off_th || tank_full) {
                        ESP_LOGI(TAG, "AUTO: moisture OK / tank full -> irrigation OFF");
                        pump_set(false);
                        board_relay_set(RELAY_VALVE, false);
                    }
                }
            }

            /* --- pump safety max runtime (both modes) --- */
            if (board_relay_get(RELAY_PUMP)) {
                int64_t run_us = esp_timer_get_time() - s_pump_started_us;
                if (run_us >= (int64_t)CONFIG_AGRI_PUMP_MAX_RUNTIME_S * 1000000LL) {
                    ESP_LOGW(TAG, "SAFETY: pump max runtime reached -> OFF");
                    pump_set(false);
                    board_relay_set(RELAY_VALVE, false);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* SMS command handling                                                 */
/* ------------------------------------------------------------------ */

static void trim(char *s)
{
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static bool numbers_match(const char *a, const char *b)
{
    /* compare on trailing 8+ digits to tolerate +213 / 0213 / 213 forms */
    char da[GSM_NUMBER_MAX_LEN] = {0}, db[GSM_NUMBER_MAX_LEN] = {0};
    int ia = 0, ib = 0;
    for (const char *p = a; *p && ia < (int)sizeof(da) - 1; p++)
        if (isdigit((unsigned char)*p)) da[ia++] = *p;
    for (const char *p = b; *p && ib < (int)sizeof(db) - 1; p++)
        if (isdigit((unsigned char)*p)) db[ib++] = *p;
    size_t la = strlen(da), lb = strlen(db);
    size_t l = la < lb ? la : lb;
    if (l < 8) return strcmp(da, db) == 0;
    return strcmp(da + la - l, db + lb - l) == 0;
}

static void send_status(const char *to)
{
    agri_config_t cfg;
    config_store_get(&cfg);

    int moisture = soil_moisture_percent();
    float t = -127, h = -1;
    dht22_read(&t, &h);
    int rssi = gsm_modem_signal_quality();
    int up_min = (int)(esp_timer_get_time() / 60000000LL);

    char msg[GSM_SMS_MAX_LEN + 1];
    snprintf(msg, sizeof(msg),
             "AGRI %s | Pump:%s Valve:%s | Soil:%d%% (th %d) | T:%.1fC H:%.1f%% | Tank:%s | CSQ:%d | Up:%dmin",
             cfg.mode == AGRI_MODE_AUTO ? "AUTO" : "MANUAL",
             board_relay_get(RELAY_PUMP) ? "ON" : "OFF",
             board_relay_get(RELAY_VALVE) ? "ON" : "OFF",
             moisture, cfg.moisture_threshold,
             t, h,
             float_switch_tank_full() ? "FULL" : "OK",
             rssi, up_min);
    gsm_sms_send(to, msg);
}

static void handle_command(const char *from, const char *body_in)
{
    char body[GSM_SMS_MAX_LEN + 1];
    snprintf(body, sizeof(body), "%s", body_in);
    trim(body);
    ESP_LOGI(TAG, "SMS from %s: '%s'", from, body);

    agri_config_t cfg;
    config_store_get(&cfg);

    /* MASTER <pin> works from any number (claims authorization) */
    if (strncasecmp(body, "MASTER", 6) == 0 && body[6] != '\0') {
        const char *pin = body + 6;
        while (isspace((unsigned char)*pin)) pin++;
        if (strcmp(pin, CONFIG_AGRI_MASTER_PIN) == 0) {
            snprintf(cfg.master_number, sizeof(cfg.master_number), "%s", from);
            config_store_set(&cfg);
            gsm_sms_send(from, "OK: you are now the master number. Send HELP for commands.");
        } else {
            gsm_sms_send(from, "ERROR: wrong PIN");
        }
        return;
    }

    /* everything else requires authorization once a master exists */
    if (cfg.master_number[0] != '\0' && !numbers_match(cfg.master_number, from)) {
        ESP_LOGW(TAG, "ignoring SMS from unauthorized %s", from);
        return;
    }

    if (strcasecmp(body, "HELP") == 0) {
        gsm_sms_send(from, "Commands: STATUS, PUMP ON/OFF, VALVE ON/OFF, AUTO, MANUAL, "
                           "THRESH <0-100>, MASTER <pin>, UNMASTER, HELP");
    } else if (strcasecmp(body, "STATUS") == 0) {
        send_status(from);
    } else if (strcasecmp(body, "PUMP ON") == 0) {
        cfg.mode = AGRI_MODE_MANUAL; config_store_set(&cfg);
        pump_set(true);
        gsm_sms_send(from, "OK: pump ON (MANUAL mode, safety timer active)");
    } else if (strcasecmp(body, "PUMP OFF") == 0) {
        pump_set(false);
        gsm_sms_send(from, "OK: pump OFF");
    } else if (strcasecmp(body, "VALVE ON") == 0) {
        cfg.mode = AGRI_MODE_MANUAL; config_store_set(&cfg);
        board_relay_set(RELAY_VALVE, true);
        gsm_sms_send(from, "OK: valve ON (MANUAL mode)");
    } else if (strcasecmp(body, "VALVE OFF") == 0) {
        board_relay_set(RELAY_VALVE, false);
        gsm_sms_send(from, "OK: valve OFF");
    } else if (strcasecmp(body, "AUTO") == 0) {
        cfg.mode = AGRI_MODE_AUTO; config_store_set(&cfg);
        gsm_sms_send(from, "OK: AUTO mode");
    } else if (strcasecmp(body, "MANUAL") == 0) {
        cfg.mode = AGRI_MODE_MANUAL; config_store_set(&cfg);
        gsm_sms_send(from, "OK: MANUAL mode");
    } else if (strncasecmp(body, "THRESH", 6) == 0) {
        int v = atoi(body + 6);
        if (v >= 0 && v <= 100) {
            cfg.moisture_threshold = v;
            config_store_set(&cfg);
            char msg[48];
            snprintf(msg, sizeof(msg), "OK: threshold = %d%%", v);
            gsm_sms_send(from, msg);
        } else {
            gsm_sms_send(from, "ERROR: usage THRESH <0-100>");
        }
    } else if (strcasecmp(body, "UNMASTER") == 0) {
        cfg.master_number[0] = '\0';
        config_store_set(&cfg);
        gsm_sms_send(from, "OK: master number cleared");
    } else {
        gsm_sms_send(from, "Unknown command. Send HELP.");
    }
}

static void sms_poll_task(void *arg)
{
    gsm_sms_t msgs[GSM_MAX_SMS_PER_POLL];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_AGRI_SMS_POLL_INTERVAL_S * 1000));
        if (!gsm_modem_is_ready()) continue;

        int n = gsm_sms_poll_unread(msgs, GSM_MAX_SMS_PER_POLL);
        for (int i = 0; i < n; i++) {
            handle_command(msgs[i].number, msgs[i].text);
        }
    }
}

void control_start(void)
{
    xTaskCreate(control_task, "control", 4096, NULL, 5, NULL);
}

void sms_poll_start(void)
{
    /* 8 KB: the poll path carries ~1.7 KB of gsm_sms_t[] plus a 3 KB
     * AT response buffer on its stack at the same time. */
    xTaskCreate(sms_poll_task, "sms_poll", 8192, NULL, 4, NULL);
}
