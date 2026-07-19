#include "sensors.h"
#include "board_config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"          /* esp_rom_delay_us */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "sensors";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

#define SOIL_SAMPLES 16

/* ------------------------------------------------------------------ */
/* ADC                                                                  */
/* ------------------------------------------------------------------ */

esp_err_t sensors_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = SOIL_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, SOIL_ADC_CHANNEL, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, AUX_ADC_CHANNEL, &chan_cfg));

#if CONFIG_IDF_TARGET_ESP32
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = SOIL_ADC_UNIT,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK);
#endif
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "ADC calibration unavailable, using raw conversion approximation");
    }
    return ESP_OK;
}

static int read_channel_mv(adc_channel_t ch)
{
    int raw = 0;
    int64_t acc = 0;
    for (int i = 0; i < SOIL_SAMPLES; i++) {
        int r = 0;
        if (adc_oneshot_read(s_adc, ch, &r) == ESP_OK) acc += r;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    raw = (int)(acc / SOIL_SAMPLES);

    if (s_cali_ok) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    }
    /* Fallback approximation: 12-bit raw over ~3.1 V full scale at 12 dB */
    return (int)((int64_t)raw * 3100 / 4095);
}

int soil_moisture_percent(void)
{
    int mv = read_channel_mv(SOIL_ADC_CHANNEL);
    /* capacitive sensors: higher voltage = dryer */
    int pct = (int)((int64_t)(SOIL_MV_DRY - mv) * 100 / (SOIL_MV_DRY - SOIL_MV_WET));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

int aux_analog_mv(void)
{
    return read_channel_mv(AUX_ADC_CHANNEL);
}

/* ------------------------------------------------------------------ */
/* Float switch                                                         */
/* ------------------------------------------------------------------ */

bool float_switch_tank_full(void)
{
    /* simple debounce: 3 consistent reads, 10 ms apart */
    int active = 0;
    for (int i = 0; i < 3; i++) {
        if (gpio_get_level(FLOAT_SWITCH_GPIO) == FLOAT_ACTIVE_LEVEL) active++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return active >= 2;
}

/* ------------------------------------------------------------------ */
/* DHT22 bit-bang driver                                                */
/* ------------------------------------------------------------------ */

static int wait_level(int level, int timeout_us)
{
    int t = 0;
    while (gpio_get_level(DHT_GPIO) == level) {
        if (++t > timeout_us) return -1;
        esp_rom_delay_us(1);
    }
    return t;
}

esp_err_t dht22_read(float *temperature, float *humidity)
{
    if (!temperature || !humidity) return ESP_ERR_INVALID_ARG;
    uint8_t data[5] = {0};

    /* --- start signal: pull low >= 1 ms, then release ~30 us --- */
    gpio_set_direction(DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);

    /* --- timing-critical section: 40 bits --- */
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    /* Sensor response: ~80 us LOW then ~80 us HIGH, then the bit stream.
     * Each bit = ~50 us LOW preamble followed by a HIGH data pulse
     * (~26-28 us = '0', ~70 us = '1'). The HIGH pulse is what we measure. */
    if (wait_level(1, 100) < 0) goto timeout;   /* line idles high, wait for sensor low  */
    if (wait_level(0, 100) < 0) goto timeout;   /* wait through the ~80 us response low  */
    if (wait_level(1, 100) < 0) goto timeout;   /* wait through the ~80 us response high */

    for (int i = 0; i < 40; i++) {
        if (wait_level(0, 70) < 0) goto timeout;         /* 50 us low preamble   */
        int high_us = wait_level(1, 90);                 /* data pulse width     */
        if (high_us < 0) goto timeout;
        data[i / 8] <<= 1;
        if (high_us > 40) data[i / 8] |= 1;              /* >40 us => '1'        */
    }
    portEXIT_CRITICAL(&mux);

    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        ESP_LOGW(TAG, "DHT22 checksum error");
        return ESP_ERR_INVALID_RESPONSE;
    }

    *humidity = ((data[0] << 8) | data[1]) / 10.0f;
    int16_t raw_t = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
    *temperature = raw_t / 10.0f;
    if (data[2] & 0x80) *temperature = -*temperature;
    return ESP_OK;

timeout:
    portEXIT_CRITICAL(&mux);
    ESP_LOGW(TAG, "DHT22 timeout (wiring/pull-up?)");
    return ESP_ERR_TIMEOUT;
}
