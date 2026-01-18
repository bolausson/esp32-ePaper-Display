#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "config.h"
#include "epd_7in3e.h"
#include "image_processor.h"

// Firmware version for OTA
#define FIRMWARE_VERSION "1.0.0"

// MIN macro for buffer size calculations
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "ESP32-S3-Display";

// Global variables
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool wifi_connected = false;
static bool webserver_mode = false;
static bool ap_mode = false;  // True when in AP mode, false when in STA mode
static bool config_saved = false;
static bool preparing_sleep = false;  // Flag to stop LED task before deep sleep
static httpd_handle_t server = NULL;
static led_strip_handle_t led_strip = NULL;
static volatile uint32_t last_client_activity = 0;  // Timestamp of last client activity
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

// Storage for WiFi credentials
static char stored_ssid[MAX_SSID_LEN] = {0};
static char stored_password[MAX_PASSWORD_LEN] = {0};
static char stored_hostname[MAX_HOSTNAME_LEN] = {0};
static char stored_domain[MAX_DOMAIN_LEN] = {0};

// Storage for IP configuration
static bool stored_use_dhcp = true;
static char stored_static_ip[MAX_IP_LEN] = {0};
static char stored_static_mask[MAX_IP_LEN] = {0};
static char stored_static_gw[MAX_IP_LEN] = {0};
static char stored_dns_primary[MAX_IP_LEN] = {0};
static char stored_dns_secondary[MAX_IP_LEN] = {0};
static char stored_dns_search[MAX_DOMAIN_LEN] = {0};

// Storage for time configuration
static char stored_ntp_server[MAX_NTP_SERVER_LEN] = {0};
static char stored_timezone[MAX_TIMEZONE_LEN] = {0};
static bool stored_use_dst = true;

// Storage for display settings
static char stored_image_url[MAX_URL_LEN] = {0};
static uint32_t stored_refresh_interval = 60;  // Default 60 minutes
static uint16_t stored_img_width = 800;   // Default display width
static uint16_t stored_img_height = 480;  // Default display height
static bool stored_img_scale = false;     // Scale image to fit display
static uint16_t stored_img_rotation = 0;  // Image rotation (0, 90, 180, 270)
static bool stored_img_mirror_h = false;  // Mirror horizontally
static bool stored_img_mirror_v = false;  // Mirror vertically
static bool stored_img_rot_first = true;  // Rotate before mirroring
static bool stored_led_disabled = false;  // Disable status LED

// Storage for schedule plans
static char stored_schedule_json[MAX_SCHEDULE_JSON] = {0};
static bool stored_schedule_enabled = false;

// Default schedule JSON (simple all-day plan)
static const char* default_schedule_json =
    "{\"plans\":[{\"name\":\"Default\",\"periods\":[{\"start\":\"00:00\",\"end\":\"00:00\",\"interval\":60}]}],"
    "\"days\":{\"Mon\":\"Default\",\"Tue\":\"Default\",\"Wed\":\"Default\",\"Thu\":\"Default\",\"Fri\":\"Default\",\"Sat\":\"Default\",\"Sun\":\"Default\"}}";

// NTP synchronization status
static bool ntp_synced = false;
static time_t last_ntp_sync = 0;

// Function prototypes
static void init_nvs(void);
static void load_config_from_nvs(void);
static void save_display_config_to_nvs(const char *url, uint32_t refresh_min,
                                        uint16_t img_width, uint16_t img_height,
                                        bool img_scale, uint16_t img_rotation,
                                        bool img_mirror_h, bool img_mirror_v, bool img_rot_first,
                                        bool led_disabled);
static void save_network_config_to_nvs(const char *ssid, const char *password,
                                        const char *hostname, const char *domain,
                                        bool use_dhcp, const char *static_ip, const char *static_mask,
                                        const char *static_gw, const char *dns_primary, const char *dns_secondary,
                                        const char *dns_search, const char *ntp_server, const char *timezone, bool use_dst);
static void init_led(void);
static void set_led_color(uint8_t r, uint8_t g, uint8_t b);
static void led_task(void *pvParameters);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static bool wifi_init_sta_with_timeout(uint32_t timeout_ms);
static void wifi_init_ap(void);
static void wifi_deinit(void);
static bool has_wifi_credentials(void);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t save_post_handler(httpd_req_t *req);
static esp_err_t ota_post_handler(httpd_req_t *req);
static httpd_handle_t start_webserver(void);
static void stop_webserver(httpd_handle_t server);
static void init_boot_button(void);
static void enter_deep_sleep(uint32_t sleep_minutes);

// Initialize NVS
static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
}

// Helper macro to load string from NVS with default
#define NVS_LOAD_STR(handle, key, dest, max_len, def) do { \
    size_t len = max_len; \
    if (nvs_get_str(handle, key, dest, &len) != ESP_OK) { \
        strncpy(dest, def, max_len - 1); \
        dest[max_len - 1] = '\0'; \
    } \
} while(0)

// Load all configuration from NVS
static void load_config_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Set defaults first (empty SSID/password means AP mode will be used)
    stored_ssid[0] = '\0';
    stored_password[0] = '\0';
    strncpy(stored_hostname, DEFAULT_HOSTNAME, MAX_HOSTNAME_LEN - 1);
    stored_domain[0] = '\0';
    stored_use_dhcp = true;
    stored_static_ip[0] = '\0';
    stored_static_mask[0] = '\0';
    stored_static_gw[0] = '\0';
    stored_dns_primary[0] = '\0';
    stored_dns_secondary[0] = '\0';
    stored_dns_search[0] = '\0';
    strncpy(stored_ntp_server, DEFAULT_NTP_SERVER, MAX_NTP_SERVER_LEN - 1);
    strncpy(stored_timezone, DEFAULT_TIMEZONE, MAX_TIMEZONE_LEN - 1);
    stored_use_dst = true;
    stored_image_url[0] = '\0';
    stored_refresh_interval = 60;
    stored_img_width = 800;
    stored_img_height = 480;
    stored_img_scale = false;
    stored_img_rotation = 0;
    stored_img_mirror_h = false;
    stored_img_mirror_v = false;
    stored_img_rot_first = true;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS not found, using defaults");
        return;
    }

    // Load WiFi settings (empty string as default means AP mode)
    NVS_LOAD_STR(nvs_handle, NVS_WIFI_SSID, stored_ssid, MAX_SSID_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_WIFI_PASS, stored_password, MAX_PASSWORD_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_HOSTNAME, stored_hostname, MAX_HOSTNAME_LEN, DEFAULT_HOSTNAME);
    NVS_LOAD_STR(nvs_handle, NVS_DOMAIN, stored_domain, MAX_DOMAIN_LEN, "");

    // Load IP settings
    uint8_t tmp_u8;
    if (nvs_get_u8(nvs_handle, NVS_USE_DHCP, &tmp_u8) == ESP_OK) {
        stored_use_dhcp = (tmp_u8 != 0);
    }
    NVS_LOAD_STR(nvs_handle, NVS_STATIC_IP, stored_static_ip, MAX_IP_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_STATIC_MASK, stored_static_mask, MAX_IP_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_STATIC_GW, stored_static_gw, MAX_IP_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_DNS_PRIMARY, stored_dns_primary, MAX_IP_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_DNS_SECONDARY, stored_dns_secondary, MAX_IP_LEN, "");
    NVS_LOAD_STR(nvs_handle, NVS_DNS_SEARCH, stored_dns_search, MAX_DOMAIN_LEN, "");

    // Load time settings
    NVS_LOAD_STR(nvs_handle, NVS_NTP_SERVER, stored_ntp_server, MAX_NTP_SERVER_LEN, DEFAULT_NTP_SERVER);
    NVS_LOAD_STR(nvs_handle, NVS_TIMEZONE, stored_timezone, MAX_TIMEZONE_LEN, DEFAULT_TIMEZONE);
    if (nvs_get_u8(nvs_handle, NVS_USE_DST, &tmp_u8) == ESP_OK) {
        stored_use_dst = (tmp_u8 != 0);
    }

    // Load display settings
    size_t url_len = MAX_URL_LEN;
    nvs_get_str(nvs_handle, NVS_IMAGE_URL, stored_image_url, &url_len);
    nvs_get_u32(nvs_handle, NVS_REFRESH_MIN, &stored_refresh_interval);

    uint16_t tmp_u16;
    if (nvs_get_u16(nvs_handle, NVS_IMG_WIDTH, &tmp_u16) == ESP_OK) stored_img_width = tmp_u16;
    if (nvs_get_u16(nvs_handle, NVS_IMG_HEIGHT, &tmp_u16) == ESP_OK) stored_img_height = tmp_u16;
    if (nvs_get_u8(nvs_handle, NVS_IMG_SCALE, &tmp_u8) == ESP_OK) stored_img_scale = (tmp_u8 != 0);
    if (nvs_get_u16(nvs_handle, NVS_IMG_ROTATION, &tmp_u16) == ESP_OK) stored_img_rotation = tmp_u16;
    if (nvs_get_u8(nvs_handle, NVS_IMG_MIRROR_H, &tmp_u8) == ESP_OK) stored_img_mirror_h = (tmp_u8 != 0);
    if (nvs_get_u8(nvs_handle, NVS_IMG_MIRROR_V, &tmp_u8) == ESP_OK) stored_img_mirror_v = (tmp_u8 != 0);
    if (nvs_get_u8(nvs_handle, NVS_IMG_ROT_FIRST, &tmp_u8) == ESP_OK) stored_img_rot_first = (tmp_u8 != 0);
    if (nvs_get_u8(nvs_handle, NVS_LED_DISABLED, &tmp_u8) == ESP_OK) stored_led_disabled = (tmp_u8 != 0);

    // Load schedule settings
    size_t sched_len = MAX_SCHEDULE_JSON;
    if (nvs_get_str(nvs_handle, NVS_SCHEDULE_JSON, stored_schedule_json, &sched_len) != ESP_OK) {
        stored_schedule_json[0] = '\0';  // Will use default_schedule_json
    }
    if (nvs_get_u8(nvs_handle, NVS_SCHEDULE_ENABLE, &tmp_u8) == ESP_OK) {
        stored_schedule_enabled = (tmp_u8 != 0);
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Loaded config - SSID: %s, Hostname: %s, DHCP: %s",
             stored_ssid[0] ? stored_ssid : "(empty)",
             stored_hostname,
             stored_use_dhcp ? "yes" : "no");
    ESP_LOGI(TAG, "Display config - URL: %s, Refresh: %lu min, Size: %dx%d, Scale: %s",
             stored_image_url[0] ? stored_image_url : "(not configured)",
             (unsigned long)stored_refresh_interval,
             stored_img_width, stored_img_height,
             stored_img_scale ? "yes" : "no");
}

// Check if we have valid WiFi credentials
static bool has_wifi_credentials(void) {
    return (stored_ssid[0] != '\0');
}

// Save display configuration to NVS
static void save_display_config_to_nvs(const char *url, uint32_t refresh_min,
                                        uint16_t img_width, uint16_t img_height,
                                        bool img_scale, uint16_t img_rotation,
                                        bool img_mirror_h, bool img_mirror_v, bool img_rot_first,
                                        bool led_disabled) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_IMAGE_URL, url);
        nvs_set_u32(nvs_handle, NVS_REFRESH_MIN, refresh_min);
        nvs_set_u16(nvs_handle, NVS_IMG_WIDTH, img_width);
        nvs_set_u16(nvs_handle, NVS_IMG_HEIGHT, img_height);
        nvs_set_u8(nvs_handle, NVS_IMG_SCALE, img_scale ? 1 : 0);
        nvs_set_u16(nvs_handle, NVS_IMG_ROTATION, img_rotation);
        nvs_set_u8(nvs_handle, NVS_IMG_MIRROR_H, img_mirror_h ? 1 : 0);
        nvs_set_u8(nvs_handle, NVS_IMG_MIRROR_V, img_mirror_v ? 1 : 0);
        nvs_set_u8(nvs_handle, NVS_IMG_ROT_FIRST, img_rot_first ? 1 : 0);
        nvs_set_u8(nvs_handle, NVS_LED_DISABLED, led_disabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        // Update stored values
        strncpy(stored_image_url, url, MAX_URL_LEN - 1);
        stored_refresh_interval = refresh_min;
        stored_img_width = img_width;
        stored_img_height = img_height;
        stored_img_scale = img_scale;
        stored_img_rotation = img_rotation;
        stored_img_mirror_h = img_mirror_h;
        stored_img_mirror_v = img_mirror_v;
        stored_img_rot_first = img_rot_first;
        stored_led_disabled = led_disabled;

        ESP_LOGI(TAG, "Display config saved - URL: %s, Refresh: %lu min, Rot: %d, LED disabled: %s",
                 url, (unsigned long)refresh_min, img_rotation, led_disabled ? "yes" : "no");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
    }
}

