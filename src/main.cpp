#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <driver/i2s.h>
#include <esp_sleep.h>
#include <math.h>

#include "config.h"
#include "secrets.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Missing include/secrets.h! Copy include/secrets.example.h to include/secrets.h and add your Wi-Fi and Groq API key."
#endif


// ==================== DISPLAY DRIVER ====================


#if OLED_USE_SSD1306 == 1
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
#else
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
#endif

// ==================== AUDIO & DSP CONSTANTS ====================
// Audio is spooled to flash (LittleFS) while the PTT button is held, so recording
// length is limited by free flash space instead of the ~128 KB of free RAM.
static const char*  REC_PATH         = "/rec.pcm";
static const size_t STAGE_SAMPLES    = 2048;            // 4 KB RAM staging buffer
static const size_t STAGE_BYTES      = STAGE_SAMPLES * sizeof(int16_t);
static const size_t FS_SAFETY_MARGIN = 32768;           // keep some flash free for LittleFS
static const int    DMA_BUF_COUNT    = 10;              // ~320 ms of I2S headroom for flash writes

static int16_t stagingBuf[STAGE_SAMPLES];
bool   fsReady = false;
float  recordGain = 1.0f;     // applied while uploading
uint32_t recCapSec = 0;       // max recording length given current free flash

// >>16 is 1:1 24-bit-to-16-bit mapping; >>15 gives clean +6dB headroom without amplifying hiss
static const int INMP441_SHIFT = 15;

// 1st-Order High-Pass DC-Blocking Filter (fc = HPF_CUTOFF_HZ, fs = I2S_SAMPLE_RATE)
static const float HPF_RC    = 1.0f / (2.0f * PI * (float)HPF_CUTOFF_HZ);
static const float HPF_DT    = 1.0f / (float)I2S_SAMPLE_RATE;
static const float HPF_ALPHA = HPF_RC / (HPF_RC + HPF_DT);

struct __attribute__((packed)) WavHeader {
  char     riff[4]       = {'R', 'I', 'F', 'F'};
  uint32_t fileSize;
  char     wave[4]       = {'W', 'A', 'V', 'E'};
  char     fmt[4]        = {'f', 'm', 't', ' '};
  uint32_t fmtSize       = 16;
  uint16_t audioFormat   = 1; // PCM
  uint16_t numChannels   = 1; // Mono
  uint32_t sampleRate    = I2S_SAMPLE_RATE;
  uint32_t byteRate      = I2S_SAMPLE_RATE * 2;
  uint16_t blockAlign    = 2;
  uint16_t bitsPerSample = 16;
  char     data[4]       = {'d', 'a', 't', 'a'};
  uint32_t dataSize;
};

// ==================== STATE MACHINE & UI ====================
enum SystemState {
  STATE_HUD_IDLE,
  STATE_LISTENING,
  STATE_TRANSCRIBING,
  STATE_THINKING,
  STATE_RESPONDING,
  STATE_MENU,
  STATE_SELF_TEST,
  STATE_STATS
};

SystemState currentState = STATE_HUD_IDLE;
String tarsReply = "TARS ONLINE. HUMOR 75%.";
String lastHeardText = "";
String activeGroqModel = GROQ_MODEL;
String sttStageLabel = "UPLOADING AUDIO...";

unsigned long lastActivityMs = 0;
unsigned long recordStartMs = 0;
bool requireRelease = false;   // set when a recording ended while PTT was still held

static const int WAVE_COLS = 120;
uint8_t waveHistory[WAVE_COLS] = {0};
int32_t sessionMaxPeak = 0;
int activeI2SChannel = 0; // 0 = Even slot, 1 = Odd slot (auto-detected)

// Menu State
const char* menuItems[] = {
  "1. HW Self-Test",
  "2. Wi-Fi Reconnect",
  "3. TARS Diagnostics",
  "4. Enter Deep Sleep"
};
const int MENU_COUNT = 4;
int menuIndex = 0;

// Debounced Button Tracker
struct Button {
  uint8_t pin;
  bool lastStableState;
  bool lastRawState;
  unsigned long lastChangeMs;
};

Button btn1 = {PIN_BTN_1, HIGH, HIGH, 0};
Button btn2 = {PIN_BTN_2, HIGH, HIGH, 0};
Button btn3 = {PIN_BTN_3, HIGH, HIGH, 0};

// ==================== EYE ANIMATION STATE ====================
float eyeOpen = 1.0f;          // 0 = closed, 1 = fully open (eased)
float eyeLookX = 0.0f, eyeLookY = 0.0f;
float eyeTargetLookX = 0.0f, eyeTargetLookY = 0.0f;
float eyeHappy = 0.0f;         // 0..1, >0.5 draws the "^ ^" smiling eyes
bool  eyeBlinking = false;
unsigned long eyeNextBlinkMs = 2000;
unsigned long eyeBlinkEndMs = 0;
unsigned long eyeNextLookMs = 1500;
unsigned long eyeNextHappyMs = 9000;
unsigned long eyeHappyUntilMs = 0;

