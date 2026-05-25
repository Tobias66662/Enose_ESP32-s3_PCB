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
#include "MP2722.h"
#include "Classifier.h"



#define BOOT_LED GPIO_NUM_38 // PCB DEBUG LED
//#define BOOT_BUTTON GPIO_NUM_0;
constexpr gpio_num_t BOOT_BUTTON = GPIO_NUM_0;

static SemaphoreHandle_t Boot_Semaphore; // Semaphore used in the ISR

//static const char *TAG = "main";

// Function declarations
void boot_button_InterruptTask(void* parameter);
void boot_init();
void boot_led_task(void *parameter);

// Interupt service routine (deferred interrupt)
static void IRAM_ATTR boot_button_isr(void *arg)
{
  BaseType_t higherPriorityTaskWoken = pdFALSE;

  xSemaphoreGiveFromISR(Boot_Semaphore, &higherPriorityTaskWoken);

  // If xSemaphoreGiveFromISR() sets higherPriorityTaskWoken = pdTRUE then a context switch is requested before the interrupt is exited.
  if(higherPriorityTaskWoken)
  {
    portYIELD_FROM_ISR();
  }
}

void boot_init()
{
  // Initialize boot LED
  xTaskCreate(
      boot_led_task,    // Function to be called
      "LED toggle", // Name of task (for debugging)
      1024,         // Stack size in bytes ()
      NULL,         // Parameter to pass to function
      1,            // Task priority (0-24, where the larger the number the higher the priority)
      NULL);        // Task handle


  // Initialize Boot button interrupt
  Boot_Semaphore = xSemaphoreCreateBinary();

  gpio_config_t GPIO_config = {
    .pin_bit_mask = (1ULL << BOOT_BUTTON),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE        // Trigger on falling edge (active low)
    };
  gpio_config(&GPIO_config);

  gpio_install_isr_service(1); // set up the interrupt
  gpio_isr_handler_add(BOOT_BUTTON, boot_button_isr, NULL); // Attach ISR function to INT_PIN

  xTaskCreate(boot_button_InterruptTask, "boot_button_InterruptTask", 2048, NULL, 10, NULL); // Create the deferred interrupt handle task
}

void boot_button_InterruptTask(void* parameter)
{
  TickType_t last_press_time = 0; // (in clock ticks)

  while(1)
  {
    if (xSemaphoreTake(Boot_Semaphore, portMAX_DELAY))
    {
      TickType_t current_time = xTaskGetTickCount(); // (in clock ticks)

      if ((current_time - last_press_time) >= pdMS_TO_TICKS(50)) // to prevent button bounce
      {
        last_press_time = current_time;

        //---------------- Code here runs whenever the boot button is pressed ---------------- //
        //sim7070g::power_off();
        gpio_set_level(BOOT_LED, 1);
        vTaskDelay(500 / portTICK_PERIOD_MS); // 0,5s delay
        gpio_set_level(BOOT_LED, 0);
      }
    }
  }
}

// Task: Blinks LED when resetting
void boot_led_task(void *parameter)
{
  gpio_set_direction(BOOT_LED, GPIO_MODE_OUTPUT); // Configure pin

  for (uint8_t i = 0; i < 3; i++)
  {
    gpio_set_level(BOOT_LED, 1);
    vTaskDelay(500 / portTICK_PERIOD_MS); // 0,5s delay
    gpio_set_level(BOOT_LED, 0);
    vTaskDelay(500 / portTICK_PERIOD_MS); // 0,5s delay
  }

  vTaskDelete(NULL); // Kill the task
}

extern "C" void app_main()
{
    boot_init(); // setup task and ISR for the boot button and Boot Led task
    sensor_init(); // Always needs to initialize the sensors before the MP2722, since the I2C bus is created by the sensors.
    //MP2722_init();
    emmitter_init();
    classifier_init();

    esp_err_t err = sim7070g::init(115200);

    if (err != ESP_OK)
    {
            printf("SIM init failed: %s\n",
                        esp_err_to_name(err));

            return;
    }

    err = sim7070g::start_session();

    if (err != ESP_OK)
    {
            printf("SIM startup failed: %s\n",
                        esp_err_to_name(err));

            return;
    }

    err = sim7070g::start_tasks();

    if (err != ESP_OK)
    {
            printf("SIM task startup failed: %s\n",
                        esp_err_to_name(err));

            return;
    }

    printf("SIM session and modem tasks started\n");
}