// Save network configuration to NVS
static void save_network_config_to_nvs(const char *ssid, const char *password,
                                        const char *hostname, const char *domain,
                                        bool use_dhcp, const char *static_ip, const char *static_mask,
                                        const char *static_gw, const char *dns_primary, const char *dns_secondary,
                                        const char *dns_search, const char *ntp_server, const char *timezone, bool use_dst) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_SSID, ssid);
        nvs_set_str(nvs_handle, NVS_WIFI_PASS, password);
        nvs_set_str(nvs_handle, NVS_HOSTNAME, hostname);
        nvs_set_str(nvs_handle, NVS_DOMAIN, domain);
        nvs_set_u8(nvs_handle, NVS_USE_DHCP, use_dhcp ? 1 : 0);
        nvs_set_str(nvs_handle, NVS_STATIC_IP, static_ip);
        nvs_set_str(nvs_handle, NVS_STATIC_MASK, static_mask);
        nvs_set_str(nvs_handle, NVS_STATIC_GW, static_gw);
        nvs_set_str(nvs_handle, NVS_DNS_PRIMARY, dns_primary);
        nvs_set_str(nvs_handle, NVS_DNS_SECONDARY, dns_secondary);
        nvs_set_str(nvs_handle, NVS_DNS_SEARCH, dns_search);
        nvs_set_str(nvs_handle, NVS_NTP_SERVER, ntp_server);
        nvs_set_str(nvs_handle, NVS_TIMEZONE, timezone);
        nvs_set_u8(nvs_handle, NVS_USE_DST, use_dst ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        // Update stored values
        strncpy(stored_ssid, ssid, MAX_SSID_LEN - 1);
        strncpy(stored_password, password, MAX_PASSWORD_LEN - 1);
        strncpy(stored_hostname, hostname, MAX_HOSTNAME_LEN - 1);
        strncpy(stored_domain, domain, MAX_DOMAIN_LEN - 1);
        stored_use_dhcp = use_dhcp;
        strncpy(stored_static_ip, static_ip, MAX_IP_LEN - 1);
        strncpy(stored_static_mask, static_mask, MAX_IP_LEN - 1);
        strncpy(stored_static_gw, static_gw, MAX_IP_LEN - 1);
        strncpy(stored_dns_primary, dns_primary, MAX_IP_LEN - 1);
        strncpy(stored_dns_secondary, dns_secondary, MAX_IP_LEN - 1);
        strncpy(stored_dns_search, dns_search, MAX_DOMAIN_LEN - 1);
        strncpy(stored_ntp_server, ntp_server, MAX_NTP_SERVER_LEN - 1);
        strncpy(stored_timezone, timezone, MAX_TIMEZONE_LEN - 1);
        stored_use_dst = use_dst;

        ESP_LOGI(TAG, "Network config saved - SSID: %s, Hostname: %s, DHCP: %s",
                 ssid, hostname, use_dhcp ? "yes" : "no");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
    }
}

// Save schedule configuration to NVS
static void save_schedule_config_to_nvs(const char *schedule_json, bool schedule_enabled) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_SCHEDULE_JSON, schedule_json);
        nvs_set_u8(nvs_handle, NVS_SCHEDULE_ENABLE, schedule_enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        // Update stored values
        strncpy(stored_schedule_json, schedule_json, MAX_SCHEDULE_JSON - 1);
        stored_schedule_json[MAX_SCHEDULE_JSON - 1] = '\0';
        stored_schedule_enabled = schedule_enabled;

        ESP_LOGI(TAG, "Schedule config saved - Enabled: %s, JSON len: %d",
                 schedule_enabled ? "yes" : "no", strlen(schedule_json));
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing schedule config");
    }
}

// NTP time sync notification callback
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "NTP time synchronized");
    ntp_synced = true;
    time(&last_ntp_sync);
}

// Convert common timezone names to POSIX TZ format
// ESP-IDF newlib doesn't have full tzdata, so we need POSIX format
static const char* get_posix_timezone(const char *tz_name) {
    // Common timezone mappings to POSIX format
    // Format: STDoffset[DST[offset],start[/time],end[/time]]
    static const struct {
        const char *name;
        const char *posix;
    } tz_map[] = {
        // Europe
        {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
        {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Europe/Moscow", "MSK-3"},
        {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
        // Americas
        {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
        {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
        {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
        {"America/Phoenix", "MST7"},  // No DST
        {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
        {"America/Sao_Paulo", "<-03>3"},
        {"America/Mexico_City", "CST6CDT,M4.1.0,M10.5.0"},
        // Asia
        {"Asia/Tokyo", "JST-9"},
        {"Asia/Shanghai", "CST-8"},
        {"Asia/Hong_Kong", "HKT-8"},
        {"Asia/Singapore", "SGT-8"},
        {"Asia/Seoul", "KST-9"},
        {"Asia/Kolkata", "IST-5:30"},
        {"Asia/Dubai", "GST-4"},
        {"Asia/Bangkok", "ICT-7"},
        {"Asia/Jakarta", "WIB-7"},
        // Australia/Pacific
        {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
        {"Australia/Brisbane", "AEST-10"},  // No DST
        {"Australia/Perth", "AWST-8"},
        {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
        {"Pacific/Honolulu", "HST10"},
        // Other
        {"UTC", "UTC0"},
        {"GMT", "GMT0"},
        {NULL, NULL}
    };

    // Search for matching timezone
    for (int i = 0; tz_map[i].name != NULL; i++) {
        if (strcmp(tz_name, tz_map[i].name) == 0) {
            return tz_map[i].posix;
        }
    }

    // If not found, return as-is (user might have entered POSIX format directly)
    return tz_name;
}

// Apply timezone setting
static void apply_timezone(void) {
    const char *posix_tz = get_posix_timezone(stored_timezone);
    ESP_LOGI(TAG, "Setting timezone: %s -> %s", stored_timezone, posix_tz);
    setenv("TZ", posix_tz, 1);
    tzset();
}

// Initialize SNTP for time synchronization
static void init_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP with server: %s", stored_ntp_server);

    // Apply timezone before SNTP init
    apply_timezone();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, stored_ntp_server);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP initialized, timezone: %s", stored_timezone);
}

// Wait for NTP sync with timeout (blocking)
// Returns true if synced, false if timeout
static bool wait_for_ntp_sync(uint32_t timeout_seconds) {
    if (strlen(stored_ntp_server) == 0) {
        ESP_LOGW(TAG, "No NTP server configured, skipping time sync");
        return false;
    }

    ESP_LOGI(TAG, "Waiting for NTP sync (timeout: %lu seconds)...", (unsigned long)timeout_seconds);

    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    while (1) {
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS / 1000) - start_time;

        if (elapsed >= timeout_seconds) {
            ESP_LOGW(TAG, "NTP sync timeout after %lu seconds", (unsigned long)timeout_seconds);
            return false;
        }

        // Check if time is synced (year > 2024 means NTP worked)
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year + 1900 > 2024) {
            ntp_synced = true;
            time(&last_ntp_sync);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "NTP synced! Current time: %s", time_str);
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Trigger manual NTP sync
static bool trigger_ntp_sync(void) {
    ESP_LOGI(TAG, "Triggering manual NTP sync");

    // If SNTP is already running, restart it to force immediate sync
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    // Re-configure with current settings
    apply_timezone();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, stored_ntp_server);
    esp_sntp_init();

    // Wait for sync with timeout (5 seconds for manual trigger)
    return wait_for_ntp_sync(5);
}

// Get current refresh interval based on schedule (returns 0 if schedule disabled or error)
// This is a simple JSON parser for our specific schedule format
static uint32_t get_scheduled_interval(void) {
    if (!stored_schedule_enabled || stored_schedule_json[0] == '\0') {
        return 0;  // Schedule not enabled, use default interval
    }

    // Get current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Day names in order (0=Mon, 6=Sun) - our schedule uses Mon-Sun
    const char* day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    // tm_wday is 0=Sunday, so convert: Sun(0)->6, Mon(1)->0, etc.
    int day_idx = (timeinfo.tm_wday == 0) ? 6 : timeinfo.tm_wday - 1;
    const char* today = day_names[day_idx];

    int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    ESP_LOGI(TAG, "Schedule check: %s %02d:%02d (%d min)", today, timeinfo.tm_hour, timeinfo.tm_min, current_minutes);

    const char* json = stored_schedule_json[0] ? stored_schedule_json : default_schedule_json;

    // Find which plan is assigned to today
    // Look for "days":{"Mon":"PlanName",...}
    char search_key[16];
    snprintf(search_key, sizeof(search_key), "\"%s\":\"", today);
    const char* day_ptr = strstr(json, search_key);
    if (!day_ptr) {
        ESP_LOGW(TAG, "Schedule: Day %s not found in assignments", today);
        return 0;
    }

    // Extract plan name
    day_ptr += strlen(search_key);
    const char* plan_end = strchr(day_ptr, '"');
    if (!plan_end) return 0;

    char plan_name[32];
    size_t plan_len = plan_end - day_ptr;
    if (plan_len >= sizeof(plan_name)) plan_len = sizeof(plan_name) - 1;
    strncpy(plan_name, day_ptr, plan_len);
    plan_name[plan_len] = '\0';

    ESP_LOGI(TAG, "Schedule: Today's plan is '%s'", plan_name);

    // Find the plan in the plans array
    // Look for "name":"PlanName" within plans array
    char plan_search[48];
    snprintf(plan_search, sizeof(plan_search), "\"name\":\"%s\"", plan_name);
    const char* plan_ptr = strstr(json, plan_search);
    if (!plan_ptr) {
        ESP_LOGW(TAG, "Schedule: Plan '%s' not found", plan_name);
        return 0;
    }

    // Find the periods array for this plan
    const char* periods_ptr = strstr(plan_ptr, "\"periods\":[");
    if (!periods_ptr) return 0;
    periods_ptr += 11;  // Skip "periods":[

    // Parse each period: {"start":"HH:MM","end":"HH:MM","interval":N}
    const char* p = periods_ptr;
    while (*p && *p != ']') {
        // Find start time
        const char* start_ptr = strstr(p, "\"start\":\"");
        if (!start_ptr || start_ptr > strchr(p, '}')) break;
        start_ptr += 9;
        int start_h = 0, start_m = 0;
        sscanf(start_ptr, "%d:%d", &start_h, &start_m);
        int start_min = start_h * 60 + start_m;

        // Find end time
        const char* end_ptr = strstr(p, "\"end\":\"");
        if (!end_ptr) break;
        end_ptr += 7;
        int end_h = 0, end_m = 0;
        sscanf(end_ptr, "%d:%d", &end_h, &end_m);
        int end_min = end_h * 60 + end_m;

        // Find interval
        const char* int_ptr = strstr(p, "\"interval\":");
        if (!int_ptr) break;
        int_ptr += 11;
        int interval = atoi(int_ptr);

        ESP_LOGD(TAG, "Period: %02d:%02d-%02d:%02d = %d min", start_h, start_m, end_h, end_m, interval);

        // Check if current time falls in this period
        bool in_period = false;
        if (start_min == end_min) {
            // All day period (00:00-00:00 means 24h)
            in_period = true;
        } else if (start_min < end_min) {
            // Normal period (e.g., 06:00-22:00)
            in_period = (current_minutes >= start_min && current_minutes < end_min);
        } else {
            // Overnight period (e.g., 22:00-06:00)
            in_period = (current_minutes >= start_min || current_minutes < end_min);
        }

        if (in_period) {
            ESP_LOGI(TAG, "Schedule: Using interval %d min (period %02d:%02d-%02d:%02d)",
                     interval, start_h, start_m, end_h, end_m);
            return (uint32_t)interval;
        }

        // Move to next period
        p = strchr(p, '}');
        if (!p) break;
        p++;
    }

    ESP_LOGW(TAG, "Schedule: No matching period found for current time");
    return 0;
}

// Get effective refresh interval (schedule or default)
static uint32_t get_effective_refresh_interval(void) {
    uint32_t scheduled = get_scheduled_interval();
    if (scheduled > 0) {
        ESP_LOGI(TAG, "Using scheduled interval: %lu min", (unsigned long)scheduled);
        return scheduled;
    }
    ESP_LOGI(TAG, "Using default interval: %lu min", (unsigned long)stored_refresh_interval);
    return stored_refresh_interval;
}

// Initialize WS2812 LED on GPIO 48 using led_strip component
static void init_led(void) {
    ESP_LOGI(TAG, "Initializing WS2812 LED on GPIO %d", LED48_GPIO);

    // LED strip general configuration
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED48_GPIO,
        .max_leds = 1,  // Only 1 LED on the Freenove ESP32-S3 WROOM board
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    // LED strip RMT backend configuration
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    // Create LED strip object
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Clear LED (turn off)
    led_strip_clear(led_strip);

    ESP_LOGI(TAG, "WS2812 LED initialized successfully");
}

// Set LED color using WS2812 protocol
static void set_led_color(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip == NULL) {
        return;
    }

    // Set pixel color (index 0 - only one LED)
    esp_err_t ret = led_strip_set_pixel(led_strip, 0, r, g, b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LED color: %s", esp_err_to_name(ret));
        return;
    }

    // Refresh to send data to LED
    ret = led_strip_refresh(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh LED: %s", esp_err_to_name(ret));
    }
}

// LED task for blinking status indication
static void led_task(void *pvParameters) {
    bool led_state = false;

    while (1) {
        // Stop controlling LED when preparing for deep sleep
        if (preparing_sleep) {
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL));
            continue;
        }

        // If LED is disabled, keep it off
        if (stored_led_disabled) {
            set_led_color(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL));
            continue;
        }

        if (ap_mode && webserver_mode) {
            // AP mode: Alternating red and green blink
            if (led_state) {
                set_led_color(50, 0, 0);   // Red
            } else {
                set_led_color(0, 50, 0);   // Green
            }
            led_state = !led_state;
        } else if (webserver_mode) {
            // STA mode with webserver: Yellow blink
            if (led_state) {
                set_led_color(50, 50, 0);  // Yellow
            } else {
                set_led_color(0, 0, 0);    // Off
            }
            led_state = !led_state;
        } else if (!wifi_connected) {
            // Red blink when not connected to WiFi
            if (led_state) {
                set_led_color(50, 0, 0);   // Red
            } else {
                set_led_color(0, 0, 0);    // Off
            }
            led_state = !led_state;
        } else {
            // Solid green when connected
            set_led_color(0, 50, 0);       // Green
        }

        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL));
    }
}

