#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "config.h"
#include "wifi_config.h"
#include "epd_7in3e.h"
#include "image_processor.h"

static const char *TAG = "ESP32-S3-Display";

// Global variables
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool wifi_connected = false;
static bool webserver_mode = false;
static bool config_saved = false;
static bool preparing_sleep = false;  // Flag to stop LED task before deep sleep
static httpd_handle_t server = NULL;
static led_strip_handle_t led_strip = NULL;
static volatile uint32_t last_client_activity = 0;  // Timestamp of last client activity

// Storage for WiFi credentials, image URL, and refresh interval
static char stored_ssid[MAX_SSID_LEN] = {0};
static char stored_password[MAX_PASSWORD_LEN] = {0};
static char stored_image_url[MAX_URL_LEN] = {0};
static uint32_t stored_refresh_interval = 60;  // Default 60 minutes
static uint16_t stored_img_width = 800;   // Default display width
static uint16_t stored_img_height = 480;  // Default display height
static bool stored_img_scale = false;     // Scale image to fit display
static uint16_t stored_img_rotation = 0;  // Image rotation (0, 90, 180, 270)
static bool stored_img_mirror_h = false;  // Mirror horizontally
static bool stored_img_mirror_v = false;  // Mirror vertically
static bool stored_img_rot_first = true;  // Rotate before mirroring

// Function prototypes
static void init_nvs(void);
static void load_credentials_from_nvs(void);
static void save_credentials_to_nvs(const char *ssid, const char *password, const char *url,
                                     uint32_t refresh_min, uint16_t img_width, uint16_t img_height,
                                     bool img_scale, uint16_t img_rotation, bool img_mirror_h,
                                     bool img_mirror_v, bool img_rot_first);
static void init_led(void);
static void set_led_color(uint8_t r, uint8_t g, uint8_t b);
static void led_task(void *pvParameters);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void wifi_init_sta(void);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t save_post_handler(httpd_req_t *req);
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

// Load credentials from NVS
static void load_credentials_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t ssid_len = MAX_SSID_LEN;
        size_t pass_len = MAX_PASSWORD_LEN;
        size_t url_len = MAX_URL_LEN;

        // Try to read stored credentials
        err = nvs_get_str(nvs_handle, NVS_WIFI_SSID, stored_ssid, &ssid_len);
        if (err != ESP_OK) {
            strcpy(stored_ssid, DEFAULT_WIFI_SSID);
            ESP_LOGI(TAG, "Using default SSID");
        }

        err = nvs_get_str(nvs_handle, NVS_WIFI_PASS, stored_password, &pass_len);
        if (err != ESP_OK) {
            strcpy(stored_password, DEFAULT_WIFI_PASSWORD);
            ESP_LOGI(TAG, "Using default password");
        }

        err = nvs_get_str(nvs_handle, NVS_IMAGE_URL, stored_image_url, &url_len);
        if (err != ESP_OK) {
            stored_image_url[0] = '\0';  // No default - empty means not configured
            ESP_LOGI(TAG, "No image URL configured");
        }

        err = nvs_get_u32(nvs_handle, "refresh_min", &stored_refresh_interval);
        if (err != ESP_OK) {
            stored_refresh_interval = 60;  // Default 60 minutes
            ESP_LOGI(TAG, "Using default refresh interval: 60 minutes");
        }

        uint16_t tmp_u16;
        err = nvs_get_u16(nvs_handle, NVS_IMG_WIDTH, &tmp_u16);
        if (err == ESP_OK) stored_img_width = tmp_u16;
        else stored_img_width = 800;

        err = nvs_get_u16(nvs_handle, NVS_IMG_HEIGHT, &tmp_u16);
        if (err == ESP_OK) stored_img_height = tmp_u16;
        else stored_img_height = 480;

        uint8_t tmp_u8;
        err = nvs_get_u8(nvs_handle, NVS_IMG_SCALE, &tmp_u8);
        if (err == ESP_OK) stored_img_scale = (tmp_u8 != 0);
        else stored_img_scale = false;

        err = nvs_get_u16(nvs_handle, NVS_IMG_ROTATION, &tmp_u16);
        if (err == ESP_OK) stored_img_rotation = tmp_u16;
        else stored_img_rotation = 0;

        err = nvs_get_u8(nvs_handle, NVS_IMG_MIRROR_H, &tmp_u8);
        if (err == ESP_OK) stored_img_mirror_h = (tmp_u8 != 0);
        else stored_img_mirror_h = false;

        err = nvs_get_u8(nvs_handle, NVS_IMG_MIRROR_V, &tmp_u8);
        if (err == ESP_OK) stored_img_mirror_v = (tmp_u8 != 0);
        else stored_img_mirror_v = false;

        err = nvs_get_u8(nvs_handle, NVS_IMG_ROT_FIRST, &tmp_u8);
        if (err == ESP_OK) stored_img_rot_first = (tmp_u8 != 0);
        else stored_img_rot_first = true;

        nvs_close(nvs_handle);
    } else {
        // Use defaults if NVS read fails
        strcpy(stored_ssid, DEFAULT_WIFI_SSID);
        strcpy(stored_password, DEFAULT_WIFI_PASSWORD);
        stored_image_url[0] = '\0';  // No default
        stored_refresh_interval = 60;
        stored_img_width = 800;
        stored_img_height = 480;
        stored_img_scale = false;
        stored_img_rotation = 0;
        stored_img_mirror_h = false;
        stored_img_mirror_v = false;
        stored_img_rot_first = true;
        ESP_LOGI(TAG, "NVS not found, using defaults");
    }

    ESP_LOGI(TAG, "Loaded config - SSID: %s, URL: %s, Refresh: %lu min, Size: %dx%d, Scale: %s, Rot: %d, MirH: %s, MirV: %s",
             stored_ssid,
             stored_image_url[0] ? stored_image_url : "(not configured)",
             (unsigned long)stored_refresh_interval,
             stored_img_width, stored_img_height,
             stored_img_scale ? "yes" : "no",
             stored_img_rotation,
             stored_img_mirror_h ? "yes" : "no",
             stored_img_mirror_v ? "yes" : "no");
}

