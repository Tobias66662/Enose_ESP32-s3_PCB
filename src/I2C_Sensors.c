#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "I2C_Sensors.h"

#include "Sensirion_Drivers/sen66_i2c.h"
#include "Sensirion_Drivers/sfa3x_i2c.h"
#include "Sensirion_Drivers/sensirion_i2c_hal.h"

#define I2C_SDA_GPIO      GPIO_NUM_18
#define I2C_SCL_GPIO      GPIO_NUM_8
//#define FAN_PIN           GPIO_NUM_16

QueueHandle_t sensor_readings_queue = NULL;

//void sensor_fan_enable(bool enable);
void sensor_readings_task(void *parameter);

void sensor_init()
{
  esp_err_t err = sensirion_i2c_hal_init((uint8_t)I2C_SDA_GPIO, (uint8_t)I2C_SCL_GPIO); // Initialize the sensors
  if (err != ESP_OK)
  {
    ESP_LOGE("Sensiron", "I2C init failed: %s", esp_err_to_name(err));
    return;
  }

  //gpio_set_direction(FAN_PIN, GPIO_MODE_OUTPUT); // Configure fan pin
  //sensor_fan_enable(false);  // Ensure fan is off on startup

  sensor_readings_queue = xQueueCreate(5, sizeof(sensor_readings_t)); // creates queue that can store 5 commands
  //xTaskCreate(sensor_readings_task, "sensor_task", 4096, sensor_readings_queue, 5, NULL); // Creates sensor_readings_task with priority 5 and with 4096 bytes on the stack allocated to it (might be too little)

}

void initalize_SEN66()
{
  //--------- Initialize SEN66 -------------
  vTaskDelay(pdMS_TO_TICKS(1000)); // To ensure that the SEN66 is enabled
  esp_err_t err = sen66_device_reset(); // Reset device
  if (err != ESP_OK)
  {
    ESP_LOGE("SEN66", "SEN66 reset failed: %d", err);
    vTaskDelete(NULL); // Kill the task if we can't initialize
  }

  err = sen66_start_continuous_measurement(); // Start reading from SEN66 sensor
  if (err != ESP_OK)
  {
    ESP_LOGE("SEN66", "SEN66 start measurement failed: %d", err);
    vTaskDelete(NULL);
  }
}

void initalize_SFA30()
{
  //--------- Initialize SFA30 -------------
  vTaskDelay(pdMS_TO_TICKS(10000)); // SFA30 needs 10 seconds after power-up before formaldehyde output is valid
  esp_err_t err = sfa3x_device_reset(); // Reset device
  if (err != ESP_OK)
  {
    ESP_LOGE("SFA30", "SFA30 reset failed: %d", err);
    vTaskDelete(NULL); // Kill the task if we can't initialize
  }
  ESP_LOGI("SFA30", "Device reset succes");

  err = sfa3x_start_continuous_measurement(); // Start reading from SFA30 sensor
  if (err != ESP_OK)
  {
    ESP_LOGE("SFA30", "SFA30 start measurement failed: %d", err);
    vTaskDelete(NULL);
  }
}

void sensor_readings_task(void* parameter)
{
  esp_err_t err;

  //--------- Initialize Sensors -------------
  initalize_SEN66();
  initalize_SFA30();

  // Wait for first measurement to be ready
  vTaskDelay(pdMS_TO_TICKS(1000));

  TickType_t last_awake_tick_count = xTaskGetTickCount(); // current tick count of the timer

  while(1)
  {
    sensor_readings_t reading = {0};

    //--------- Read SEN66 -------------
    uint16_t massConcentrationPm1p0 = 0;
    uint16_t massConcentrationPm2p5 = 0;
    uint16_t massConcentrationPm4p0 = 0;
    uint16_t massConcentrationPm10p0 = 0;
    int16_t sen66_humidity = 0;
    int16_t sen66_temperature = 0;
    int16_t vocIndex = 0;
    int16_t noxIndex = 0;
    uint16_t co2 = 0;

    err = sen66_read_measured_values_as_integers(
      &massConcentrationPm1p0, &massConcentrationPm2p5,
      &massConcentrationPm4p0, &massConcentrationPm10p0, &sen66_humidity,
      &sen66_temperature, &vocIndex, &noxIndex, &co2);
    if (err != ESP_OK)
    {
      ESP_LOGE("SEN66", "SEN66 read failed: %d", err);
    }
    else
    {
      reading.pm1p0 = massConcentrationPm1p0 / 10.0f;
      reading.pm2p5 = massConcentrationPm2p5 / 10.0f;
      reading.pm4p0 = massConcentrationPm4p0 / 10.0f;
      reading.pm10p0 = massConcentrationPm10p0 / 10.0f;
      reading.sen66_temperature = sen66_temperature / 200.0f;
      reading.sen66_humidity = sen66_humidity / 100.0f;
      reading.voc_index = vocIndex / 10.0f;
      reading.nox_index = noxIndex / 10.0f;
      reading.co2 = co2;
    }

    //--------- Read SFA30 -------------
    int16_t hcho;
    int16_t sfa30_humidity;
    int16_t sfa30_temperature;

    err = sfa3x_read_measured_values(&hcho, &sfa30_humidity, &sfa30_temperature);
    if (err != ESP_OK)
    {
      ESP_LOGE("SFA30", "SFA30 read failed: %d", err);
    }
    else
    {
      reading.hcho = hcho / 5.0f;
      reading.sfa30_humidity = sfa30_humidity / 100.0f;
      reading.sfa30_temperature = sfa30_temperature / 200.0f;
    }

    ESP_LOGI("Sensors", "Sensor readings sent to Queue");
    //--------- Send Reading to Queue -------------
    if (xQueueSend(sensor_readings_queue, &reading, pdMS_TO_TICKS(100)) == pdFALSE)
    {
      ESP_LOGW("Sensors", "Sensor readings queue full");
    }

    // //--------- LOG reading (for testing purposes) ------------- (Comment this out when using the cellular modem)
    // ESP_LOGI("SEN66", "PM1: %.1f µg/m³, PM2.5: %.1f µg/m³, PM4: %.1f µg/m³, PM10: %.1f µg/m³, Humidity: %.1f %%, Temp: %.1f °C, CO2: %u ppm, VOC: %.1f, NOX: %.1f",
    //   reading.pm1p0,
    //   reading.pm2p5,
    //   reading.pm4p0,
    //   reading.pm10p0,
    //   reading.sen66_humidity,
    //   reading.sen66_temperature,
    //   reading.co2,
    //   reading.voc_index,
    //   reading.nox_index);

    // ESP_LOGI("SFA30", "HCHO: %.1f ppb, Humidity: %.1f %%, Temp: %.1f °C",
    //   reading.hcho,
    //   reading.sfa30_humidity,
    //   reading.sfa30_temperature);

    // Ensure that 1 second passes between every reading
    vTaskDelayUntil(&last_awake_tick_count, pdMS_TO_TICKS(1000)); // This function updates "last_awake_tick_count" to equal the current tick count when finishing wait
  }

}

