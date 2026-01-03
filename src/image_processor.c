/**
 * @file image_processor.c
 * @brief Image download, PNG decode, and Floyd-Steinberg dither for e-Paper display
 */

#include "image_processor.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "pngle.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "IMG_PROC";

// Error message buffer
static char error_msg[128] = {0};

// RGB pixel buffer for the display (800x480, allocated in PSRAM)
static int16_t *rgb_buffer = NULL;  // Using int16 for dithering error accumulation

// Source image buffer for scaling (allocated dynamically based on source size)
static uint8_t *src_buffer = NULL;
static uint32_t src_buffer_width = 0;
static uint32_t src_buffer_height = 0;

// Scaling settings
static uint16_t cfg_src_width = 0;   // Expected source width (0 = auto)
static uint16_t cfg_src_height = 0;  // Expected source height (0 = auto)
static bool cfg_scale_to_fit = false; // Scale image to fit display

// E-paper 6-color palette (RGB values)
// Black, White, Yellow, Red, Orange, Blue, Green
static const uint8_t palette[7][3] = {
    {0, 0, 0},       // 0: Black
    {255, 255, 255}, // 1: White
    {255, 255, 0},   // 2: Yellow
    {255, 0, 0},     // 3: Red
    {255, 128, 0},   // 4: Orange
    {0, 0, 255},     // 5: Blue
    {0, 255, 0}      // 6: Green
};

// HTTP response buffer
static uint8_t *http_buffer = NULL;
static size_t http_buffer_size = 0;
static size_t http_buffer_pos = 0;

/**
 * @brief Calculate color distance squared (for finding closest palette color)
 */
static inline int32_t color_distance_sq(int16_t r1, int16_t g1, int16_t b1,
                                         uint8_t r2, uint8_t g2, uint8_t b2) {
    int32_t dr = r1 - r2;
    int32_t dg = g1 - g2;
    int32_t db = b1 - b2;
    return dr * dr + dg * dg + db * db;
}

/**
 * @brief Find the closest palette color index for a given RGB color
 */