// ==================== BUTTON HELPERS ====================
bool updateButton(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRawState) {
    b.lastChangeMs = millis();
    b.lastRawState = raw;
  }
  if ((millis() - b.lastChangeMs) > BUTTON_DEBOUNCE_MS) {
    if (raw != b.lastStableState) {
      b.lastStableState = raw;
      if (b.lastStableState == LOW) {
        lastActivityMs = millis();
        return true;
      }
    }
  }
  return false;
}

bool isButtonHeld(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

// ==================== STORAGE ====================
void updateRecordingCapacity() {
  if (!fsReady) { recCapSec = 0; return; }
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  size_t freeBytes = (total > used) ? (total - used) : 0;
  size_t usable = (freeBytes > FS_SAFETY_MARGIN) ? (freeBytes - FS_SAFETY_MARGIN) : 0;
  recCapSec = usable / (I2S_SAMPLE_RATE * sizeof(int16_t));
}

// ==================== I2S MICROPHONE SETUP ====================
void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = I2S_BUFFER_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = PIN_I2S_SCK,
    .ws_io_num = PIN_I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = PIN_I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// ==================== DEEP SLEEP ====================
void enterDeepSleep() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(18, 35, "POWERING DOWN...");
  u8g2.sendBuffer();
  delay(400);

  u8g2.setPowerSave(1);
  i2s_driver_uninstall(I2S_NUM_0);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BTN_1, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// ==================== WI-FI MANAGEMENT & RF DIAGNOSTICS ====================
bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(6, 26, "SCANNING 2.4GHz RF...");
  u8g2.sendBuffer();

  int netCount = WiFi.scanNetworks();
  u8g2.clearBuffer();
  if (netCount <= 0) {
    u8g2.drawStr(4, 24, "RF ERR: 0 NETS FOUND");
    u8g2.drawStr(4, 38, "CHECK ANTENNA / PWR");
    u8g2.sendBuffer();
    delay(2500);
    return false;
  }

  char hdr[22];
  snprintf(hdr, sizeof(hdr), "FOUND %d 2.4G NETS:", netCount);
  u8g2.drawStr(0, 12, hdr);
  for (int i = 0; i < min(netCount, 2); i++) {
    char netLine[24];
    snprintf(netLine, sizeof(netLine), "%.13s %ddBm", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    u8g2.drawStr(0, 26 + (i * 12), netLine);
  }
  String connBanner = "LINKING " + String(WIFI_SSID);
  u8g2.drawStr(0, 56, connBanner.c_str());
  u8g2.sendBuffer();
  delay(1200);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  unsigned long startAttempt = millis();
  int dotCount = 0;

  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < WIFI_CONNECT_TIMEOUT_MS) {
    u8g2.clearBuffer();
    u8g2.drawStr(6, 22, "LINKING UPLINK...");

    char stLine[22];
    snprintf(stLine, sizeof(stLine), "STATUS: %d  RSSI:%d", WiFi.status(), WiFi.RSSI());
    u8g2.drawStr(6, 38, stLine);

    u8g2.drawBox(6, 50, (dotCount % 22) * 5, 4);
    u8g2.sendBuffer();
    dotCount++;
    delay(250);
  }

  WiFi.scanDelete();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    return true;
  }

  u8g2.clearBuffer();
  char failLine[22];
  snprintf(failLine, sizeof(failLine), "FAIL CODE: %d", WiFi.status());
  u8g2.drawStr(6, 26, failLine);
  if (WiFi.status() == WL_NO_SSID_AVAIL) {
    u8g2.drawStr(6, 42, "SSID NOT IN 2.4G");
  } else {
    u8g2.drawStr(6, 42, "PASS/WPA3/RF BLOCK");
  }
  u8g2.sendBuffer();
  delay(2500);

  return false;
}

