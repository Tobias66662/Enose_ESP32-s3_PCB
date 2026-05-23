#pragma once

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "I2C_Sensors.h"

extern QueueHandle_t classifier_label_queue;

typedef struct
{
  char label[32];
} classifier_label_t;

void classifier_init();
void classifierTask(void *parameter);
