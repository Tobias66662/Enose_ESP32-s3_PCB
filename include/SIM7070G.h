#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace sim7070g {
// =====================================================
// USER CONFIGURATION
// =====================================================

// APN provided by your SIM/network provider
inline constexpr const char *APN = "www.internet.mtelia.dk";

// MQTT broker settings
inline constexpr const char *MQTT_BROKER = "broker.hivemq.com";
inline constexpr uint16_t MQTT_PORT = 1883;

// Every device MUST have a unique client ID
// Examples:
// "telescent_device_A"
// "telescent_device_B"
inline constexpr const char *MQTT_CLIENT_ID =
    "telescent_device_A";

// Topic this device publishes TO
inline constexpr const char *MQTT_PUBLISH_TOPIC =
    "telescent/deviceA";

// Topic this device subscribes TO
// "#" = wildcard for everything under telescent/
inline constexpr const char *MQTT_SUBSCRIBE_TOPIC =
    "telescent/#";

// Default MQTT QoS
// 0 = at most once
// 1 = at least once
// 2 = exactly once
inline constexpr uint8_t MQTT_QOS = 1;

// =====================================================
// MODEM CONTROL
// =====================================================

// Configures the modem UART and prepares the power-key GPIO.
esp_err_t init(uint32_t baud_rate = 115200);

// Pulses the modem power key to turn on the SIM7070G
// and waits until it responds with 'OK' to AT.
esp_err_t power_on();

// Requests the modem to shut down cleanly
// and waits for it to power off.
esp_err_t power_off();

// Sends a raw AT command and returns the modem response.
esp_err_t send_command(
    const char *command,
    std::string &response,
    uint32_t timeout_ms);

// =====================================================
// NETWORK / LTE
// =====================================================

// Configure CAT-M / NB-IoT mode
// mode:
// 1 = CAT-M
// 2 = NB-IoT
// 3 = CAT-M + NB-IoT
esp_err_t set_network_mode(uint8_t mode);

// Configure PDP/APN context
esp_err_t configure_apn();

// Activate PDP/data connection
esp_err_t activate_data_connection();

// Check LTE registration state
esp_err_t check_network_registration();

// Check packet service attachment
esp_err_t check_packet_attachment();

// =====================================================
// MQTT
// =====================================================

// Configure MQTT broker and client ID
esp_err_t mqtt_configure();

// Connect to MQTT broker
esp_err_t mqtt_connect();

// Disconnect from MQTT broker
esp_err_t mqtt_disconnect();

// Subscribe to configured topic
esp_err_t mqtt_subscribe();

// Publish payload to configured publish topic
esp_err_t mqtt_publish(const std::string &payload);

} // namespace sim7070g
