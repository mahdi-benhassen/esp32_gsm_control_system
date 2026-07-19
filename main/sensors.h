/**
 * sensors.h — DHT22 (1-wire), soil moisture (ADC1) and float switch.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize ADC oneshot unit + channels. Call once at boot. */
esp_err_t sensors_init(void);

/** Read DHT22: temperature in °C, relative humidity in %.
 *  Returns ESP_ERR_INVALID_RESPONSE on checksum/timeout failure.
 *  Rate limit: do not call more often than once per 2 s. */
esp_err_t dht22_read(float *temperature, float *humidity);

/** Soil moisture in percent 0..100 (calibrated via SOIL_MV_DRY/WET). */
int soil_moisture_percent(void);

/** Raw averaged ADC millivolts of the aux analog input (GPIO 35). */
int aux_analog_mv(void);

/** True while the tank-full float switch is active. */
bool float_switch_tank_full(void);

#ifdef __cplusplus
}
#endif
