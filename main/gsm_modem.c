/**
 * gsm_modem.c — AT command driver for SIM800-family GSM modules.
 *
 * Design:
 *  - A dedicated RX task continuously drains the UART into a ring buffer.
 *  - Synchronous transactions (gsm_at_cmd) are serialized with a mutex:
 *    the RX accumulator is reset, the command is sent, and we wait until
 *    the expected terminator (OK / ERROR / custom) appears or a timeout.
 *  - SMS listing is parsed line-by-line from the transaction buffer.
 */
#include "gsm_modem.h"
#include "board_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gsm";

#define AT_RESP_BUF_SIZE     3072
#define AT_CMD_TIMEOUT_MS    3000
#define MODEM_BOOT_RETRIES   10
#define RX_TASK_STACK        4096
#define RX_TASK_PRIO         12

static SemaphoreHandle_t s_at_mutex;      /* serializes AT transactions   */
static SemaphoreHandle_t s_resp_sem;      /* signaled on new RX bytes     */
static char     s_resp[AT_RESP_BUF_SIZE]; /* accumulator for current cmd  */
static volatile size_t s_resp_len;
static volatile bool   s_collect;         /* RX task appends when true    */
static bool     s_ready;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                    */
/* ------------------------------------------------------------------ */

static bool resp_contains(const char *needle)
{
    if (!needle || s_resp_len == 0) return false;
    s_resp[s_resp_len < AT_RESP_BUF_SIZE ? s_resp_len : AT_RESP_BUF_SIZE - 1] = '\0';
    return strstr(s_resp, needle) != NULL;
}

/**
 * Send `cmd` (CR is appended) and wait for `expect` (default "OK") or
 * "ERROR" / "+CME ERROR" / "+CMS ERROR". The full response (excluding the
 * echo line best-effort) stays in s_resp for the caller to parse.
 */
static esp_err_t gsm_at_cmd(const char *cmd, const char *expect,
                            uint32_t timeout_ms, char *resp_out, size_t resp_out_size)
{
    if (expect == NULL) expect = "OK";

    /* drain pending RX noise */
    s_resp_len = 0;
    s_resp[0] = '\0';
    s_collect = true;

    uart_write_bytes(GSM_UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(GSM_UART_PORT, "\r", 1);

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t result = ESP_ERR_TIMEOUT;

    while (esp_timer_get_time() < deadline) {
        /* wait for RX task to signal new data (or poll every 20 ms) */
        xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(20));

        if (resp_contains(expect))            { result = ESP_OK;    break; }
        if (resp_contains("ERROR"))           { result = ESP_FAIL;  break; }
    }

    s_collect = false;
    vTaskDelay(pdMS_TO_TICKS(20)); /* let trailing bytes land */

    if (resp_out && resp_out_size > 0) {
        size_t n = s_resp_len < resp_out_size - 1 ? s_resp_len : resp_out_size - 1;
        memcpy(resp_out, s_resp, n);
        resp_out[n] = '\0';
    }
    ESP_LOGD(TAG, "AT >>> %s | resp(%u): %s", cmd, (unsigned)s_resp_len, s_resp);
    return result;
}

