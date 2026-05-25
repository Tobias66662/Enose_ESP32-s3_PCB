#include "SIM7070G.h"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "I2C_Sensors.h"
#include "Emitter.h"

QueueHandle_t modem_label_queue  = nullptr;
SemaphoreHandle_t modem_uart_mutex = nullptr;

namespace {

constexpr uart_port_t kModemUart = UART_NUM_1;
constexpr gpio_num_t kModemTxPin = GPIO_NUM_43;
constexpr gpio_num_t kModemRxPin = GPIO_NUM_44;
constexpr gpio_num_t kModemPwrKeyPin = GPIO_NUM_2;

constexpr int kUartBufferSize = 1024;

constexpr uint32_t kPowerOnReadyTimeoutMs = 1200;
constexpr uint32_t kInterCommandDelayMs = 250;

void modem_transmit_task(void *parameter);
void modem_uart_reader_task(void *parameter);
void modem_receive_task(void *parameter);

/* Internal helper function called by start_session to run the modem bring-up flow
    (power on, AT test, network setup, APN, data session, MQTT config and subscribe).
*/
esp_err_t run_modem_session_setup()
{
    esp_err_t err = sim7070g::power_on();

    if (err != ESP_OK)
    {
        return err;
    }

    {
        std::string response;

        err = sim7070g::send_command("AT\r\n", response, 3000);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    {
        std::string response;

        err = sim7070g::send_command("AT+CPIN?\r\n", response, 5000);
        if (err != ESP_OK)
        {
            return err;
        }

        if (response.find("READY") == std::string::npos)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }

    err = sim7070g::set_network_mode(1);
    if (err != ESP_OK)
    {
        return err;
    }

    err = sim7070g::check_network_registration();
    if (err != ESP_OK)
    {
        return err;
    }

    err = sim7070g::check_packet_attachment();
    if (err != ESP_OK)
    {
        return err;
    }

    err = sim7070g::configure_apn();
    if (err != ESP_OK)
    {
        return err;
    }

    {
        constexpr int kMaxDataAttempts = 5;
        constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(2000);

        for (int attempt = 1; attempt <= kMaxDataAttempts; ++attempt)
        {
            err = sim7070g::activate_data_connection();
            if (err == ESP_OK)
            {
                break;
            }

            vTaskDelay(kRetryDelay);
        }

        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = sim7070g::mqtt_configure();
    if (err != ESP_OK)
    {
        return err;
    }

    err = sim7070g::mqtt_connect();
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    err = sim7070g::mqtt_subscribe();
    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

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
          //   ESP_LOGI("SIM7070G", "response detected");
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
    if (modem_uart_mutex == nullptr)
    {
        return false;
    }

    if (xSemaphoreTake(modem_uart_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return false;
    }

    uart_flush_input(kModemUart);

    modem_write(command);

    response.clear();

    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start_tick = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks)
    {
        uint8_t chunk[128];

        const int received =
            uart_read_bytes(
                kModemUart,
                chunk,
                sizeof(chunk),
                pdMS_TO_TICKS(100));

        if (received <= 0)
        {
            continue;
        }

        response.append(
            reinterpret_cast<char *>(chunk),
            received);

        if (response.find("\r\nOK\r\n") != std::string::npos ||
            response.find("\nOK\n") != std::string::npos ||
            response.find("\r\nERROR\r\n") != std::string::npos ||
            response.find("\nERROR\n") != std::string::npos ||
            response.find(">") != std::string::npos)
        {
            xSemaphoreGive(modem_uart_mutex);
            return true;
        }
    }

    xSemaphoreGive(modem_uart_mutex);
    return !response.empty();
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
    modem_label_queue = xQueueCreate(5, sizeof(scent_label_t));

    if (modem_label_queue == nullptr)
    {
      return ESP_ERR_NO_MEM;
    }

  esp_err_t err =
    modem_power_pin_init();

  if (err != ESP_OK)
  {
    return err;
  }

    modem_uart_mutex = xSemaphoreCreateMutex();

    if (modem_uart_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

  return modem_uart_init(baud_rate);
}

esp_err_t start_session()
{
    constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(3000);

    int attempt = 0;

    while (true)
    {
        ++attempt;

        esp_err_t err = run_modem_session_setup();
        if (err == ESP_OK)
        {
            return ESP_OK;
        }

        ESP_LOGW(
            "SIM7070G",
            "Session setup attempt %d failed: %s",
            attempt,
            esp_err_to_name(err));

        /* Ensure we start from a clean modem state before retrying. */
        sim7070g::power_off();
        vTaskDelay(kRetryDelay);
    }
}

esp_err_t start_tasks()
{
    if (classifier_label_queue == nullptr || modem_label_queue == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(
        modem_transmit_task,
        "modem_transmit_task",
        4096,
        nullptr,
        6,
        nullptr);

    if (ok != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(
        modem_uart_reader_task,
        "modem_uart_reader_task",
        4096,
        nullptr,
        5,
        nullptr);

    if (ok != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(
        modem_receive_task,
        "modem_receive_task",
        3072,
        nullptr,
        4,
        nullptr);

    if (ok != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
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

// Read any pending data from the modem UART (URC / unsolicited messages).
esp_err_t get_response(std::string &response, uint32_t timeout_ms)
{
    if (modem_uart_mutex == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(modem_uart_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // read_response returns true when data was received
    if (read_response(response, timeout_ms))
    {
        xSemaphoreGive(modem_uart_mutex);
        return ESP_OK;
    }

    xSemaphoreGive(modem_uart_mutex);
    return ESP_ERR_TIMEOUT;
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

    // -------------------------------------------------
    // Configure MQTT broker
    // -------------------------------------------------

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

    // -------------------------------------------------
    // Configure MQTT client ID
    // -------------------------------------------------

    std::string client_cmd =
        "AT+SMCONF=\"CLIENTID\",\"" +
        std::string(mqtt_client_id()) +
        "\"\r\n";

    err = send_checked_command(
        client_cmd,
        10000);

    if (err != ESP_OK)
    {
        return err;
    }

    // -------------------------------------------------
    // Enable clean session
    // -------------------------------------------------

    err = send_checked_command(
        "AT+SMCONF=\"CLEANSS\",1\r\n",
        10000);

    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
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
        std::string(mqtt_subscribe_topic()) +
        "\"," +
        std::to_string(MQTT_QOS) +
        "\r\n";

    std::string response;

    const bool got_response =
        send_raw_command(
            cmd.c_str(),
            response,
            10000);

    printf("\nSUB CMD: %s", cmd.c_str());
    printf("SUB RESPONSE:\n%s\n",
           response.c_str());

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

esp_err_t mqtt_publish(
    const std::string &payload)
{
    std::string cmd =
        "AT+SMPUB=\"" +
        std::string(mqtt_publish_topic()) +
        "\"," +
        std::to_string(payload.length()) +
        "," +
        std::to_string(MQTT_QOS) +
        ",0\r\n";

    std::string response;

    // -------------------------------------------------
    // STEP 1:
    // Send SMPUB command
    // Expect '>' prompt
    // -------------------------------------------------

    const bool got_response =
        send_raw_command(
            cmd.c_str(),
            response,
            5000);

    printf("\nCMD: %s", cmd.c_str());
    printf("RSP:\n%s\n", response.c_str());

    if (!got_response)
    {
        return ESP_ERR_TIMEOUT;
    }

    // IMPORTANT:
    // Expect payload prompt
    if (!response_contains(response, ">"))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // -------------------------------------------------
    // STEP 2:
    // Send payload
    // -------------------------------------------------

    response.clear();

    const bool payload_response =
        send_raw_command(
            payload.c_str(),
            response,
            10000);

    printf("PAYLOAD: %s\n", payload.c_str());
    printf("RSP:\n%s\n", response.c_str());

    if (!payload_response)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (response_contains(response, "OK"))
    {
        return ESP_OK;
    }

    return ESP_ERR_INVALID_RESPONSE;
}

} // namespace sim7070g

namespace {

/* Scans a modem response for a known scent label and returns the matching enum value. */
scent_label_t parse_scent_label_from_response(const std::string &response)
{
  ESP_LOGI("sim70707G", "string to parse: %s", response.c_str());
  if (response.find("cinnamon") != std::string::npos) // Check if the response sting contains "cinnamon", retures npos if it doesen't
  {
    return SCENT_CINNAMON;
  }

  if (response.find("banana") != std::string::npos)
  {
    return SCENT_BANANA;
  }

  if (response.find("coconut") != std::string::npos)
  {
    return SCENT_COCONUT;
  }

  if (response.find("empty") != std::string::npos)
  {
    return SCENT_EMPTY;
  }

  return SCENT_UNKNOWN;
}


// FreeRTOS task that publishes classifier labels to MQTT.
void modem_transmit_task(void* parameter)
{
  (void)parameter;
  classifier_label_t detected_label = {};

  while(1)  // Inifinite loop that waits for an event and then responds
  {
    // executes when data is available in the classifier_label_queue
    if (xQueueReceive(classifier_label_queue, &detected_label, portMAX_DELAY))
    {
      // send "label" to the modem using UART
      esp_err_t err = sim7070g::mqtt_publish(detected_label.label);

      if (err != ESP_OK)
      {
        ESP_LOGW("SIM7070G", "MQTT publish failed for %s: %s", detected_label.label, esp_err_to_name(err));
      }
      else
      {
        ESP_LOGI("SIM7070G", "Published label: %s", detected_label.label);
      }
    }

  }

}

// Polls modem UART, extracts known labels from URCs, and forwards them to the receive queue.
void modem_uart_reader_task(void *parameter)
{
  (void)parameter;

  while (1)
  {
    ESP_LOGE("sim7070G", "loop beggining");
    std::string response;
    esp_err_t err = sim7070g::get_response(response, 1000); // Gets the raw string response from the UART buffer
    if (err != ESP_OK)
    {
    //   ESP_LOGE("sim7070G", "get_response returned an error");
      continue;
    }
    ESP_LOGE("sim7070G", "get_response did not get error");
    scent_label_t received_label = parse_scent_label_from_response(response); // Check for labels in the response string

    if (received_label == SCENT_UNKNOWN)
    {
      printf("URC:\n%s\n", response.c_str());
      continue;
    }

    if (received_label == SCENT_EMPTY)
    {
      printf("URC:\n%s\n", response.c_str());
      continue;
    }

    if (xQueueSend(modem_label_queue, &received_label, pdMS_TO_TICKS(100)) == pdFALSE)
    {
      ESP_LOGW("SIM7070G", "UART receive queue full");
    }
    else
    {
      ESP_LOGI("sim7070G", "Label sent to modem_label_queue");
    }
  }
}

// Handles parsed incoming labels coming from the modem UART reader task.
void modem_receive_task(void* parameter)
{
  (void)parameter;
  scent_label_t received_label = SCENT_UNKNOWN;
  emitter_cmd_t emitter_cmd = {};

  while(1)  // Inifinite loop that waits for an event and then responds
  {
    emitter_cmd = {};
    // executes when data is available in the modem_label_queue
    if (xQueueReceive(modem_label_queue, &received_label, portMAX_DELAY))
    {
      switch (received_label)
      {
      case SCENT_CINNAMON:
        ESP_LOGI("SIM7070G", "Received cinnamon command");
        // send specific commands to emmiter to produce scent
        //emitter_cmd = {.type = EMITTER_CMD_SET_CHANNELS, .PWM_channel_mask = 0b00000001, .duration_ms = 3000}; // Turn on emitter 1 for 1 second
        emitter_cmd.type = EMITTER_CMD_SET_CHANNELS;
        emitter_cmd.PWM_channel_mask = 0b00000001;
        emitter_cmd.duration_ms = 5000;

        xQueueSend(emitter_cmd_queue, &emitter_cmd, pdMS_TO_TICKS(100));
        break;

      case SCENT_BANANA:
        ESP_LOGI("SIM7070G", "Received banana command");
        // send specific commands to emmiter to produce scent
        emitter_cmd.type = EMITTER_CMD_SET_CHANNELS;
        emitter_cmd.PWM_channel_mask = 0b00000010;
        emitter_cmd.duration_ms = 5000;

        xQueueSend(emitter_cmd_queue, &emitter_cmd, pdMS_TO_TICKS(100));
        break;

      case SCENT_COCONUT:
        ESP_LOGI("SIM7070G", "Received coconut command");
        // send specific commands to emmiter to produce scent
        emitter_cmd.type = EMITTER_CMD_SET_CHANNELS;
        emitter_cmd.PWM_channel_mask = 0b00000100;
        emitter_cmd.duration_ms = 5000;

        xQueueSend(emitter_cmd_queue, &emitter_cmd, pdMS_TO_TICKS(100));
        break;

      case SCENT_EMPTY:
        ESP_LOGI("SIM7070G", "Received empty command");
        break;

      default:
        ESP_LOGW("SIM7070G", "Unknown label received: %d", received_label);
        break;
      }
    }

  }

}


} // namespace
