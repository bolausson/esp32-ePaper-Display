#ifndef CONFIG_H
#define CONFIG_H

// GPIO Pin Definitions for Freenove ESP32-S3 WROOM
#define LED48_GPIO          48  // WS2812 RGB LED
#define BOOT_BUTTON_GPIO    0   // Boot button (GPIO0)

// NVS Storage Keys
#define NVS_NAMESPACE       "storage"
#define NVS_WIFI_SSID       "wifi_ssid"
#define NVS_WIFI_PASS       "wifi_pass"
#define NVS_IMAGE_URL       "image_url"
#define NVS_IMG_WIDTH       "img_width"
#define NVS_IMG_HEIGHT      "img_height"
#define NVS_IMG_SCALE       "img_scale"
#define NVS_IMG_ROTATION    "img_rot"
#define NVS_IMG_MIRROR_H    "img_mir_h"
#define NVS_IMG_MIRROR_V    "img_mir_v"
#define NVS_IMG_ROT_FIRST   "img_rot_1st"

// WiFi Configuration
#define WIFI_MAXIMUM_RETRY  5
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// Web Server Configuration
#define WEB_SERVER_PORT     80
#define WEB_SERVER_TIMEOUT  300000  // 5 minutes in milliseconds

// Deep Sleep Configuration
#define DEEP_SLEEP_WAKEUP_TIME  0  // 0 = wake only on button press

// LED Blink Timing (milliseconds)
#define LED_BLINK_INTERVAL  500
#define LED_SLEEP_BLINK_DURATION  3000

// Maximum string lengths
#define MAX_SSID_LEN        32
#define MAX_PASSWORD_LEN    64
#define MAX_URL_LEN         256

#endif // CONFIG_H