// ==================== EYES ====================
// Soft, solid rounded eyes. They blink (sometimes twice), glance around with eased
// motion, squash slightly when blinking and occasionally smile as "^ ^".
void updateEyes() {
  unsigned long now = millis();

  // --- blinking ---
  if (!eyeBlinking && now >= eyeNextBlinkMs) {
    eyeBlinking = true;
    eyeBlinkEndMs = now + 120;
  } else if (eyeBlinking && now >= eyeBlinkEndMs) {
    eyeBlinking = false;
    if (random(100) < 20) {
      eyeNextBlinkMs = now + 220;                 // quick double blink
    } else {
      eyeNextBlinkMs = now + random(2200, 5500);
    }
  }

  // --- glancing around ---
  if (now >= eyeNextLookMs) {
    if (random(100) < 40) {
      eyeTargetLookX = 0;
      eyeTargetLookY = 0;
    } else {
      eyeTargetLookX = (float)random(-9, 10);
      eyeTargetLookY = (float)random(-4, 5);
    }
    eyeNextLookMs = now + random(1400, 3800);
  }

  // --- smiling ---
  if (currentState == STATE_HUD_IDLE && now >= eyeNextHappyMs) {
    eyeHappyUntilMs = now + 1400;
    eyeNextHappyMs = now + random(9000, 18000);
  }
  bool happy = now < eyeHappyUntilMs;
  if (currentState == STATE_RESPONDING && (now - lastActivityMs) < 1500) happy = true;

  // --- targets per state ---
  float tOpen = eyeBlinking ? 0.0f : 1.0f;
  if (currentState == STATE_TRANSCRIBING) {
    tOpen *= 0.6f;                                 // thinking squint
    eyeTargetLookX = sinf(now / 220.0f) * 9.0f;    // scanning left/right
    eyeTargetLookY = 0;
  }

  // --- easing (close fast, open slightly slower) ---
  eyeOpen   += (tOpen - eyeOpen) * (eyeBlinking ? 0.65f : 0.35f);
  eyeLookX  += (eyeTargetLookX - eyeLookX) * 0.25f;
  eyeLookY  += (eyeTargetLookY - eyeLookY) * 0.25f;
  eyeHappy  += ((happy ? 1.0f : 0.0f) - eyeHappy) * 0.4f;
}

void drawEyes(int cy, float scale) {
  const int baseW = 26;
  const int baseH = 30;
  const int gap   = 22;

  int w        = max(6, (int)(baseW * scale));
  int hFull    = max(6, (int)(baseH * scale));
  int spacing  = (int)((baseW + gap) * scale);
  int ey       = cy + (int)(eyeLookY * scale);

  for (int i = 0; i < 2; i++) {
    int cx = 64 + (i == 0 ? -spacing / 2 : spacing / 2) + (int)(eyeLookX * scale);

    if (eyeHappy > 0.5f) {
      // "^" shaped smiling eye: thick upper half-circle arc
      int R = max(3, (int)(w * 0.42f));
      int thick = max(2, (int)(5 * scale));
      int arcY = ey + R / 2;
      for (int t = 0; t < thick; t++) {
        u8g2.drawCircle(cx, arcY, R - t, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      }
    } else {
      float o = constrain(eyeOpen, 0.0f, 1.0f);
      int h  = max(3, (int)(hFull * o));
      int ww = w + (int)((1.0f - o) * 4.0f * scale);     // slight squash when closing
      int r  = min((int)(9 * scale), min(ww, h) / 2);
      if (r < 1) r = 1;
      u8g2.drawRBox(cx - ww / 2, ey - h / 2, ww, h, r);
    }
  }
}

// ==================== DISPLAY RENDERING ====================
void drawTopBar() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "TARS");

  if (WiFi.status() == WL_CONNECTED) {
    u8g2.drawStr(34, 9, "[NET]");
  } else {
    u8g2.drawStr(34, 9, "[OFF]");
  }

  u8g2.drawHLine(0, 11, 128);
}

void drawWrappedText(const String &text, int startY, int maxLines) {
  u8g2.setFont(u8g2_font_6x10_tf);
  int len = text.length();
  int idx = 0;
  int line = 0;

  while (idx < len && line < maxLines) {
    int remaining = len - idx;
    int take = min((int)OLED_COLS, remaining);

    if (idx + take < len && text[idx + take] != ' ') {
      int lastSpace = text.lastIndexOf(' ', idx + take);
      if (lastSpace > idx) {
        take = lastSpace - idx;
      }
    }

    String lineStr = text.substring(idx, idx + take);
    lineStr.trim();
    u8g2.drawStr(0, startY + (line * 10), lineStr.c_str());

    idx += take;
    while (idx < len && text[idx] == ' ') idx++;
    line++;
  }
}

