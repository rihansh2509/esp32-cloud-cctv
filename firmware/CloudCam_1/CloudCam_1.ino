/*
 * ESP32-S3 (N16R8) CLOUD CCTV CAMERA
 * ------------------------------------------------------------------
 * Joins your home Wi-Fi and pushes JPEG frames to the relay server
 * (server/ folder, hosted free on Render). Watch from anywhere at
 * https://<your-app>.onrender.com
 *
 * NOTHING is hard-coded. Configure over the Serial Monitor (115200,
 * line ending "New Line") by typing:
 *
 *   ssid=MyHomeWiFi
 *   pass=MyWifiPassword
 *   host=https://esp32-cloud-cctv.onrender.com
 *   key=THE_CAM_KEY_YOU_SET_ON_RENDER
 *   id=cam1                (optional, default below)
 *   show                   (print current config)
 *   clear                  (erase config)   reboot
 *
 * Settings are saved to flash and survive reboots/re-uploads.
 *
 * Bandwidth: while somebody has the web page open the camera sends as
 * fast as it can (~5-10 fps). When nobody is watching it sends one frame
 * every 5 s, which also keeps the free Render instance awake.
 *
 * Arduino IDE (Tools): ESP32S3 Dev Module | USB CDC On Boot = Enabled
 *   PSRAM = OPI PSRAM | Flash Size = 16MB | Partition 16M (3MB APP/9.9MB FATFS)
 */
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

#define DEFAULT_CAM_ID "cam1"          // CloudCam_2 uses "cam2"

// ============================ PICK YOUR BOARD ===============================
#define CAMERA_MODEL_FREENOVE_ESP32S3_CAM     // Freenove ESP32-S3 WROOM CAM / generic "ESP32-S3 CAM N16R8"
// #define CAMERA_MODEL_XIAO_ESP32S3_SENSE    // Seeed XIAO ESP32-S3 Sense

#if defined(CAMERA_MODEL_FREENOVE_ESP32S3_CAM)
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
#elif defined(CAMERA_MODEL_XIAO_ESP32S3_SENSE)
  #define PWDN_GPIO_NUM  -1
  #define RESET_GPIO_NUM -1
  #define XCLK_GPIO_NUM  10
  #define SIOD_GPIO_NUM  40
  #define SIOC_GPIO_NUM  39
  #define Y9_GPIO_NUM    48
  #define Y8_GPIO_NUM    11
  #define Y7_GPIO_NUM    12
  #define Y6_GPIO_NUM    14
  #define Y5_GPIO_NUM    16
  #define Y4_GPIO_NUM    18
  #define Y3_GPIO_NUM    17
  #define Y2_GPIO_NUM    15
  #define VSYNC_GPIO_NUM 38
  #define HREF_GPIO_NUM  47
  #define PCLK_GPIO_NUM  13
#endif

// ============================ TUNING ========================================
#define IDLE_INTERVAL_MS   5000    // frame interval when nobody is watching
#define FAIL_BACKOFF_MS    3000    // wait after a failed upload
#define STATUS_EVERY_MS    10000   // serial status line
#define HTTP_TIMEOUT_MS    8000

// ============================ STATE =========================================
Preferences prefs;
String cfgSsid, cfgPass, cfgHost, cfgKey, cfgId;

WiFiClient       plainClient;
WiFiClientSecure tlsClient;
HTTPClient       http;
bool   httpBegun = false;
bool   cameraOK  = false;
int    viewers   = 0;

uint32_t lastPost = 0, lastStatus = 0, backoffUntil = 0, lastWifiTry = 0;
uint32_t statFrames = 0, statBytes = 0, totalFrames = 0;
String   serialBuf;

// ============================ CONFIG ========================================
void loadConfig() {
  prefs.begin("cloudcam", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgHost = prefs.getString("host", "");
  cfgKey  = prefs.getString("key",  "");
  cfgId   = prefs.getString("id",   DEFAULT_CAM_ID);
  if (cfgId.length() == 0) cfgId = DEFAULT_CAM_ID;
}

void printConfig() {
  Serial.println("---------------- CONFIG ----------------");
  Serial.printf("id   = %s\n", cfgId.c_str());
  Serial.printf("ssid = %s\n", cfgSsid.length() ? cfgSsid.c_str() : "(not set)");
  Serial.printf("pass = %s\n", cfgPass.length() ? "********" : "(not set)");
  Serial.printf("host = %s\n", cfgHost.length() ? cfgHost.c_str() : "(not set)");
  Serial.printf("key  = %s\n", cfgKey.length()  ? "********" : "(not set)");
  Serial.printf("wifi = %s", WiFi.status() == WL_CONNECTED ? "connected " : "not connected\n");
  if (WiFi.status() == WL_CONNECTED) Serial.printf("%s  rssi %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("cam  = %s\n", cameraOK ? "ok" : "FAILED");
  Serial.println("----------------------------------------");
}

void printHelp() {
  Serial.println("Commands (type one per line):");
  Serial.println("  ssid=<wifi name>     pass=<wifi password>");
  Serial.println("  host=https://xxxx.onrender.com   key=<CAM_KEY from Render>");
  Serial.println("  id=cam1 | show | clear | reboot | help");
}

void wifiStart() {
  if (!cfgSsid.length()) return;
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  lastWifiTry = millis();
  Serial.printf("[WIFI] connecting to \"%s\" ...\n", cfgSsid.c_str());
}

void httpReset() {
  if (httpBegun) { http.end(); httpBegun = false; }
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int eq = line.indexOf('=');
  String k = eq > 0 ? line.substring(0, eq) : line;
  String v = eq > 0 ? line.substring(eq + 1) : "";
  k.trim(); k.toLowerCase(); v.trim();

  if      (k == "ssid") { cfgSsid = v; prefs.putString("ssid", v); Serial.println("[OK] ssid saved"); wifiStart(); }
  else if (k == "pass") { cfgPass = v; prefs.putString("pass", v); Serial.println("[OK] pass saved"); wifiStart(); }
  else if (k == "host") {
    while (v.endsWith("/")) v.remove(v.length() - 1);
    if (v.length() && !v.startsWith("http")) v = "https://" + v;
    cfgHost = v; prefs.putString("host", v); httpReset(); Serial.printf("[OK] host = %s\n", v.c_str());
  }
  else if (k == "key")  { cfgKey = v; prefs.putString("key", v); Serial.println("[OK] key saved"); }
  else if (k == "id")   { v.toLowerCase(); cfgId = v.length() ? v : DEFAULT_CAM_ID; prefs.putString("id", cfgId); httpReset(); Serial.printf("[OK] id = %s\n", cfgId.c_str()); }
  else if (k == "show") printConfig();
  else if (k == "help" || k == "?") printHelp();
  else if (k == "clear") { prefs.clear(); Serial.println("[OK] config erased, rebooting"); delay(300); ESP.restart(); }
  else if (k == "reboot") { Serial.println("rebooting"); delay(200); ESP.restart(); }
  else { Serial.printf("? unknown command \"%s\"\n", k.c_str()); printHelp(); }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (serialBuf.length()) handleCommand(serialBuf); serialBuf = ""; }
    else if (serialBuf.length() < 200) serialBuf += c;
  }
}

