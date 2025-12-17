#ifndef CONFIG_H
#define CONFIG_H

// SD Card configuration (Maker Pi Pico W)
#define SD_CS_PIN 15
#define SD_SCK_PIN 10
#define SD_MOSI_PIN 11
#define SD_MISO_PIN 12

// WiFi Configuration
#define AP_SSID "Embedded_Project"
#define AP_PASSWORD "12345678"
#define HTTP_PORT 80
#define WIFI_LED_PIN CYW43_WL_GPIO_LED_PIN
#define WIFI_BUTTON_PIN 21

// Button configuration
#define BUTTON_PIN 20
#define DEBOUNCE_DELAY_MS 50

// Maximum files to list
#define MAX_FILES_TO_LIST 32

// ADC configuration
#define ADC_TEMP_CHANNEL 4
#define ADC_VSYS_PIN 29
#define ADC_CONVERSION_FACTOR (3.3f / (1 << 12))
#define ADC_VOLTAGE_DIVIDER 3.0f

#endif // CONFIG_H

