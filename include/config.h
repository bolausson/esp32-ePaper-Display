#ifndef CONFIG_H
#define CONFIG_H

// GPIO Pin Definitions for Freenove ESP32-S3 WROOM
#define LED48_GPIO          48  // WS2812 RGB LED
#define BOOT_BUTTON_GPIO    0   // Boot button (GPIO0)

// NVS Storage Keys - Display Settings
#define NVS_NAMESPACE       "storage"
#define NVS_IMAGE_URL       "image_url"
#define NVS_IMG_WIDTH       "img_width"
#define NVS_IMG_HEIGHT      "img_height"
#define NVS_IMG_SCALE       "img_scale"
#define NVS_IMG_ROTATION    "img_rot"
#define NVS_IMG_MIRROR_H    "img_mir_h"
#define NVS_IMG_MIRROR_V    "img_mir_v"
#define NVS_IMG_ROT_FIRST   "img_rot_1st"
#define NVS_REFRESH_MIN     "refresh_min"

// NVS Storage Keys - WiFi Settings
#define NVS_WIFI_SSID       "wifi_ssid"
#define NVS_WIFI_PASS       "wifi_pass"
#define NVS_HOSTNAME        "hostname"
#define NVS_DOMAIN          "domain"

// NVS Storage Keys - IP Configuration
#define NVS_USE_DHCP        "use_dhcp"
#define NVS_STATIC_IP       "static_ip"
#define NVS_STATIC_MASK     "static_mask"
#define NVS_STATIC_GW       "static_gw"
#define NVS_DNS_PRIMARY     "dns_pri"
#define NVS_DNS_SECONDARY   "dns_sec"
#define NVS_DNS_SEARCH      "dns_search"

// NVS Storage Keys - Time Configuration
#define NVS_NTP_SERVER      "ntp_server"
#define NVS_TIMEZONE        "timezone"
#define NVS_USE_DST         "use_dst"

// NVS Storage Keys - Schedule Plans
#define NVS_SCHEDULE_JSON   "sched_json"
#define NVS_SCHEDULE_ENABLE "sched_en"

// WiFi Configuration
#define WIFI_MAXIMUM_RETRY  5
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// AP Mode Configuration
#define AP_SSID             "ESP32-Display-Setup"
#define AP_PASSWORD         ""  // Open network (empty = no password)
#define AP_CHANNEL          1
#define AP_MAX_CONNECTIONS  4
#define WIFI_STA_TIMEOUT_MS 60000   // 1 minute timeout for STA connection
#define AP_MODE_TIMEOUT_MS  300000  // 5 minutes in AP mode before retry

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
#define MAX_HOSTNAME_LEN    64
#define MAX_DOMAIN_LEN      64
#define MAX_IP_LEN          16
#define MAX_NTP_SERVER_LEN  64
#define MAX_TIMEZONE_LEN    48
#define MAX_SCHEDULE_JSON   2048  // Max size for schedule plans JSON

// Schedule Plan limits
#define MAX_SCHEDULE_PLANS  4     // Maximum number of schedule plans
#define MAX_PERIODS_PER_PLAN 8    // Maximum time periods per plan

// Default values
#define DEFAULT_HOSTNAME    "esp32-display"
#define DEFAULT_NTP_SERVER  "pool.ntp.org"
#define DEFAULT_TIMEZONE    "Europe/Berlin"

#endif // CONFIG_H

