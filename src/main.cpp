#include <stdio.h>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"

#include "Sensirion_Drivers/sensirion_i2c_hal.h"
#include "I2C_Sensors.h"
#include "Emitter.h"
#include "SIM7070G.h"


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
  SetupTasks(); // Setup the tasks

  esp_err_t err = sim7070g::init(115200);
  if (err != ESP_OK)
  {
    printf("SIM init failed: %s\n", esp_err_to_name(err));
    return;
  }

  while (1)
  {
    err = sim7070g::power_on();
    if (err != ESP_OK)
    {
      printf("SIM power on failed: %s\n", esp_err_to_name(err));
      return;
    }

    std::string response;

    printf("Sending AT\n");
    err = sim7070g::send_command("AT\r\n", response, 1500);
    if (err == ESP_OK)
    {
      printf("AT response:\n%s\n", response.c_str());
    }
    else
    {
      printf("AT failed: %s\n", esp_err_to_name(err));
    }

    printf("Sending AT+CPIN?\n");
    err = sim7070g::send_command("AT+CPIN?\r\n", response, 3000);
    if (err == ESP_OK)
    {
      printf("AT+CPIN? response:\n%s\n", response.c_str());
    }
    else
    {
      printf("AT+CPIN? failed: %s\n", esp_err_to_name(err));
    }

    err = sim7070g::power_off();
    if (err != ESP_OK)
    {
      printf("SIM power off failed: %s\n", esp_err_to_name(err));
    }
  }

  // vTaskDelay(pdMS_TO_TICKS(3000)); // Wait for serial monitor to open
  // sensor_init();
  // sensor_fan_enable(true);

  //xTaskCreate(SEN66_sensor_readings, "SEN66_sensor_readings", 4096, NULL, 5, NULL);
  //xTaskCreate(SFA30_sensor_readings, "SFA30_sensor_readings", 4096, NULL, 5, NULL);

  // ESP_ERROR_CHECK(emmitter_init());
  // BoostConverter_enable(false);
  // vTaskDelay(pdMS_TO_TICKS(5000));
  // BoostConverter_enable(true);

  // while (1)
  // {
  //   //emitter_enable(1, true);
  //   //emitter_enable(8, true);
  //   emitter_enable_all(true);
  //   vTaskDelay(pdMS_TO_TICKS(1000));
  //   emitter_enable_all(false);
  //   // emitter_enable(1, false);
  //   // emitter_enable(8, false);
  //   vTaskDelay(pdMS_TO_TICKS(2000));
  // }


  

}
