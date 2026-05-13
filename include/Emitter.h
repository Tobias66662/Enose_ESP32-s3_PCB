#pragma once

#include <stdint.h>
#include <stdbool.h>  
#include "esp_err.h"

esp_err_t emmitter_init();

esp_err_t emitter_set_frequency(uint32_t freq_hz);

esp_err_t emitter_set_duty(uint8_t channel, uint16_t duty_cycle);

void emitter_enable(uint8_t channel, bool enable);

void emitter_enable_all(bool enable);

void BoostConverter_enable(bool enable);