void renderHUD() {
  u8g2.clearBuffer();
  drawTopBar();

  if (currentState == STATE_HUD_IDLE) {
    updateEyes();
    drawEyes(33, 1.0f);

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(14, 62, "HOLD [PTT] TO SPEAK");
  }
  else if (currentState == STATE_LISTENING) {
    u8g2.drawFrame(2, 14, 124, 36);
    u8g2.drawHLine(4, 32, 120);

    for (int x = 0; x < WAVE_COLS; x++) {
      int h = waveHistory[x];
      if (h > 0) {
        u8g2.drawVLine(4 + x, 32 - h, (h * 2) + 1);
      }
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    float sec = (millis() - recordStartMs) / 1000.0f;
    char statusLine[28];
    if (sessionMaxPeak > 300) {
      snprintf(statusLine, sizeof(statusLine), "CH%d OK      %.1fs", activeI2SChannel, sec);
    } else {
      snprintf(statusLine, sizeof(statusLine), "SPEAK LOUD  %.1fs", sec);
    }
    u8g2.drawStr(2, 62, statusLine);
  }
  else if (currentState == STATE_TRANSCRIBING) {
    updateEyes();
    drawEyes(30, 0.9f);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(10, 52, sttStageLabel.c_str());
    char pkLine[22];
    snprintf(pkLine, sizeof(pkLine), "CH%d PEAK: %ld", activeI2SChannel, (long)sessionMaxPeak);
    u8g2.drawStr(10, 63, pkLine);
  }
  else if (currentState == STATE_THINKING) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 22, "YOU SAID -> AI:");
    drawWrappedText(lastHeardText, 34, 3);
  }
  else if (currentState == STATE_RESPONDING) {
    updateEyes();
    drawEyes(19, 0.4f);
    drawWrappedText(tarsReply, 33, 4);
  }

  u8g2.sendBuffer();
}

void renderMenu() {
  u8g2.clearBuffer();
  drawTopBar();
  u8g2.setFont(u8g2_font_6x10_tf);

  if (currentState == STATE_MENU) {
    for (int i = 0; i < MENU_COUNT; i++) {
      int y = 24 + (i * 12);
      if (i == menuIndex) {
        u8g2.drawBox(0, y - 9, 128, 11);
        u8g2.setDrawColor(0);
        u8g2.drawStr(4, y, menuItems[i]);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(4, y, menuItems[i]);
      }
    }
  }
  else if (currentState == STATE_SELF_TEST) {
    int32_t rawBuf[64] = {0};
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, rawBuf, sizeof(rawBuf), &bytesRead, 20);
    long magL = 0, magR = 0;
    int frames = bytesRead / (2 * sizeof(int32_t));
    for (int i = 0; i < frames; i++) {
      magL += abs(rawBuf[i * 2] >> INMP441_SHIFT);
      magR += abs(rawBuf[i * 2 + 1] >> INMP441_SHIFT);
    }
    if (frames > 0) {
      magL /= frames;
      magR /= frames;
    }
    long activeMag = max(magL, magR);

    char line1[24], line2[24], line3[24];
    snprintf(line1, sizeof(line1), "BTN:%d%d%d  FS:%s",
             isButtonHeld(PIN_BTN_1), isButtonHeld(PIN_BTN_2), isButtonHeld(PIN_BTN_3),
             fsReady ? "OK" : "ERR");
    snprintf(line2, sizeof(line2), "L:%ld R:%ld", magL, magR);
    snprintf(line3, sizeof(line3), "RAW:%08lX", (unsigned long)rawBuf[0]);

    u8g2.drawStr(0, 24, line1);
    u8g2.drawStr(0, 36, line2);
    u8g2.drawStr(0, 48, line3);

    int barW = constrain(map(activeMag, 0, 8000, 0, 128), 0, 128);
    u8g2.drawBox(0, 54, barW, 8);
  }
  else if (currentState == STATE_STATS) {
    char l1[24], l2[24], l3[28];
    snprintf(l1, sizeof(l1), "HUMOR:   %d%%", TARS_HUMOR_LEVEL);
    snprintf(l2, sizeof(l2), "HONESTY: %d%%", TARS_HONESTY_LEVEL);
    snprintf(l3, sizeof(l3), "RAM:%uK REC:%us", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)recCapSec);

    u8g2.drawStr(0, 25, l1);
    u8g2.drawStr(0, 38, l2);
    u8g2.drawStr(0, 51, l3);
    u8g2.drawStr(0, 63, "[BTN3] EXIT");
  }

  u8g2.sendBuffer();
}

