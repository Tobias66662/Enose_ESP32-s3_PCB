#include "MP2722.h"

#define MP2722_ADDR     0x6B
#define INT_PIN         GPIO_NUM_17
#define LOW_POWER_LED   GPIO_NUM_1
#define STATUS_REG_NUMB 3  // Number of status register that the interrupt task needs to check

#define BATT_LOW_STAT     BIT(4) // Checks bit 4
#define BATT_MISSING      BIT(6)
#define CHG_STAT_0        BIT(5)
#define CHG_STAT_1        BIT(6)
#define CHG_STAT_2        BIT(7)
#define CHG_STAT_MASK     (CHG_STAT_0 | CHG_STAT_1 | CHG_STAT_2)


static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t handle_MP2722;

static const char *TAG = "MP2722";
static SemaphoreHandle_t Semaphore; // Semaphore used in the ISR

void MP2722_setRegisters();


//-------------- Interrupt stuff ----------------//
// Interupt service routine (deferred interrupt)
static void IRAM_ATTR MP2722_isr(void *arg)
{
  BaseType_t higherPriorityTaskWoken = pdFALSE;

  xSemaphoreGiveFromISR(Semaphore, &higherPriorityTaskWoken);

  // If xSemaphoreGiveFromISR() sets higherPriorityTaskWoken = pdTRUE then a context switch is requested before the interrupt is exited.
  if(higherPriorityTaskWoken)
  {
    portYIELD_FROM_ISR();
  }
}

// Deferred interrupt handle task that is run whenever the interrupt occurs
static void MP2722_interruptTask(void *parameter)
{
  uint8_t reg16_prev = 0; //Status register for low battery
  uint8_t reg13_prev = 0; //Status register for Chargning state
  uint8_t reg14_prev = 0; //Status register for missing battery and NTC
  uint8_t status_reg = 0;

  // pull current status register values
  MP2722_readRegister(0x16, &reg16_prev);
  MP2722_readRegister(0x13, &reg13_prev);
  MP2722_readRegister(0x14, &reg14_prev);

  while(1)
  {
    if(xSemaphoreTake(Semaphore, portMAX_DELAY) == pdTRUE) // This task waits indefinitly in a blocked state until a semaphore is available, at which point it ruturns to a ready state
    {
      //check for low battery
      MP2722_readRegister(0x16, &status_reg);
      if(status_reg != reg16_prev) // Check if changes have happend since last pull
      {
        reg16_prev = status_reg;
        if (status_reg & BATT_LOW_STAT)
        {
          gpio_set_level(LOW_POWER_LED, 1); // Turn on low power indicator LED
        }
        else
        {
          gpio_set_level(LOW_POWER_LED, 0);
        }
      }

      // Checks for change in chargning state
      MP2722_readRegister(0x13, &status_reg);
      if(status_reg != reg13_prev)
      {
        reg13_prev = status_reg;
        if ((status_reg & CHG_STAT_MASK) == 0) // Check if the 3 bit are 0
        {
          ESP_LOGI(TAG, "Battery is not charging");
        }
        else
        {
          ESP_LOGI(TAG, "Battery is charging");
        }
      }

      // Checks if battery is connected
      MP2722_readRegister(0x14, &status_reg);
      if(status_reg != reg14_prev)
      {
        reg14_prev = status_reg;
        if (status_reg & BATT_MISSING)
        {
          ESP_LOGI(TAG, "No battery connected");
        }
        else
        {
          ESP_LOGI(TAG, "Battery connected");
        }
      }

    }
  }
}

void MP2722_interruptInit()
{
  Semaphore = xSemaphoreCreateBinary();

  gpio_config_t GPIO_config = {
    .pin_bit_mask = (1ULL << INT_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,     // MP2722 INT pin is active low/open drain
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE        // Trigger on falling edge (active low)
    };
  gpio_config(&GPIO_config);

  //gpio_install_isr_service(0); // set up the interrupt
  gpio_isr_handler_add(INT_PIN, MP2722_isr, NULL); // Attach ISR function to INT_PIN

  xTaskCreate(MP2722_interruptTask, "mp2722_int_task", 2048, NULL, 10, NULL); // Create the deferred interrupt handle task
}


//-------------- Not interrupt stuff ----------------//
void MP2722_init()
{
  i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = MP2722_ADDR;
    dev_config.scl_speed_hz = 100000;

  esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config, &handle_MP2722);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to add MP2722 to I2C bus");
    return;
  }

  MP2722_setRegisters();

  gpio_set_direction(LOW_POWER_LED, GPIO_MODE_OUTPUT); // Sets the power status leds a output
  gpio_set_level(LOW_POWER_LED, 0);

  MP2722_interruptInit();
  return;
}

void MP2722_readRegister(uint8_t reg, uint8_t* value)
{
  esp_err_t err = i2c_master_transmit_receive(handle_MP2722, &reg, 1, value, 1, pdMS_TO_TICKS(100));
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "MP2722 I2C transmit Failed");
  }
}

void MP2722_writeRegister(uint8_t reg, uint8_t value)
{
  uint8_t data[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(handle_MP2722, data, sizeof(data), pdMS_TO_TICKS(100));
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "MP2722 I2C transmit Failed");
  }
}

// Makes desired changes to the default settings of the battery charger
void MP2722_setRegisters()
{
  // Change the system minimum regulation voltage to 3.15V
  MP2722_writeRegister(0x06, 0b00001100);

  // Sent INT pulse if battery falls bellow 3.3V
  MP2722_writeRegister(0x0C, 0b01011101);

  // Masks(disables) unwanted interrupts
  MP2722_writeRegister(0x10, 0b01111101);
}
