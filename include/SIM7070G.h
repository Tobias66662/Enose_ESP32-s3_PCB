#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace sim7070g {

esp_err_t init(uint32_t baud_rate = 115200);
esp_err_t power_on();
esp_err_t power_off();
esp_err_t send_command(const char *command, std::string &response, uint32_t timeout_ms);

} // namespace sim7070g