// ============================ CAMERA ========================================
bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    Serial.printf("[CAM] PSRAM: %u MB\n", ESP.getPsramSize() / (1024 * 1024));
    config.frame_size   = FRAMESIZE_VGA;     // 640x480, ~20-35 KB per frame
    config.jpeg_quality = 14;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    Serial.println("[CAM] NO PSRAM - set Tools > PSRAM = OPI PSRAM. Falling back to QVGA.");
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("[CAM] init FAILED 0x%x (check pin model)\n", err); return false; }
  sensor_t *s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, 0); s->set_hmirror(s, 0); }  // set to 1 if the picture is upside down / mirrored
  return true;
}

// ============================ UPLOAD ========================================
bool postFrame(camera_fb_t *fb) {
  if (!httpBegun) {
    String url = cfgHost + "/api/frame/" + cfgId;
    bool tls = cfgHost.startsWith("https://");
    if (tls) { tlsClient.setInsecure(); httpBegun = http.begin(tlsClient, url); }
    else       httpBegun = http.begin(plainClient, url);
    if (!httpBegun) { Serial.println("[HTTP] bad host URL"); return false; }
    http.setReuse(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    Serial.printf("[HTTP] connecting to %s\n", url.c_str());
  }
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Cam-Key", cfgKey);

  int code = http.POST(fb->buf, fb->len);
  if (code == 200) {
    String body = http.getString();
    int i = body.indexOf("\"viewers\":");
    if (i >= 0) viewers = body.substring(i + 10).toInt();
    return true;
  }
  if (code > 0) {
    String body = http.getString();
    Serial.printf("[HTTP] server said %d %s\n", code, body.c_str());
    if (code == 401) Serial.println("[HTTP] -> wrong key. Type  key=<CAM_KEY from Render>");
  } else {
    Serial.printf("[HTTP] failed: %s\n", http.errorToString(code).c_str());
  }
  httpReset();
  return false;
}

// ============================ SETUP / LOOP ==================================
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-S3 CLOUD CCTV CAMERA ===");
  loadConfig();
  cameraOK = initCamera();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  wifiStart();

  printConfig();
  if (!cfgSsid.length() || !cfgHost.length()) {
    Serial.println("\n>>> Not configured yet. Type the commands below in the Serial Monitor:");
    printHelp();
  }
}

void loop() {
  pollSerial();
  uint32_t now = millis();

  // ---- Wi-Fi supervision
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) Serial.printf("[WIFI] connected, IP %s\n", WiFi.localIP().toString().c_str());
  if (!connected && wasConnected) { Serial.println("[WIFI] link lost"); httpReset(); }
  wasConnected = connected;
  if (!connected && cfgSsid.length() && now - lastWifiTry > 20000) wifiStart();

  // ---- status line
  if (now - lastStatus > STATUS_EVERY_MS) {
    float secs = (now - lastStatus) / 1000.0f;
    if (connected && cfgHost.length())
      Serial.printf("[STAT] %s | viewers %d | %.1f fps | %.0f KB/s | total %lu frames\n",
                    viewers ? "LIVE" : "idle", viewers, statFrames / secs, statBytes / secs / 1024.0f, (unsigned long)totalFrames);
    statFrames = 0; statBytes = 0; lastStatus = now;
  }

  // ---- upload
  if (!connected || !cameraOK || !cfgHost.length() || now < backoffUntil) { delay(10); return; }
  uint32_t interval = viewers > 0 ? 0 : IDLE_INTERVAL_MS;
  if (now - lastPost < interval) { delay(5); return; }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[CAM] capture failed"); delay(100); return; }
  bool ok = postFrame(fb);
  size_t len = fb->len;
  esp_camera_fb_return(fb);

  lastPost = millis();
  if (ok) { statFrames++; statBytes += len; totalFrames++; }
  else    { backoffUntil = millis() + FAIL_BACKOFF_MS; viewers = 0; }
}