// DNS server task for captive portal - responds to all queries with AP IP
static void dns_server_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting DNS server for captive portal");

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    // Set socket timeout
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t rx_buffer[512];
    uint8_t tx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (ap_mode) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 12) {
            continue;  // Invalid DNS packet or timeout
        }

        // Build DNS response - copy the query header and modify flags
        memcpy(tx_buffer, rx_buffer, len);

        // Set response flags: QR=1 (response), AA=1 (authoritative), RCODE=0 (no error)
        tx_buffer[2] = 0x84;  // QR=1, Opcode=0, AA=1, TC=0, RD=0
        tx_buffer[3] = 0x00;  // RA=0, Z=0, RCODE=0

        // Set answer count to 1
        tx_buffer[6] = 0x00;
        tx_buffer[7] = 0x01;

        // Find the end of the question section
        int question_end = 12;
        while (question_end < len && rx_buffer[question_end] != 0) {
            question_end += rx_buffer[question_end] + 1;
        }
        question_end += 5;  // Skip null byte + QTYPE (2) + QCLASS (2)

        // Add answer section
        int answer_start = question_end;
        tx_buffer[answer_start] = 0xC0;      // Name pointer
        tx_buffer[answer_start + 1] = 0x0C;  // Points to offset 12 (query name)
        tx_buffer[answer_start + 2] = 0x00;  // TYPE A
        tx_buffer[answer_start + 3] = 0x01;
        tx_buffer[answer_start + 4] = 0x00;  // CLASS IN
        tx_buffer[answer_start + 5] = 0x01;
        tx_buffer[answer_start + 6] = 0x00;  // TTL (60 seconds)
        tx_buffer[answer_start + 7] = 0x00;
        tx_buffer[answer_start + 8] = 0x00;
        tx_buffer[answer_start + 9] = 0x3C;
        tx_buffer[answer_start + 10] = 0x00; // RDLENGTH (4 bytes for IP)
        tx_buffer[answer_start + 11] = 0x04;
        // IP address 192.168.4.1
        tx_buffer[answer_start + 12] = 192;
        tx_buffer[answer_start + 13] = 168;
        tx_buffer[answer_start + 14] = 4;
        tx_buffer[answer_start + 15] = 1;

        int response_len = answer_start + 16;

        sendto(sock, tx_buffer, response_len, 0,
               (struct sockaddr *)&client_addr, client_addr_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", disconnected->reason);

        // Log common disconnect reasons
        switch(disconnected->reason) {
            case WIFI_REASON_AUTH_EXPIRE:
            case WIFI_REASON_AUTH_LEAVE:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                ESP_LOGE(TAG, "Authentication failed - check WiFi password!");
                break;
            case WIFI_REASON_NO_AP_FOUND:
                ESP_LOGE(TAG, "AP not found - check WiFi SSID!");
                break;
            case WIFI_REASON_ASSOC_FAIL:
                ESP_LOGE(TAG, "Association failed - AP may be full or rejecting connections");
                break;
            default:
                ESP_LOGW(TAG, "Other disconnect reason: %d", disconnected->reason);
                break;
        }

        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP (attempt %d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to AP after %d attempts", WIFI_MAXIMUM_RETRY);
        }
        wifi_connected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_connected = true;
        // Initialize SNTP for time synchronization
        init_sntp();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station connected to AP, AID=%d", event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station disconnected from AP, AID=%d", event->aid);
    }
}

// Initialize network interfaces (called once at startup)
static void wifi_init_netif(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create both STA and AP network interfaces
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    s_wifi_event_group = xEventGroupCreate();
    ESP_LOGI(TAG, "WiFi network interfaces initialized");
}

