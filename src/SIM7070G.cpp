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

    esp_err_t err =
        uart_param_config(kModemUart, &config);

    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_set_pin(
        kModemUart,
        kModemTxPin,
        kModemRxPin,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);

    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_driver_install(
        kModemUart,
        kUartBufferSize,
        kUartBufferSize,
        0,
        nullptr,
        0);

    if (err != ESP_OK)
    {
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

    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_set_level(kModemPwrKeyPin, 0);

    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

void modem_write(const char *data)
{
    uart_write_bytes(
        kModemUart,
        data,
        std::strlen(data));

    uart_wait_tx_done(
        kModemUart,
        pdMS_TO_TICKS(1000));
}

bool read_response(
    std::string &response,
    uint32_t timeout_ms)
{
    response.clear();

    const TickType_t timeout_ticks =
        pdMS_TO_TICKS(timeout_ms);

    TickType_t last_activity =
        xTaskGetTickCount();

    while ((xTaskGetTickCount() - last_activity)
           < timeout_ticks)
    {
        uint8_t chunk[128];

        const int received =
            uart_read_bytes(
                kModemUart,
                chunk,
                sizeof(chunk),
                pdMS_TO_TICKS(100));

        if (received > 0)
        {
            response.append(
                reinterpret_cast<char *>(chunk),
                received);

            last_activity =
                xTaskGetTickCount();
        }
    }

    return !response.empty();
}

bool send_raw_command(
    const char *command,
    std::string &response,
    uint32_t timeout_ms)
{
    uart_flush_input(kModemUart);

    modem_write(command);

    return read_response(response, timeout_ms);
}

bool response_contains(
    const std::string &response,
    const char *token)
{
    return response.find(token)
           != std::string::npos;
}

esp_err_t send_checked_command(
    const std::string &command,
    uint32_t timeout_ms,
    std::string *out_response = nullptr)
{
    std::string response;

    const bool got_response =
        send_raw_command(
            command.c_str(),
            response,
            timeout_ms);

    printf("\nCMD: %s", command.c_str());
    printf("RSP:\n%s\n", response.c_str());

    if (out_response != nullptr)
    {
        *out_response = response;
    }

    if (!got_response)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (response_contains(response, "OK"))
    {
        return ESP_OK;
    }

    if (response_contains(response, "ERROR"))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t wait_for_modem_ready()
{
    std::string response;

    for (int attempt = 0;
         attempt < 8;
         ++attempt)
    {
        const bool got_response =
            send_raw_command(
                "AT\r\n",
                response,
                kPowerOnReadyTimeoutMs);

        if (got_response
            && response_contains(response, "OK"))
        {
            send_raw_command(
                "ATE0\r\n",
                response,
                1000);

            return ESP_OK;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                kInterCommandDelayMs));
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

    send_raw_command(
        "AT+CPOWD=1\r\n",
        response,
        5000);

    vTaskDelay(pdMS_TO_TICKS(5000));
}

} // namespace

namespace sim7070g {

esp_err_t init(uint32_t baud_rate)
{
    esp_err_t err =
        modem_power_pin_init();

    if (err != ESP_OK)
    {
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

esp_err_t send_command(
    const char *command,
    std::string &response,
    uint32_t timeout_ms)
{
    if (command == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return send_checked_command(
        command,
        timeout_ms,
        &response);
}

// =====================================================
// NETWORK
// =====================================================

esp_err_t set_network_mode(uint8_t mode)
{
    std::string cmd =
        "AT+CMNB=" +
        std::to_string(mode) +
        "\r\n";

    return send_checked_command(
        cmd,
        10000);
}

esp_err_t configure_apn()
{
    std::string cmd =
        "AT+CGDCONT=1,\"IP\",\"" +
        std::string(APN) +
        "\"\r\n";

    return send_checked_command(
        cmd,
        10000);
}

esp_err_t activate_data_connection()
{
    return send_checked_command(
        "AT+CNACT=0,1\r\n",
        20000);
}

esp_err_t check_network_registration()
{
    return send_checked_command(
        "AT+CEREG?\r\n",
        10000);
}

esp_err_t check_packet_attachment()
{
    return send_checked_command(
        "AT+CGATT?\r\n",
        10000);
}

// =====================================================
// MQTT
// =====================================================

esp_err_t mqtt_configure()
{
    esp_err_t err;

    std::string broker_cmd =
        "AT+SMCONF=\"URL\",\"" +
        std::string(MQTT_BROKER) +
        "\"," +
        std::to_string(MQTT_PORT) +
        "\r\n";

    err = send_checked_command(
        broker_cmd,
        10000);

    if (err != ESP_OK)
    {
        return err;
    }

    std::string client_cmd =
        "AT+SMCONF=\"CLIENTID\",\"" +
        std::string(MQTT_CLIENT_ID) +
        "\"\r\n";

    return send_checked_command(
        client_cmd,
        10000);
}

esp_err_t mqtt_connect()
{
    return send_checked_command(
        "AT+SMCONN\r\n",
        20000);
}

esp_err_t mqtt_disconnect()
{
    return send_checked_command(
        "AT+SMDISC\r\n",
        10000);
}

esp_err_t mqtt_subscribe()
{
    std::string cmd =
        "AT+SMSUB=\"" +
        std::string(MQTT_SUBSCRIBE_TOPIC) +
        "\"," +
        std::to_string(MQTT_QOS) +
        "\r\n";

    return send_checked_command(
        cmd,
        10000);
}

esp_err_t mqtt_publish(
    const std::string &payload)
{
    std::string cmd =
        "AT+SMPUB=\"" +
        std::string(MQTT_PUBLISH_TOPIC) +
        "\"," +
        std::to_string(payload.length()) +
        "," +
        std::to_string(MQTT_QOS) +
        ",0\r\n";

    std::string response;

    esp_err_t err =
        send_checked_command(
            cmd,
            10000,
            &response);

    if (err != ESP_OK)
    {
        return err;
    }

    return send_checked_command(
        payload,
        10000);
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