// ==================== AUDIO CAPTURE (SPOOLED TO FLASH) ====================
// Records for as long as PTT is held (or until flash is full). Returns the number
// of 16-bit samples stored in REC_PATH.
size_t recordAudioWithDSP() {
  if (!fsReady) return 0;

  File f = LittleFS.open(REC_PATH, FILE_WRITE);   // truncates any old recording
  if (!f) return 0;

  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  size_t freeBytes = (total > used) ? (total - used) : 0;
  size_t maxBytes  = (freeBytes > FS_SAFETY_MARGIN) ? (freeBytes - FS_SAFETY_MARGIN) : 0;

  sessionMaxPeak = 0;
  memset(waveHistory, 0, sizeof(waveHistory));

  int32_t i2sStereoChunk[512]; // 256 stereo frames (Left + Right interleaved)
  size_t bytesRead = 0;

  // 1. Flush 250ms of startup frames (INMP441 wake-up transient) and pick the live channel
  i2s_zero_dma_buffer(I2S_NUM_0);
  unsigned long warmupStart = millis();
  uint64_t energyCh0 = 0;
  uint64_t energyCh1 = 0;

  while (millis() - warmupStart < 250) {
    if (i2s_read(I2S_NUM_0, i2sStereoChunk, sizeof(i2sStereoChunk), &bytesRead, 20) == ESP_OK && bytesRead > 0) {
      int frames = bytesRead / (2 * sizeof(int32_t));
      for (int i = 0; i < frames; i++) {
        energyCh0 += abs(i2sStereoChunk[i * 2] >> INMP441_SHIFT);
        energyCh1 += abs(i2sStereoChunk[i * 2 + 1] >> INMP441_SHIFT);
      }
    }
  }

  activeI2SChannel = (energyCh1 > energyCh0) ? 1 : 0;

  float prevX = 0.0f;
  float prevY = 0.0f;
  recordStartMs = millis();
  int renderTick = 0;

  size_t stagedSamples = 0;
  size_t bytesOnFlash = 0;
  bool full = false;

  auto flushStage = [&]() -> bool {
    if (stagedSamples == 0) return true;
    size_t bytes = stagedSamples * sizeof(int16_t);
    if (bytesOnFlash + bytes > maxBytes) return false;
    if (f.write((const uint8_t*)stagingBuf, bytes) != bytes) return false;
    bytesOnFlash += bytes;
    stagedSamples = 0;
    return true;
  };

  // 2. Capture the active mono channel until PTT is released or flash is full
  while (isButtonHeld(PIN_BTN_2) && !full) {
    if (i2s_read(I2S_NUM_0, i2sStereoChunk, sizeof(i2sStereoChunk), &bytesRead, 50) == ESP_OK && bytesRead > 0) {
      int framesInChunk = bytesRead / (2 * sizeof(int32_t));
      int32_t chunkPeak = 0;

      for (int i = 0; i < framesInChunk; i++) {
        int32_t rawWord = i2sStereoChunk[(i * 2) + activeI2SChannel];
        float x = (float)(rawWord >> INMP441_SHIFT);

        // 80Hz High-Pass DC-Blocking Filter
        float y = HPF_ALPHA * (prevY + x - prevX);
        prevX = x;
        prevY = y;

        int32_t sample = (int32_t)constrain(y, -32767.0f, 32767.0f);
        int32_t absSample = abs(sample);

        if (absSample > chunkPeak) chunkPeak = absSample;
        if (absSample > sessionMaxPeak) sessionMaxPeak = absSample;

        stagingBuf[stagedSamples++] = (int16_t)sample;

        if (stagedSamples == STAGE_SAMPLES) {
          if (!flushStage()) {   // flash full or write error
            full = true;
            break;
          }
        }
      }

      // Proportional oscilloscope scaling (0..12000 -> 0..15 px) so normal speech sits mid-scale
      memmove(waveHistory, waveHistory + 1, WAVE_COLS - 1);
      uint8_t barH = (uint8_t)constrain(map(chunkPeak, 0, 12000, 0, 15), 0, 15);
      waveHistory[WAVE_COLS - 1] = barH;

      // Redraw every 2nd chunk so I2C OLED refresh never starves the I2S DMA
      if ((++renderTick & 1) == 0) renderHUD();
    }
  }

  if (!full) flushStage();   // write the final partial block
  f.close();

  size_t totalSamples = bytesOnFlash / sizeof(int16_t);
  Serial.printf("[REC] %u samples (%.1fs) stored, peak=%ld, full=%d\n",
                (unsigned)totalSamples, totalSamples / (float)I2S_SAMPLE_RATE,
                (long)sessionMaxPeak, (int)full);

  // Gentle peak normalization (max 4x boost to 16000) is applied while uploading
  recordGain = 1.0f;
  if (sessionMaxPeak > 250 && sessionMaxPeak < 16000) {
    recordGain = 16000.0f / (float)sessionMaxPeak;
    if (recordGain > 4.0f) recordGain = 4.0f;
  }

  lastActivityMs = millis();
  return totalSamples;
}