// Initialize WiFi in station mode with timeout
static bool wifi_init_sta_with_timeout(uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Starting WiFi STA mode, timeout: %lu ms", (unsigned long)timeout_ms);

    // Reset state
    s_retry_num = 0;
    wifi_connected = false;
    ap_mode = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Configure STA
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = strlen(stored_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    strncpy((char *)wifi_config.sta.ssid, stored_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, stored_password, sizeof(wifi_config.sta.password));

    ESP_LOGI(TAG, "Connecting to SSID: '%s'", stored_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", stored_ssid);
        return true;
    } else {
        ESP_LOGW(TAG, "Failed to connect to SSID: %s (timeout or auth failure)", stored_ssid);
        esp_wifi_stop();
        return false;
    }
}

// Initialize WiFi in Access Point mode
static void wifi_init_ap(void) {
    ESP_LOGI(TAG, "Starting WiFi AP mode: %s", AP_SSID);

    ap_mode = true;
    wifi_connected = false;

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .password = AP_PASSWORD,
            .max_connection = AP_MAX_CONNECTIONS,
            .authmode = strlen(AP_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    ESP_LOGI(TAG, "AP started. Connect to '%s' and visit http://" IPSTR, AP_SSID, IP2STR(&ip_info.ip));

    // Start DNS server for captive portal
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

// Stop WiFi
static void wifi_deinit(void) {
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi stopped");
}

// HTML page CSS styles (shared between tabs)
static const char* html_styles =
"<style>"
"body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;}"
"h1,h2,h3{color:#333;margin-top:0;}"
".container{background:white;padding:20px;border-radius:10px;max-width:550px;margin:0 auto;box-shadow:0 2px 5px rgba(0,0,0,0.1);}"
".tabs{display:flex;border-bottom:2px solid #ddd;margin-bottom:15px;}"
".tab{padding:12px 20px;cursor:pointer;border:none;background:none;font-size:16px;color:#666;border-bottom:3px solid transparent;margin-bottom:-2px;}"
".tab.active{color:#2196F3;border-bottom-color:#2196F3;font-weight:bold;}"
".tab:hover{color:#2196F3;}"
".tab-content{display:none;}"
".tab-content.active{display:block;}"
"input[type=text],input[type=password],input[type=number],select{width:100%%;padding:10px;margin:8px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}"
"input[type=submit],.btn{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;width:100%%;font-size:16px;margin:5px 0;display:block;text-align:center;text-decoration:none;box-sizing:border-box;}"
"input[type=submit]:hover,.btn:hover{opacity:0.9;}"
".btn-test{width:auto!important;display:inline-block!important;padding:10px 20px;}"
".test-buttons{display:flex;flex-wrap:wrap;gap:8px;}"
".btn-blue{background:#2196F3;}"
".btn-orange{background:#FF9800;}"
".btn-red{background:#f44336;}"
"label{font-weight:bold;color:#555;display:block;margin-top:10px;}"
".info{background:#e7f3fe;border-left:4px solid #2196F3;padding:10px;margin:10px 0;word-wrap:break-word;}"
".info a{color:#1565c0;word-break:break-all;}"
".row{display:flex;gap:10px;}"
".row input,.row select{flex:1;}"
".checkbox-row{display:flex;align-items:center;margin:10px 0;}"
".checkbox-row input{width:auto;margin-right:10px;}"
".section{border-top:1px solid #ddd;margin-top:20px;padding-top:15px;}"
".subsection{background:#f9f9f9;padding:15px;border-radius:8px;margin:15px 0;}"
".subsection h3{font-size:14px;margin-bottom:10px;}"
".radio-row{display:flex;align-items:center;margin:5px 0;}"
".radio-row input{width:auto;margin-right:8px;}"
".help{font-size:0.85em;color:#888;margin-top:20px;}"
".help p{margin:5px 0;}"
".ap-notice{background:#fff3cd;border-left:4px solid #ffc107;padding:10px;margin:10px 0;}"
".tz-help{font-size:0.85em;color:#666;margin:5px 0 15px 0;}"
".tz-help a{color:#2196F3;}"
".time-display{background:#e8f5e9;padding:15px;border-radius:8px;margin-bottom:15px;border:1px solid #c8e6c9;}"
".time-display.not-synced{background:#fff3e0;border-color:#ffe0b2;}"
".time-display .current-time{font-size:1.5em;font-weight:bold;color:#333;margin-bottom:5px;}"
".time-display .time-info{font-size:0.9em;color:#666;margin:3px 0;}"
".time-display .sync-status{font-size:0.85em;padding:3px 8px;border-radius:12px;display:inline-block;}"
".time-display .sync-status.synced{background:#c8e6c9;color:#2e7d32;}"
".time-display .sync-status.not-synced{background:#ffe0b2;color:#e65100;}"
".sync-btn{background:#2196F3;color:white;border:none;padding:8px 16px;border-radius:4px;cursor:pointer;font-size:14px;margin-top:10px;}"
".sync-btn:hover{background:#1976D2;}"
".sync-btn:disabled{background:#ccc;cursor:not-allowed;}"
".sync-btn .spinner{display:inline-block;width:12px;height:12px;border:2px solid #fff;border-top-color:transparent;border-radius:50%%;animation:spin 1s linear infinite;margin-right:6px;vertical-align:middle;}"
"@keyframes spin{to{transform:rotate(360deg);}}"
".progress-container{background:#e0e0e0;border-radius:4px;height:24px;margin:15px 0;overflow:hidden;}"
".progress-bar{background:#4CAF50;height:100%%;width:0%%;transition:width 0.3s;display:flex;align-items:center;justify-content:center;color:white;font-size:12px;}"
".ota-status{padding:10px;margin:10px 0;border-radius:4px;display:none;}"
".ota-success{background:#d4edda;border:1px solid #c3e6cb;color:#155724;}"
".ota-error{background:#f8d7da;border:1px solid #f5c6cb;color:#721c24;}"
".file-input{margin:15px 0;}"
".version-info{background:#e7f3fe;padding:15px;border-radius:8px;margin-bottom:15px;}"
".version-info p{margin:5px 0;}"
".plan-tabs{display:flex;gap:4px;border-bottom:2px solid #e0e0e0;margin:12px 0;flex-wrap:wrap;}"
".plan-tab{padding:6px 12px;cursor:pointer;border-radius:6px 6px 0 0;background:#f0f0f0;font-size:13px;}"
".plan-tab.active{background:#2196F3;color:white;}"
".plan-tab-add{background:#e8f5e9;color:#2e7d32;}"
".plan-content{display:none;padding:12px;border:1px solid #e0e0e0;border-top:none;border-radius:0 0 8px 8px;}"
".plan-content.active{display:block;}"
".day-grid{display:flex;flex-wrap:wrap;gap:6px;margin:12px 0;justify-content:center;}"
".day-card{text-align:center;padding:8px 4px;border:2px solid #e0e0e0;border-radius:6px;background:#fafafa;min-width:70px;flex:1 1 auto;max-width:100px;}"
".day-card.today{border-color:#4CAF50;background:#e8f5e9;}"
".day-name{font-weight:600;font-size:12px;margin-bottom:4px;}"
".day-card select{width:100%%;padding:3px;font-size:11px;border-radius:4px;}"
".period-table{width:100%%;border-collapse:collapse;margin:8px 0;}"
".period-table th{text-align:left;padding:6px;background:#f5f5f5;font-size:12px;}"
".period-table td{padding:4px;}"
".period-table input[type=time]{width:80px;padding:4px;font-size:12px;}"
".period-table input[type=number]{width:60px;padding:4px;font-size:12px;}"
".period-table .btn-del{padding:2px 8px;font-size:11px;}"
".preset-btns{display:flex;gap:6px;margin:8px 0;flex-wrap:wrap;}"
".preset-btn{padding:4px 10px;font-size:11px;background:#e0e0e0;border:none;border-radius:4px;cursor:pointer;}"
".sched-enable{display:flex;align-items:center;gap:8px;margin:10px 0;padding:10px;background:#e3f2fd;border-radius:6px;}"
"</style>";

// HTML page JavaScript for tabs and OTA
static const char* html_script =
"<script>"
"function showTab(tabId){"
"document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active'));"
"document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
"document.getElementById(tabId).classList.add('active');"
"document.querySelector('[onclick*=\"'+tabId+'\"]').classList.add('active');"
"}"
"function toggleDhcp(){"
"var dhcp=document.getElementById('dhcp_on').checked;"
"document.querySelectorAll('.static-ip').forEach(e=>e.disabled=dhcp);"
"}"
"function uploadFirmware(){"
"var fileInput=document.getElementById('firmware-file');"
"var file=fileInput.files[0];"
"if(!file){alert('Please select a firmware file');return;}"
"if(!file.name.endsWith('.bin')){alert('Please select a .bin file');return;}"
"var progressBar=document.getElementById('ota-progress');"
"var progressText=document.getElementById('ota-progress-text');"
"var statusDiv=document.getElementById('ota-status');"
"var uploadBtn=document.getElementById('upload-btn');"
"uploadBtn.disabled=true;"
"statusDiv.style.display='none';"
"progressBar.style.width='0%%';"
"progressText.textContent='0%%';"
"document.querySelector('.progress-container').style.display='block';"
"var xhr=new XMLHttpRequest();"
"xhr.open('POST','/ota',true);"
"xhr.upload.onprogress=function(e){"
"if(e.lengthComputable){"
"var pct=Math.round((e.loaded/e.total)*100);"
"progressBar.style.width=pct+'%%';"
"progressText.textContent=pct+'%%';"
"}};"
"xhr.onload=function(){"
"uploadBtn.disabled=false;"
"if(xhr.status==200){"
"statusDiv.className='ota-status ota-success';"
"statusDiv.innerHTML='<strong>Success!</strong> '+xhr.responseText;"
"statusDiv.style.display='block';"
"}else{"
"statusDiv.className='ota-status ota-error';"
"statusDiv.innerHTML='<strong>Error:</strong> '+xhr.responseText;"
"statusDiv.style.display='block';"
"}};"
"xhr.onerror=function(){"
"uploadBtn.disabled=false;"
"statusDiv.className='ota-status ota-error';"
"statusDiv.innerHTML='<strong>Error:</strong> Upload failed';"
"statusDiv.style.display='block';"
"};"
"xhr.send(file);"
"}"
"var timeUpdateInterval=null;"
"function updateTime(){"
"fetch('/api/time').then(r=>r.json()).then(d=>{"
"var el=document.getElementById('currentTime');"
"if(el)el.textContent=d.time;"
"var tz=document.getElementById('tzDisplay');"
"if(tz)tz.textContent=d.timezone;"
"var st=document.getElementById('syncStatus');"
"var td=document.getElementById('timeDisplay');"
"if(st&&td){"
"if(d.synced){st.textContent='Synced';st.className='sync-status synced';td.className='time-display';}"
"else{st.textContent='Not Synced';st.className='sync-status not-synced';td.className='time-display not-synced';}"
"}"
"}).catch(e=>console.log('Time update error:',e));}"
"function syncNtp(){"
"var btn=document.getElementById('syncBtn');"
"btn.disabled=true;btn.innerHTML='<span class=\"spinner\"></span>Syncing...';"
"fetch('/api/ntp_sync',{method:'POST'}).then(r=>r.json()).then(d=>{"
"btn.disabled=false;btn.textContent='Sync Now';"
"if(d.success){updateTime();}else{alert('NTP sync failed. Check NTP server settings.');}"
"}).catch(e=>{btn.disabled=false;btn.textContent='Sync Now';alert('Sync error: '+e);});}"
"function startTimeUpdate(){updateTime();timeUpdateInterval=setInterval(updateTime,1000);}"
"if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',startTimeUpdate);}else{startTimeUpdate();}"
"</script>";

// Schedule Plans JavaScript
static const char* html_schedule_script =
"<script>"
"var DAYS=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];"
"var schedData=%s;"  // JSON data injected here
"var activePlan=0;"
"function initSched(){renderDays();renderPlanTabs();renderPlanContent();}"
"function renderDays(){"
"var c=document.getElementById('dayGrid');"
"var today=new Date().getDay();var ti=today===0?6:today-1;"
"c.innerHTML=DAYS.map((d,i)=>'<div class=\"day-card'+(i===ti?' today':'')+'\"><div class=\"day-name\">'+d+'</div>'"
"+'<select onchange=\"setDay(\\''+d+'\\',this.value)\">'+schedData.plans.map(p=>'<option'+(schedData.days[d]===p.name?' selected':'')+'>'+p.name+'</option>').join('')+'</select></div>').join('');"
"}"
"function renderPlanTabs(){"
"var c=document.getElementById('planTabs');"
"c.innerHTML=schedData.plans.map((p,i)=>'<div class=\"plan-tab'+(i===activePlan?' active':'')+'\" onclick=\"selPlan('+i+')\">'+p.name+'</div>').join('')"
"+(schedData.plans.length<4?'<div class=\"plan-tab plan-tab-add\" onclick=\"addPlan()\">+ New</div>':'');"
"}"
"function renderPlanContent(){"
"var c=document.getElementById('planContent');"
"var p=schedData.plans[activePlan];"
"c.innerHTML='<div class=\"row\"><input type=\"text\" value=\"'+p.name+'\" onchange=\"renamePlan(this.value)\" style=\"flex:1\">'+(schedData.plans.length>1?'<button type=\"button\" class=\"btn btn-red btn-small\" onclick=\"delPlan()\">Delete</button>':'')+'</div>'"
"+'<table class=\"period-table\"><tr><th>Start</th><th>End</th><th>Interval</th><th></th></tr>'"
"+p.periods.map((r,i)=>'<tr><td><input type=\"time\" value=\"'+r.start+'\" onchange=\"updPeriod('+i+',\\'start\\',this.value)\"></td>'"
"+'<td><input type=\"time\" value=\"'+r.end+'\" onchange=\"updPeriod('+i+',\\'end\\',this.value)\"></td>'"
"+'<td><input type=\"number\" value=\"'+r.interval+'\" min=\"1\" max=\"1440\" onchange=\"updPeriod('+i+',\\'interval\\',this.value)\"> min</td>'"
"+'<td>'+(p.periods.length>1?'<button type=\"button\" class=\"btn btn-red btn-del\" onclick=\"delPeriod('+i+')\">X</button>':'')+'</td></tr>').join('')"
"+'</table><div class=\"preset-btns\"><button type=\"button\" class=\"preset-btn\" onclick=\"addPeriod()\">+ Add</button>'"
"+'<button type=\"button\" class=\"preset-btn\" onclick=\"preset(\\'simple\\')\">Simple</button>'"
"+'<button type=\"button\" class=\"preset-btn\" onclick=\"preset(\\'daynight\\')\">Day/Night</button></div>';"
"syncHidden();"
"}"
"function selPlan(i){activePlan=i;renderPlanTabs();renderPlanContent();}"
"function addPlan(){"
"var n=prompt('Plan name:','Plan '+(schedData.plans.length+1));"
"if(n&&!schedData.plans.find(p=>p.name===n)){schedData.plans.push({name:n,periods:[{start:'00:00',end:'00:00',interval:60}]});activePlan=schedData.plans.length-1;renderDays();renderPlanTabs();renderPlanContent();}"
"}"
"function renamePlan(n){"
"if(!n.trim())return;var old=schedData.plans[activePlan].name;"
"if(schedData.plans.find((p,i)=>i!==activePlan&&p.name===n)){alert('Name exists');return;}"
"schedData.plans[activePlan].name=n;"
"DAYS.forEach(d=>{if(schedData.days[d]===old)schedData.days[d]=n;});"
"renderDays();renderPlanTabs();syncHidden();"
"}"
"function delPlan(){"
"if(schedData.plans.length<2)return;"
"var name=schedData.plans[activePlan].name;"
"var fb=schedData.plans.find((p,i)=>i!==activePlan).name;"
"DAYS.forEach(d=>{if(schedData.days[d]===name)schedData.days[d]=fb;});"
"schedData.plans.splice(activePlan,1);activePlan=0;"
"renderDays();renderPlanTabs();renderPlanContent();"
"}"
"function setDay(d,v){schedData.days[d]=v;syncHidden();}"
"function addPeriod(){schedData.plans[activePlan].periods.push({start:'00:00',end:'00:00',interval:60});renderPlanContent();}"
"function delPeriod(i){if(schedData.plans[activePlan].periods.length>1){schedData.plans[activePlan].periods.splice(i,1);renderPlanContent();}}"
"function updPeriod(i,f,v){schedData.plans[activePlan].periods[i][f]=f==='interval'?parseInt(v):v;syncHidden();}"
"function preset(t){"
"var p=schedData.plans[activePlan];"
"if(t==='simple')p.periods=[{start:'00:00',end:'00:00',interval:60}];"
"else if(t==='daynight')p.periods=[{start:'06:00',end:'22:00',interval:30},{start:'22:00',end:'06:00',interval:120}];"
"renderPlanContent();"
"}"
"function syncHidden(){document.getElementById('schedJson').value=JSON.stringify(schedData);}"
"function toggleSchedEnable(){var en=document.getElementById('schedEnable').checked;document.getElementById('schedSection').style.display=en?'block':'none';}"
"</script>";

// HTML page header
static const char* html_header =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-S3 Configuration</title>"
"%s%s</head><body><div class='container'>"
"<h1>ESP32-S3 Display</h1>";

// Display tab content
static const char* html_display_tab =
"%s"  // AP mode notice if applicable
"<div class='info'>"
"<p><strong>SSID:</strong> %s | <strong>Hostname:</strong> %s</p>"
"<p><strong>Image:</strong> <a href='%s' target='_blank'>%s</a></p>"
"<p><strong>Refresh:</strong> %lu min | <strong>Size:</strong> %dx%d | <strong>Scale:</strong> %s</p>"
"</div>"
"<form action='/save' method='POST'>"
"<input type='hidden' name='tab' value='display'>"
"<label>Image URL:</label>"
"<textarea name='url' maxlength='2047' required style='width:100%%;resize:vertical;min-height:80px;box-sizing:border-box;font-family:inherit;font-size:inherit;'>%s</textarea>"
"<p style='font-size:0.85em;color:#666;margin-top:2px;'>Maximum 2048 characters. Supports long URLs including signed cloud storage URLs.</p>"
"<label>Refresh Interval (minutes):</label>"
"<input type='number' name='refresh' value='%lu' min='1' max='1440' required>"
"<p style='font-size:0.85em;color:#666;margin-top:2px;'>Used as fallback when schedule is disabled or no period matches.</p>"
"<label>Image Dimensions:</label>"
"<div class='row'>"
"<input type='number' name='img_width' value='%d' min='100' max='2000' placeholder='Width'>"
"<input type='number' name='img_height' value='%d' min='100' max='2000' placeholder='Height'>"
"</div>"
"<div class='checkbox-row'>"
"<input type='checkbox' name='img_scale' value='1' %s>"
"<label>Scale to fit display (800x480)</label>"
"</div>"
"<label>Rotation:</label>"
"<select name='img_rotation'>"
"<option value='0' %s>0&deg;</option>"
"<option value='90' %s>90&deg;</option>"
"<option value='180' %s>180&deg;</option>"
"<option value='270' %s>270&deg;</option>"
"</select>"
"<div class='checkbox-row'>"
"<input type='checkbox' name='img_mirror_h' value='1' %s><label>Mirror H</label>"
"<input type='checkbox' name='img_mirror_v' value='1' %s style='margin-left:20px;'><label>Mirror V</label>"
"</div>"
"<label>Transform Order:</label>"
"<select name='img_rot_first'>"
"<option value='1' %s>Rotate then Mirror</option>"
"<option value='0' %s>Mirror then Rotate</option>"
"</select>"
"<div class='checkbox-row'>"
"<input type='checkbox' name='led_disabled' value='1' %s>"
"<label>Disable Status LED</label>"
"</div>"
"<p style='font-size:0.85em;color:#666;margin-top:2px;'>Disable the status LED entirely!</p>"
"<div style='display:flex;gap:10px;margin-top:15px;'>"
"<input type='submit' value='Save' style='flex:1;'>"
"<input type='submit' formaction='/apply' value='Apply' style='flex:1;background:#2196F3;'>"
"</div>"
"</form>"
"<div class='section'>"
"<h3>Display Actions</h3>"
"<div class='test-buttons'>"
"<a href='/action/test' class='btn btn-test btn-blue'>Test</a>"
"<a href='/action/show' class='btn btn-test btn-orange'>Show</a>"
"<a href='/action/clear' class='btn btn-test btn-red'>Clear</a>"
"</div>"
"</div>";

// Network tab content
static const char* html_network_tab =
"<form action='/save_network' method='POST'>"
"<div class='subsection'>"
"<h3>WiFi Settings</h3>"
"<label>SSID:</label>"
"<input type='text' name='ssid' value='%s' maxlength='31' required>"
"<label>Password:</label>"
"<input type='password' name='password' value='%s' maxlength='63'>"
"<label>Hostname:</label>"
"<input type='text' name='hostname' value='%s' maxlength='31'>"
"<label>Domain:</label>"
"<input type='text' name='domain' value='%s' maxlength='63' placeholder='local'>"
"</div>"
"<div class='subsection'>"
"<h3>IP Configuration</h3>"
"<div class='radio-row'>"
"<input type='radio' name='use_dhcp' id='dhcp_on' value='1' %s onchange='toggleDhcp()'>"
"<label for='dhcp_on'>DHCP (Automatic)</label>"
"</div>"
"<div class='radio-row'>"
"<input type='radio' name='use_dhcp' id='dhcp_off' value='0' %s onchange='toggleDhcp()'>"
"<label for='dhcp_off'>Static IP</label>"
"</div>"
"<label>IP Address:</label>"
"<input type='text' name='static_ip' value='%s' class='static-ip' %s placeholder='192.168.1.100'>"
"<label>Subnet Mask:</label>"
"<input type='text' name='static_mask' value='%s' class='static-ip' %s placeholder='255.255.255.0'>"
"<label>Gateway:</label>"
"<input type='text' name='static_gw' value='%s' class='static-ip' %s placeholder='192.168.1.1'>"
"<label>Primary DNS:</label>"
"<input type='text' name='dns_primary' value='%s' class='static-ip' %s placeholder='8.8.8.8'>"
"<label>Secondary DNS:</label>"
"<input type='text' name='dns_secondary' value='%s' class='static-ip' %s placeholder='8.8.4.4'>"
"</div>"
"<div class='subsection'>"
"<h3>Time Settings</h3>"
"<div id='timeDisplay' class='time-display'>"
"<div class='current-time' id='currentTime'>--:--:--</div>"
"<div class='time-info'>Timezone: <span id='tzDisplay'>--</span></div>"
"<div class='time-info'>Status: <span id='syncStatus' class='sync-status not-synced'>Checking...</span></div>"
"<button type='button' class='sync-btn' id='syncBtn' onclick='syncNtp()'>Sync Now</button>"
"</div>"
"<label>NTP Server:</label>"
"<input type='text' name='ntp_server' value='%s' maxlength='63'>"
"<label>Timezone:</label>"
"<input type='text' name='timezone' value='%s' maxlength='63' placeholder='Europe/Berlin'>"
"<p class='tz-help'>Enter a TZ database identifier (e.g., America/New_York, Asia/Tokyo, UTC). "
"<a href='https://en.wikipedia.org/wiki/List_of_tz_database_time_zones' target='_blank'>View full list</a></p>"
"<div class='checkbox-row'>"
"<input type='checkbox' name='use_dst' value='1' %s>"
"<label>Enable Daylight Saving Time</label>"
"</div>"
"</div>"
"<input type='submit' value='Save Network Settings'>"
"</form>";

// Firmware tab content (OTA update)
static const char* html_firmware_tab =
"<h2>Firmware Update</h2>"
"<div class='version-info'>"
"<p><strong>Running Partition:</strong> %s</p>"
"<p><strong>Build Date:</strong> " __DATE__ " " __TIME__ "</p>"
"</div>"
"<div class='subsection'>"
"<h3>Upload New Firmware</h3>"
"<p>Select a compiled firmware binary (.bin) file to upload.</p>"
"<div class='file-input'>"
"<input type='file' id='firmware-file' accept='.bin'>"
"</div>"
"<div class='progress-container' style='display:none;'>"
"<div class='progress-bar' id='ota-progress'><span id='ota-progress-text'>0%%</span></div>"
"</div>"
"<div id='ota-status' class='ota-status'></div>"
"<button type='button' class='btn btn-blue' id='upload-btn' onclick='uploadFirmware()'>Upload &amp; Install</button>"
"</div>"
"<div class='subsection'>"
"<h3>Instructions</h3>"
"<p>1. Build your firmware using PlatformIO</p>"
"<p>2. Find the .bin file in .pio/build/freenove_esp32_s3_wroom/</p>"
"<p>3. Select the firmware.bin file above</p>"
"<p>4. Click 'Upload &amp; Install' to update</p>"
"<p>5. Device will reboot automatically after successful update</p>"
"<p><strong>Note:</strong> If the new firmware fails to start, the device will automatically roll back to the previous version.</p>"
"</div>";

// Schedule tab content
static const char* html_schedule_tab =
"<h2>Schedule Plans</h2>"
"<form action='/save' method='POST'>"
"<input type='hidden' name='tab' value='schedule'>"
"<input type='hidden' name='sched_json' id='schedJson' value=''>"
"<div class='sched-enable'>"
"<input type='checkbox' id='schedEnable' name='sched_enable' %s onchange='toggleSchedEnable()'>"
"<label for='schedEnable' style='margin:0;font-weight:normal;'>Enable schedule-based refresh intervals</label>"
"</div>"
"<div id='schedSection' style='display:%s;'>"
"<div class='subsection'>"
"<h3>Day Assignments</h3>"
"<p style='font-size:12px;color:#666;'>Assign a plan to each day of the week</p>"
"<div id='dayGrid' class='day-grid'></div>"
"</div>"
"<div class='subsection'>"
"<h3>Plans</h3>"
"<div id='planTabs' class='plan-tabs'></div>"
"<div id='planContent' class='plan-content active'></div>"
"</div>"
"</div>"
"<input type='submit' value='Save Schedule'>"
"</form>"
"<script>initSched();</script>";

// HTML page footer
static const char* html_footer =
"<div class='help'>"
"<p><strong>Save:</strong> Saves config only</p>"
"<p><strong>Apply:</strong> Saves, shows image, starts sleep cycle</p>"
"</div>"
"<div style='text-align:center;margin-top:20px;padding:10px;border-top:1px solid #ddd;font-size:0.85em;color:#666;'>"
"<a href='https://github.com/bolausson/esp32-ePaper-Display' target='_blank' style='color:#2196F3;text-decoration:none;'>GitHub: bolausson/esp32-ePaper-Display</a>"
"</div>"
"</div></body></html>";

// Captive portal redirect handler - returns 302 redirect to config page
static esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Captive portal detection - redirecting to config page");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Android connectivity check - /generate_204
static esp_err_t android_captive_handler(httpd_req_t *req) {
    return captive_portal_redirect_handler(req);
}

// Apple captive portal detection - /hotspot-detect.html
static esp_err_t apple_captive_handler(httpd_req_t *req) {
    return captive_portal_redirect_handler(req);
}

// Windows connectivity check - /connecttest.txt
static esp_err_t windows_captive_handler(httpd_req_t *req) {
    return captive_portal_redirect_handler(req);
}

// Firefox captive portal detection - /success.txt
static esp_err_t firefox_captive_handler(httpd_req_t *req) {
    return captive_portal_redirect_handler(req);
}

// Wildcard handler for any other requests - redirect to config page
static esp_err_t wildcard_captive_handler(httpd_req_t *req) {
    // Check if request is for our IP (192.168.4.1) - if not, redirect
    // Get the Host header
    char host_header[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host_header, sizeof(host_header)) == ESP_OK) {
        // If host is our IP, don't redirect (let other handlers process)
        if (strncmp(host_header, "192.168.4.1", 11) == 0) {
            // This shouldn't be called for our IP due to handler priority
            return ESP_FAIL;  // Let other handlers try
        }
    }

    ESP_LOGI(TAG, "Wildcard redirect - Host: %s, URI: %s", host_header, req->uri);
    return captive_portal_redirect_handler(req);
}

// Root GET handler
static esp_err_t root_get_handler(httpd_req_t *req) {
    // Update last client activity timestamp
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Client connected - serving config page");

    // Use static buffer to avoid stack overflow in httpd task
    static char response[24576];  // Increased for schedule tab
    char *p = response;
    int remaining = sizeof(response);
    int len;

    // Use safe pointers for potentially empty strings
    const char *display_url = (stored_image_url[0] != '\0') ? stored_image_url : "(not configured)";
    const char *form_url = (stored_image_url[0] != '\0') ? stored_image_url : "";
    const char *disabled_str = stored_use_dhcp ? "disabled" : "";

    // AP mode notice
    const char *ap_notice = ap_mode ?
        "<div class='ap-notice'><strong>AP Mode:</strong> Connect to your WiFi network in the Network tab.</div>" : "";

    // Get current partition info for firmware tab
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const char *partition_label = running_partition ? running_partition->label : "unknown";

    // Build header with styles and script
    len = snprintf(p, remaining, html_header, html_styles, html_script);
    p += len; remaining -= len;

    // Add tabs
    len = snprintf(p, remaining,
        "<div class='tabs'>"
        "<button class='tab active' onclick=\"showTab('display')\">Display</button>"
        "<button class='tab' onclick=\"showTab('schedule')\">Schedule</button>"
        "<button class='tab' onclick=\"showTab('network')\">Network</button>"
        "<button class='tab' onclick=\"showTab('firmware')\">Firmware</button>"
        "</div>");
    p += len; remaining -= len;

    // Display tab content
    len = snprintf(p, remaining, "<div id='display' class='tab-content active'>");
    p += len; remaining -= len;

    len = snprintf(p, remaining, html_display_tab,
             ap_notice,
             stored_ssid, stored_hostname,
             display_url, display_url,
             (unsigned long)stored_refresh_interval,
             stored_img_width, stored_img_height,
             stored_img_scale ? "yes" : "no",
             form_url,
             (unsigned long)stored_refresh_interval,
             stored_img_width, stored_img_height,
             stored_img_scale ? "checked" : "",
             (stored_img_rotation == 0) ? "selected" : "",
             (stored_img_rotation == 90) ? "selected" : "",
             (stored_img_rotation == 180) ? "selected" : "",
             (stored_img_rotation == 270) ? "selected" : "",
             stored_img_mirror_h ? "checked" : "",
             stored_img_mirror_v ? "checked" : "",
             stored_img_rot_first ? "selected" : "",
             stored_img_rot_first ? "" : "selected",
             stored_led_disabled ? "checked" : "");
    p += len; remaining -= len;

    len = snprintf(p, remaining, "</div>");
    p += len; remaining -= len;

    // Schedule tab content - inject schedule script with JSON data
    static char schedule_script_buf[8192];  // Static to avoid stack overflow
    const char* sched_json = stored_schedule_json[0] ? stored_schedule_json : default_schedule_json;
    snprintf(schedule_script_buf, sizeof(schedule_script_buf), html_schedule_script, sched_json);

    len = snprintf(p, remaining, "%s<div id='schedule' class='tab-content'>", schedule_script_buf);
    p += len; remaining -= len;

    len = snprintf(p, remaining, html_schedule_tab,
             stored_schedule_enabled ? "checked" : "",
             stored_schedule_enabled ? "block" : "none");
    p += len; remaining -= len;

    len = snprintf(p, remaining, "</div>");
    p += len; remaining -= len;

    // Network tab content
    len = snprintf(p, remaining, "<div id='network' class='tab-content'>");
    p += len; remaining -= len;

    len = snprintf(p, remaining, html_network_tab,
             stored_ssid, stored_password,
             stored_hostname, stored_domain,
             stored_use_dhcp ? "checked" : "",
             stored_use_dhcp ? "" : "checked",
             stored_static_ip, disabled_str,
             stored_static_mask, disabled_str,
             stored_static_gw, disabled_str,
             stored_dns_primary, disabled_str,
             stored_dns_secondary, disabled_str,
             stored_ntp_server,
             stored_timezone,
             stored_use_dst ? "checked" : "");
    p += len; remaining -= len;

    len = snprintf(p, remaining, "</div>");
    p += len; remaining -= len;

    // Firmware tab content
    len = snprintf(p, remaining, "<div id='firmware' class='tab-content'>");
    p += len; remaining -= len;

    len = snprintf(p, remaining, html_firmware_tab, partition_label);
    p += len; remaining -= len;

    len = snprintf(p, remaining, "</div>");
    p += len; remaining -= len;

    // Footer
    len = snprintf(p, remaining, "%s", html_footer);
    p += len; remaining -= len;

    ESP_LOGI(TAG, "Response length: %d bytes", (int)(p - response));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Helper function to URL decode
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

// Parse POST data
static void parse_post_data(char *buf, char *ssid, char *password, char *url,
                             uint32_t *refresh, uint16_t *img_width, uint16_t *img_height,
                             bool *img_scale, uint16_t *img_rotation, bool *img_mirror_h,
                             bool *img_mirror_v, bool *img_rot_first, bool *led_disabled) {
    char *token;
    char *saveptr;
    char temp_str[16] = {0};
    *img_scale = false;      // Default to false, will be set true if checkbox is present
    *img_mirror_h = false;   // Default to false
    *img_mirror_v = false;   // Default to false
    *img_rot_first = true;   // Default to rotate first
    *led_disabled = false;   // Default to false

    token = strtok_r(buf, "&", &saveptr);
    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            char *key = token;
            char *value = eq + 1;

            if (strcmp(key, "ssid") == 0) {
                url_decode(ssid, value);
            } else if (strcmp(key, "password") == 0) {
                url_decode(password, value);
            } else if (strcmp(key, "url") == 0) {
                url_decode(url, value);
            } else if (strcmp(key, "refresh") == 0) {
                url_decode(temp_str, value);
                *refresh = (uint32_t)atoi(temp_str);
                if (*refresh < 1) *refresh = 1;
                if (*refresh > 1440) *refresh = 1440;
            } else if (strcmp(key, "img_width") == 0) {
                url_decode(temp_str, value);
                int w = atoi(temp_str);
                if (w < 100) w = 100;
                if (w > 2000) w = 2000;
                *img_width = (uint16_t)w;
            } else if (strcmp(key, "img_height") == 0) {
                url_decode(temp_str, value);
                int h = atoi(temp_str);
                if (h < 100) h = 100;
                if (h > 2000) h = 2000;
                *img_height = (uint16_t)h;
            } else if (strcmp(key, "img_scale") == 0) {
                *img_scale = true;  // Checkbox is present = checked
            } else if (strcmp(key, "img_rotation") == 0) {
                url_decode(temp_str, value);
                int r = atoi(temp_str);
                // Normalize to 0, 90, 180, 270
                r = (r / 90) * 90 % 360;
                if (r < 0) r = 0;
                *img_rotation = (uint16_t)r;
            } else if (strcmp(key, "img_mirror_h") == 0) {
                *img_mirror_h = true;  // Checkbox is present = checked
            } else if (strcmp(key, "img_mirror_v") == 0) {
                *img_mirror_v = true;  // Checkbox is present = checked
            } else if (strcmp(key, "img_rot_first") == 0) {
                url_decode(temp_str, value);
                *img_rot_first = (atoi(temp_str) != 0);
            } else if (strcmp(key, "led_disabled") == 0) {
                *led_disabled = true;  // Checkbox is present = checked
            }
        }
        token = strtok_r(NULL, "&", &saveptr);
    }
}

// Save POST handler
static esp_err_t save_post_handler(httpd_req_t *req) {
    // Update last client activity timestamp
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Save request received");

    // Use static buffer for schedule JSON (URL-encoded JSON can be ~3x larger)
    static char buf[8192];
    int ret, remaining = req->content_len;

    ESP_LOGI(TAG, "Content length: %d bytes", remaining);

    if (remaining > sizeof(buf) - 1) {
        ESP_LOGE(TAG, "Content too long: %d > %d", remaining, sizeof(buf) - 1);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Check which tab this save is for
    char *tab_ptr = strstr(buf, "tab=");
    char tab_type[16] = "display";  // Default
    if (tab_ptr) {
        char *tab_val = tab_ptr + 4;
        char *tab_end = strchr(tab_val, '&');
        if (tab_end) {
            size_t len = tab_end - tab_val;
            if (len < sizeof(tab_type)) {
                strncpy(tab_type, tab_val, len);
                tab_type[len] = '\0';
            }
        } else {
            strncpy(tab_type, tab_val, sizeof(tab_type) - 1);
            tab_type[sizeof(tab_type) - 1] = '\0';
        }
    }

    ESP_LOGI(TAG, "Save request for tab: %s", tab_type);

    // Handle schedule tab save
    if (strcmp(tab_type, "schedule") == 0) {
        // Parse schedule-specific fields
        bool sched_enable = (strstr(buf, "sched_enable=on") != NULL);

        // Extract sched_json value (URL-encoded JSON)
        char *json_ptr = strstr(buf, "sched_json=");
        if (json_ptr) {
            json_ptr += 11;  // Skip "sched_json="
            char *json_end = strchr(json_ptr, '&');
            size_t json_len = json_end ? (size_t)(json_end - json_ptr) : strlen(json_ptr);

            ESP_LOGI(TAG, "Schedule JSON length (encoded): %d", json_len);

            // URL-encoded JSON can be ~3x the decoded size
            if (json_len < sizeof(buf) - 1) {
                // Use static buffers to avoid stack overflow
                static char decoded_json[MAX_SCHEDULE_JSON];
                static char temp[8192];
                strncpy(temp, json_ptr, json_len);
                temp[json_len] = '\0';
                url_decode(decoded_json, temp);

                ESP_LOGI(TAG, "Schedule JSON length (decoded): %d", strlen(decoded_json));

                save_schedule_config_to_nvs(decoded_json, sched_enable);
                ESP_LOGI(TAG, "Schedule saved - Enabled: %s", sched_enable ? "yes" : "no");
            } else {
                ESP_LOGE(TAG, "Schedule JSON too long: %d", json_len);
            }
        } else {
            ESP_LOGW(TAG, "No sched_json field found in request");
        }
    } else {
        // Handle display tab save - only save display settings, NOT network settings
        char new_url[MAX_URL_LEN] = {0};
        uint32_t new_refresh = 60;
        uint16_t new_img_width = 800;
        uint16_t new_img_height = 480;
        bool new_img_scale = false;
        uint16_t new_img_rotation = 0;
        bool new_img_mirror_h = false;
        bool new_img_mirror_v = false;
        bool new_img_rot_first = true;
        bool new_led_disabled = false;

        // Make a copy since parse_post_data modifies the buffer
        char buf_copy[3072];
        strncpy(buf_copy, buf, sizeof(buf_copy) - 1);
        buf_copy[sizeof(buf_copy) - 1] = '\0';

        // Dummy variables for ssid/password - not used for display tab
        char dummy_ssid[MAX_SSID_LEN] = {0};
        char dummy_password[MAX_PASSWORD_LEN] = {0};

        parse_post_data(buf_copy, dummy_ssid, dummy_password, new_url, &new_refresh,
                        &new_img_width, &new_img_height, &new_img_scale,
                        &new_img_rotation, &new_img_mirror_h, &new_img_mirror_v, &new_img_rot_first,
                        &new_led_disabled);

        ESP_LOGI(TAG, "Received display config - URL: %s, Refresh: %lu min, Rot: %d, MirH: %s, MirV: %s, LED disabled: %s",
                 new_url, (unsigned long)new_refresh,
                 new_img_rotation, new_img_mirror_h ? "yes" : "no", new_img_mirror_v ? "yes" : "no",
                 new_led_disabled ? "yes" : "no");

        // Save display config to NVS only - DO NOT touch network settings
        save_display_config_to_nvs(new_url, new_refresh, new_img_width, new_img_height,
                                    new_img_scale, new_img_rotation, new_img_mirror_h,
                                    new_img_mirror_v, new_img_rot_first, new_led_disabled);
    }

    // Send success response with redirect back to main page
    const char* resp_str =
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<meta http-equiv='refresh' content='2;url=/'>"
        "<style>body{font-family:Arial;text-align:center;margin-top:50px;background-color:#f0f0f0;}"
        ".message{background-color:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;box-shadow:0 2px 5px rgba(0,0,0,0.1);}"
        "h1{color:#4CAF50;}</style></head><body><div class='message'>"
        "<h1>&#10004; Configuration Saved!</h1>"
        "<p>Redirecting back...</p>"
        "</div></body></html>";

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // Don't set config_saved - just save, don't trigger apply
    return ESP_OK;
}

// OTA firmware update POST handler
static esp_err_t ota_post_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "OTA update request received");

    // Maximum firmware size (based on partition size minus some margin)
    const size_t MAX_FIRMWARE_SIZE = 1900000;  // ~1.8MB

    // Validate content length
    if (req->content_len == 0) {
        ESP_LOGE(TAG, "OTA: No content received");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data received");
        return ESP_FAIL;
    }

    if (req->content_len > MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "OTA: Firmware too large (%d bytes, max %d)", req->content_len, MAX_FIRMWARE_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware file too large");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: Receiving firmware (%d bytes)", req->content_len);

    // Get the next OTA partition to write to
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "OTA: No update partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: Writing to partition '%s' at offset 0x%lx",
             update_partition->label, update_partition->address);

    // Begin OTA update
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start OTA update");
        return ESP_FAIL;
    }

    // Buffer for receiving firmware chunks
    char *buf = malloc(4096);
    if (buf == NULL) {
        ESP_LOGE(TAG, "OTA: Failed to allocate receive buffer");
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int received;
    int remaining = req->content_len;
    int total_written = 0;
    bool header_skipped = false;

    while (remaining > 0) {
        // Receive a chunk
        received = httpd_req_recv(req, buf, MIN(remaining, 4096));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                // Retry on timeout
                continue;
            }
            ESP_LOGE(TAG, "OTA: Receive error");
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware upload failed");
            return ESP_FAIL;
        }

        // For first chunk, check if we need to skip multipart headers
        char *data = buf;
        int data_len = received;

        if (!header_skipped) {
            // Look for the end of headers (double CRLF)
            char *header_end = strstr(buf, "\r\n\r\n");
            if (header_end != NULL) {
                header_end += 4;  // Skip past \r\n\r\n
                data_len = received - (header_end - buf);
                data = header_end;
                header_skipped = true;
                ESP_LOGI(TAG, "OTA: Skipped %d bytes of headers", (int)(header_end - buf));
            }
        }

        // Write chunk to flash
        if (data_len > 0) {
            err = esp_ota_write(ota_handle, data, data_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA: esp_ota_write failed (%s)", esp_err_to_name(err));
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write firmware");
                return ESP_FAIL;
            }
            total_written += data_len;
        }

        remaining -= received;

        // Log progress periodically
        if (total_written % 102400 < 4096) {
            ESP_LOGI(TAG, "OTA: Progress %d/%d bytes (%.1f%%)",
                     total_written, req->content_len,
                     (float)total_written / req->content_len * 100);
        }
    }

    free(buf);

    ESP_LOGI(TAG, "OTA: Received complete, validating firmware...");

    // End OTA update (this validates the firmware)
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "OTA: Firmware validation failed - image is corrupted");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware validation failed - file may be corrupted");
        } else {
            ESP_LOGE(TAG, "OTA: esp_ota_end failed (%s)", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalization failed");
        }
        return ESP_FAIL;
    }

    // Set the new partition as boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: Update successful! Rebooting in 2 seconds...");

    // Send success response
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Firmware update successful! Device will reboot now...");

    // Give time for response to be sent
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Reboot to new firmware
    esp_restart();

    return ESP_OK;  // Never reached
}

