#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

void sensor_init();
void SEN66_sensor_readings(void *parameter);
void SFA30_sensor_readings(void *parameter);
void sensor_fan_enable(bool enable);
