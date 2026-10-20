# ESP32-S3-CAM OV3660 MJPEG camera

This project serves a live OV3660 camera feed and a responsive control page from
an ESP32-S3-CAM DevKit. The sketch is [esp32-s3-cam-ov3660.ino](./esp32-s3-cam-ov3660.ino).

## Features

- Connects to a 2.4 GHz Wi-Fi network and retries if the connection drops.
- Checks for at least 8 MiB of PSRAM before initializing the camera.
- Places two JPEG camera frame buffers in PSRAM.
- Serves a mobile-friendly page, MJPEG stream, resolution selector, and flash
  LED control.
- Uses one active stream client at a time. The page and control requests remain
  available while the stream is running.

## Requirements

- ESP32-S3-CAM DevKit with **16 MB flash** and **8 MB Octal (OPI) PSRAM**.
- OV3660 camera module compatible with your board's camera connector.
- A USB data cable and a 2.4 GHz Wi-Fi network.
- Arduino IDE 2.x.
- Espressif Systems' **esp32** board package, preferably a current 3.x release.
  The package includes `esp_camera.h`, `WiFi.h`, and the PSRAM support used by
  this sketch; no separate camera library is needed.

The ESP32-S3 does not connect to 5 GHz Wi-Fi networks. The computer or phone
used to view the page must be able to reach the ESP32 on the same local network.

## 1. Install the ESP32 board package

1. Open **Settings/Preferences** in Arduino IDE.
2. In **Additional boards manager URLs**, add Espressif's stable package index
   if it is not already listed:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Open **Boards Manager** from **Tools > Board**, search for **esp32**, and install
   **esp32 by Espressif Systems**.

Menu labels can differ slightly by Arduino IDE and board-package version.

## 2. Prepare the sketch

Arduino sketches are normally stored in a directory whose name matches the
`.ino` file. Create a directory named `esp32-s3-cam-ov3660`, place
`esp32-s3-cam-ov3660.ino` inside it, and open that file in Arduino IDE. For
example:

```text
esp32-s3-cam-ov3660/
└── esp32-s3-cam-ov3660.ino
```

Before uploading, edit the Wi-Fi settings near the top of the sketch:

```cpp
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

Replace `YOUR_WIFI_SSID` with the exact network name and `YOUR_WIFI_PASSWORD`
with that network's password. Keep the surrounding double quotes, and do not
share or commit real credentials to a public repository.

## 3. Select the board and memory settings

Connect the DevKit by USB, then select its port under **Tools → Port** and
choose the board profile matching the manufacturer's documentation. A common
generic profile is **ESP32S3 Dev Module**.

Set the relevant **Tools** options to match the board:

| Setting | Value |
| --- | --- |
| Flash Size | **16MB (128Mb)** |
| PSRAM | **OPI PSRAM** / **Octal PSRAM** |
| Partition Scheme | A scheme for 16 MB flash with enough application space |
| USB CDC On Boot | Enable if using the native USB port for Serial Monitor; otherwise follow the board instructions |

Some vendor-specific board profiles select flash and PSRAM settings
automatically. Do not select QSPI PSRAM: this board configuration requires
Octal/OPI PSRAM. At startup, the sketch prints the detected PSRAM size and
stops camera initialization if 8 MiB is not available.

## 4. Verify the GPIO mapping

**Do not assume every ESP32-S3-CAM DevKit uses the same pinout.** The following
is the pin map currently defined in the sketch; confirm every GPIO against your
specific board's schematic or vendor example before powering the camera. GPIOs
may be reserved or routed differently on another board revision.

| Camera signal | Sketch GPIO |
| --- | ---: |
| D0 / Y2 | 11 |
| D1 / Y3 | 9 |
| D2 / Y4 | 8 |
| D3 / Y5 | 10 |
| D4 / Y6 | 12 |
| D5 / Y7 | 18 |
| D6 / Y8 | 17 |
| D7 / Y9 | 16 |
| XCLK | 15 |
| PCLK | 13 |
| VSYNC | 6 |
| HREF | 7 |
| SCCB SDA | 4 |
| SCCB SCL | 5 |
| PWDN | -1 (not connected/controlled) |
| RESET | -1 (not connected/controlled) |

These are **ESP32 GPIO numbers**, not physical header-pin numbers. If your
board's camera pinout differs, update the corresponding `#define` values near
the top of the sketch. Keep the signal-to-`pin_d0` through `pin_d7` assignments
in `initializeCamera()` consistent with the camera data bus.

