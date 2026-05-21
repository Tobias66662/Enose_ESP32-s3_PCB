#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum
{
  EMITTER_CMD_ENABLE_BOOST,     // Enable boost converter
  EMITTER_CMD_DISABLE_BOOST,    // Disable boost converter
  EMITTER_CMD_SET_FREQUENCY,    // Change PWM clock frequency
  EMITTER_CMD_SET_CHANNELS,     // Set what channels are enabled for a set dutation
  EMITTER_CMD_ENABLE_ALL,       // Enable all channels
  EMITTER_CMD_DISABLE_ALL,      // Disable all channels
} emitter_cmd_type_t;

typedef struct
{
  emitter_cmd_type_t type;
  uint8_t PWM_channel_mask;   // bit mask used with the "EMITTER_CMD_SET_CHANNELS" command. Example: 0b00101011 enables channels 1, 2, 4, and 6, and disables the remaining channels.
  uint32_t frequency_hz;      // value has to be within the range of 99 to 108 kHz
  uint32_t duration_ms;       // setting this to 0 will casue the channels to stay enabled indefinitly until a new command manually diables them
} emitter_cmd_t;

extern QueueHandle_t emitter_cmd_queue; // Creates the emitter command queue as external variable that can be accessed form the other source files

esp_err_t emmitter_init(void);
esp_err_t emitter_set_frequency(uint32_t freq_hz);
esp_err_t emitter_set_duty(uint8_t channel, uint16_t duty_cycle);
void emitter_enable(uint8_t channel, bool enable);
void emitter_enable_all(bool enable);
void BoostConverter_enable(bool enable);
void emitter_task(void *parameter);
