#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

void MP2722_interruptInit();
void MP2722_init();
void MP2722_readRegister(uint8_t reg, uint8_t *value);
void MP2722_writeRegister(uint8_t reg, uint8_t value);
void MP2722_setRegisters();
