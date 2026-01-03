# Quick Reference - ESP32-S3 Display

## 🚀 Quick Commands

### Build
```bash
/c/Users/conta/.platformio/penv/Scripts/platformio.exe run
```

### Upload
```bash
/c/Users/conta/.platformio/penv/Scripts/platformio.exe run --target upload
```

### Monitor
```bash
/c/Users/conta/.platformio/penv/Scripts/platformio.exe device monitor
```

### Build + Upload + Monitor (All-in-One)
```bash
/c/Users/conta/.platformio/penv/Scripts/platformio.exe run --target upload --target monitor
```

---

## 📝 Configuration Files

### WiFi Credentials
**File:** `include/wifi_config.h`
```c
#define DEFAULT_WIFI_SSID "YourNetworkName"
#define DEFAULT_WIFI_PASSWORD "YourPassword"
```

### GPIO Pins
**File:** `include/config.h`
```c
#define LED48_GPIO 48        // WS2812 RGB LED
#define BOOT_BUTTON_GPIO 0   // Boot button
```

---

## 🔄 Operation Modes

### Normal Mode (Default)
1. Power on device (don't press BOOT button)
2. Device loads credentials from NVS
3. Connects to WiFi
4. Downloads image (future feature)
5. Updates e-paper display (future feature)
6. Enters deep sleep after 5 seconds

### Configuration Mode (Web Server)
1. **Press and HOLD** BOOT button
2. **Press** RESET button (or power on)
3. **Release** BOOT button
4. Device starts web server
5. Check serial monitor for IP address
6. Open browser to `http://[IP_ADDRESS]`
7. Configure WiFi and image URL
8. Click "Save and Sleep"

---

## 🔍 Serial Monitor Output

### Normal Boot
```
=== ESP32-S3 Display Starting ===
Not a deep sleep wakeup (normal boot)
NVS initialized
Loaded credentials - SSID: YourSSID, URL: YourURL
LED initialized on GPIO 48
Boot button initialized on GPIO 0
Boot button not pressed - normal operation mode
```

### Configuration Mode
```
=== ESP32-S3 Display Starting ===
Wakeup caused by boot button
Boot button pressed - entering webserver mode
WiFi initialization finished. Connecting to SSID: YourSSID
Got IP address: 192.168.1.100
Web server started successfully
```

### WiFi Connected
```
I (xxx) ESP32-S3-Display: LED: GREEN (WiFi connected)
I (xxx) ESP32-S3-Display: Got IP address: 192.168.1.100
```

### WiFi Disconnected
```
I (xxx) ESP32-S3-Display: LED: RED (WiFi disconnected)
```

---

## 🎨 LED Status Indicators

| Color | Meaning | When |
|-------|---------|------|
| 🔴 **RED** (blinking) | WiFi disconnected | Trying to connect |
| 🟢 **GREEN** (solid) | WiFi connected | Successfully connected |
| 🟡 **YELLOW** (blinking) | Web server mode | Configuration mode active |
| 🔵 **BLUE** (blinking 3s) | Entering sleep | About to sleep |

*Note: Currently LED colors are shown via serial output only. Full WS2812 RGB control to be implemented.*

---

## 🐛 Troubleshooting

### Build Fails
```bash
# Clean build
/c/Users/conta/.platformio/penv/Scripts/platformio.exe run --target clean

# Rebuild
/c/Users/conta/.platformio/penv/Scripts/platformio.exe run
```

### Upload Fails
1. Hold BOOT button while uploading
2. Check USB cable connection
3. Verify COM port in Device Manager
4. Try different USB port

### WiFi Won't Connect
1. Verify SSID and password in `include/wifi_config.h`
2. Ensure WiFi is 2.4GHz (ESP32 doesn't support 5GHz)
3. Check router is accessible
4. Look for error messages in serial monitor

### Can't Access Web Server
1. Ensure BOOT button was pressed during startup
2. Verify IP address from serial monitor
3. Check you're on the same WiFi network
4. Try pinging the IP address
5. Check firewall settings

### Device Won't Wake from Sleep
1. Press BOOT button (GPIO 0)
2. Or press RESET button to restart
3. Check button is properly connected

---

## 📊 Project Status

### ✅ Implemented
- [x] NVS persistent storage
- [x] WiFi connectivity
- [x] Web server with configuration page
- [x] Deep sleep mode
- [x] Boot button wake-up
- [x] Serial debugging
- [x] LED status indication (via serial)

### 🚧 To Be Implemented
- [ ] WS2812 RGB LED driver (RMT-based)
- [ ] E-paper display driver
- [ ] HTTP image download
- [ ] Image processing (PNG decode, dither)
- [ ] Display update logic
- [ ] OTA firmware updates
- [ ] Battery monitoring

---

## 📁 Project Structure

```
ESP32-S3-Display/
├── include/
│   ├── config.h              # GPIO pins and system config
│   └── wifi_config.h         # WiFi credentials (edit this!)
├── src/
│   └── main.c                # Main application code
├── platformio.ini            # PlatformIO configuration
├── README.md                 # Project overview
├── QUICK_START.md            # Getting started guide
├── BUILD_AND_TEST.md         # Detailed build instructions
├── TESTING_RESULTS.md        # Latest test results
├── IMPLEMENTATION_SUMMARY.md # Technical details
└── QUICK_REFERENCE.md        # This file
```

---

## 🔗 Useful Links

- **PlatformIO Docs:** https://docs.platformio.org/
- **ESP-IDF Docs:** https://docs.espressif.com/projects/esp-idf/
- **Freenove ESP32-S3:** https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board
- **Board Specs:** https://docs.platformio.org/page/boards/espressif32/freenove_esp32_s3_wroom.html

---

## 💡 Tips

1. **Always check serial monitor** - It shows detailed debug information
2. **WiFi must be 2.4GHz** - ESP32 doesn't support 5GHz networks
3. **BOOT button is GPIO 0** - Used for wake-up and configuration mode
4. **Deep sleep saves power** - Device wakes on BOOT button press
5. **Credentials persist** - Stored in NVS, survive power cycles
6. **Web server timeout** - 5 minutes, then enters deep sleep

---

## 🎯 Next Steps

1. **Edit WiFi credentials** in `include/wifi_config.h`
2. **Build and upload** the firmware
3. **Test normal mode** - Should connect to WiFi
4. **Test config mode** - Press BOOT during startup
5. **Access web interface** - Configure via browser
6. **Verify persistence** - Credentials saved across reboots

---

## 📞 Support

If you encounter issues:
1. Check serial monitor output
2. Review `TESTING_RESULTS.md` for known issues
3. See `BUILD_AND_TEST.md` for troubleshooting
4. Check PlatformIO and ESP-IDF documentation