void SEN66_sensor_readings(void *parameter)
{
  esp_err_t err;
  initalize_SEN66(); // Initalize sensor

  vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for first measurement to be ready

  uint16_t massConcentrationPm1p0 = 0;
  uint16_t massConcentrationPm2p5 = 0;
  uint16_t massConcentrationPm4p0 = 0;
  uint16_t massConcentrationPm10p0 = 0;
  int16_t humidity = 0;
  int16_t temperature = 0;
  int16_t vocIndex = 0;
  int16_t noxIndex = 0;
  uint16_t co2 = 0;

  while (1)
  {
    err = sen66_read_measured_values_as_integers(
      &massConcentrationPm1p0, &massConcentrationPm2p5,
      &massConcentrationPm4p0, &massConcentrationPm10p0, &humidity,
      &temperature, &vocIndex, &noxIndex, &co2);
    if (err != ESP_OK)
    {
      ESP_LOGE("SEN66", "SEN66 read failed: %d", err);
    }
    else
    {
      ESP_LOGI("SEN66", "PM1:   %.1f µg/m³",  massConcentrationPm1p0 / 10.0f);
      ESP_LOGI("SEN66", "PM2.5: %.1f µg/m³",  massConcentrationPm2p5 / 10.0f);
      ESP_LOGI("SEN66", "PM4:   %.1f µg/m³",  massConcentrationPm4p0 / 10.0f);
      ESP_LOGI("SEN66", "PM10:  %.1f µg/m³",  massConcentrationPm10p0 / 10.0f);
      ESP_LOGI("SEN66", "CO2:   %u ppm",      co2);
      ESP_LOGI("SEN66", "Temp:  %.1f °C",     temperature / 200.0f);
      ESP_LOGI("SEN66", "RH:    %.1f %%",     humidity / 100.0f);
      ESP_LOGI("SEN66", "VOC:   %.1f",        vocIndex / 10.0f);
      ESP_LOGI("SEN66", "NOx:   %.1f",        noxIndex / 10.0f);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  sen66_stop_measurement();
  vTaskDelete(NULL);
}

void SFA30_sensor_readings(void *parameter)
{
  esp_err_t err;
  initalize_SFA30(); // Initalize sensor

  vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for first measurement to be ready

  int16_t hcho;
  int16_t humidity;
  int16_t temperature;

  while (1)
  {
    err = sfa3x_read_measured_values(&hcho, &humidity, &temperature);
    if (err != ESP_OK)
    {
      ESP_LOGE("SFA30", "SFA30 read failed: %d", err);
    }
    else
    {
      ESP_LOGI("SFA30", "HCHO: %.1f ppb",         hcho / 5.0f);
      ESP_LOGI("SFA30", "Humidity: %.1f %%",  humidity / 100.0f);
      ESP_LOGI("SFA30", "Temp:  %.1f °C",     temperature / 200.0f);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  sfa3x_stop_measurement();
  vTaskDelete(NULL);
}

void sensor_fan_enable(bool enable)
{
  esp_err_t err = gpio_set_level(FAN_PIN, enable ? 1 : 0); // if enable is true, pass 1, otherwise pass 0
  if (err != ESP_OK)
  {
    ESP_LOGE("FAN", "Fan failed to enable");
    return;
  }
  ESP_LOGI("FAN", "Fan enabled");
}
