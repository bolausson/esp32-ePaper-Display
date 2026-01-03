/**
 * @file epd_7in3e.h
 * @brief Waveshare 7.3inch e-Paper (E) Spectra 6 Driver for ESP32-S3
 * 
 * Based on Waveshare demo code, ported to ESP-IDF
 * Display: 800x480 pixels, 6 colors (Black, White, Yellow, Red, Blue, Green)
 */

#ifndef EPD_7IN3E_H
#define EPD_7IN3E_H

#include <stdint.h>
#include "esp_err.h"

// Display resolution
#define EPD_7IN3E_WIDTH       800
#define EPD_7IN3E_HEIGHT      480

// Color definitions (4 bits per pixel, 2 pixels per byte)
#define EPD_7IN3E_BLACK   0x0
#define EPD_7IN3E_WHITE   0x1
#define EPD_7IN3E_YELLOW  0x2
#define EPD_7IN3E_RED     0x3
#define EPD_7IN3E_BLUE    0x5
#define EPD_7IN3E_GREEN   0x6

// Pin configuration - adjust these to match your wiring
#define EPD_PIN_MOSI      11
#define EPD_PIN_CLK       12
#define EPD_PIN_CS        10
#define EPD_PIN_DC        9
#define EPD_PIN_RST       8
#define EPD_PIN_BUSY      7

// SPI configuration
#define EPD_SPI_HOST      SPI2_HOST
#define EPD_SPI_SPEED_HZ  4000000  // 4 MHz

/**
 * @brief Initialize the e-Paper display hardware (SPI and GPIO)
 * @return ESP_OK on success
 */
esp_err_t epd_7in3e_init_hw(void);

/**
 * @brief Initialize the e-Paper display controller
 */
void epd_7in3e_init(void);

/**
 * @brief Clear the display with a single color
 * @param color Color to fill (use EPD_7IN3E_* constants)
 */
void epd_7in3e_clear(uint8_t color);

/**
 * @brief Display image from buffer
 * @param image Pointer to image buffer (800x480/2 = 192000 bytes)
 *              Each byte contains 2 pixels (4 bits each)
 */
void epd_7in3e_display(const uint8_t *image);

/**
 * @brief Display a test pattern showing all 6 colors
 */
void epd_7in3e_show_color_blocks(void);

/**
 * @brief Put the display into deep sleep mode
 */
void epd_7in3e_sleep(void);

/**
 * @brief Deinitialize hardware (free SPI bus)
 */
void epd_7in3e_deinit_hw(void);

#endif // EPD_7IN3E_H

