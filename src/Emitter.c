#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include <string.h>
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "driver/gpio.h"

#include "Emitter.h"

// PWM Constatns
static const uint32_t MAX_FREQ = 108000;           // 108 kHz - Maximum frequency corresponding to max emitter power
static const uint32_t MIN_FREQ = 99000;            // 99 kHz - Minimum frequency corresponding to min emitter power
#define DUTY_RESOLUTION   LEDC_TIMER_8_BIT  // 0-255
#define DUTY_CYCLE        128               // 50 % duty cycle (255 / 2)
#define EMITTER_TIMER     LEDC_TIMER_0
#define SPEED_MODE        LEDC_LOW_SPEED_MODE

#define NUMBER_OF_TRANSDUCERS 8
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

QueueHandle_t emitter_cmd_queue = NULL;

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
      .sleep_mode  = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
      .flags       = { .output_invert = 0 },
      .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_config), TAG, "Channel %d config failed", i);
  }

  ESP_LOGI(TAG, "Initialised %u channels @ %lu Hz", NUMBER_OF_TRANSDUCERS, MIN_FREQ);


  emitter_cmd_queue = xQueueCreate(10, sizeof(emitter_cmd_t)); // creates queue that can store 10 commands
  xTaskCreate(emitter_task, "Emitter Task", 4096, NULL, 6, NULL); // Creates emitter_task with priority 6 and with 4096 bytes on the stack allocated to it (might be too much)


  BoostConverter_enable(true);
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

// Turn on all or off specific channels/transducers
void emitter_set_channels(uint8_t channel_mask)
{
  for (uint8_t i = 0; i < NUMBER_OF_TRANSDUCERS; i++)
  {
    bool enable = (channel_mask & (1 << i)) != 0;
    emitter_enable(i + 1, enable);
  }
}

// Turn all transducers on or off
void emitter_enable_all(bool enable)
{
  for (int i = 1; i <= NUMBER_OF_TRANSDUCERS; i++)
  {
    emitter_enable(i, enable);
  }
}

void BoostConverter_enable(bool enable)
{
  gpio_set_level(BOOST_CONVERTER_ENABLE_PIN, enable);
}

// Task controlling the Emitter based on commands recieved from Cellular Modem
void emitter_task(void *parameter)
{
  emitter_cmd_t cmd;
  bool emitting = false; // used to indicate if any transducer is currently enabled
  TickType_t stop_time = 0;

  while(1)
  {
    TickType_t wait_time = portMAX_DELAY;

    if (emitting)
    {
      TickType_t current_tick_count = xTaskGetTickCount();

      // Check if the stop time has already passed (edge case fail safe) (if statement not be true under normal operation)
      if ((int32_t)(current_tick_count - stop_time) >= 0) // this formats works even if the tick counter underflows
      {
        emitter_enable_all(false);
        emitting = false;
        //BoostConverter_enable(false);
      }

      // Update the duration that "xQueueReceive" funciton will wait befefore timeout, to correspond to cmd.duration_ms
      wait_time = stop_time - current_tick_count;
    }

    if (xQueueReceive(emitter_cmd_queue, &cmd, wait_time))
    {
      switch (cmd.type)
      {
      case EMITTER_CMD_ENABLE_BOOST:
        BoostConverter_enable(true);
        break;

      case EMITTER_CMD_DISABLE_BOOST:
        emitter_enable_all(false);
        emitting = false;
        BoostConverter_enable(false);
        break;

      case EMITTER_CMD_SET_FREQUENCY:
        emitter_set_frequency(cmd.frequency_hz);
        break;

      case EMITTER_CMD_SET_CHANNELS:
        //BoostConverter_enable(true);  // temp for testing
        ESP_LOGI("emitter", "set channel mask");
        emitter_set_channels(cmd.PWM_channel_mask);
        if (cmd.PWM_channel_mask != 0) // Doesen't set the emitter flag if this command is used to disable all channels
        {
          emitting = true;
          if (cmd.duration_ms > 0) // If the duration is set to 0, the channls will stay on indefinitly until a new command diables them
          {
            stop_time = xTaskGetTickCount() + pdMS_TO_TICKS(cmd.duration_ms);
          }
        }
        else
        {
          emitting = false;
        }
        break;

      case EMITTER_CMD_ENABLE_ALL:
        //BoostConverter_enable(true); // temp for testing
        emitter_enable_all(true);
        emitting = true;
        if (cmd.duration_ms > 0) // If the duration is set to 0, the channls will stay on indefinitly until a new command diables them
        {
          stop_time = xTaskGetTickCount() + pdMS_TO_TICKS(cmd.duration_ms);
        }
        break;

      case EMITTER_CMD_DISABLE_ALL:
        //emitter_enable_all(false);
        emitting = false;
        break;

      default:
        ESP_LOGE(TAG, "%d is an invalid emitter command", cmd.type);
        break;
      }
    }
    else // xQueueReceive timed out (happens when the defined emission duration expires)
    {
      if (emitting)
      {
        emitter_enable_all(false);
        emitting = false;
        //BoostConverter_enable(false);
      }
    }
  }
}