// Save credentials to NVS
static void save_credentials_to_nvs(const char *ssid, const char *password, const char *url,
                                     uint32_t refresh_min, uint16_t img_width, uint16_t img_height,
                                     bool img_scale, uint16_t img_rotation, bool img_mirror_h,
                                     bool img_mirror_v, bool img_rot_first) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_SSID, ssid);
        nvs_set_str(nvs_handle, NVS_WIFI_PASS, password);
        nvs_set_str(nvs_handle, NVS_IMAGE_URL, url);
        nvs_set_u32(nvs_handle, "refresh_min", refresh_min);
        nvs_set_u16(nvs_handle, NVS_IMG_WIDTH, img_width);
        nvs_set_u16(nvs_handle, NVS_IMG_HEIGHT, img_height);
        nvs_set_u8(nvs_handle, NVS_IMG_SCALE, img_scale ? 1 : 0);
        nvs_set_u16(nvs_handle, NVS_IMG_ROTATION, img_rotation);
        nvs_set_u8(nvs_handle, NVS_IMG_MIRROR_H, img_mirror_h ? 1 : 0);
        nvs_set_u8(nvs_handle, NVS_IMG_MIRROR_V, img_mirror_v ? 1 : 0);
        nvs_set_u8(nvs_handle, NVS_IMG_ROT_FIRST, img_rot_first ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        // Update stored values
        strcpy(stored_ssid, ssid);
        strcpy(stored_password, password);
        strcpy(stored_image_url, url);
        stored_refresh_interval = refresh_min;
        stored_img_width = img_width;
        stored_img_height = img_height;
        stored_img_scale = img_scale;
        stored_img_rotation = img_rotation;
        stored_img_mirror_h = img_mirror_h;
        stored_img_mirror_v = img_mirror_v;
        stored_img_rot_first = img_rot_first;

        ESP_LOGI(TAG, "Config saved - SSID: %s, URL: %s, Refresh: %lu min, Rot: %d, MirH: %s, MirV: %s",
                 ssid, url, (unsigned long)refresh_min, img_rotation,
                 img_mirror_h ? "yes" : "no", img_mirror_v ? "yes" : "no");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
    }
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

        if (webserver_mode) {
            // Yellow blink when webserver is running
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

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi started, connecting...");
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
    }
}

