# Quick Start Guide

## 1. Edit WiFi Credentials

Open `include/wifi_config.h` and edit:

```c
#define DEFAULT_WIFI_SSID "YourWiFiName"
#define DEFAULT_WIFI_PASSWORD "YourWiFiPassword"
```

## 2. Build and Upload

In VS Code with PlatformIO:

1. Click the PlatformIO icon in the left sidebar
2. Click "Build" (checkmark icon)
3. Click "Upload" (arrow icon)
4. Click "Monitor" (plug icon) to see serial output

Or use terminal:
```bash
pio run --target upload && pio device monitor
```

## 3. First Boot

The device will:
- Connect to WiFi using default credentials
- Show green LED (connected)
- Enter deep sleep after 5 seconds

## 4. Configure via Web Interface

1. Press and HOLD the BOOT button
2. Press RESET button (or reconnect power)
3. Release BOOT button
4. LED blinks YELLOW = web server mode
5. Check serial monitor for IP address (e.g., "Got IP address: 192.168.1.100")
6. Open browser: `http://192.168.1.100`
7. Enter new WiFi credentials and image URL
8. Click "Save and Sleep"
9. LED blinks BLUE for 3 seconds
10. Device enters deep sleep

## 5. Wake from Sleep

Press BOOT button to wake up

## LED Status Guide

| Color | Pattern | Meaning |
|-------|---------|---------|
| 🔴 Red | Blinking | Not connected to WiFi |
| 🟢 Green | Solid | Connected to WiFi |
| 🟡 Yellow | Blinking | Web server running |
| 🔵 Blue | Blinking | Entering deep sleep (3 sec) |

## Serial Monitor Commands

Baud rate: **115200**

Watch for:
- WiFi connection status
- IP address assignment
- Web server start/stop
- Configuration changes
- Sleep/wake events

## Troubleshooting

**Can't connect to WiFi?**
- Check SSID and password in `include/wifi_config.h`
- Ensure WiFi is 2.4GHz (not 5GHz)

**Web server not accessible?**
- Make sure BOOT button was pressed during startup
- Check serial monitor for IP address
- Verify you're on the same WiFi network

**LED not working?**
- Check that LED48 is on GPIO 48
- Verify WS2812 LED has power

**Device won't wake?**
- Ensure BOOT button is on GPIO 0
- Button should connect GPIO 0 to GND when pressed

