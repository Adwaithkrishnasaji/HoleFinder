#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp32-hal-psram.h>
#include <cstdio>
#include <new>
#include <cstring>

// Set these for your local network.
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Common ESP32-S3-CAM pin map. Verify it against your board's schematic.
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// Change these if your board uses a different flash LED pin or polarity.
constexpr int FLASH_LED_GPIO = 2;
constexpr bool FLASH_LED_ACTIVE_LOW = false;

constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr size_t REQUIRED_PSRAM_BYTES = 8U * 1024U * 1024U;

WiFiServer server(80);
bool ledEnabled = false;
bool streamActive = false;
bool wifiWasConnected = false;
portMUX_TYPE streamMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastWiFiAttempt = 0;

struct StreamClientContext {
  WiFiClient client;
};

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#10151d">
  <title>ESP32-S3 Camera</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; }
    * { box-sizing: border-box; }
    body { margin: 0; min-height: 100vh; background: #10151d; color: #edf2f7; }
    main { width: min(100% - 28px, 960px); margin: 32px auto; }
    header { display: flex; align-items: center; justify-content: space-between; gap: 16px; margin-bottom: 20px; }
    h1 { margin: 0; font-size: clamp(1.35rem, 4vw, 2rem); }
    .status { color: #72e0a4; font-size: .9rem; }
    .card { overflow: hidden; border: 1px solid #293342; border-radius: 16px; background: #171e29; box-shadow: 0 16px 48px #0004; }
    .feed { display: block; width: 100%; min-height: 220px; max-height: 72vh; object-fit: contain; background: #080b10; }
    .controls { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; padding: 16px; }
    label { color: #b8c3d2; font-size: .92rem; }
    select, button { min-height: 42px; border: 1px solid #3a4658; border-radius: 10px; background: #222c3a; color: #edf2f7; font: inherit; padding: 8px 12px; }
    button { cursor: pointer; }
    button:hover, select:hover { border-color: #7aaeff; }
    button[aria-pressed="true"] { background: #8c5425; border-color: #e6a45f; }
    #message { min-height: 1.4em; padding: 0 16px 14px; color: #b8c3d2; font-size: .9rem; }
    @media (max-width: 520px) {
      main { width: min(100% - 20px, 960px); margin: 18px auto; }
      .controls { align-items: stretch; flex-direction: column; }
      select, button { width: 100%; }
      .feed { min-height: 180px; }
    }
  </style>
</head>
<body>
  <main>
    <header><h1>ESP32-S3 Camera</h1><span class="status">● Live camera</span></header>
    <section class="card">
      <img class="feed" src="/stream" alt="Live camera stream">
      <div class="controls">
        <label for="resolution">Resolution</label>
        <select id="resolution" aria-label="Camera resolution">
          <option value="QVGA">QVGA · 320×240</option>
          <option value="VGA" selected>VGA · 640×480</option>
          <option value="SVGA">SVGA · 800×600</option>
          <option value="XGA">XGA · 1024×768</option>
          <option value="UXGA">UXGA · 1600×1200</option>
          <option value="QXGA">QXGA · 2048×1536</option>
        </select>
        <button id="led" type="button" aria-pressed="false">Flash LED: Off</button>
      </div>
      <div id="message" role="status" aria-live="polite"></div>
    </section>
  </main>
  <script>
    const message = document.getElementById('message');
    const resolution = document.getElementById('resolution');
    const led = document.getElementById('led');
    async function control(path) {
      const response = await fetch(path, { cache: 'no-store' });
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || 'Request failed');
      return result;
    }
    resolution.addEventListener('change', async () => {
      message.textContent = 'Changing resolution…';
      try {
        await control('/control?framesize=' + encodeURIComponent(resolution.value));
        message.textContent = 'Resolution set to ' + resolution.value + '.';
      } catch (error) {
        message.textContent = error.message;
      }
    });
    led.addEventListener('click', async () => {
      const nextState = led.getAttribute('aria-pressed') !== 'true';
      try {
        const result = await control('/led?state=' + (nextState ? '1' : '0'));
        led.setAttribute('aria-pressed', String(result.enabled));
        led.textContent = 'Flash LED: ' + (result.enabled ? 'On' : 'Off');
        message.textContent = '';
      } catch (error) {
        message.textContent = error.message;
      }
    });
  </script>
</body>
</html>
)rawliteral";

void setFlashLed(bool enabled) {
  ledEnabled = enabled;
  const uint8_t onLevel = FLASH_LED_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t offLevel = FLASH_LED_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(FLASH_LED_GPIO, enabled ? onLevel : offLevel);
}

void sendResponse(WiFiClient &client, int status, const char *reason,
                  const char *contentType, const char *body) {
  client.printf("HTTP/1.1 %d %s\r\n", status, reason);
  client.printf("Content-Type: %s\r\n", contentType);
  client.printf("Content-Length: %u\r\n", static_cast<unsigned>(strlen(body)));
  client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
  client.write(reinterpret_cast<const uint8_t *>(body), strlen(body));
}

void sendJson(WiFiClient &client, int status, const char *reason,
              const char *body) {
  sendResponse(client, status, reason, "application/json; charset=utf-8", body);
}

bool writeAll(WiFiClient &client, const uint8_t *data, size_t length) {
  size_t written = 0;
  while (written < length && client.connected()) {
    const size_t amount = client.write(data + written, length - written);
    if (amount == 0) {
      delay(1);
      continue;
    }
    written += amount;
  }
  return written == length;
}

void clearStreamActive() {
  portENTER_CRITICAL(&streamMux);
  streamActive = false;
  portEXIT_CRITICAL(&streamMux);
}

void streamClientTask(void *parameter) {
  StreamClientContext *context = static_cast<StreamClientContext *>(parameter);
  WiFiClient client = context->client;
  delete context;
  client.setNoDelay(true);

  client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n\r\n");

  static const char framePrefix[] = "--frame\r\nContent-Type: image/jpeg\r\n";
  static const char frameSeparator[] = "\r\n";
  while (client.connected()) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr) {
      Serial.println("Camera frame capture failed");
      delay(10);
      continue;
    }

    char frameHeader[80];
    const int headerLength =
        snprintf(frameHeader, sizeof(frameHeader), "%sContent-Length: %u\r\n\r\n",
                 framePrefix, static_cast<unsigned>(frame->len));
    const bool sent =
        headerLength > 0 &&
        writeAll(client, reinterpret_cast<const uint8_t *>(frameHeader),
                 static_cast<size_t>(headerLength)) &&
        writeAll(client, frame->buf, frame->len) &&
        writeAll(client, reinterpret_cast<const uint8_t *>(frameSeparator),
                 sizeof(frameSeparator) - 1);
    esp_camera_fb_return(frame);
    if (!sent) {
      break;
    }
    taskYIELD();
  }

  client.stop();
  clearStreamActive();
  vTaskDelete(nullptr);
}

bool handleStreamRequest(WiFiClient &client) {
  bool alreadyStreaming;
  portENTER_CRITICAL(&streamMux);
  alreadyStreaming = streamActive;
  if (!alreadyStreaming) {
    streamActive = true;
  }
  portEXIT_CRITICAL(&streamMux);

  if (alreadyStreaming) {
    sendJson(client, 503, "Service Unavailable",
             "{\"error\":\"A stream is already active\"}");
    return false;
  }

  StreamClientContext *context = new (std::nothrow) StreamClientContext{client};
  if (context == nullptr) {
    clearStreamActive();
    sendJson(client, 503, "Service Unavailable",
             "{\"error\":\"Unable to allocate stream context\"}");
    return false;
  }

  const BaseType_t result =
      xTaskCreatePinnedToCore(streamClientTask, "camera-stream", 8192, context,
                              1, nullptr, 0);
  if (result != pdPASS) {
    delete context;
    clearStreamActive();
    sendJson(client, 503, "Service Unavailable",
             "{\"error\":\"Unable to start stream task\"}");
    return false;
  }
  return true;
}

bool readRequest(WiFiClient &client, String &path) {
  client.setTimeout(1500);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  if (!requestLine.startsWith("GET ")) {
    return false;
  }

  const int pathStart = 4;
  const int pathEnd = requestLine.indexOf(' ', pathStart);
  if (pathEnd < 0) {
    return false;
  }
  path = requestLine.substring(pathStart, pathEnd);

  while (client.connected()) {
    String header = client.readStringUntil('\n');
    if (header == "\r" || header.length() == 0) {
      break;
    }
  }
  return true;
}

String queryValue(const String &path, const char *key) {
  const String prefix = String(key) + "=";
  const int queryStart = path.indexOf('?');
  if (queryStart < 0) {
    return String();
  }
  int itemStart = queryStart + 1;
  while (itemStart < path.length()) {
    const int itemEnd = path.indexOf('&', itemStart);
    const String item = path.substring(
        itemStart, itemEnd < 0 ? path.length() : itemEnd);
    if (item.startsWith(prefix)) {
      return item.substring(prefix.length());
    }
    if (itemEnd < 0) {
      break;
    }
    itemStart = itemEnd + 1;
  }
  return String();
}

framesize_t frameSizeForName(const String &name) {
  if (name == "QVGA") return FRAMESIZE_QVGA;
  if (name == "VGA") return FRAMESIZE_VGA;
  if (name == "SVGA") return FRAMESIZE_SVGA;
  if (name == "XGA") return FRAMESIZE_XGA;
  if (name == "UXGA") return FRAMESIZE_UXGA;
  if (name == "QXGA") return FRAMESIZE_QXGA;
  return FRAMESIZE_INVALID;
}

bool handleRequest(WiFiClient &client, const String &path) {
  const int queryStart = path.indexOf('?');
  const String route = queryStart < 0 ? path : path.substring(0, queryStart);

  if (route == "/") {
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n");
    client.printf("Content-Length: %u\r\n", static_cast<unsigned>(strlen(INDEX_HTML)));
    client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
    client.write(reinterpret_cast<const uint8_t *>(INDEX_HTML), strlen(INDEX_HTML));
  } else if (route == "/stream") {
    return handleStreamRequest(client);
  } else if (route == "/control") {
    const String requestedSize = queryValue(path, "framesize");
    const framesize_t frameSize = frameSizeForName(requestedSize);
    if (frameSize == FRAMESIZE_INVALID) {
      sendJson(client, 400, "Bad Request",
               "{\"error\":\"Unsupported camera resolution\"}");
      return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == nullptr || sensor->set_framesize(sensor, frameSize) != 0) {
      sendJson(client, 500, "Internal Server Error",
               "{\"error\":\"Camera rejected the resolution\"}");
      return false;
    }
    sendJson(client, 200, "OK", "{\"ok\":true}");
  } else if (route == "/led") {
    const String state = queryValue(path, "state");
    if (state != "0" && state != "1") {
      sendJson(client, 400, "Bad Request",
               "{\"error\":\"LED state must be 0 or 1\"}");
      return false;
    }
    setFlashLed(state == "1");
    sendJson(client, 200, "OK",
             ledEnabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
  } else {
    sendJson(client, 404, "Not Found", "{\"error\":\"Not found\"}");
  }
  return false;
}

bool initializeCamera() {
  if (!psramFound()) {
    psramInit();
  }
  const size_t psramBytes = ESP.getPsramSize();
  Serial.printf("PSRAM: %u bytes\r\n", static_cast<unsigned>(psramBytes));
  if (!psramFound() || psramBytes < REQUIRED_PSRAM_BYTES) {
    Serial.println("Required 8 MB PSRAM is unavailable; check board settings.");
    return false;
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\r\n", error);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr || sensor->id.PID != OV3660_PID) {
    Serial.println("Camera sensor is not identified as an OV3660.");
    esp_camera_deinit();
    return false;
  }
  Serial.println("OV3660 initialized; frame buffers are in PSRAM.");
  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();
  Serial.printf("Connecting to Wi-Fi: %s\r\n", WIFI_SSID);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(FLASH_LED_GPIO, OUTPUT);
  setFlashLed(false);

  if (!initializeCamera()) {
    Serial.println("Camera setup stopped. Check the pin map and PSRAM settings.");
    while (true) {
      delay(1000);
    }
  }

  connectWiFi();
  server.begin();
  Serial.println("HTTP server listening on port 80.");
}

void loop() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !wifiWasConnected) {
    Serial.printf("Wi-Fi connected. Open http://%s/\r\n",
                  WiFi.localIP().toString().c_str());
  }
  wifiWasConnected = wifiConnected;

  if (!wifiConnected &&
      millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL_MS) {
    Serial.println("Wi-Fi disconnected; retrying connection.");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWiFiAttempt = millis();
  }

  WiFiClient client = server.available();
  if (!client) {
    delay(1);
    return;
  }

  String path;
  bool connectionHandedToStream = false;
  if (readRequest(client, path)) {
    connectionHandedToStream = handleRequest(client, path);
  } else {
    sendJson(client, 400, "Bad Request", "{\"error\":\"Invalid GET request\"}");
  }
  if (!connectionHandedToStream) {
    client.stop();
  }
}

/*
  Arduino IDE setup:
  - Install Espressif Systems' "esp32" board package (use a current 3.x release).
  - Select "ESP32S3 Dev Module" (or your vendor's matching ESP32-S3-CAM entry).
  - Flash Size: 16MB; PSRAM: OPI PSRAM (Octal).
  - Select a 16MB-compatible partition scheme with enough app space.
  - Set USB CDC On Boot as needed for your board's USB connection.
  - Replace the Wi-Fi credentials and verify the GPIO map against your board.
  Open Serial Monitor at 115200 baud to find the board's IP address.
*/