// Apply POST handler - saves display config and triggers image display + sleep
static esp_err_t apply_post_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Apply request received");

    char buf[768];
    char new_url[MAX_URL_LEN] = {0};
    uint32_t new_refresh = 60;
    uint16_t new_img_width = 800;
    uint16_t new_img_height = 480;
    bool new_img_scale = false;
    uint16_t new_img_rotation = 0;
    bool new_img_mirror_h = false;
    bool new_img_mirror_v = false;
    bool new_img_rot_first = true;
    bool new_led_disabled = false;
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Dummy variables for ssid/password - not used for display/apply
    char dummy_ssid[MAX_SSID_LEN] = {0};
    char dummy_password[MAX_PASSWORD_LEN] = {0};

    // Parse the POST data
    parse_post_data(buf, dummy_ssid, dummy_password, new_url, &new_refresh,
                    &new_img_width, &new_img_height, &new_img_scale,
                    &new_img_rotation, &new_img_mirror_h, &new_img_mirror_v, &new_img_rot_first,
                    &new_led_disabled);

    ESP_LOGI(TAG, "Applying display config - URL: %s, LED disabled: %s", new_url, new_led_disabled ? "yes" : "no");

    // Save display config to NVS only - DO NOT touch network settings
    save_display_config_to_nvs(new_url, new_refresh, new_img_width, new_img_height,
                                new_img_scale, new_img_rotation, new_img_mirror_h,
                                new_img_mirror_v, new_img_rot_first, new_led_disabled);

    // Send response indicating we're applying
    const char* resp_str =
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>body{font-family:Arial;text-align:center;margin-top:50px;background-color:#f0f0f0;}"
        ".message{background-color:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;box-shadow:0 2px 5px rgba(0,0,0,0.1);}"
        "h1{color:#2196F3;}</style></head><body><div class='message'>"
        "<h1>&#10004; Applying Configuration...</h1>"
        "<p>Downloading image and entering deep sleep.</p>"
        "</div></body></html>";

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // Signal that config should be applied (triggers image download + sleep)
    config_saved = true;

    return ESP_OK;
}

