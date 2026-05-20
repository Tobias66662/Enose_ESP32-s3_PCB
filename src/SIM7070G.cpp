#include "SIM7070G.h"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "I2C_Sensors.h"
#include "Emitter.h"

namespace {
constexpr uart_port_t kModemUart = UART_NUM_1;
constexpr gpio_num_t kModemTxPin = GPIO_NUM_43;
constexpr gpio_num_t kModemRxPin = GPIO_NUM_44;
constexpr gpio_num_t kModemPwrKeyPin = GPIO_NUM_2;
constexpr int kUartBufferSize = 1024;
constexpr uint32_t kPowerOnReadyTimeoutMs = 1200;
constexpr uint32_t kInterCommandDelayMs = 250;

esp_err_t modem_uart_init(uint32_t baud_rate)
{
  uart_config_t config = {};
  config.baud_rate = static_cast<int>(baud_rate);
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_APB;

  esp_err_t err = uart_param_config(kModemUart, &config);
  if (err != ESP_OK) {
    return err;
  }

  err = uart_set_pin(kModemUart, kModemTxPin, kModemRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    return err;
  }

  err = uart_driver_install(kModemUart, kUartBufferSize, kUartBufferSize, 0, nullptr, 0);
  if (err != ESP_OK) {
    return err;
  }

  uart_flush_input(kModemUart);
  return ESP_OK;
}

esp_err_t modem_power_pin_init()
{
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << kModemPwrKeyPin;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;

  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    return err;
  }

  err = gpio_set_level(kModemPwrKeyPin, 0);
  if (err != ESP_OK) {
    return err;
  }

  return ESP_OK;
}

void modem_write(const char *data)
{
  uart_write_bytes(kModemUart, data, std::strlen(data));
  uart_wait_tx_done(kModemUart, pdMS_TO_TICKS(1000));
}

bool read_response(std::string &response, uint32_t timeout_ms)
{
  response.clear();
  const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
  TickType_t last_activity = xTaskGetTickCount();

  while ((xTaskGetTickCount() - last_activity) < timeout_ticks) {
    uint8_t chunk[128];
    const int received = uart_read_bytes(kModemUart, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
    if (received > 0) {
      response.append(reinterpret_cast<char *>(chunk), received);
      last_activity = xTaskGetTickCount();
    }
  }

  return !response.empty();
}

bool send_command(const char *command, std::string &response, uint32_t timeout_ms)
{
  uart_flush_input(kModemUart);
  modem_write(command);
  return read_response(response, timeout_ms);
}

bool response_contains(const std::string &response, const char *token)
{
  return response.find(token) != std::string::npos;
}

esp_err_t wait_for_modem_ready()
{
  std::string response;

  for (int attempt = 0; attempt < 8; ++attempt) {
    const bool got_response = send_command("AT\r\n", response, kPowerOnReadyTimeoutMs);
    if (got_response && response_contains(response, "OK")) {
      // Disable local echo to make parsing and logs cleaner.
      send_command("ATE0\r\n", response, 1000);
      return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(kInterCommandDelayMs));
  }

  return ESP_ERR_TIMEOUT;
}

void modem_power_on()
{
  printf("Powering modem on\n");
  gpio_set_level(kModemPwrKeyPin, 1);
  vTaskDelay(pdMS_TO_TICKS(1000));
  gpio_set_level(kModemPwrKeyPin, 0);
  vTaskDelay(pdMS_TO_TICKS(7000));
}

void modem_power_off()
{
  printf("Powering modem off\n");
  std::string response;
  send_command("AT+CPOWD=1\r\n", response, 5000);
  vTaskDelay(pdMS_TO_TICKS(5000));  // Keep modem off for 5 seconds
}

} // namespace

namespace sim7070g {

esp_err_t init(uint32_t baud_rate)
{
  esp_err_t err = modem_power_pin_init();
  if (err != ESP_OK) {
    return err;
  }

  return modem_uart_init(baud_rate);
}

esp_err_t power_on()
{
  modem_power_on();
  return wait_for_modem_ready();
}

esp_err_t power_off()
{
  modem_power_off();
  return ESP_OK;
}

esp_err_t send_command(const char *command, std::string &response, uint32_t timeout_ms)
{
  if (command == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  const bool got_response = ::send_command(command, response, timeout_ms);
  if (!got_response) {
    return ESP_ERR_TIMEOUT;
  }

  if (response_contains(response, "\r\nOK\r\n") || response_contains(response, "\nOK\n")) {
    return ESP_OK;
  }

  if (response_contains(response, "\r\nERROR\r\n") || response_contains(response, "\nERROR\n")) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  return ESP_ERR_TIMEOUT;
}

} // namespace sim7070g

// FreeRTOS task that will control the modem and recive commands form the modem (Central task of the whole program)
void modem_manager_task(void* parameter)
{
  // Initialize modem and necessary variables/parameters here
  sensor_readings_t i2c_sesor_readings;

  while(1)  // Inifinite loop that waits for an event and then responds
  {

    // executes when data is available in the sensor_readings_queue
    if (xQueueReceive(sensor_readings_queue, &i2c_sesor_readings, portMAX_DELAY))
    {
      // send "i2c_sesor_readings" to the modem using UART
    }

  }

}