static uint8_t find_closest_color(int16_t r, int16_t g, int16_t b) {
    // Clamp values to 0-255
    if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
    if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
    if (b < 0) { b = 0; } else if (b > 255) { b = 255; }

    uint8_t best_idx = 0;
    int32_t best_dist = INT32_MAX;

    for (int i = 0; i < 7; i++) {
        int32_t dist = color_distance_sq(r, g, b, palette[i][0], palette[i][1], palette[i][2]);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    return best_idx;
}

/**
 * @brief PNG init callback - called when image header is parsed
 * Allocates source buffer for scaling if needed
 */
static void png_init_callback(pngle_t *pngle, uint32_t w, uint32_t h) {
    ESP_LOGI(TAG, "PNG header: %lux%lu", (unsigned long)w, (unsigned long)h);

    if (cfg_scale_to_fit && (w != IMAGE_WIDTH || h != IMAGE_HEIGHT)) {
        // Allocate source buffer for scaling
        src_buffer_width = w;
        src_buffer_height = h;
        size_t src_size = w * h * 3;
        src_buffer = heap_caps_malloc(src_size, MALLOC_CAP_SPIRAM);
        if (src_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate source buffer (%d bytes)", (int)src_size);
            // Fall back to direct mode (will crop)
            src_buffer_width = 0;
            src_buffer_height = 0;
        } else {
            memset(src_buffer, 255, src_size);  // White background
            ESP_LOGI(TAG, "Allocated source buffer for scaling (%d bytes)", (int)src_size);
        }
    }
}

/**
 * @brief PNG decode callback - called for each pixel
 * When scaling is enabled, stores to src_buffer; otherwise directly to rgb_buffer
 */
static void png_draw_callback(pngle_t *pngle, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    if (cfg_scale_to_fit && src_buffer != NULL) {
        // Store in source buffer for later scaling
        if (x < src_buffer_width && y < src_buffer_height) {
            uint32_t idx = (y * src_buffer_width + x) * 3;
            src_buffer[idx + 0] = rgba[0];
            src_buffer[idx + 1] = rgba[1];
            src_buffer[idx + 2] = rgba[2];
        }
    } else {
        // Direct mode: store in rgb_buffer (crop if larger)
        if (x >= IMAGE_WIDTH || y >= IMAGE_HEIGHT) return;
        if (rgb_buffer == NULL) return;

        uint32_t idx = (y * IMAGE_WIDTH + x) * 3;
        rgb_buffer[idx + 0] = rgba[0];
        rgb_buffer[idx + 1] = rgba[1];
        rgb_buffer[idx + 2] = rgba[2];
    }
}

/**
 * @brief Scale source image to display size using bilinear interpolation
 */
static void scale_image_to_display(void) {
    if (src_buffer == NULL || rgb_buffer == NULL) return;
    if (src_buffer_width == 0 || src_buffer_height == 0) return;

    ESP_LOGI(TAG, "Scaling image from %lux%lu to %dx%d",
             (unsigned long)src_buffer_width, (unsigned long)src_buffer_height,
             IMAGE_WIDTH, IMAGE_HEIGHT);

    float x_ratio = (float)src_buffer_width / IMAGE_WIDTH;
    float y_ratio = (float)src_buffer_height / IMAGE_HEIGHT;

    for (uint32_t dst_y = 0; dst_y < IMAGE_HEIGHT; dst_y++) {
        for (uint32_t dst_x = 0; dst_x < IMAGE_WIDTH; dst_x++) {
            // Calculate source position
            float src_x = dst_x * x_ratio;
            float src_y = dst_y * y_ratio;

            // Get integer and fractional parts
            uint32_t x0 = (uint32_t)src_x;
            uint32_t y0 = (uint32_t)src_y;
            uint32_t x1 = (x0 + 1 < src_buffer_width) ? x0 + 1 : x0;
            uint32_t y1 = (y0 + 1 < src_buffer_height) ? y0 + 1 : y0;
            float x_frac = src_x - x0;
            float y_frac = src_y - y0;

            // Get four surrounding pixels
            uint32_t idx00 = (y0 * src_buffer_width + x0) * 3;
            uint32_t idx01 = (y0 * src_buffer_width + x1) * 3;
            uint32_t idx10 = (y1 * src_buffer_width + x0) * 3;
            uint32_t idx11 = (y1 * src_buffer_width + x1) * 3;

            // Bilinear interpolation for each channel
            for (int c = 0; c < 3; c++) {
                float top = src_buffer[idx00 + c] * (1 - x_frac) + src_buffer[idx01 + c] * x_frac;
                float bot = src_buffer[idx10 + c] * (1 - x_frac) + src_buffer[idx11 + c] * x_frac;
                float val = top * (1 - y_frac) + bot * y_frac;

                uint32_t dst_idx = (dst_y * IMAGE_WIDTH + dst_x) * 3;
                rgb_buffer[dst_idx + c] = (int16_t)(val + 0.5f);
            }
        }
    }

    ESP_LOGI(TAG, "Scaling complete");
}

/**
 * @brief Apply Floyd-Steinberg dithering and convert to e-paper format
 */
static void apply_dithering(uint8_t *output_buffer) {
    ESP_LOGI(TAG, "Applying Floyd-Steinberg dithering...");

    for (uint32_t y = 0; y < IMAGE_HEIGHT; y++) {
        for (uint32_t x = 0; x < IMAGE_WIDTH; x++) {
            uint32_t idx = (y * IMAGE_WIDTH + x) * 3;

            // Get current pixel color (with accumulated error)
            int16_t old_r = rgb_buffer[idx + 0];
            int16_t old_g = rgb_buffer[idx + 1];
            int16_t old_b = rgb_buffer[idx + 2];

            // Find closest palette color
            uint8_t color_idx = find_closest_color(old_r, old_g, old_b);

            // Calculate quantization error
            int16_t err_r = old_r - palette[color_idx][0];
            int16_t err_g = old_g - palette[color_idx][1];
            int16_t err_b = old_b - palette[color_idx][2];

            // Distribute error to neighboring pixels (Floyd-Steinberg coefficients)
            // Right pixel: 7/16
            if (x + 1 < IMAGE_WIDTH) {
                uint32_t nidx = idx + 3;
                rgb_buffer[nidx + 0] += (err_r * 7) / 16;
                rgb_buffer[nidx + 1] += (err_g * 7) / 16;
                rgb_buffer[nidx + 2] += (err_b * 7) / 16;
            }
            // Bottom-left pixel: 3/16
            if (y + 1 < IMAGE_HEIGHT && x > 0) {
                uint32_t nidx = ((y + 1) * IMAGE_WIDTH + (x - 1)) * 3;
                rgb_buffer[nidx + 0] += (err_r * 3) / 16;
                rgb_buffer[nidx + 1] += (err_g * 3) / 16;
                rgb_buffer[nidx + 2] += (err_b * 3) / 16;
            }
            // Bottom pixel: 5/16
            if (y + 1 < IMAGE_HEIGHT) {
                uint32_t nidx = ((y + 1) * IMAGE_WIDTH + x) * 3;
                rgb_buffer[nidx + 0] += (err_r * 5) / 16;
                rgb_buffer[nidx + 1] += (err_g * 5) / 16;
                rgb_buffer[nidx + 2] += (err_b * 5) / 16;
            }
            // Bottom-right pixel: 1/16
            if (y + 1 < IMAGE_HEIGHT && x + 1 < IMAGE_WIDTH) {
                uint32_t nidx = ((y + 1) * IMAGE_WIDTH + (x + 1)) * 3;
                rgb_buffer[nidx + 0] += (err_r * 1) / 16;
                rgb_buffer[nidx + 1] += (err_g * 1) / 16;
                rgb_buffer[nidx + 2] += (err_b * 1) / 16;
            }

            // Pack into output buffer (2 pixels per byte)
            uint32_t out_idx = (y * IMAGE_WIDTH + x) / 2;
            if ((x & 1) == 0) {
                output_buffer[out_idx] = (color_idx << 4);
            } else {
                output_buffer[out_idx] |= color_idx;
            }
        }

        // Yield periodically to prevent watchdog timeout
        if ((y % 50) == 0) {
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "Dithering complete");
}

/**
 * @brief HTTP event handler for downloading image data
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_buffer && http_buffer_pos + evt->data_len <= http_buffer_size) {
                memcpy(http_buffer + http_buffer_pos, evt->data, evt->data_len);
                http_buffer_pos += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t image_processor_init(void) {
    ESP_LOGI(TAG, "Initializing image processor");

    // Allocate RGB buffer in PSRAM (800x480x3 = 1,152,000 bytes as int16 = 2,304,000 bytes)
    rgb_buffer = heap_caps_malloc(IMAGE_WIDTH * IMAGE_HEIGHT * 3 * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM);
    if (rgb_buffer == NULL) {
        snprintf(error_msg, sizeof(error_msg), "Failed to allocate RGB buffer in PSRAM");
        ESP_LOGE(TAG, "%s", error_msg);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Image processor initialized (RGB buffer: %d bytes in PSRAM)",
             IMAGE_WIDTH * IMAGE_HEIGHT * 3 * (int)sizeof(int16_t));
    return ESP_OK;
}

void image_processor_set_scaling(uint16_t src_width, uint16_t src_height, bool scale_to_fit) {
    cfg_src_width = src_width;
    cfg_src_height = src_height;
    cfg_scale_to_fit = scale_to_fit;
    ESP_LOGI(TAG, "Scaling config: src=%dx%d, scale_to_fit=%s",
             src_width, src_height, scale_to_fit ? "yes" : "no");
}

esp_err_t image_download_and_process(const char *url, uint8_t *output_buffer) {
    esp_err_t ret = ESP_OK;
    pngle_t *pngle = NULL;

    if (url == NULL || output_buffer == NULL) {
        snprintf(error_msg, sizeof(error_msg), "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    if (rgb_buffer == NULL) {
        snprintf(error_msg, sizeof(error_msg), "Image processor not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Downloading image from: %s", url);

    // Clear RGB buffer
    memset(rgb_buffer, 0, IMAGE_WIDTH * IMAGE_HEIGHT * 3 * sizeof(int16_t));
    memset(output_buffer, 0, IMAGE_BUFFER_SIZE);

    // Allocate HTTP buffer (max 2MB for PNG)
    http_buffer_size = 2 * 1024 * 1024;
    http_buffer = heap_caps_malloc(http_buffer_size, MALLOC_CAP_SPIRAM);
    if (http_buffer == NULL) {
        snprintf(error_msg, sizeof(error_msg), "Failed to allocate HTTP buffer");
        ESP_LOGE(TAG, "%s", error_msg);
        return ESP_ERR_NO_MEM;
    }
    http_buffer_pos = 0;

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(error_msg, sizeof(error_msg), "Failed to initialize HTTP client");
        ESP_LOGE(TAG, "%s", error_msg);
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Perform HTTP request
    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        snprintf(error_msg, sizeof(error_msg), "HTTP request failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "%s", error_msg);
        goto cleanup;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        snprintf(error_msg, sizeof(error_msg), "HTTP error: %d", status_code);
        ESP_LOGE(TAG, "%s", error_msg);
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes, decoding PNG...", (int)http_buffer_pos);

    // Reset source buffer state
    if (src_buffer) {
        heap_caps_free(src_buffer);
        src_buffer = NULL;
    }
    src_buffer_width = 0;
    src_buffer_height = 0;

    // Initialize PNG decoder
    pngle = pngle_new();
    if (pngle == NULL) {
        snprintf(error_msg, sizeof(error_msg), "Failed to create PNG decoder");
        ESP_LOGE(TAG, "%s", error_msg);
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    pngle_set_init_callback(pngle, png_init_callback);
    pngle_set_draw_callback(pngle, png_draw_callback);

    // Feed PNG data to decoder
    int fed = pngle_feed(pngle, http_buffer, http_buffer_pos);
    if (fed < 0) {
        snprintf(error_msg, sizeof(error_msg), "PNG decode error: %s", pngle_error(pngle));
        ESP_LOGE(TAG, "%s", error_msg);
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Check image dimensions
    uint32_t png_width = pngle_get_width(pngle);
    uint32_t png_height = pngle_get_height(pngle);
    ESP_LOGI(TAG, "PNG dimensions: %dx%d", (int)png_width, (int)png_height);

    // If scaling was used, scale to display size now
    if (cfg_scale_to_fit && src_buffer != NULL) {
        scale_image_to_display();
    } else if (png_width != IMAGE_WIDTH || png_height != IMAGE_HEIGHT) {
        ESP_LOGW(TAG, "Image size mismatch (expected %dx%d), image was cropped/padded",
                 IMAGE_WIDTH, IMAGE_HEIGHT);
    }

    // Apply dithering and convert to e-paper format
    apply_dithering(output_buffer);

    ESP_LOGI(TAG, "Image processing complete");

cleanup:
    if (pngle) pngle_destroy(pngle);
    if (client) esp_http_client_cleanup(client);
    if (http_buffer) {
        heap_caps_free(http_buffer);
        http_buffer = NULL;
    }
    if (src_buffer) {
        heap_caps_free(src_buffer);
        src_buffer = NULL;
        src_buffer_width = 0;
        src_buffer_height = 0;
    }

    return ret;
}

const char* image_processor_get_error(void) {
    return error_msg;
}

void image_processor_deinit(void) {
    if (rgb_buffer) {
        heap_caps_free(rgb_buffer);
        rgb_buffer = NULL;
    }
    ESP_LOGI(TAG, "Image processor deinitialized");
}
