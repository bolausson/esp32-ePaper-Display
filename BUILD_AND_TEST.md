# Build and Test Instructions

## Prerequisites

- ✅ VS Code installed
- ✅ PlatformIO extension installed
- ✅ USB cable to connect ESP32-S3 board
- ✅ Freenove ESP32-S3 WROOM board

## Step 1: Configure WiFi Credentials

1. Open `include/wifi_config.h`
2. Edit the default credentials:

```c
#define DEFAULT_WIFI_SSID "YourNetworkName"
#define DEFAULT_WIFI_PASSWORD "YourNetworkPassword"
```

3. Save the file

## Step 2: Build the Project

### Using VS Code PlatformIO

1. Open the project folder in VS Code
2. Click the PlatformIO icon (alien head) in the left sidebar
3. Under "Project Tasks" → "freenove_esp32_s3_wroom"
4. Click "Build" (or press `Ctrl+Alt+B`)

### Using Terminal

```bash
cd ESP32-S3-Display
pio run
```

**Expected Output:**
```
Building .pio/build/freenove_esp32_s3_wroom/firmware.bin
...
SUCCESS
```

## Step 3: Upload to Board

### Using VS Code PlatformIO

1. Connect ESP32-S3 board via USB
2. In PlatformIO sidebar
3. Click "Upload" (or press `Ctrl+Alt+U`)

### Using Terminal

```bash
pio run --target upload
```

**Expected Output:**
```
Uploading .pio/build/freenove_esp32_s3_wroom/firmware.bin
...
Hard resetting via RTS pin...
SUCCESS
```

## Step 4: Monitor Serial Output

### Using VS Code PlatformIO

1. In PlatformIO sidebar
2. Click "Monitor" (or press `Ctrl+Alt+S`)

### Using Terminal

```bash
pio device monitor
```

**Expected Output:**
```
ESP32-S3 Display Starting
NVS initialized
Loaded credentials - SSID: Void, URL: 
LED initialized on GPIO 48
Boot button initialized on GPIO 0
Boot button not pressed - normal operation mode
...
```

## Step 5: Test Normal Operation

1. **Power on the board** (without pressing BOOT button)
2. **Watch serial monitor** for:
   - WiFi connection attempt
   - IP address assignment
   - Green LED (solid) when connected
   - Red LED (blinking) if connection fails
3. **After 5 seconds**: Device enters deep sleep
4. **LED turns off** (deep sleep active)

## Step 6: Test Configuration Mode

1. **Press and HOLD** the BOOT button
2. **Press** the RESET button (or reconnect power)
3. **Release** the BOOT button
4. **Watch serial monitor** for:
   ```
   Wakeup caused by boot button
   Boot button pressed - entering webserver mode
   WiFi initialization finished
   Got IP address: 192.168.1.XXX
   Web server started successfully
   ```
5. **LED blinks YELLOW** (web server mode)

## Step 7: Test Web Interface

1. **Note the IP address** from serial monitor (e.g., 192.168.1.100)
2. **Open browser** on a device connected to the same WiFi
3. **Navigate to**: `http://192.168.1.100`
4. **You should see**: Configuration page with form
5. **Test the form**:
   - Enter new WiFi SSID
   - Enter new WiFi password
   - Enter image URL (optional)
   - Click "Save and Sleep"
6. **Watch for**:
   - Success message in browser
   - Serial output: "Credentials saved to NVS"
   - LED blinks BLUE for 3 seconds
   - Device enters deep sleep

## Step 8: Verify Persistent Storage

1. **Wake the device** (press BOOT button)
2. **Check serial monitor** for:
   ```
   Loaded credentials - SSID: YourNewSSID, URL: YourNewURL
   ```
3. **Verify** credentials were saved and loaded

## Troubleshooting

### Build Errors

**Error: "led_strip.h: No such file or directory"**
- Solution: The ESP-IDF framework should include this. Try:
  ```bash
  pio pkg update
  ```

**Error: "espressif32 platform not found"**
- Solution: Install platform:
  ```bash
  pio platform install espressif32
  ```

### Upload Errors

**Error: "Failed to connect to ESP32"**
- Hold BOOT button while uploading
- Check USB cable and port
- Try different USB port
- Check board is powered

**Error: "Serial port not found"**
- Install USB drivers for CH343 (check Freenove documentation)
- Check Device Manager (Windows) for COM port

### Runtime Errors

**WiFi connection fails**
- Verify SSID and password are correct
- Ensure WiFi is 2.4GHz (ESP32 doesn't support 5GHz)
- Check WiFi router is accessible
- Check serial monitor for error messages

**LED doesn't light up**
- Verify LED48 is on GPIO 48
- Check WS2812 LED has power
- Try different LED brightness values

**Web server not accessible**
- Ensure BOOT button was pressed during startup
- Verify IP address from serial monitor
- Check you're on the same WiFi network
- Try pinging the IP address
- Check firewall settings

**Device doesn't wake from sleep**
- Verify BOOT button is on GPIO 0
- Check button connection (should pull GPIO 0 to GND)
- Try pressing button longer

## Expected Serial Output Examples

### Normal Boot (No Button)
```
=== ESP32-S3 Display Starting ===
Not a deep sleep wakeup (normal boot)
NVS initialized
Loaded credentials - SSID: Void, URL: 
LED initialized on GPIO 48
Boot button initialized on GPIO 0
Boot button not pressed - normal operation mode
In normal mode, the device would:
  1. Connect to WiFi
  2. Download image from: 
  3. Display image on e-paper
  4. Enter deep sleep
Preparing to enter deep sleep mode...
Entering deep sleep. Press BOOT button to wake up.
```

### Configuration Mode (Button Pressed)
```
=== ESP32-S3 Display Starting ===
Wakeup caused by boot button
NVS initialized
Loaded credentials - SSID: Void, URL: 
LED initialized on GPIO 48
Boot button initialized on GPIO 0
Boot button pressed - entering webserver mode
WiFi initialization finished. Connecting to SSID: Void
Got IP address: 192.168.1.100
Connected to AP SSID: Void
Starting HTTP server on port 80
Web server started successfully
Web server is running. Configure via web interface.
```

## Success Criteria

✅ Project builds without errors
✅ Firmware uploads successfully  
✅ Serial monitor shows boot messages
✅ LED indicates status correctly
✅ WiFi connects successfully
✅ Web server accessible via browser
✅ Configuration can be saved
✅ Device enters/exits deep sleep
✅ Credentials persist across power cycles

## Next Steps

Once all tests pass:
1. Integrate e-paper display hardware
2. Implement image download functionality
3. Add image processing and display code
4. Test complete workflow

