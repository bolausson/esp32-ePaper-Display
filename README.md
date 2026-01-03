# ESP32-S3 E-Paper Display Controller

WiFi-enabled firmware for driving a 7-color e-paper display from an ESP32-S3. Downloads PNG images from any URL and renders them with Floyd-Steinberg dithering for optimal color reproduction. Perfect for dashboards, weather displays, or digital signage.

![ESP32-S3 ePaper Display](images/ESP32-S3-ePaper.jpg)

## Features

- 📥 **Image Download** - Fetches PNG images from any HTTP/HTTPS URL
- 🎨 **7-Color Dithering** - Floyd-Steinberg dithering for Black, White, Red, Yellow, Orange, Blue, Green
- 🔄 **Auto Scaling** - Bilinear interpolation to scale images to display size
- 🌐 **Web Configuration** - Browser-based setup portal for WiFi and image settings
- 😴 **Deep Sleep** - Configurable refresh interval with ultra-low power sleep
- 💡 **LED Status** - RGB LED feedback for connection and operation status
- 🔘 **Setup Mode** - Hold boot button during startup to enter configuration mode

## Hardware

| Component | Model |
|-----------|-------|
| Microcontroller | ESP32-S3-WROOM with 8MB PSRAM |
| Display | Waveshare 7.3inch e-Paper (F) - 800×480, 7-color |
| LED | WS2812 RGB (onboard or external) |

### Pin Connections

| E-Paper Pin | ESP32-S3 GPIO | Notes |
|-------------|---------------|-------|
| VCC | 3.3V | Power supply |
| GND | GND | Ground |
| DIN (MOSI) | GPIO 11 | SPI2 default MOSI |
| CLK (SCK) | GPIO 12 | SPI2 default SCK |
| CS | GPIO 10 | SPI2 default CS |
| DC | GPIO 9 | Data/Command control |
| RST | GPIO 8 | Reset |
| BUSY | GPIO 7 | Busy status input |

| Other | GPIO | Notes |
|-------|------|-------|
| WS2812 LED | GPIO 48 | Onboard RGB LED |
| Boot Button | GPIO 0 | Setup mode trigger |

> **Pin Selection Rationale:** GPIO 11, 12, 10 are the default SPI2 (HSPI) pins on ESP32-S3. GPIO 7, 8, 9 are general-purpose GPIOs that are safe to use (not strapping pins). This avoids GPIO 0 (boot button), GPIO 48 (LED), and USB pins (19, 20).

### Power Consumption

| State | Power |
|-------|-------|
| Active (downloading & displaying) | < 1 W |
| Deep Sleep | ~0.05 W |

**Example:** Refreshing the display every minute for 10 minutes consumes approximately 0.04 Wh (0.008 Ah) — ideal for battery-powered or solar applications.

## Software Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- ESP-IDF framework (automatically installed by PlatformIO)

## Installation

1. Clone the repository:
   ```bash
   git clone https://github.com/bolausson/esp32-ePaper-Display.git
   cd esp32-ePaper-Display
   ```

2. Create your WiFi configuration:
   ```bash
   cp include/wifi_config.h.example include/wifi_config.h
   ```
   Edit `include/wifi_config.h` with your WiFi credentials (optional - can also configure via web interface).

3. Build and upload:
   ```bash
   pio run --target upload
   ```

4. Monitor serial output (optional):
   ```bash
   pio device monitor
   ```

## Usage

### First-Time Setup

1. Power on the device while holding the **Boot button**
2. The LED will blink yellow indicating web server mode
3. Connect to WiFi - the device connects to your configured network
4. Open `http://<device-ip>/` in a browser
5. Configure:
   - WiFi credentials
   - Image URL (PNG, ideally 800×480)
   - Refresh interval (minutes)
   - Image scaling options
6. Click **Save Configuration**

### Normal Operation

1. Device wakes from deep sleep
2. Connects to WiFi
3. Downloads and processes the image
4. Updates the e-paper display
5. Returns to deep sleep for the configured interval

### Re-entering Setup Mode

Hold the **Boot button** while pressing **Reset**, or during wake-up from deep sleep.

## LED Status Indicators

| Color | Pattern | Meaning |
|-------|---------|---------|
| Yellow | Blinking | Web server running |
| Red | Blinking | WiFi not connected |
| Green | Solid | WiFi connected |
| Blue | Solid | Downloading image |
| Cyan | Solid | Updating display |
| Blue | 3 blinks | Entering deep sleep |

## Image Requirements

- **Format:** PNG
- **Recommended size:** 800×480 pixels
- **Scaling:** Enable "Scale to fit" for other sizes
- **Colors:** Best results with the 7-color palette

### Example: Grafana Dashboard

This project works great with Grafana's image rendering:
```
https://your-grafana-server/render/d/dashboard-id/dashboard-name?width=800&height=480&theme=light
```

## Project Structure

```
├── src/
│   ├── main.c              # Main application
│   ├── epd_7in3e.c         # E-paper display driver
│   └── image_processor.c   # PNG decode, scale, dither
├── include/
│   ├── epd_7in3e.h
│   └── image_processor.h
├── lib/
│   └── pngle/              # PNG decoder library
├── platformio.ini          # PlatformIO configuration
└── partitions_singleapp_large.csv
```

## Configuration Options

| Setting | Description | Default |
|---------|-------------|---------|
| WiFi SSID | Your WiFi network name | - |
| WiFi Password | Your WiFi password | - |
| Image URL | URL to PNG image | - |
| Refresh Interval | Minutes between updates | 60 |
| Image Width | Expected source image width | 800 |
| Image Height | Expected source image height | 480 |
| Scale to Fit | Scale image to 800×480 | No |

## Troubleshooting

### WiFi connection fails
- Verify WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)
- Check serial monitor for error messages
- Re-enter setup mode to update credentials

### Image not displaying correctly
- Ensure image URL is accessible from the device's network
- Check that the URL returns a valid PNG image
- For HTTPS URLs, the server must have a valid certificate

### Device doesn't wake from sleep
- Press the Boot button while the device is sleeping
- The boot button also triggers wake-up from deep sleep

## License

This project is licensed under the GNU General Public License v3.0 - See [LICENSE](LICENSE) for details.

## Acknowledgments

- [Waveshare](https://www.waveshare.com/) for the e-paper display
- [pngle](https://github.com/kikuchan/pngle) - Lightweight PNG decoder
- [ESP-IDF](https://github.com/espressif/esp-idf) - Espressif IoT Development Framework