// Initialize WiFi in station mode
static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // Copy SSID and password
    strncpy((char *)wifi_config.sta.ssid, stored_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, stored_password, sizeof(wifi_config.sta.password));

    ESP_LOGI(TAG, "WiFi configuration:");
    ESP_LOGI(TAG, "  SSID: '%s' (length: %d)", stored_ssid, strlen(stored_ssid));
    ESP_LOGI(TAG, "  Password: '%s' (length: %d)", stored_password, strlen(stored_password));
    ESP_LOGI(TAG, "  Auth mode: WPA2-PSK");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to SSID: %s", stored_ssid);

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", stored_ssid);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID: %s", stored_ssid);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// HTML page for configuration
static const char* html_page =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESP32-S3 Configuration</title>"
"<style>"
"body { font-family: Arial, sans-serif; margin: 20px; background-color: #f0f0f0; }"
"h1, h2 { color: #333; }"
".container { background-color: white; padding: 20px; border-radius: 10px; max-width: 500px; margin: 0 auto; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }"
"input[type=text], input[type=password], input[type=number] { width: 100%%; padding: 10px; margin: 8px 0; box-sizing: border-box; border: 1px solid #ddd; border-radius: 4px; }"
"input[type=submit], .btn { background-color: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; width: 100%%; font-size: 16px; margin: 5px 0; display: block; text-align: center; text-decoration: none; box-sizing: border-box; }"
"input[type=submit]:hover, .btn:hover { opacity: 0.9; }"
".btn.btn-test { width: auto !important; display: inline-block !important; padding: 10px 20px; }"
".test-buttons { display: flex; flex-wrap: wrap; gap: 8px; }"
".btn-blue { background-color: #2196F3; }"
".btn-orange { background-color: #FF9800; }"
".btn-red { background-color: #f44336; }"
"label { font-weight: bold; color: #555; }"
".info { background-color: #e7f3fe; border-left: 4px solid #2196F3; padding: 10px; margin: 10px 0; word-wrap: break-word; overflow-wrap: break-word; }"
".info a { color: #1565c0; word-break: break-all; }"
".row { display: flex; gap: 10px; }"
".row input { flex: 1; }"
".checkbox-row { display: flex; align-items: center; margin: 10px 0; }"
".checkbox-row input { width: auto; margin-right: 10px; }"
".section { border-top: 1px solid #ddd; margin-top: 20px; padding-top: 15px; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>ESP32-S3 Display Configuration</h1>"
"<div class='info'>"
"<p><strong>Current SSID:</strong> %s</p>"
"<p><strong>Image URL:</strong> <a href='%s' target='_blank'>%s</a></p>"
"<p><strong>Refresh:</strong> %lu min | <strong>Size:</strong> %dx%d | <strong>Scale:</strong> %s</p>"
"<p><strong>Rotation:</strong> %d&deg; | <strong>Mirror H:</strong> %s | <strong>Mirror V:</strong> %s | <strong>Order:</strong> %s</p>"
"</div>"
"<form action='/save' method='POST'>"
"<label for='ssid'>WiFi SSID:</label><br>"
"<input type='text' id='ssid' name='ssid' value='%s' maxlength='31' required><br><br>"
"<label for='password'>WiFi Password:</label><br>"
"<input type='password' id='password' name='password' value='%s' maxlength='63' required><br><br>"
"<label for='url'>Image Download URL:</label><br>"
"<input type='text' id='url' name='url' value='%s' maxlength='255' required><br><br>"
"<label for='refresh'>Refresh Interval (minutes):</label><br>"
"<input type='number' id='refresh' name='refresh' value='%lu' min='1' max='1440' required><br><br>"
"<label>Image Dimensions:</label><br>"
"<div class='row'>"
"<input type='number' id='img_width' name='img_width' value='%d' min='100' max='2000' placeholder='Width'>"
"<input type='number' id='img_height' name='img_height' value='%d' min='100' max='2000' placeholder='Height'>"
"</div>"
"<div class='checkbox-row'>"
"<input type='checkbox' id='img_scale' name='img_scale' value='1' %s>"
"<label for='img_scale'>Scale image to fit display (800x480)</label>"
"</div><br>"
"<label for='img_rotation'>Image Rotation:</label><br>"
"<select id='img_rotation' name='img_rotation' style='width:100%%;padding:10px;margin:8px 0;border:1px solid #ddd;border-radius:4px;'>"
"<option value='0' %s>0&deg; (No rotation)</option>"
"<option value='90' %s>90&deg; (Clockwise)</option>"
"<option value='180' %s>180&deg; (Upside down)</option>"
"<option value='270' %s>270&deg; (Counter-clockwise)</option>"
"</select><br><br>"
"<div class='checkbox-row'>"
"<input type='checkbox' id='img_mirror_h' name='img_mirror_h' value='1' %s>"
"<label for='img_mirror_h'>Mirror Horizontal</label>"
"</div>"
"<div class='checkbox-row'>"
"<input type='checkbox' id='img_mirror_v' name='img_mirror_v' value='1' %s>"
"<label for='img_mirror_v'>Mirror Vertical</label>"
"</div><br>"
"<label for='img_rot_order'>Transform Order:</label><br>"
"<select id='img_rot_order' name='img_rot_first' style='width:100%%;padding:10px;margin:8px 0;border:1px solid #ddd;border-radius:4px;'>"
"<option value='1' %s>Rotate then Mirror</option>"
"<option value='0' %s>Mirror then Rotate</option>"
"</select><br><br>"
"<div style='display:flex;gap:10px;'>"
"<input type='submit' value='Save Configuration' style='flex:1;'>"
"<input type='submit' formaction='/apply' value='Apply' style='flex:1;background-color:#2196F3;'>"
"</div>"
"</form>"
"<div class='section'>"
"<h2>Display Test</h2>"
"<div class='test-buttons'>"
"<a href='/action/test' class='btn btn-test btn-blue'>Test Pattern</a>"
"<a href='/action/show' class='btn btn-test btn-orange'>Show Image</a>"
"<a href='/action/clear' class='btn btn-test btn-red'>Clear</a>"
"</div>"
"</div>"
"<div style='text-align: left; color: #888; margin-top: 20px; font-size: 0.8em;'>"
"<p style='margin: 5px 0;'><strong>Save:</strong> Saves the config only.</p>"
"<p style='margin: 5px 0;'><strong>Apply:</strong> Saves the config, shows image and starts the sleep/refresh cycle.</p>"
"<p style='margin: 5px 0;'><strong>Test Pattern:</strong> Shows the test pattern.</p>"
"<p style='margin: 5px 0;'><strong>Show Image:</strong> Downloads the image, applies the saved config and shows the image.</p>"
"<p style='margin: 5px 0;'><strong>Clear:</strong> Clears display to white (should be used before long-term storage).</p>"
"</div>"
"</div>"
"</body>"
"</html>";

