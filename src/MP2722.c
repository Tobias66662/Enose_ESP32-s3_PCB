#include "MP2722.h"
#include "Sensirion_Drivers/sensirion_i2c_hal.h"

#define MP2722_ADDR     0x3F
#define INT_PIN         GPIO_NUM_17
#define LOW_POWER_LED   GPIO_NUM_1
#define STATUS_REG_NUMB 3  // Number of status register that the interrupt task needs to check

#define BATT_LOW_STAT     BIT(4) // Checks bit 4
#define BATT_MISSING      BIT(6)
#define CHG_STAT_0        BIT(5)
#define CHG_STAT_1        BIT(6)
#define CHG_STAT_2        BIT(7)
#define CHG_STAT_MASK     (CHG_STAT_0 | CHG_STAT_1 | CHG_STAT_2)

#define MP2722_REG_FIRST_STATUS  0x11
#define MP2722_REG_LAST_STATUS   0x16

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

  esp_err_t err = i2c_master_bus_add_device(sensirion_i2c_hal_get_bus_handle(), &dev_config, &handle_MP2722);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to add MP2722 to I2C bus");
    return;
  }

  MP2722_setRegisters();

  gpio_set_direction(LOW_POWER_LED, GPIO_MODE_OUTPUT); // Sets the power status leds a output
  gpio_set_level(LOW_POWER_LED, 0);

  //MP2722_interruptInit();
  return;
}

void MP2722_readRegister(uint8_t reg, uint8_t* value)
{
  esp_err_t err = i2c_master_transmit_receive(handle_MP2722, &reg, 1, value, 1, pdMS_TO_TICKS(100));
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "MP2722 read reg 0x%02X failed: %s", reg, esp_err_to_name(err));
  }
}

