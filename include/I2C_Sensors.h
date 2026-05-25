#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Readings from SEN66
    float pm1p0;
    float pm2p5;
    float pm4p0;
    float pm10p0;
    float sen66_temperature;    // temperature in celcuius
    float sen66_humidity;       // Humidity in percent
    float voc_index;
    float nox_index;
    uint16_t co2;               // co2 in parts per million (ppm)

    // Readings from SFA30
    float hcho;                 // hcho in parts per billion (ppb)
    float sfa30_temperature;    // temperature in celcuius
    float sfa30_humidity;       // Humidity in percent
} sensor_readings_t;

extern QueueHandle_t sensor_readings_queue; // Creates a queue to store the sensor readings that can then be accessed from SIM7070G

void sensor_init();
void initalize_SEN66();
void initalize_SFA30();
void sensor_readings_task(void *parameter);
void SEN66_sensor_readings(void *parameter);
void SFA30_sensor_readings(void *parameter);
void sensor_fan_enable(bool enable);

#ifdef __cplusplus
}
#endif
