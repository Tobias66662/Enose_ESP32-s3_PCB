#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include <string.h>
#include "esp_rom_sys.h"
#include "esp_check.h"

#include "Emitter.h"

// PWM Constatns
static const uint32_t MAX_FREQ = 108000;           // 108 kHz - Maximum frequency corresponding to max emitter power
static const uint32_t MIN_FREQ = 99000;            // 99 kHz - Minimum frequency corresponding to min emitter power
#define DUTY_RESOLUTION   LEDC_TIMER_8_BIT  // 0-255
#define DUTY_CYCLE        128               // 50 % duty cycle (255 / 2)
#define EMITTER_TIMER     LEDC_TIMER_0
#define SPEED_MODE        LEDC_LOW_SPEED_MODE

static const uint8_t NUMBER_OF_TRANSDUCERS = 8;
#define BOOST_CONVERTER_ENABLE_PIN GPIO_NUM_15

// Pin Assignment
static const uint8_t EMITTER_PINS[NUMBER_OF_TRANSDUCERS] = {
    GPIO_NUM_47,  //PWM 1 (channel 1)
    GPIO_NUM_21,  //PWM 2 (channel 2)
    GPIO_NUM_14,  //PWM 3 (channel 3)
    GPIO_NUM_13,  //PWM 4 (channel 4)
    GPIO_NUM_12,  //PWM 5 (channel 5)
    GPIO_NUM_11,  //PWM 6 (channel 6)
    GPIO_NUM_10,  //PWM 7 (channel 7)
    GPIO_NUM_9,   //PWM 8 (channel 8)
};

static const char *TAG = "Emitter";

esp_err_t emmitter_init()
{
  gpio_set_direction(BOOST_CONVERTER_ENABLE_PIN, GPIO_MODE_OUTPUT); // Set up the boost converter enable pin

  // Configure the timer
  ledc_timer_config_t timer_config = {
    .speed_mode      = SPEED_MODE,
    .duty_resolution = DUTY_RESOLUTION,
    .timer_num       = EMITTER_TIMER,
    .freq_hz         = MAX_FREQ, // Initialize to have max emitter power
    .clk_cfg         = LEDC_AUTO_CLK,
    .deconfigure     = false,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "Timer config failed");


  // Configure 8 PWM channels (one per transducer)
  for (int i = 0; i < NUMBER_OF_TRANSDUCERS; i++) {
    ledc_channel_config_t ch_config = {
      .gpio_num   = EMITTER_PINS[i],
      .speed_mode = SPEED_MODE,
      .channel    = (ledc_channel_t)i,
      .intr_type  = LEDC_INTR_DISABLE,
      .timer_sel  = EMITTER_TIMER,
      .duty       = 0,    // off by default
      .hpoint     = 0,
      .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
      .flags      = { .output_invert = 0 },
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_config), TAG, "Channel %d config failed", i);
  }

  ESP_LOGI(TAG, "Initialised %u channels @ %lu Hz", NUMBER_OF_TRANSDUCERS, MIN_FREQ);
  return ESP_OK;
}

esp_err_t emitter_set_frequency(uint32_t freq_hz)
{
  if ((freq_hz < MIN_FREQ) || (freq_hz > MAX_FREQ))
  {
    ESP_LOGE(TAG, "%lu is outide the allowed range (%lu - %lu)", freq_hz, MIN_FREQ, MAX_FREQ);
    return ESP_FAIL;
  }
  ESP_RETURN_ON_ERROR(ledc_set_freq(SPEED_MODE, EMITTER_TIMER, freq_hz), TAG, "Failed to set frequency");
  return ESP_OK;
}

esp_err_t emitter_set_duty(uint8_t channel, uint16_t duty_cycle)
{
  ESP_RETURN_ON_ERROR(ledc_set_duty(SPEED_MODE, (ledc_channel_t)(channel-1), duty_cycle), TAG, "Failed to set duty cycle for channel %d", channel);
  ESP_RETURN_ON_ERROR(ledc_update_duty(SPEED_MODE, (ledc_channel_t)(channel-1)), TAG, "Failed to update duty cycle for channel %d", channel);

  return ESP_OK;
}

// Turn individual transducers on or off
void emitter_enable(uint8_t channel, bool enable)
{
  if(enable)
  {
    emitter_set_duty(channel, DUTY_CYCLE);
  }
  else
  {
    emitter_set_duty(channel, 0);
  }

}

// Turn all transducers on or off
void emitter_enable_all(bool enable)
{
  for (int i = 0; i < NUMBER_OF_TRANSDUCERS; i++)
  {
    emitter_enable(i, enable);
  }
}

void BoostConverter_enable(bool enable)
{
  gpio_set_level(BOOST_CONVERTER_ENABLE_PIN, enable);
}