// Helper function to parse network POST data
static void parse_network_post_data(char *buf, char *ssid, char *password, char *hostname, char *domain,
                                     bool *use_dhcp, char *static_ip, char *static_mask, char *static_gw,
                                     char *dns_primary, char *dns_secondary, char *ntp_server, char *timezone, bool *use_dst) {
    char *token;
    char *saveptr;
    char decoded[MAX_URL_LEN];

    *use_dhcp = true;  // Default
    *use_dst = true;   // Default

    token = strtok_r(buf, "&", &saveptr);
    while (token != NULL) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            char *key = token;
            char *value = eq + 1;
            url_decode(decoded, value);

            if (strcmp(key, "ssid") == 0) strncpy(ssid, decoded, MAX_SSID_LEN - 1);
            else if (strcmp(key, "password") == 0) strncpy(password, decoded, MAX_PASSWORD_LEN - 1);
            else if (strcmp(key, "hostname") == 0) strncpy(hostname, decoded, MAX_HOSTNAME_LEN - 1);
            else if (strcmp(key, "domain") == 0) strncpy(domain, decoded, MAX_DOMAIN_LEN - 1);
            else if (strcmp(key, "use_dhcp") == 0) *use_dhcp = (atoi(decoded) == 1);
            else if (strcmp(key, "static_ip") == 0) strncpy(static_ip, decoded, MAX_IP_LEN - 1);
            else if (strcmp(key, "static_mask") == 0) strncpy(static_mask, decoded, MAX_IP_LEN - 1);
            else if (strcmp(key, "static_gw") == 0) strncpy(static_gw, decoded, MAX_IP_LEN - 1);
            else if (strcmp(key, "dns_primary") == 0) strncpy(dns_primary, decoded, MAX_IP_LEN - 1);
            else if (strcmp(key, "dns_secondary") == 0) strncpy(dns_secondary, decoded, MAX_IP_LEN - 1);
            else if (strcmp(key, "ntp_server") == 0) strncpy(ntp_server, decoded, MAX_NTP_SERVER_LEN - 1);
            else if (strcmp(key, "timezone") == 0) strncpy(timezone, decoded, MAX_TIMEZONE_LEN - 1);
            else if (strcmp(key, "use_dst") == 0) *use_dst = true;
        }
        token = strtok_r(NULL, "&", &saveptr);
    }
}

