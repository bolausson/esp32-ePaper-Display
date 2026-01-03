/**
 * @file image_processor.h
 * @brief Image download, decode, and dither for e-Paper display
 */

#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// E-paper display dimensions
#define IMAGE_WIDTH  800
#define IMAGE_HEIGHT 480

// Image buffer size (2 pixels per byte for 6-color palette)
#define IMAGE_BUFFER_SIZE (IMAGE_WIDTH * IMAGE_HEIGHT / 2)

/**
 * @brief Initialize the image processor
 * @return ESP_OK on success
 */
esp_err_t image_processor_init(void);

/**
 * @brief Set scaling parameters for image processing
 * @param src_width Expected source image width (0 = auto-detect)
 * @param src_height Expected source image height (0 = auto-detect)
 * @param scale_to_fit If true, scale image to fit 800x480 display
 */
void image_processor_set_scaling(uint16_t src_width, uint16_t src_height, bool scale_to_fit);

/**
 * @brief Download and process an image from URL
 * @param url The URL to download the image from
 * @param output_buffer Buffer to store the processed image (must be IMAGE_BUFFER_SIZE bytes)
 * @return ESP_OK on success
 */
esp_err_t image_download_and_process(const char *url, uint8_t *output_buffer);

/**
 * @brief Get the last error message
 * @return Pointer to error message string
 */
const char* image_processor_get_error(void);

/**
 * @brief Free any allocated resources
 */
void image_processor_deinit(void);

#endif // IMAGE_PROCESSOR_H

