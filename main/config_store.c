#include "config_store.h"

#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "cfg";

#define NVS_NAMESPACE   "agri"
#define KEY_MODE        "mode"
#define KEY_THRESHOLD   "thresh"
#define KEY_MASTER      "master"

static agri_config_t s_cfg;

static void load_defaults(void)
{
    s_cfg.mode = AGRI_MODE_AUTO;
    s_cfg.moisture_threshold = CONFIG_AGRI_DEFAULT_MOISTURE_THRESHOLD;
    s_cfg.master_number[0] = '\0';
}

esp_err_t config_store_init(void)
{
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace missing, using defaults");
        return err;
    }

    uint8_t mode = 0;
    if (nvs_get_u8(h, KEY_MODE, &mode) == ESP_OK && mode <= AGRI_MODE_MANUAL) {
        s_cfg.mode = (agri_mode_t)mode;
    }
    uint8_t th = 0;
    if (nvs_get_u8(h, KEY_THRESHOLD, &th) == ESP_OK && th <= 100) {
        s_cfg.moisture_threshold = th;
    }
    size_t len = sizeof(s_cfg.master_number);
    nvs_get_str(h, KEY_MASTER, s_cfg.master_number, &len); /* optional */

    nvs_close(h);
    ESP_LOGI(TAG, "config: mode=%s threshold=%d%% master='%s'",
             s_cfg.mode == AGRI_MODE_AUTO ? "AUTO" : "MANUAL",
             s_cfg.moisture_threshold,
             s_cfg.master_number[0] ? s_cfg.master_number : "(none)");
    return ESP_OK;
}

void config_store_get(agri_config_t *cfg)
{
    if (cfg) *cfg = s_cfg;
}

esp_err_t config_store_set(const agri_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err  = nvs_set_u8(h, KEY_MODE, (uint8_t)s_cfg.mode);
    err |= nvs_set_u8(h, KEY_THRESHOLD, (uint8_t)s_cfg.moisture_threshold);
    err |= nvs_set_str(h, KEY_MASTER, s_cfg.master_number);
    err |= nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) ESP_LOGW(TAG, "NVS save failed");
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}
