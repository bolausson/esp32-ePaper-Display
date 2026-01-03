/**
 * @file epd_7in3e.c
 * @brief Waveshare 7.3inch e-Paper (E) Spectra 6 Driver for ESP32-S3
 */

#include "epd_7in3e.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "EPD_7IN3E";

// Yield to other tasks periodically to prevent watchdog timeout
// Call this every N iterations during long loops
// Using vTaskDelay(1) ensures proper watchdog feeding
#define EPD_YIELD_INTERVAL 4000
static inline void epd_yield_if_needed(uint32_t counter) {
    if ((counter % EPD_YIELD_INTERVAL) == 0) {
        vTaskDelay(1);  // Minimum delay to feed watchdog and yield to other tasks
    }
}

static spi_device_handle_t spi_handle = NULL;

// Delay helper
static void epd_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// GPIO write helper
static void epd_gpio_write(gpio_num_t pin, uint32_t level) {
    gpio_set_level(pin, level);
}

// GPIO read helper
static int epd_gpio_read(gpio_num_t pin) {
    return gpio_get_level(pin);
}

// SPI write byte
static void epd_spi_write_byte(uint8_t data) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
    };
    spi_device_transmit(spi_handle, &t);
}

// Hardware reset
static void epd_reset(void) {
    epd_gpio_write(EPD_PIN_RST, 1);
    epd_delay_ms(20);
    epd_gpio_write(EPD_PIN_RST, 0);
    epd_delay_ms(2);
    epd_gpio_write(EPD_PIN_RST, 1);
    epd_delay_ms(20);
}

// Send command
static void epd_send_command(uint8_t cmd) {
    epd_gpio_write(EPD_PIN_DC, 0);
    epd_gpio_write(EPD_PIN_CS, 0);
    epd_spi_write_byte(cmd);
    epd_gpio_write(EPD_PIN_CS, 1);
}

// Send data byte
static void epd_send_data(uint8_t data) {
    epd_gpio_write(EPD_PIN_DC, 1);
    epd_gpio_write(EPD_PIN_CS, 0);
    epd_spi_write_byte(data);
    epd_gpio_write(EPD_PIN_CS, 1);
}

// Wait for busy pin to go HIGH (idle)
static void epd_wait_busy(void) {
    ESP_LOGI(TAG, "Waiting for display...");
    while (epd_gpio_read(EPD_PIN_BUSY) == 0) {
        epd_delay_ms(10);
    }
    ESP_LOGI(TAG, "Display ready");
}

// Turn on display (refresh)
static void epd_turn_on_display(void) {
    epd_send_command(0x04);  // POWER_ON
    epd_wait_busy();

    // Second setting
    epd_send_command(0x06);
    epd_send_data(0x6F);
    epd_send_data(0x1F);
    epd_send_data(0x17);
    epd_send_data(0x49);

    epd_send_command(0x12);  // DISPLAY_REFRESH
    epd_send_data(0x00);
    epd_wait_busy();

    epd_send_command(0x02);  // POWER_OFF
    epd_send_data(0x00);
    epd_wait_busy();
}

esp_err_t epd_7in3e_init_hw(void) {
    ESP_LOGI(TAG, "Initializing e-Paper hardware...");

    // Check if already initialized
    if (spi_handle != NULL) {
        ESP_LOGI(TAG, "e-Paper hardware already initialized");
        return ESP_OK;
    }

    // Configure GPIO pins
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Configure BUSY pin as input
    io_conf.pin_bit_mask = (1ULL << EPD_PIN_BUSY);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Set initial states
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_RST, 1);

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,  // Not used
        .sclk_io_num = EPD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure SPI device
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_SPEED_HZ,
        .mode = 0,  // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = -1,  // We control CS manually
        .queue_size = 1,
    };

    ret = spi_bus_add_device(EPD_SPI_HOST, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "e-Paper hardware initialized");
    return ESP_OK;
}