/* RX task: drain UART into the response accumulator */
static void gsm_rx_task(void *arg)
{
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(GSM_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (s_collect) {
            for (int i = 0; i < n; i++) {
                size_t pos = s_resp_len;
                if (pos < AT_RESP_BUF_SIZE - 1) {
                    s_resp[pos] = (char)buf[i];
                    s_resp_len = pos + 1;
                }
            }
        } else {
            /* discard: URCs (+CMTI etc.) are handled by polling instead */
        }
        xSemaphoreGive(s_resp_sem);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t gsm_modem_init(void)
{
    s_at_mutex = xSemaphoreCreateMutex();
    s_resp_sem = xSemaphoreCreateBinary();
    configASSERT(s_at_mutex && s_resp_sem);

    const uart_config_t cfg = {
        .baud_rate  = GSM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GSM_UART_PORT, GSM_RX_BUF_SIZE,
                                        GSM_TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GSM_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GSM_UART_PORT, GSM_UART_TX_GPIO, GSM_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(gsm_rx_task, "gsm_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL);

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < MODEM_BOOT_RETRIES; i++) {
        if (gsm_at_cmd("AT", "OK", 1000, NULL, 0) == ESP_OK) { err = ESP_OK; break; }
        ESP_LOGW(TAG, "modem not answering, retry %d/%d", i + 1, MODEM_BOOT_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_at_mutex);
        ESP_LOGE(TAG, "SIM800 not detected on UART%d", GSM_UART_PORT);
        return err;
    }

    /* Basic configuration: echo off, SMS text mode, GSM charset */
    gsm_at_cmd("ATE0", "OK", AT_CMD_TIMEOUT_MS, NULL, 0);
    gsm_at_cmd("AT+CMGF=1", "OK", AT_CMD_TIMEOUT_MS, NULL, 0);
    gsm_at_cmd("AT+CSCS=\"GSM\"", "OK", AT_CMD_TIMEOUT_MS, NULL, 0);
    /* Route SMS indications to the modem's own storage; we poll the inbox. */
    gsm_at_cmd("AT+CNMI=1,0,0,0,0", "OK", AT_CMD_TIMEOUT_MS, NULL, 0);
    /* SIM800 can take a while to register — don't fail boot on that. */

    s_ready = true;
    xSemaphoreGive(s_at_mutex);

    ESP_LOGI(TAG, "SIM800 modem initialized");
    return ESP_OK;
}

bool gsm_modem_is_ready(void)
{
    return s_ready;
}

bool gsm_modem_registered(void)
{
    if (!s_ready) return false;
    bool registered = false;
    char resp[256];

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    if (gsm_at_cmd("AT+CREG?", "OK", AT_CMD_TIMEOUT_MS, resp, sizeof(resp)) == ESP_OK) {
        /* +CREG: 0,1 (home) or +CREG: 0,5 (roaming) */
        registered = strstr(resp, ",1") != NULL || strstr(resp, ",5") != NULL;
    }
    xSemaphoreGive(s_at_mutex);
    return registered;
}

int gsm_modem_signal_quality(void)
{
    if (!s_ready) return -1;
    char resp[256];
    int rssi = -1;

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    if (gsm_at_cmd("AT+CSQ", "OK", AT_CMD_TIMEOUT_MS, resp, sizeof(resp)) == ESP_OK) {
        const char *p = strstr(resp, "+CSQ:");
        if (p) {
            int v = -1;
            if (sscanf(p + 5, "%d", &v) == 1 && v != 99) rssi = v;
        }
    }
    xSemaphoreGive(s_at_mutex);
    return rssi;
}

esp_err_t gsm_sms_send(const char *number, const char *text)
{
    if (!s_ready || !number || !text) return ESP_ERR_INVALID_ARG;

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    /* Modem answers "> " prompt after AT+CMGS */
    esp_err_t err = gsm_at_cmd(cmd, ">", AT_CMD_TIMEOUT_MS, NULL, 0);
    if (err == ESP_OK) {
        char body[GSM_SMS_MAX_LEN + 2];
        snprintf(body, sizeof(body), "%.*s", GSM_SMS_MAX_LEN, text);
        s_resp_len = 0; s_resp[0] = '\0'; s_collect = true;
        uart_write_bytes(GSM_UART_PORT, body, strlen(body));
        char ctrlz = 0x1A;
        uart_write_bytes(GSM_UART_PORT, &ctrlz, 1);

        int64_t deadline = esp_timer_get_time() + 20000LL * 1000; /* SMS can be slow */
        err = ESP_ERR_TIMEOUT;
        while (esp_timer_get_time() < deadline) {
            xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(50));
            if (resp_contains("+CMGS:")) { err = ESP_OK; break; }
            if (resp_contains("ERROR"))  { err = ESP_FAIL; break; }
        }
        s_collect = false;
    }

    xSemaphoreGive(s_at_mutex);
    if (err == ESP_OK) ESP_LOGI(TAG, "SMS sent to %s", number);
    else               ESP_LOGW(TAG, "SMS send to %s failed (%s)", number, esp_err_to_name(err));
    return err;
}

/* Parse one "+CMGL:" header line: +CMGL: <idx>,"<stat>","<addr>",,"<ts>" */
static bool parse_cmgl_header(char *line, int *idx, char *number, size_t num_sz,
                              char *ts, size_t ts_sz)
{
    int index = -1;
    char *p = strchr(line, ':');
    if (!p) return false;
    if (sscanf(p + 1, "%d", &index) != 1) return false;

    /* fields are quoted; find the 2nd and last quoted strings */
    char *quotes[8];
    int qn = 0;
    for (char *c = line; *c && qn < 8; c++) {
        if (*c == '"') quotes[qn++] = c;
    }
    if (qn < 4 || index < 0) return false;

    /* sender is between quotes[2] and quotes[3] */
    size_t len = (size_t)(quotes[3] - quotes[2] - 1);
    if (len >= num_sz) len = num_sz - 1;
    memcpy(number, quotes[2] + 1, len);
    number[len] = '\0';

    /* timestamp between the last pair of quotes */
    if (qn >= 6 && ts && ts_sz) {
        size_t tl = (size_t)(quotes[qn - 1] - quotes[qn - 2] - 1);
        if (tl >= ts_sz) tl = ts_sz - 1;
        memcpy(ts, quotes[qn - 2] + 1, tl);
        ts[tl] = '\0';
    }

    *idx = index;
    return true;
}

int gsm_sms_poll_unread(gsm_sms_t *out, int max)
{
    if (!s_ready || !out || max <= 0) return -1;
    if (max > GSM_MAX_SMS_PER_POLL) max = GSM_MAX_SMS_PER_POLL;

    char resp[AT_RESP_BUF_SIZE];
    int count = 0;
    int indexes[GSM_MAX_SMS_PER_POLL];
    int n_idx = 0;

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    esp_err_t err = gsm_at_cmd("AT+CMGL=\"REC UNREAD\"", "OK", 8000, resp, sizeof(resp));

    if (err == ESP_OK) {
        /* split the response into NUL-terminated lines first, so a
         * message with an empty body never consumes the next header */
        char *lines[64];
        int nlines = 0;
        char *p = resp;
        while (*p && nlines < (int)(sizeof(lines) / sizeof(lines[0]))) {
            while (*p == '\r' || *p == '\n') p++;
            if (*p == '\0') break;
            lines[nlines++] = p;
            while (*p && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }

        for (int i = 0; i < nlines && count < max; i++) {
            if (strncmp(lines[i], "+CMGL:", 6) != 0) continue;
            int idx;
            gsm_sms_t *m = &out[count];
            memset(m, 0, sizeof(*m));
            if (!parse_cmgl_header(lines[i], &idx, m->number, sizeof(m->number),
                                   m->timestamp, sizeof(m->timestamp))) {
                continue;
            }
            const char *body = "";
            if (i + 1 < nlines &&
                strncmp(lines[i + 1], "+CMGL:", 6) != 0 &&
                strcmp(lines[i + 1], "OK") != 0) {
                body = lines[i + 1];
                i++; /* consume the body line */
            }
            snprintf(m->text, sizeof(m->text), "%.*s", GSM_SMS_MAX_LEN, body);
            indexes[n_idx++] = idx;
            count++;
        }
    }

    /* delete processed messages from SIM storage */
    for (int i = 0; i < n_idx; i++) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", indexes[i]);
        gsm_at_cmd(cmd, "OK", AT_CMD_TIMEOUT_MS, NULL, 0);
    }

    xSemaphoreGive(s_at_mutex);
    return count;
}
