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


#define PIN GPIO_NUM_38 // PCB DEBUG LED 

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
      printf("SIM init failed: %s\n",
            esp_err_to_name(err));

      return;
  }

  while (1)
  {
      bool modem_powered_on = false;
      bool mqtt_connected = false;

      printf("\n============================\n");
      printf("Starting SIM7070G cycle\n");
      printf("============================\n");

      // =================================================
      // POWER ON MODEM
      // =================================================

      err = sim7070g::power_on();

      if (err != ESP_OK)
      {
          printf("SIM power on failed: %s\n",
                esp_err_to_name(err));

          goto shutdown;
      }

      modem_powered_on = true;

      // =================================================
      // BASIC AT TEST
      // =================================================

      {
          std::string response;

          err = sim7070g::send_command(
              "AT\r\n",
              response,
              3000);

          if (err != ESP_OK)
          {
              printf("AT failed: %s\n",
                    esp_err_to_name(err));

              goto shutdown;
          }

          printf("AT response:\n%s\n",
                response.c_str());
      }

      // =================================================
      // SIM READY CHECK
      // =================================================

      {
          std::string response;

          err = sim7070g::send_command(
              "AT+CPIN?\r\n",
              response,
              5000);

          if (err != ESP_OK)
          {
              printf("CPIN failed: %s\n",
                    esp_err_to_name(err));

              goto shutdown;
          }

          printf("CPIN response:\n%s\n",
                response.c_str());

          if (response.find("READY")
              == std::string::npos)
          {
              printf("SIM card not ready\n");

              goto shutdown;
          }
      }

      // =================================================
      // NETWORK MODE
      // =================================================

      err = sim7070g::set_network_mode(1);

      if (err != ESP_OK)
      {
          printf("Failed to set CAT-M mode\n");

          goto shutdown;
      }

      // =================================================
      // CHECK LTE REGISTRATION
      // =================================================

      err = sim7070g::check_network_registration();

      if (err != ESP_OK)
      {
          printf("LTE registration check failed\n");

          goto shutdown;
      }

      // =================================================
      // CHECK PACKET ATTACHMENT
      // =================================================

      err = sim7070g::check_packet_attachment();

      if (err != ESP_OK)
      {
          printf("Packet attachment failed\n");

          goto shutdown;
      }

      // =================================================
      // CONFIGURE APN
      // =================================================

      err = sim7070g::configure_apn();

      if (err != ESP_OK)
      {
          printf("APN configuration failed\n");

          goto shutdown;
      }

      // =================================================
      // ACTIVATE DATA CONNECTION
      // =================================================

      // Try activating the data connection up to 5 times before giving up
      {
          const int kMaxDataAttempts = 5;
          const TickType_t kRetryDelay = pdMS_TO_TICKS(2000);
          int attempt = 0;

          for (attempt = 1; attempt <= kMaxDataAttempts; ++attempt)
          {
              err = sim7070g::activate_data_connection();

              if (err == ESP_OK)
              {
                  break;
              }

              printf("Data connection attempt %d/%d failed: %s\n",
                     attempt,
                     kMaxDataAttempts,
                     esp_err_to_name(err));

              vTaskDelay(kRetryDelay);
          }

          if (err != ESP_OK)
          {
              printf("Data connection failed after %d attempts\n",
                     kMaxDataAttempts);

              goto shutdown;
          }

          printf("Data connection established\n");
      }

      // =================================================
      // MQTT CONFIGURATION
      // =================================================

      err = sim7070g::mqtt_configure();

      if (err != ESP_OK)
      {
          printf("MQTT configuration failed\n");

          goto shutdown;
      }

      // =================================================
      // MQTT CONNECT
      // =================================================

      err = sim7070g::mqtt_connect();

      if (err != ESP_OK)
      {
          printf("MQTT connect failed\n");

          goto shutdown;
      }

      mqtt_connected = true;

      printf("MQTT connected\n");

      // =================================================
      // MQTT SUBSCRIBE
      // =================================================

      vTaskDelay(pdMS_TO_TICKS(1000));
      err = sim7070g::mqtt_subscribe();

      if (err != ESP_OK)
      {
          printf("MQTT subscribe failed\n");

          goto shutdown;
      }

      printf("MQTT subscribed\n");

      // =================================================
      // MQTT PUBLISH
      // =================================================

      err = sim7070g::mqtt_publish(
          "hello from " +
          std::string(sim7070g::MQTT_CLIENT_ID));

      if (err != ESP_OK)
      {
          printf("MQTT publish failed\n");

          goto shutdown;
      }

      printf("MQTT publish success\n");

      // =================================================
      // WAIT FOR INCOMING MQTT MESSAGES
      // =================================================

      printf("Listening for MQTT messages...\n");

      for (int i = 0; i < 60; ++i)
      {
          // Poll modem UART for unsolicited responses (URCs)
          std::string urc;

          esp_err_t r = sim7070g::get_response(urc, 500);

          if (r == ESP_OK)
          {
              printf("URC:\n%s\n", urc.c_str());
          }

          vTaskDelay(pdMS_TO_TICKS(1000));
      }

  shutdown:

      // =================================================
      // MQTT DISCONNECT
      // =================================================

      if (mqtt_connected)
      {
          err = sim7070g::mqtt_disconnect();

          if (err != ESP_OK)
          {
              printf("MQTT disconnect failed: %s\n",
                    esp_err_to_name(err));
          }
          else
          {
              printf("MQTT disconnected\n");
          }
      }

      vTaskDelay(pdMS_TO_TICKS(3000)); // delay so modem has time to unsubscribe and disconnect before powering off
      // =================================================
      // POWER OFF MODEM
      // =================================================

      if (modem_powered_on)
      {
          err = sim7070g::power_off();

          if (err != ESP_OK)
          {
              printf("SIM power off failed: %s\n",
                    esp_err_to_name(err));
          }
          else
          {
              printf("SIM powered off\n");
          }
      }

      // =================================================
      // SAFE POWER-DOWN WAIT
      // =================================================

      printf("Waiting 10 seconds before next cycle...\n");

      vTaskDelay(pdMS_TO_TICKS(10000));
  } // end of while(1)

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