// ==================== STAGE 1: GROQ WHISPER STT ====================
static String reportDrop(WiFiClientSecure &client, size_t sent, size_t total) {
  char errBuf[100] = {0};
  int e = client.lastError(errBuf, sizeof(errBuf));
  Serial.printf("[STT] DROPPED at %u/%u bytes, tls err=%d '%s', rssi=%d, heap=%u\n",
                (unsigned)sent, (unsigned)total, e, errBuf, WiFi.RSSI(), ESP.getFreeHeap());

  // The server may have answered early (401 bad key, 400, 413...)
  String early = "";
  while (client.available() && early.length() < 300) early += (char)client.read();
  Serial.println("[STT] early server reply: " + early);

  int code = 0;
  int sp = early.indexOf(' ');
  if (early.startsWith("HTTP/") && sp > 0) code = early.substring(sp + 1, sp + 4).toInt();

  client.stop();
  if (code > 0) return "ERR: STT HTTP " + String(code);
  return "ERR: DROP @" + String((unsigned)(sent / 1024)) + "K E" + String(e);
}

// Does the actual TLS upload, streaming the recording from flash. The I2S driver is
// uninstalled by the caller so its DMA buffers are free for Wi-Fi/TLS.
static String transcribeInner(size_t numSamples) {
  sttStageLabel = "TLS HANDSHAKE...";
  renderHUD();

  File f = LittleFS.open(REC_PATH, FILE_READ);
  if (!f) return "ERR: REC FILE MISSING";

  size_t pcmBytes = numSamples * sizeof(int16_t);
  if (f.size() < pcmBytes) pcmBytes = f.size() & ~((size_t)1);
  if (pcmBytes < 2) { f.close(); return ""; }

  Serial.printf("[STT] pre-TLS  heap free=%u maxblock=%u rssi=%d bytes=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI(), (unsigned)pcmBytes);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  if (!client.connect("api.groq.com", 443)) {
    char errBuf[100] = {0};
    int e = client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[STT] TLS connect FAIL err=%d %s\n", e, errBuf);
    f.close();
    return "ERR: STT TLS FAIL";
  }

  Serial.printf("[STT] post-TLS heap free=%u maxblock=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  WavHeader wav;
  wav.dataSize = pcmBytes;
  wav.fileSize = pcmBytes + sizeof(WavHeader) - 8;

  String boundary = "----TARSBoundary7MA4YWxkTrZu0gW";

  String partModel =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
    "whisper-large-v3-turbo\r\n";

  String partLang =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
    "en\r\n";

  String partFileHeader =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"tars.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  String partEnd = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = partModel.length() + partLang.length() + partFileHeader.length() +
                    sizeof(WavHeader) + pcmBytes + partEnd.length();

  sttStageLabel = "UPLOADING AUDIO...";
  renderHUD();

  // HTTP/1.0 forces Groq to close the socket immediately after sending the JSON body
  client.print("POST /openai/v1/audio/transcriptions HTTP/1.0\r\n");
  client.print("Host: api.groq.com\r\n");
  client.print("Authorization: Bearer " + String(GROQ_API_KEY) + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(totalLen) + "\r\n\r\n");

  client.print(partModel);
  client.print(partLang);
  client.print(partFileHeader);
  client.write((const uint8_t*)&wav, sizeof(WavHeader));

  // Stream the PCM from flash in 2 KB pieces, applying the normalization gain on the fly.
  static uint8_t upBuf[2048];
  size_t sent = 0;
  unsigned long lastWriteOk = millis();
  unsigned long uploadStart = millis();

  while (sent < pcmBytes) {
    size_t want = min(sizeof(upBuf), (size_t)(pcmBytes - sent));
    size_t got = f.read(upBuf, want);
    if (got < 2 || (got & 1)) {
      f.close();
      client.stop();
      return "ERR: REC READ";
    }

    if (recordGain != 1.0f) {
      int16_t* s = (int16_t*)upBuf;
      for (size_t i = 0; i < got / 2; i++) {
        int32_t boosted = (int32_t)(s[i] * recordGain);
        s[i] = (int16_t)constrain(boosted, -32767, 32767);
      }
    }

    size_t off = 0;
    while (off < got) {
      if (!client.connected()) {
        f.close();
        return reportDrop(client, sent + off, pcmBytes);
      }
      size_t written = client.write(upBuf + off, got - off);
      if (written > 0) {
        off += written;
        lastWriteOk = millis();
      } else {
        if (millis() - lastWriteOk > 10000) {
          Serial.printf("[STT] STALL at %u/%u bytes, rssi=%d, heap=%u\n",
                        (unsigned)(sent + off), (unsigned)pcmBytes, WiFi.RSSI(), ESP.getFreeHeap());
          f.close();
          client.stop();
          return "ERR: STT UPLOAD STALL";
        }
        delay(5);
      }
      yield();
    }
    sent += got;
  }
  f.close();

  client.print(partEnd);
  client.flush();

  Serial.printf("[STT] upload done: %u bytes in %lums\n", (unsigned)pcmBytes, millis() - uploadStart);

  sttStageLabel = "WAITING WHISPER...";
  renderHUD();

  // Wait up to 20s for Whisper server inference
  unsigned long timeout = millis();
  while (!client.available() && client.connected() && (millis() - timeout < 20000)) {
    delay(15);
  }
  if (!client.available()) {
    client.stop();
    return "ERR: STT TIMEOUT";
  }

  // Status line, then skip remaining HTTP response headers
  String statusLine = client.readStringUntil('\n');
  int httpCode = 0;
  int sp = statusLine.indexOf(' ');
  if (sp > 0) httpCode = statusLine.substring(sp + 1, sp + 4).toInt();
  Serial.println("[STT] status: " + statusLine);

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  // Read JSON body until server closes HTTP/1.0 socket
  String responseBody = "";
  unsigned long readStart = millis();
  while ((client.connected() || client.available()) && (millis() - readStart < 5000)) {
    while (client.available()) {
      responseBody += (char)client.read();
    }
    delay(5);
  }

  client.stop();

  int jsonStart = responseBody.indexOf('{');
  int jsonEnd   = responseBody.lastIndexOf('}');
  if (jsonStart == -1 || jsonEnd == -1) {
    return "ERR: STT PARSE " + String(httpCode);
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, responseBody.substring(jsonStart, jsonEnd + 1));
  if (err) {
    return "ERR: STT JSON";
  }

  if (httpCode != 200) {
    const char* msg = doc["error"]["message"];
    Serial.println("[STT] error body: " + responseBody);
    return "ERR: STT " + String(httpCode) + " " + String(msg ? msg : "");
  }

  const char* text = doc["text"];
  if (!text || strlen(text) == 0) {
    return "";
  }
  String cleanText = String(text);
  cleanText.trim();
  return cleanText;
}

String transcribeWithGroqWhisper(size_t numSamples) {
  // Fully release the I2S driver (frees the DMA buffers) for the duration of the
  // TLS upload so Wi-Fi + mbedTLS have as much contiguous heap as possible.
  i2s_driver_uninstall(I2S_NUM_0);

  String result = transcribeInner(numSamples);

  initI2S(); // bring the mic back for the next push-to-talk
  return result;
}

// ==================== STAGE 2: GROQ CHAT (WITH AUTO-MODEL DISCOVERY) ====================
String fetchFirstActiveChatModel() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.useHTTP10(true);
  if (!http.begin(client, "https://api.groq.com/openai/v1/models")) {
    return "";
  }

  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.setTimeout(12000);

  int code = http.GET();
  String resp = http.getString();
  http.end();
  client.stop();

  if (code != 200) return "";

  JsonDocument filter;
  filter["data"][0]["id"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) {
    return "";
  }

  String fallbackModel = "";
  for (JsonObject m : doc["data"].as<JsonArray>()) {
    const char* id = m["id"];
    if (!id) continue;

    if (strstr(id, "whisper") || strstr(id, "guard") ||
        strstr(id, "tts") || strstr(id, "playai") || strstr(id, "embed")) {
      continue;
    }

    if (fallbackModel.length() == 0) {
      fallbackModel = String(id);
    }
    if (strstr(id, "llama") || strstr(id, "qwen") || strstr(id, "gpt-oss")) {
      return String(id);
    }
  }
  return fallbackModel;
}

