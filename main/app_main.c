/**
 * app_main.c — ESP32 GSM Agriculture Control System entry point.
 *
 * Boot sequence: NVS -> GPIO -> ADC/sensors -> config -> GSM modem -> tasks.
 */
#include "board_config.h"
#include "sensors.h"
#include "gsm_modem.h"
#include "config_store.h"
#include "control.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"

static const char *TAG = "main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "=== %s v%s (ESP-IDF %s) ===",
             app->project_name, app->version, app->idf_ver);

    /* NVS (erase+retry if a full/corrupt partition is found) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    board_gpio_init();
    ESP_ERROR_CHECK(sensors_init());
    config_store_init();

    /* Modem: retries internally; on failure the system keeps running and
     * the LED fast-blinks until the modem answers (handled in control). */
    err = gsm_modem_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSM modem init failed — check SIM800 power/wiring/antenna");
    } else {
        ESP_LOGI(TAG, "GSM ready, network registration in progress...");
    }

    control_start();
    sms_poll_start();

    ESP_LOGI(TAG, "boot complete, free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
}