// Save Network POST handler
static esp_err_t save_network_post_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Save network request received");

    char buf[1024];
    char new_ssid[MAX_SSID_LEN] = {0};
    char new_password[MAX_PASSWORD_LEN] = {0};
    char new_hostname[MAX_HOSTNAME_LEN] = {0};
    char new_domain[MAX_DOMAIN_LEN] = {0};
    bool new_use_dhcp = true;
    char new_static_ip[MAX_IP_LEN] = {0};
    char new_static_mask[MAX_IP_LEN] = {0};
    char new_static_gw[MAX_IP_LEN] = {0};
    char new_dns_primary[MAX_IP_LEN] = {0};
    char new_dns_secondary[MAX_IP_LEN] = {0};
    char new_ntp_server[MAX_NTP_SERVER_LEN] = {0};
    char new_timezone[MAX_TIMEZONE_LEN] = {0};
    bool new_use_dst = true;
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse the POST data
    parse_network_post_data(buf, new_ssid, new_password, new_hostname, new_domain,
                            &new_use_dhcp, new_static_ip, new_static_mask, new_static_gw,
                            new_dns_primary, new_dns_secondary, new_ntp_server, new_timezone, &new_use_dst);

    ESP_LOGI(TAG, "Network config - SSID: %s, Hostname: %s, DHCP: %s",
             new_ssid, new_hostname, new_use_dhcp ? "yes" : "no");

    // Save network config to NVS
    save_network_config_to_nvs(new_ssid, new_password, new_hostname, new_domain,
                                new_use_dhcp, new_static_ip, new_static_mask, new_static_gw,
                                new_dns_primary, new_dns_secondary, stored_dns_search,
                                new_ntp_server, new_timezone, new_use_dst);

    // Send success response
    const char* resp_str =
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<meta http-equiv='refresh' content='2;url=/'>"
        "<style>body{font-family:Arial;text-align:center;margin-top:50px;background-color:#f0f0f0;}"
        ".message{background-color:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;box-shadow:0 2px 5px rgba(0,0,0,0.1);}"
        "h1{color:#4CAF50;}</style></head><body><div class='message'>"
        "<h1>&#10004; Network Settings Saved!</h1>"
        "<p>Redirecting back...</p>"
        "<p><small>Restart device to apply WiFi changes.</small></p>"
        "</div></body></html>";

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // Set config_saved to trigger reconnection if in AP mode
    if (ap_mode && new_ssid[0] != '\0') {
        config_saved = true;
    }

    return ESP_OK;
}

// API handler for getting current time info (JSON response)
static esp_err_t api_time_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Check if time is likely synced (year > 2023 means NTP worked)
    bool likely_synced = (timeinfo.tm_year + 1900) > 2023;

    char response[256];
    snprintf(response, sizeof(response),
        "{\"time\":\"%s\",\"synced\":%s,\"timezone\":\"%s\",\"epoch\":%ld,\"last_sync\":%ld}",
        time_str,
        (ntp_synced && likely_synced) ? "true" : "false",
        stored_timezone,
        (long)now,
        (long)last_ntp_sync);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// API handler for triggering NTP sync
static esp_err_t api_ntp_sync_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Manual NTP sync requested");

    bool success = trigger_ntp_sync();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

    char response[256];
    snprintf(response, sizeof(response),
        "{\"success\":%s,\"time\":\"%s\",\"synced\":%s,\"timezone\":\"%s\"}",
        success ? "true" : "false",
        time_str,
        ntp_synced ? "true" : "false",
        stored_timezone);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Forward declarations for display functions used by action handler
static void do_show_test_pattern(void);
static void do_show_image_from_url(void);
static void do_clear_display(void);

// Action handler - shows loading page with JavaScript to call the actual action
static esp_err_t action_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    const char *action = req->uri + strlen("/action/");
    ESP_LOGI(TAG, "Action page requested: %s", action);

    const char *resp_title = "Action";
    const char *resp_msg = "Processing...";
    const char *resp_color = "#888";

    if (strcmp(action, "test") == 0) {
        resp_title = "Test Pattern";
        resp_msg = "Displaying test pattern...";
        resp_color = "#2196F3";
    } else if (strcmp(action, "show") == 0) {
        resp_title = "Show Image";
        resp_msg = "Downloading and displaying image...";
        resp_color = "#FF9800";
    } else if (strcmp(action, "clear") == 0) {
        resp_title = "Clear Display";
        resp_msg = "Clearing display...";
        resp_color = "#f44336";
    }

    // Send loading page immediately, then JavaScript fetches the actual action
    static char resp[1024];
    snprintf(resp, sizeof(resp),
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>body{font-family:Arial;text-align:center;margin-top:50px;background-color:#f0f0f0;}"
        ".message{background-color:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;box-shadow:0 2px 5px rgba(0,0,0,0.1);}"
        "h1{color:%s;}.spinner{border:4px solid #f3f3f3;border-top:4px solid %s;border-radius:50%%;width:40px;height:40px;animation:spin 1s linear infinite;margin:20px auto;}"
        "@keyframes spin{0%%{transform:rotate(0deg);}100%%{transform:rotate(360deg);}}</style></head>"
        "<body><div class='message'><h1>%s</h1><div class='spinner'></div><p id='status'>%s</p></div>"
        "<script>fetch('/do/%s').then(r=>r.text()).then(t=>{document.getElementById('status').innerHTML=t+'<br><small>Redirecting...</small>';setTimeout(()=>location.href='/',2000);}).catch(e=>{document.getElementById('status').innerHTML='Error: '+e;});</script>"
        "</body></html>", resp_color, resp_color, resp_title, resp_msg, action);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Do handler - actually performs the action
static esp_err_t do_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    const char *action = req->uri + strlen("/do/");
    ESP_LOGI(TAG, "Performing action: %s", action);

    const char *result = "Unknown action";

    if (strcmp(action, "test") == 0) {
        do_show_test_pattern();
        result = "&#10004; Test pattern displayed!";
    } else if (strcmp(action, "show") == 0) {
        do_show_image_from_url();
        result = "&#10004; Image displayed!";
    } else if (strcmp(action, "clear") == 0) {
        do_clear_display();
        result = "&#10004; Display cleared!";
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Start web server
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;  // Enable wildcard matching
    config.max_uri_handlers = 16;  // Increase from default 8 for captive portal handlers
    config.stack_size = 16384;  // Increase stack size for schedule JSON handling

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t save = {
            .uri       = "/save",
            .method    = HTTP_POST,
            .handler   = save_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &save);

        httpd_uri_t apply = {
            .uri       = "/apply",
            .method    = HTTP_POST,
            .handler   = apply_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &apply);

        httpd_uri_t save_network = {
            .uri       = "/save_network",
            .method    = HTTP_POST,
            .handler   = save_network_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &save_network);

        // API endpoints for time info and NTP sync
        httpd_uri_t api_time = {
            .uri       = "/api/time",
            .method    = HTTP_GET,
            .handler   = api_time_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &api_time);

        httpd_uri_t api_ntp_sync = {
            .uri       = "/api/ntp_sync",
            .method    = HTTP_POST,
            .handler   = api_ntp_sync_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &api_ntp_sync);

        httpd_uri_t action = {
            .uri       = "/action/*",
            .method    = HTTP_GET,
            .handler   = action_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &action);

        httpd_uri_t do_action = {
            .uri       = "/do/*",
            .method    = HTTP_GET,
            .handler   = do_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &do_action);

        // OTA firmware update handler
        httpd_uri_t ota = {
            .uri       = "/ota",
            .method    = HTTP_POST,
            .handler   = ota_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &ota);

        // Register captive portal handlers only in AP mode
        if (ap_mode) {
            ESP_LOGI(TAG, "Registering captive portal handlers");

            // Android connectivity check
            httpd_uri_t android_captive = {
                .uri       = "/generate_204",
                .method    = HTTP_GET,
                .handler   = android_captive_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(server, &android_captive);

            // Apple captive portal detection
            httpd_uri_t apple_captive = {
                .uri       = "/hotspot-detect.html",
                .method    = HTTP_GET,
                .handler   = apple_captive_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(server, &apple_captive);

            // Windows connectivity check
            httpd_uri_t windows_captive = {
                .uri       = "/connecttest.txt",
                .method    = HTTP_GET,
                .handler   = windows_captive_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(server, &windows_captive);

            // Firefox captive portal detection
            httpd_uri_t firefox_captive = {
                .uri       = "/success.txt",
                .method    = HTTP_GET,
                .handler   = firefox_captive_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(server, &firefox_captive);

            // Wildcard handler for any other requests - must be last
            httpd_uri_t wildcard = {
                .uri       = "/*",
                .method    = HTTP_GET,
                .handler   = wildcard_captive_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(server, &wildcard);
        }

        ESP_LOGI(TAG, "Web server started successfully");
        return server;
    }

    ESP_LOGE(TAG, "Error starting web server!");
    return NULL;
}

// Stop web server
static void stop_webserver(httpd_handle_t server) {
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "Web server stopped");
    }
}

// Display action: Show test pattern
static void do_show_test_pattern(void) {
    ESP_LOGI(TAG, "Showing test pattern...");
    set_led_color(50, 50, 0);  // Yellow while working

    // Initialize display if not already done
    esp_err_t ret = epd_7in3e_init_hw();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init display hardware");
        set_led_color(50, 0, 0);  // Red on error
        return;
    }
    epd_7in3e_init();

    // Show color blocks
    epd_7in3e_show_color_blocks();
    epd_7in3e_sleep();

    set_led_color(0, 50, 0);  // Green on success
    ESP_LOGI(TAG, "Test pattern displayed");
}

// Display action: Show image from URL
static void do_show_image_from_url(void) {
    ESP_LOGI(TAG, "Showing image from URL: %s", stored_image_url);

    if (stored_image_url[0] == '\0') {
        ESP_LOGE(TAG, "No image URL configured");
        set_led_color(50, 0, 0);  // Red on error
        return;
    }

    set_led_color(0, 0, 50);  // Blue while downloading

    // Initialize display
    esp_err_t ret = epd_7in3e_init_hw();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init display hardware");
        set_led_color(50, 0, 0);
        return;
    }
    epd_7in3e_init();

    // Initialize image processor
    ret = image_processor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init image processor: %s", image_processor_get_error());
        set_led_color(50, 0, 0);
        epd_7in3e_show_color_blocks();  // Fallback
        epd_7in3e_sleep();
        return;
    }

    // Allocate image buffer
    uint8_t *image_buffer = heap_caps_malloc(IMAGE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (image_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate image buffer");
        set_led_color(50, 0, 0);
        image_processor_deinit();
        return;
    }

    // Configure scaling and transforms
    image_processor_set_scaling(stored_img_width, stored_img_height, stored_img_scale);
    image_processor_set_transform(stored_img_rotation, stored_img_mirror_h, stored_img_mirror_v, stored_img_rot_first);

    // Download and process image
    ret = image_download_and_process(stored_image_url, image_buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download/process image: %s", image_processor_get_error());
        set_led_color(50, 0, 0);
        epd_7in3e_show_color_blocks();  // Fallback
    } else {
        set_led_color(0, 50, 50);  // Cyan while displaying
        epd_7in3e_display(image_buffer);
        set_led_color(0, 50, 0);  // Green on success
        ESP_LOGI(TAG, "Image displayed successfully");
    }

    heap_caps_free(image_buffer);
    image_processor_deinit();
    epd_7in3e_sleep();
}

// Display action: Clear display
static void do_clear_display(void) {
    ESP_LOGI(TAG, "Clearing display...");
    set_led_color(50, 50, 0);  // Yellow while working

    // Initialize display
    esp_err_t ret = epd_7in3e_init_hw();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init display hardware");
        set_led_color(50, 0, 0);
        return;
    }
    epd_7in3e_init();

    // Clear to white
    epd_7in3e_clear(EPD_7IN3E_WHITE);
    epd_7in3e_sleep();

    set_led_color(0, 50, 0);  // Green on success
    ESP_LOGI(TAG, "Display cleared");
}

