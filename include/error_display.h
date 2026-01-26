/**
 * @file error_display.h
 * @brief Error message display for e-Paper using bitmap fonts
 * 
 * Provides functions to render user-friendly error messages on the
 * e-paper display when image download/processing fails.
 */

#ifndef ERROR_DISPLAY_H
#define ERROR_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"

// Error types for categorizing failures
typedef enum {
    ERROR_TYPE_INIT,        // Initialization failure (memory, hardware)
    ERROR_TYPE_NETWORK,     // Network/connectivity issues
    ERROR_TYPE_HTTP,        // HTTP errors (404, 500, etc.)
    ERROR_TYPE_IMAGE,       // Image decode/processing errors
    ERROR_TYPE_UNKNOWN      // Unknown/other errors
} error_type_t;

/**
 * @brief Render an error screen to the display buffer
 * 
 * Creates a user-friendly error screen with:
 * - Error type icon/header
 * - Human-readable error message
 * - Technical details (from image_processor_get_error)
 * - Suggestion for resolution
 * 
 * @param buffer Output buffer (must be IMAGE_BUFFER_SIZE bytes, 192000)
 * @param error_type Category of error
 * @param error_detail Technical error message (can be NULL)
 */
void error_display_render(uint8_t *buffer, error_type_t error_type, const char *error_detail);

/**
 * @brief Display an error screen directly on the e-paper
 * 
 * Allocates a buffer, renders the error, displays it, and frees the buffer.
 * Use this when you don't have an existing buffer allocated.
 * 
 * @param error_type Category of error
 * @param error_detail Technical error message (can be NULL)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer allocation fails
 */
esp_err_t error_display_show(error_type_t error_type, const char *error_detail);

/**
 * @brief Determine error type from error message string
 * 
 * Analyzes the error message to categorize it appropriately.
 * 
 * @param error_msg Error message string
 * @return Categorized error type
 */
error_type_t error_display_categorize(const char *error_msg);

#endif // ERROR_DISPLAY_H

