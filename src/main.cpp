#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"

#include "Sensirion_Drivers/sensirion_i2c_hal.h"
#include "I2C_Sensors.h"
#include "Emitter.h"


#define PIN GPIO_NUM_2 // PCB DEBUG LED

//static const char *TAG = "main";

// Function
void SetupTasks();


// Task: Blinks LED when resetting
void BOOT_LED(void *parameter)
{
  gpio_set_direction(PIN, GPIO_MODE_OUTPUT); // Configure pin

  for (uint8_t i = 0; i < 3; i++)
  {
    gpio_set_level(PIN, 1);
    vTaskDelay(500 / portTICK_PERIOD_MS); // 0,5s delay
    gpio_set_level(PIN, 0);
    vTaskDelay(500 / portTICK_PERIOD_MS); // 0,5s delay
  }

  vTaskDelete(NULL); // Kill the task
}

void SetupTasks()
{
  // Setup LED task
  xTaskCreate(
      BOOT_LED,    // Function to be called
      "LED toggle", // Name of task (for debugging)
      1024,         // Stack size in bytes ()
      NULL,         // Parameter to pass to function
      1,            // Task priority (0-24, where the larger the number the higher the priority)
      NULL);        // Task handle

}


extern "C" void app_main()
{
  SetupTasks(); // Setsup the tasks

  // vTaskDelay(pdMS_TO_TICKS(3000)); // Wait for serial monitor to open
  // sensor_init();
  // sensor_fan_enable(true);

  //xTaskCreate(SEN66_sensor_readings, "SEN66_sensor_readings", 4096, NULL, 5, NULL);
  //xTaskCreate(SFA30_sensor_readings, "SFA30_sensor_readings", 4096, NULL, 5, NULL);

  ESP_ERROR_CHECK(emmitter_init());
  BoostConverter_enable(false);
  vTaskDelay(pdMS_TO_TICKS(5000));
  BoostConverter_enable(true);

  while (1)
  {
    //emitter_enable(1, true);
    //emitter_enable(8, true);
    emitter_enable_all(true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    emitter_enable_all(false);
    // emitter_enable(1, false);
    // emitter_enable(8, false);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