// Initialize boot button
static void init_boot_button(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Boot button initialized on GPIO %d", BOOT_BUTTON_GPIO);
}

// Enter deep sleep mode for specified minutes
static void enter_deep_sleep(uint32_t sleep_minutes) {
    ESP_LOGI(TAG, "Preparing to enter deep sleep for %lu minutes...", (unsigned long)sleep_minutes);

    // Stop LED task from overriding our LED control
    preparing_sleep = true;
    vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL + 50));  // Wait for LED task to notice

    // Blink blue LED briefly before sleep
    for (int i = 0; i < 3; i++) {
        set_led_color(0, 0, 50);  // Blue
        vTaskDelay(pdMS_TO_TICKS(200));
        set_led_color(0, 0, 0);   // Off
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Turn off LED
    set_led_color(0, 0, 0);

    // Configure timer wake-up
    uint64_t sleep_time_us = (uint64_t)sleep_minutes * 60 * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleep_time_us);

    // Also configure boot button wake-up for reconfiguration
    esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);  // Wake on LOW (button pressed)

    ESP_LOGI(TAG, "Entering deep sleep. Will wake in %lu minutes or on button press.", (unsigned long)sleep_minutes);

    // Small delay to allow serial output to complete
    vTaskDelay(pdMS_TO_TICKS(100));

    // Enter deep sleep
    esp_deep_sleep_start();
}

// Run webserver until config is saved or timeout
static void run_webserver_loop(void) {
    webserver_mode = true;
    config_saved = false;

    server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start webserver");
        return;
    }

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "Web server running - configure at this IP");
    ESP_LOGI(TAG, "Timeout: %d seconds without client activity", WEB_SERVER_TIMEOUT / 1000);
    ESP_LOGI(TAG, "==============================================");

    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    while (!config_saved) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t idle_time = current_time - last_client_activity;

        // Timeout only if no client activity
        if (idle_time >= (WEB_SERVER_TIMEOUT / 1000)) {
            ESP_LOGI(TAG, "No client activity for %lu seconds", (unsigned long)idle_time);
            break;
        }
    }

    // Small delay to let HTTP response complete
    vTaskDelay(pdMS_TO_TICKS(500));
    stop_webserver(server);
    server = NULL;
    webserver_mode = false;
}

// Main application
void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3 Display Starting ===");

    // Check wake-up reason
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    bool woke_from_button = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);
    bool woke_from_timer = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER);

    if (woke_from_button) {
        ESP_LOGI(TAG, "Wakeup: BUTTON pressed");
    } else if (woke_from_timer) {
        ESP_LOGI(TAG, "Wakeup: TIMER expired");
    } else {
        ESP_LOGI(TAG, "Wakeup: Normal boot / power on");
    }

    // OTA rollback validation - mark this firmware as valid
    // If we got here, the firmware booted successfully
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA: First boot after update - marking firmware as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    ESP_LOGI(TAG, "Running from partition: %s", running ? running->label : "unknown");

    // Initialize NVS
    init_nvs();

    // Load configuration from NVS
    load_config_from_nvs();

    // Initialize LED
    init_led();

    // Initialize boot button
    init_boot_button();

    // Start LED task
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);

    // Initialize WiFi network interfaces
    wifi_init_netif();

    // Determine if we need webserver mode
    bool need_webserver = woke_from_button || (strlen(stored_image_url) == 0);

    // WiFi connection strategy:
    // 1. If we have credentials, try STA mode with timeout
    // 2. If STA fails or no credentials, switch to AP mode
    // 3. In AP mode, run webserver for configuration
    // 4. After config saved, retry STA mode

    bool connected = false;

    if (has_wifi_credentials()) {
        ESP_LOGI(TAG, "Attempting WiFi STA connection (timeout: %d ms)", WIFI_STA_TIMEOUT_MS);
        connected = wifi_init_sta_with_timeout(WIFI_STA_TIMEOUT_MS);
    }

    if (!connected) {
        // No credentials or STA connection failed - start AP mode
        ESP_LOGI(TAG, "Starting AP mode for configuration");
        wifi_init_ap();
        webserver_mode = true;

        // Start webserver in AP mode
        server = start_webserver();
        if (server == NULL) {
            ESP_LOGE(TAG, "Failed to start webserver in AP mode!");
        } else {
            ESP_LOGI(TAG, "Webserver started in AP mode. Connect to '%s' to configure.", AP_SSID);
        }

        // Wait for configuration with AP timeout
        uint32_t ap_start_time = xTaskGetTickCount();
        config_saved = false;

        while (!config_saved) {
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Check AP timeout
            uint32_t elapsed = (xTaskGetTickCount() - ap_start_time) * portTICK_PERIOD_MS;
            if (elapsed >= AP_MODE_TIMEOUT_MS) {
                ESP_LOGI(TAG, "AP mode timeout, retrying STA connection...");

                // Stop AP and try STA again
                stop_webserver(server);
                server = NULL;
                wifi_deinit();

                if (has_wifi_credentials()) {
                    connected = wifi_init_sta_with_timeout(WIFI_STA_TIMEOUT_MS);
                    if (connected) {
                        webserver_mode = false;
                        break;
                    }
                }

                // STA failed again, restart AP mode
                wifi_init_ap();
                server = start_webserver();
                ap_start_time = xTaskGetTickCount();
            }
        }

        // Config was saved, stop AP and try STA
        if (config_saved && server != NULL) {
            ESP_LOGI(TAG, "Configuration saved, switching to STA mode...");
            stop_webserver(server);
            server = NULL;
            wifi_deinit();

            connected = wifi_init_sta_with_timeout(WIFI_STA_TIMEOUT_MS);
            webserver_mode = false;
        }
    }

    if (!connected) {
        ESP_LOGE(TAG, "WiFi connection failed! Staying awake with red LED.");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "WiFi connected!");

    // Wait for NTP sync if NTP server is configured (60 second timeout)
    // This ensures time is accurate before downloading images
    if (strlen(stored_ntp_server) > 0) {
        ESP_LOGI(TAG, "Waiting for time synchronization...");
        if (!wait_for_ntp_sync(60)) {
            ESP_LOGW(TAG, "Time sync failed or timed out, continuing anyway");
        }
    }

    // If woke from button or need webserver, run it in STA mode
    if (need_webserver && !config_saved) {
        if (woke_from_button) {
            ESP_LOGI(TAG, "Button wake - starting webserver for reconfiguration");
        } else {
            ESP_LOGI(TAG, "No image URL configured - starting webserver for setup");
        }

        run_webserver_loop();

        // If still no URL after webserver, keep blinking and waiting
        if (strlen(stored_image_url) == 0) {
            ESP_LOGW(TAG, "Still no image URL configured! Staying awake.");
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    // At this point we have a valid configuration
    ESP_LOGI(TAG, "Configuration valid:");
    ESP_LOGI(TAG, "  Image URL: %s", stored_image_url);
    ESP_LOGI(TAG, "  Refresh interval: %lu minutes", (unsigned long)stored_refresh_interval);

    // Initialize e-Paper display hardware
    ESP_LOGI(TAG, "Initializing e-Paper display...");
    esp_err_t epd_ret = epd_7in3e_init_hw();
    if (epd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize e-Paper hardware!");
        // Continue anyway, display might still work
    }

    // Initialize e-Paper controller
    epd_7in3e_init();

    // Initialize image processor
    ESP_LOGI(TAG, "Initializing image processor...");
    esp_err_t img_ret = image_processor_init();
    if (img_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize image processor: %s", image_processor_get_error());
        ESP_LOGI(TAG, "Falling back to color test pattern");
        epd_7in3e_show_color_blocks();
        epd_7in3e_sleep();
        enter_deep_sleep(get_effective_refresh_interval());
    }

    // Allocate buffer for processed image in PSRAM
    uint8_t *image_buffer = heap_caps_malloc(IMAGE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (image_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate image buffer in PSRAM");
        ESP_LOGI(TAG, "Falling back to color test pattern");
        epd_7in3e_show_color_blocks();
        image_processor_deinit();
        epd_7in3e_sleep();
        enter_deep_sleep(get_effective_refresh_interval());
    }

    // Configure scaling, transforms, and download image
    image_processor_set_scaling(stored_img_width, stored_img_height, stored_img_scale);
    image_processor_set_transform(stored_img_rotation, stored_img_mirror_h, stored_img_mirror_v, stored_img_rot_first);
    ESP_LOGI(TAG, "Downloading image from: %s", stored_image_url);
    set_led_color(0, 0, 50);  // Blue while downloading

    img_ret = image_download_and_process(stored_image_url, image_buffer);
    if (img_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download/process image: %s", image_processor_get_error());
        ESP_LOGI(TAG, "Falling back to color test pattern");
        set_led_color(50, 0, 0);  // Red on error
        vTaskDelay(pdMS_TO_TICKS(1000));
        epd_7in3e_show_color_blocks();
    } else {
        ESP_LOGI(TAG, "Image processed successfully, displaying...");
        set_led_color(0, 50, 50);  // Cyan while displaying

        // Display the processed image
        epd_7in3e_display(image_buffer);
        ESP_LOGI(TAG, "Image displayed successfully");
    }

    // Clean up
    heap_caps_free(image_buffer);
    image_processor_deinit();

    // Put display to sleep before MCU deep sleep
    epd_7in3e_sleep();

    // Enter deep sleep for the configured/scheduled interval
    enter_deep_sleep(get_effective_refresh_interval());
}