The flash LED is configured separately:

```cpp
constexpr int FLASH_LED_GPIO = 2;
constexpr bool FLASH_LED_ACTIVE_LOW = false;
```

Change the GPIO to the board's flash LED pin and set `FLASH_LED_ACTIVE_LOW` to
`true` if the LED turns on at a low logic level. Some boards have no onboard
flash LED or use a driver circuit; check the schematic and do not connect a
high-current LED directly to a GPIO.

## 5. Compile and upload

1. In Arduino IDE, open the prepared sketch and confirm the board and port.
2. Click **Verify** to compile.
3. Click **Upload**. If the board does not enter upload mode automatically,
   follow its instructions for holding **BOOT** while pressing **RESET**.
4. Open **Tools → Serial Monitor** and set the baud rate to **115200**.
5. Wait for camera initialization and Wi-Fi connection. The monitor should show
   the detected PSRAM size and, after connecting, a line similar to:

   ```text
   Wi-Fi connected. Open http://192.168.1.123/
   ```

6. Open that address in a browser on a device connected to the same local
   network.

The web server listens on HTTP port 80. Its routes are:

| Route | Purpose |
| --- | --- |
| `/` | Camera page and controls |
| `/stream` | MJPEG live stream |
| `/control?framesize=VGA` | Set a supported resolution |
| `/led?state=1` or `/led?state=0` | Turn the flash LED on or off |

The page starts at VGA and offers QVGA, VGA, SVGA, XGA, UXGA, and QXGA. Maximum
usable resolution and frame rate depend on the sensor, lighting, Wi-Fi link,
and board pinout; higher resolutions usually lower the frame rate. The sketch
allows one active `/stream` connection at a time.

## Troubleshooting

- **PSRAM missing or less than 8 MiB:** Check that the selected board supports
  8 MB OPI PSRAM and that **PSRAM** is set to **OPI/Octal**. Verify the hardware
  is actually the N16R8 configuration.
- **Camera initialization failed:** Check the camera connector orientation,
  board-specific GPIO mapping, power, and that the camera module is seated.
- **Sensor is not identified as an OV3660:** Confirm the installed module is an
  OV3660 and that the SCCB and camera data pins match the board schematic.
- **No Wi-Fi address appears:** Check the SSID/password, use a 2.4 GHz network,
  and ensure the access point permits devices to communicate with each other.
  The sketch retries Wi-Fi connection every five seconds.
- **Page loads but video does not:** Check Serial Monitor for camera capture
  errors, reload the page, and ensure another browser/device is not already
  using the single stream connection.
- **LED control has no effect:** Check `FLASH_LED_GPIO` and
  `FLASH_LED_ACTIVE_LOW` for your board revision.
- **Upload or Serial Monitor fails:** Confirm the correct port, USB data cable,
  and board-specific boot/USB settings. Some boards require the BOOT/RESET
  sequence to enter upload mode.

## Changing camera defaults

In `initializeCamera()`, `config.frame_size` sets the startup resolution and
`config.jpeg_quality` controls JPEG quality (a lower number generally means
higher quality and larger frames). The camera is configured for JPEG, two
PSRAM-resident frame buffers, and the latest-frame grab mode. If you change
these settings or use a different camera module, check the camera driver's
supported resolutions and available memory.
