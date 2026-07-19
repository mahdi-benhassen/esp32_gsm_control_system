/**
 * board_config.h — Hardware pin map
 *
 * Follows the OFFICIAL Kincony KC868-A2 pin definition (KC868-A2 V2.x
 * schematic + Kincony forum "KC868-A2 ESP32 I/O pin define"):
 *
 *   Relay1 (pump) ..... GPIO15      Relay2 (valve) ..... GPIO2
 *   GSM (SIM800) ...... TX=GPIO13 / RX=GPIO34
 *   1-wire GPIO-1 ..... GPIO33      1-wire GPIO-2 ...... GPIO14
 *   Digital input 1 ... GPIO36      Digital input 2 .... GPIO39
 *   RS485 ............. RX=GPIO35 / TX=GPIO32
 *   I2C bus ........... SDA=GPIO4 / SCL=GPIO16
 *   Ethernet LAN8720 .. MDC=GPIO23 / MDIO=GPIO18 / CLK=GPIO17_OUT
 *
 * Note: GPIO32/35 double as RS485 TX/RX on the stock board — they are only
 * usable as ADC inputs when RS485 is not in use. For a fully stock KC868-A2
 * prefer an I2C ADC (e.g. ADS1115 on GPIO4/16) for analog sensors.
 * Kincony has shipped several revisions — ALWAYS verify against the
 * schematic of your exact board revision.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GSM modem (SIM800L) on UART2 — official KC868-A2: TX=13, RX=34      */
/* ------------------------------------------------------------------ */
#define GSM_UART_PORT        UART_NUM_2
#define GSM_UART_TX_GPIO     GPIO_NUM_13   /* ESP32 TX  -> SIM800 RX   */
#define GSM_UART_RX_GPIO     GPIO_NUM_34   /* ESP32 RX  <- SIM800 TX   */
#define GSM_UART_BAUD        115200
#define GSM_RX_BUF_SIZE      2048
#define GSM_TX_BUF_SIZE      1024

/* ------------------------------------------------------------------ */
/* Relay outputs (active HIGH on KC868-A2)                             */
/* Note: GPIO2/GPIO15 are also boot-strapping pins — that is how the   */
/* stock board wires them, the relay optocouplers do not disturb boot. */
/* ------------------------------------------------------------------ */
#define RELAY_COUNT          2
#define RELAY_PUMP_GPIO      GPIO_NUM_15   /* Relay 1: water pump      */
#define RELAY_VALVE_GPIO     GPIO_NUM_2    /* Relay 2: solenoid valve  */
#define RELAY_ACTIVE_LEVEL   1

/* ------------------------------------------------------------------ */
/* Analog inputs (ADC1 only — ADC2 conflicts with internal RF)         */
/* On the stock KC868-A2 these pins are wired to the RS485 transceiver */
/* (GPIO32=TX, GPIO35=RX): usable as ADC only when RS485 is unused.    */
/* ------------------------------------------------------------------ */
#define SOIL_ADC_UNIT        ADC_UNIT_1
#define SOIL_ADC_CHANNEL     ADC_CHANNEL_4 /* GPIO 32: soil moisture   */
#define SOIL_ADC_GPIO        GPIO_NUM_32
#define AUX_ADC_CHANNEL      ADC_CHANNEL_7 /* GPIO 35: tank level etc. */
#define AUX_ADC_GPIO         GPIO_NUM_35
#define ADC_ATTEN_USED       ADC_ATTEN_DB_12   /* ~0..3.1 V full scale */

/* Soil moisture calibration: raw ADC millivolts measured in air (dry)   */
/* and in water (wet). Adjust for YOUR sensor — see README.              */
#define SOIL_MV_DRY          2950
#define SOIL_MV_WET          1100

/* ------------------------------------------------------------------ */
/* DHT22 temperature / humidity sensor on 1-wire terminal 1 (GPIO33)   */
/* ------------------------------------------------------------------ */
#define DHT_GPIO             GPIO_NUM_33

/* ------------------------------------------------------------------ */
/* Digital inputs (GPIO 36/39 are input-only, no internal pulls)       */
/* ------------------------------------------------------------------ */
#define FLOAT_SWITCH_GPIO    GPIO_NUM_36   /* DI1: tank-full switch    */
#define FLOAT_ACTIVE_LEVEL   1             /* level when "tank full"   */
#define DIGITAL_IN2_GPIO     GPIO_NUM_39   /* DI2: spare digital input */

/* ------------------------------------------------------------------ */
/* Status LED on 1-wire terminal 2 (GPIO14 — Kincony also offers this  */
/* terminal for LED strips; a plain LED + 330R resistor works too).    */
/* ------------------------------------------------------------------ */
#define STATUS_LED_GPIO      GPIO_NUM_14
#define STATUS_LED_ACTIVE    1

/* Relay indices for control layer */
typedef enum {
    RELAY_PUMP  = 0,
    RELAY_VALVE = 1,
} relay_id_t;

/** Initialize all board GPIOs (relays off, inputs, LED). */
void board_gpio_init(void);

/** Switch a relay on/off. */
void board_relay_set(relay_id_t id, bool on);

/** Read a relay state (cached). */
bool board_relay_get(relay_id_t id);

/** Status LED helper. */
void board_led_set(bool on);

#ifdef __cplusplus
}
#endif