String askGroqTARS(const String &userText) {
  activeGroqModel.trim();

  for (int attempt = 0; attempt < 2; attempt++) {
    delay(150);

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(15);

    HTTPClient http;
    http.useHTTP10(true);
    if (!http.begin(client, "https://api.groq.com/openai/v1/chat/completions")) {
      return "ERR: LLM INIT FAIL";
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
    http.setTimeout(15000);

    JsonDocument reqDoc;
    reqDoc["model"] = activeGroqModel;
    reqDoc["max_tokens"] = 60;
    reqDoc["temperature"] = 0.7;

    JsonArray messages = reqDoc["messages"].to<JsonArray>();
    JsonObject sysMsg = messages.add<JsonObject>();
    sysMsg["role"] = "system";
    sysMsg["content"] = SYSTEM_PROMPT;

    JsonObject usrMsg = messages.add<JsonObject>();
    usrMsg["role"] = "user";
    usrMsg["content"] = userText;

    String payload;
    serializeJson(reqDoc, payload);

    int httpCode = http.POST(payload);
    String resp = http.getString();
    http.end();
    client.stop();

    if (httpCode == 404 && attempt == 0) {
      String discovered = fetchFirstActiveChatModel();
      if (discovered.length() > 0 && discovered != activeGroqModel) {
        activeGroqModel = discovered;
        continue;
      }
      return "ERR: NO CHAT MODELS ON KEY";
    }

    if (httpCode != 200) {
      if (httpCode <= 0) {
        return "TLS ERR (" + String(httpCode) + ") RAM:" + String(ESP.getFreeHeap() / 1024) + "K";
      }
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, resp) && errDoc["error"]["message"].is<const char*>()) {
        return "ERR " + String(httpCode) + ": " + errDoc["error"]["message"].as<String>();
      }
      return "HTTP ERR: " + String(httpCode);
    }

    JsonDocument resDoc;
    if (deserializeJson(resDoc, resp)) {
      return "ERR: LLM JSON";
    }

    const char* reply = resDoc["choices"][0]["message"]["content"];
    if (!reply) return "NO RESPONSE DATA.";

    String cleanReply = String(reply);
    cleanReply.trim();
    return cleanReply;
  }

  return "ERR: LLM RETRY FAIL";
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_BTN_1, INPUT_PULLUP);
  pinMode(PIN_BTN_2, INPUT_PULLUP);
  pinMode(PIN_BTN_3, INPUT_PULLUP);

  while (digitalRead(PIN_BTN_1) == LOW) {
    delay(10);
  }

  u8g2.begin();
  u8g2.setPowerSave(0);

  // First boot formats the flash filesystem, which can take a few seconds
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(16, 34, "INIT STORAGE...");
  u8g2.sendBuffer();

  fsReady = LittleFS.begin(true);   // true = format if mount fails
  updateRecordingCapacity();
  Serial.printf("[BOOT] fs=%s rec_cap=%us heap free=%u maxblock=%u\n",
                fsReady ? "OK" : "FAIL", (unsigned)recCapSec,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  initI2S();

  if (!fsReady) {
    tarsReply = "STORAGE FAIL";
  }

  ensureWiFiConnected();
  lastActivityMs = millis();
}