void epd_7in3e_init(void) {
    ESP_LOGI(TAG, "Initializing e-Paper display controller...");

    epd_reset();
    epd_wait_busy();
    epd_delay_ms(30);

    epd_send_command(0xAA);  // CMDH
    epd_send_data(0x49);
    epd_send_data(0x55);
    epd_send_data(0x20);
    epd_send_data(0x08);
    epd_send_data(0x09);
    epd_send_data(0x18);

    epd_send_command(0x01);
    epd_send_data(0x3F);

    epd_send_command(0x00);
    epd_send_data(0x5F);
    epd_send_data(0x69);

    epd_send_command(0x03);
    epd_send_data(0x00);
    epd_send_data(0x54);
    epd_send_data(0x00);
    epd_send_data(0x44);

    epd_send_command(0x05);
    epd_send_data(0x40);
    epd_send_data(0x1F);
    epd_send_data(0x1F);
    epd_send_data(0x2C);

    epd_send_command(0x06);
    epd_send_data(0x6F);
    epd_send_data(0x1F);
    epd_send_data(0x17);
    epd_send_data(0x49);

    epd_send_command(0x08);
    epd_send_data(0x6F);
    epd_send_data(0x1F);
    epd_send_data(0x1F);
    epd_send_data(0x22);

    epd_send_command(0x30);
    epd_send_data(0x03);

    epd_send_command(0x50);
    epd_send_data(0x3F);

    epd_send_command(0x60);
    epd_send_data(0x02);
    epd_send_data(0x00);

    epd_send_command(0x61);  // Resolution setting
    epd_send_data(0x03);     // 800 = 0x320
    epd_send_data(0x20);
    epd_send_data(0x01);     // 480 = 0x1E0
    epd_send_data(0xE0);

    epd_send_command(0x84);
    epd_send_data(0x01);

    epd_send_command(0xE3);
    epd_send_data(0x2F);

    epd_send_command(0x04);  // Power on
    epd_wait_busy();

    ESP_LOGI(TAG, "e-Paper display controller initialized");
}

void epd_7in3e_clear(uint8_t color) {
    ESP_LOGI(TAG, "Clearing display with color 0x%X...", color);

    uint16_t width = EPD_7IN3E_WIDTH / 2;  // 2 pixels per byte
    uint16_t height = EPD_7IN3E_HEIGHT;
    uint8_t data = (color << 4) | color;

    epd_send_command(0x10);  // Data start
    for (uint32_t i = 0; i < (uint32_t)width * height; i++) {
        epd_send_data(data);
        epd_yield_if_needed(i);
    }

    epd_turn_on_display();
    ESP_LOGI(TAG, "Display cleared");
}

void epd_7in3e_display(const uint8_t *image) {
    if (image == NULL) {
        ESP_LOGE(TAG, "Image buffer is NULL");
        return;
    }

    ESP_LOGI(TAG, "Displaying image...");

    uint16_t width = EPD_7IN3E_WIDTH / 2;
    uint16_t height = EPD_7IN3E_HEIGHT;

    epd_send_command(0x10);
    for (uint32_t i = 0; i < (uint32_t)width * height; i++) {
        epd_send_data(image[i]);
        epd_yield_if_needed(i);
    }

    epd_turn_on_display();
    ESP_LOGI(TAG, "Image displayed");
}

void epd_7in3e_show_color_blocks(void) {
    ESP_LOGI(TAG, "Showing color test blocks...");

    const uint8_t colors[6] = {
        EPD_7IN3E_BLACK, EPD_7IN3E_WHITE, EPD_7IN3E_YELLOW,
        EPD_7IN3E_RED, EPD_7IN3E_BLUE, EPD_7IN3E_GREEN
    };

    epd_send_command(0x10);

    // Each color block: 800/2 * 480/6 = 400 * 80 = 32000 bytes
    uint32_t counter = 0;
    for (int c = 0; c < 6; c++) {
        uint8_t data = (colors[c] << 4) | colors[c];
        for (uint32_t i = 0; i < 32000; i++) {
            epd_send_data(data);
            epd_yield_if_needed(counter++);
        }
    }

    epd_turn_on_display();
    ESP_LOGI(TAG, "Color blocks displayed");
}

void epd_7in3e_sleep(void) {
    ESP_LOGI(TAG, "Putting display to sleep...");

    epd_send_command(0x02);  // Power off
    epd_send_data(0x00);
    epd_wait_busy();

    epd_send_command(0x07);  // Deep sleep
    epd_send_data(0xA5);

    ESP_LOGI(TAG, "Display in sleep mode");
}

void epd_7in3e_deinit_hw(void) {
    if (spi_handle != NULL) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }
    spi_bus_free(EPD_SPI_HOST);
    ESP_LOGI(TAG, "e-Paper hardware deinitialized");
}