// Root GET handler
static esp_err_t root_get_handler(httpd_req_t *req) {
    // Update last client activity timestamp
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Client connected - serving config page");

    // Use static buffer to avoid stack overflow in httpd task
    static char response[6144];

    // Use safe pointers for potentially empty strings
    const char *display_url = (stored_image_url[0] != '\0') ? stored_image_url : "(not configured)";
    const char *form_url = (stored_image_url[0] != '\0') ? stored_image_url : "";

    int len = snprintf(response, sizeof(response), html_page,
             stored_ssid,
             display_url, display_url,  // URL appears twice: href and display text
             (unsigned long)stored_refresh_interval,
             stored_img_width, stored_img_height,
             stored_img_scale ? "yes" : "no",
             stored_img_rotation,
             stored_img_mirror_h ? "yes" : "no",
             stored_img_mirror_v ? "yes" : "no",
             stored_img_rot_first ? "Rot-&gt;Mir" : "Mir-&gt;Rot",
             stored_ssid,
             stored_password,
             form_url,
             (unsigned long)stored_refresh_interval,
             stored_img_width,
             stored_img_height,
             stored_img_scale ? "checked" : "",
             (stored_img_rotation == 0) ? "selected" : "",
             (stored_img_rotation == 90) ? "selected" : "",
             (stored_img_rotation == 180) ? "selected" : "",
             (stored_img_rotation == 270) ? "selected" : "",
             stored_img_mirror_h ? "checked" : "",
             stored_img_mirror_v ? "checked" : "",
             stored_img_rot_first ? "selected" : "",
             stored_img_rot_first ? "" : "selected");

    ESP_LOGI(TAG, "Response length: %d bytes", len);

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
                             bool *img_mirror_v, bool *img_rot_first) {
    char *token;
    char *saveptr;
    char temp_str[16] = {0};
    *img_scale = false;      // Default to false, will be set true if checkbox is present
    *img_mirror_h = false;   // Default to false
    *img_mirror_v = false;   // Default to false
    *img_rot_first = true;   // Default to rotate first

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

    char buf[768];
    char new_ssid[MAX_SSID_LEN] = {0};
    char new_password[MAX_PASSWORD_LEN] = {0};
    char new_url[MAX_URL_LEN] = {0};
    uint32_t new_refresh = 60;
    uint16_t new_img_width = 800;
    uint16_t new_img_height = 480;
    bool new_img_scale = false;
    uint16_t new_img_rotation = 0;
    bool new_img_mirror_h = false;
    bool new_img_mirror_v = false;
    bool new_img_rot_first = true;
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
    parse_post_data(buf, new_ssid, new_password, new_url, &new_refresh,
                    &new_img_width, &new_img_height, &new_img_scale,
                    &new_img_rotation, &new_img_mirror_h, &new_img_mirror_v, &new_img_rot_first);

    ESP_LOGI(TAG, "Received config - SSID: %s, URL: %s, Refresh: %lu min, Rot: %d, MirH: %s, MirV: %s",
             new_ssid, new_url, (unsigned long)new_refresh,
             new_img_rotation, new_img_mirror_h ? "yes" : "no", new_img_mirror_v ? "yes" : "no");

    // Save to NVS
    save_credentials_to_nvs(new_ssid, new_password, new_url, new_refresh,
                            new_img_width, new_img_height, new_img_scale,
                            new_img_rotation, new_img_mirror_h, new_img_mirror_v, new_img_rot_first);

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

// Apply POST handler - saves config and triggers image display + sleep
static esp_err_t apply_post_handler(httpd_req_t *req) {
    last_client_activity = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ESP_LOGI(TAG, "Apply request received");

    char buf[768];
    char new_ssid[MAX_SSID_LEN] = {0};
    char new_password[MAX_PASSWORD_LEN] = {0};
    char new_url[MAX_URL_LEN] = {0};
    uint32_t new_refresh = 60;
    uint16_t new_img_width = 800;
    uint16_t new_img_height = 480;
    bool new_img_scale = false;
    uint16_t new_img_rotation = 0;
    bool new_img_mirror_h = false;
    bool new_img_mirror_v = false;
    bool new_img_rot_first = true;
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
    parse_post_data(buf, new_ssid, new_password, new_url, &new_refresh,
                    &new_img_width, &new_img_height, &new_img_scale,
                    &new_img_rotation, &new_img_mirror_h, &new_img_mirror_v, &new_img_rot_first);

    ESP_LOGI(TAG, "Applying config - SSID: %s, URL: %s", new_ssid, new_url);

    // Save to NVS
    save_credentials_to_nvs(new_ssid, new_password, new_url, new_refresh,
                            new_img_width, new_img_height, new_img_scale,
                            new_img_rotation, new_img_mirror_h, new_img_mirror_v, new_img_rot_first);

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

    // Initialize NVS
    init_nvs();

    // Load configuration from NVS
    load_credentials_from_nvs();

    // Initialize LED
    init_led();

    // Initialize boot button
    init_boot_button();

    // Start LED task
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);

    // Connect to WiFi
    ESP_LOGI(TAG, "Connecting to WiFi: %s", stored_ssid);
    wifi_init_sta();

    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi connection failed! Staying awake with red LED.");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "WiFi connected!");

    // DECISION: Do we need to run the webserver?
    // - If woke from button -> run webserver for reconfiguration
    // - If no image URL configured -> run webserver for initial setup
    bool need_webserver = woke_from_button || (strlen(stored_image_url) == 0);

    if (need_webserver) {
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
        enter_deep_sleep(stored_refresh_interval);
    }

    // Allocate buffer for processed image in PSRAM
    uint8_t *image_buffer = heap_caps_malloc(IMAGE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (image_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate image buffer in PSRAM");
        ESP_LOGI(TAG, "Falling back to color test pattern");
        epd_7in3e_show_color_blocks();
        image_processor_deinit();
        epd_7in3e_sleep();
        enter_deep_sleep(stored_refresh_interval);
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

    // Enter deep sleep for the configured interval
    enter_deep_sleep(stored_refresh_interval);
}