// ==================== MAIN LOOP ====================
void loop() {
  bool p1 = updateButton(btn1);
  bool p2 = updateButton(btn2);
  bool p3 = updateButton(btn3);

  if (p3) {
    if (currentState == STATE_MENU || currentState == STATE_SELF_TEST || currentState == STATE_STATS) {
      currentState = STATE_HUD_IDLE;
    } else {
      currentState = STATE_MENU;
    }
  }

  // ==================== MENU & SUB-SCREENS ====================
  if (currentState == STATE_MENU) {
    if (p1) {
      menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
    }
    if (p2) {
      if (menuIndex == 0) currentState = STATE_SELF_TEST;
      else if (menuIndex == 1) {
        ensureWiFiConnected();
        currentState = STATE_HUD_IDLE;
      }
      else if (menuIndex == 2) currentState = STATE_STATS;
      else if (menuIndex == 3) enterDeepSleep();
    }
    renderMenu();
    delay(25);
    return;
  }

  if (currentState == STATE_SELF_TEST || currentState == STATE_STATS) {
    renderMenu();
    delay(25);
    return;
  }

  // ==================== HUD & VOICE ASSISTANT ====================
  if (p1) {
    enterDeepSleep();
  }

  // After a recording that ended while PTT was still held (flash full), wait for release
  if (requireRelease && !isButtonHeld(PIN_BTN_2)) requireRelease = false;

  if (isButtonHeld(PIN_BTN_2) && !requireRelease) {
    currentState = STATE_LISTENING;
    size_t samples = recordAudioWithDSP();
    if (isButtonHeld(PIN_BTN_2)) requireRelease = true;

    if (samples > (I2S_SAMPLE_RATE / 3)) {
      currentState = STATE_TRANSCRIBING;
      renderHUD();

      if (ensureWiFiConnected()) {
        String transcript = transcribeWithGroqWhisper(samples);
        Serial.println("Heard: " + transcript);

        if (transcript.length() > 0 && !transcript.startsWith("ERR:")) {
          lastHeardText = transcript;
          currentState = STATE_THINKING;
          renderHUD();

          tarsReply = askGroqTARS(transcript);
        } else if (transcript.length() == 0) {
          tarsReply = "UNCLEAR CH" + String(activeI2SChannel) + " PK:" + String(sessionMaxPeak);
        } else {
          tarsReply = transcript;
        }
      } else {
        tarsReply = "UPLINK OFFLINE.";
      }

      currentState = STATE_RESPONDING;
      lastActivityMs = millis();
    } else {
      currentState = STATE_HUD_IDLE;
    }

    if (fsReady) LittleFS.remove(REC_PATH);
    updateRecordingCapacity();
  }

  if (currentState == STATE_RESPONDING && (millis() - lastActivityMs > 12000)) {
    currentState = STATE_HUD_IDLE;
  }

  if (millis() - lastActivityMs > IDLE_SLEEP_TIMEOUT_MS) {
    enterDeepSleep();
  }

  renderHUD();
  delay(25);
}