void MP2722_writeRegister(uint8_t reg, uint8_t value)
{
  uint8_t data[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(handle_MP2722, data, sizeof(data), pdMS_TO_TICKS(100));
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "MP2722 write reg 0x%02X failed: %s", reg, esp_err_to_name(err));
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


//******************************** Code to dump register values for debugging ********************************//
static void MP2722_printBinary(uint8_t value)
{
  for (int i = 7; i >= 0; i--)
  {
    printf("%d", (value >> i) & 0x01);
  }
}

static const char* MP2722_decodeDpdmStat(uint8_t code)
{
  switch (code)
  {
    case 0x0: return "Not started / 500mA";
    case 0x1: return "USB SDP / 500mA";
    case 0x2: return "USB DCP / 2A";
    case 0x3: return "USB CDP / 1.5A";
    case 0x4: return "Divider 1 / 1A";
    case 0x5: return "Divider 2 / 2.1A";
    case 0x6: return "Divider 3 / 2.4A";
    case 0x7: return "Divider 4 / 2A";
    case 0x8: return "Unknown / 500mA";
    case 0x9: return "High-voltage adapter / 2A";
    case 0xE: return "Divider 5 / 3A";
    default:  return "Reserved/unknown";
  }
}

static const char* MP2722_decodeChgStat(uint8_t code)
{
  switch (code)
  {
    case 0x0: return "Not charging";
    case 0x1: return "Trickle charge";
    case 0x2: return "Pre-charge";
    case 0x3: return "Fast charge";
    case 0x4: return "Constant-voltage charge";
    case 0x5: return "Charging done";
    default:  return "Reserved/unknown";
  }
}

static const char* MP2722_decodeChgFault(uint8_t code)
{
  switch (code)
  {
    case 0x0: return "Normal";
    case 0x1: return "Input OVP";
    case 0x2: return "Charge timer expired";
    case 0x3: return "Battery OVP";
    default:  return "Unknown";
  }
}

static const char* MP2722_decodeNtcFault(uint8_t code)
{
  switch (code)
  {
    case 0x0: return "Normal";
    case 0x1: return "Warm";
    case 0x2: return "Cool";
    case 0x3: return "Cold";
    case 0x4: return "Hot";
    default:  return "Reserved/unknown";
  }
}

void MP2722_dumpStatusRegisters(void)
{
  uint8_t reg11 = 0;
  uint8_t reg12 = 0;
  uint8_t reg13 = 0;
  uint8_t reg14 = 0;
  uint8_t reg15 = 0;
  uint8_t reg16 = 0;

  MP2722_readRegister(0x11, &reg11);
  MP2722_readRegister(0x12, &reg12);
  MP2722_readRegister(0x13, &reg13);
  MP2722_readRegister(0x14, &reg14);
  MP2722_readRegister(0x15, &reg15);
  MP2722_readRegister(0x16, &reg16);

  printf("\n========== MP2722 raw status registers ==========\n");

  printf("REG11h = 0x%02X  b", reg11);
  MP2722_printBinary(reg11);
  printf("\n");

  printf("REG12h = 0x%02X  b", reg12);
  MP2722_printBinary(reg12);
  printf("\n");

  printf("REG13h = 0x%02X  b", reg13);
  MP2722_printBinary(reg13);
  printf("\n");

  printf("REG14h = 0x%02X  b", reg14);
  MP2722_printBinary(reg14);
  printf("\n");

  printf("REG15h = 0x%02X  b", reg15);
  MP2722_printBinary(reg15);
  printf("\n");

  printf("REG16h = 0x%02X  b", reg16);
  MP2722_printBinary(reg16);
  printf("\n");

  printf("\n========== MP2722 decoded status ==========\n");

  uint8_t dpdm_stat     = (reg11 >> 4) & 0x0F;
  bool vindpm_stat      = (reg11 >> 1) & 0x01;
  bool iindpm_stat      = (reg11 >> 0) & 0x01;

  bool vin_gd           = (reg12 >> 6) & 0x01;
  bool vin_rdy          = (reg12 >> 5) & 0x01;
  bool legacy_cable     = (reg12 >> 4) & 0x01;
  bool therm_stat       = (reg12 >> 3) & 0x01;
  bool vsys_stat        = (reg12 >> 2) & 0x01;
  bool watchdog_fault   = (reg12 >> 1) & 0x01;
  bool watchdog_bark    = (reg12 >> 0) & 0x01;

  uint8_t chg_stat      = (reg13 >> 5) & 0x07;
  uint8_t boost_fault   = (reg13 >> 2) & 0x07;
  uint8_t chg_fault     = (reg13 >> 0) & 0x03;

  bool ntc_missing      = (reg14 >> 7) & 0x01;
  bool batt_missing     = (reg14 >> 6) & 0x01;
  uint8_t ntc1_fault    = (reg14 >> 3) & 0x07;
  uint8_t ntc2_fault    = (reg14 >> 0) & 0x07;

  uint8_t cc1_snk_stat  = (reg15 >> 6) & 0x03;
  uint8_t cc2_snk_stat  = (reg15 >> 4) & 0x03;
  uint8_t cc1_src_stat  = (reg15 >> 2) & 0x03;
  uint8_t cc2_src_stat  = (reg15 >> 0) & 0x03;

  bool topoff_active    = (reg16 >> 6) & 0x01;
  bool bfet_stat        = (reg16 >> 5) & 0x01;
  bool batt_low_stat    = (reg16 >> 4) & 0x01;

  printf("REG11 DPDM_STAT      : 0x%X -> %s\n", dpdm_stat, MP2722_decodeDpdmStat(dpdm_stat));
  printf("REG11 VINDPM_STAT    : %d\n", vindpm_stat);
  printf("REG11 IINDPM_STAT    : %d\n", iindpm_stat);

  printf("REG12 VIN_GD         : %d  %s\n", vin_gd, vin_gd ? "Input source good" : "Input source NOT good");
  printf("REG12 VIN_RDY        : %d  %s\n", vin_rdy, vin_rdy ? "Input detection finished" : "Input detection NOT finished");
  printf("REG12 LEGACY_CABLE   : %d\n", legacy_cable);
  printf("REG12 THERM_STAT     : %d\n", therm_stat);
  printf("REG12 VSYS_STAT      : %d\n", vsys_stat);
  printf("REG12 WATCHDOG_FAULT : %d\n", watchdog_fault);
  printf("REG12 WATCHDOG_BARK  : %d\n", watchdog_bark);

  printf("REG13 CHG_STAT       : 0x%X -> %s\n", chg_stat, MP2722_decodeChgStat(chg_stat));
  printf("REG13 BOOST_FAULT    : 0x%X\n", boost_fault);
  printf("REG13 CHG_FAULT      : 0x%X -> %s\n", chg_fault, MP2722_decodeChgFault(chg_fault));

  printf("REG14 NTC_MISSING    : %d\n", ntc_missing);
  printf("REG14 BATT_MISSING   : %d\n", batt_missing);
  printf("REG14 NTC1_FAULT     : 0x%X -> %s\n", ntc1_fault, MP2722_decodeNtcFault(ntc1_fault));
  printf("REG14 NTC2_FAULT     : 0x%X -> %s\n", ntc2_fault, MP2722_decodeNtcFault(ntc2_fault));

  printf("REG15 CC1_SNK_STAT   : 0x%X\n", cc1_snk_stat);
  printf("REG15 CC2_SNK_STAT   : 0x%X\n", cc2_snk_stat);
  printf("REG15 CC1_SRC_STAT   : 0x%X\n", cc1_src_stat);
  printf("REG15 CC2_SRC_STAT   : 0x%X\n", cc2_src_stat);

  printf("REG16 TOPOFF_ACTIVE  : %d\n", topoff_active);
  printf("REG16 BFET_STAT      : %d  %s\n", bfet_stat, bfet_stat ? "Battery discharging" : "Battery charging or disabled");
  printf("REG16 BATT_LOW_STAT  : %d\n", batt_low_stat);

  printf("==========================================\n\n");

  fflush(stdout);
}
