/**
 * config_store.h — Persistent settings in NVS (namespace "agri").
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "gsm_modem.h"   /* GSM_NUMBER_MAX_LEN */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGRI_MODE_AUTO = 0,
    AGRI_MODE_MANUAL,
} agri_mode_t;

typedef struct {
    agri_mode_t mode;                          /* AUTO / MANUAL                */
    int  moisture_threshold;                   /* percent                      */
    char master_number[GSM_NUMBER_MAX_LEN];    /* authorized phone, "" = none  */
} agri_config_t;

/** nvs_flash_init must have been called before. Loads defaults on error. */
esp_err_t config_store_init(void);

/** Copy current settings into *cfg. */
void config_store_get(agri_config_t *cfg);

/** Update settings in RAM and persist to NVS. */
esp_err_t config_store_set(const agri_config_t *cfg);

#ifdef __cplusplus
}
#endif
