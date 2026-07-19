/**
 * gsm_modem.h — Minimal, self-contained AT driver for SIM800-family GSM
 * modules (SIM800L/SIM800C, works for most text-mode compatible modems).
 *
 * No external components: uses only the ESP-IDF UART driver + FreeRTOS.
 * All public functions are thread-safe (serialized by an internal mutex).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GSM_NUMBER_MAX_LEN   24
#define GSM_SMS_MAX_LEN      160
#define GSM_MAX_SMS_PER_POLL 8

typedef struct {
    char number[GSM_NUMBER_MAX_LEN];   /* sender, e.g. "+2135551234" */
    char timestamp[24];                /* "26/07/19,12:30:00+08"     */
    char text[GSM_SMS_MAX_LEN + 1];    /* SMS body (text mode)       */
} gsm_sms_t;

/** Initialize UART, start RX task, probe the modem with AT.
 *  Retries internally; returns ESP_OK when the modem answered. */
esp_err_t gsm_modem_init(void);

/** True once the modem answered AT and passed basic config. */
bool gsm_modem_is_ready(void);

/** Network registration status (AT+CREG?): true when registered
 *  on home network or roaming. Caches nothing — queries the modem. */
bool gsm_modem_registered(void);

/** Signal quality 0..31 (99 = unknown). Returns -1 on error. */
int gsm_modem_signal_quality(void);

/** Send an SMS. Text is clipped to GSM_SMS_MAX_LEN. */
esp_err_t gsm_sms_send(const char *number, const char *text);

/** Poll the inbox for unread SMS (AT+CMGL="REC UNREAD").
 *  Parsed messages are copied into out[] and then deleted from the SIM.
 *  Returns the number of messages stored in out[] (0..max), or <0 on error. */
int gsm_sms_poll_unread(gsm_sms_t *out, int max);

#ifdef __cplusplus
}
#endif
