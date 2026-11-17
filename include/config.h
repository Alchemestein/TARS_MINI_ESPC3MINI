/**
 * TARS - Hardware Pinout & Tactical Configuration
 * MCU: ESP32-C3 Super Mini
 */
#ifndef CONFIG_H
#define CONFIG_H

// ==================== MASTER PINOUT ====================
// Battery Voltage ADC (100k / 100k Divider)
#define PIN_BATTERY_ADC       1

// Display: 1.3" I2C OLED (SH1106 / SSD1306)
#define PIN_I2C_SDA           8
#define PIN_I2C_SCL           9
#define OLED_USE_SSD1306      0
#define OLED_COLS             21

// Microphone: INMP441 Digital I2S
#define PIN_I2S_WS            7   // Word Select (LRCK)
#define PIN_I2S_SCK           6   // Serial Clock (BCLK)
#define PIN_I2S_SD            10  // Serial Data (DIN)

// Tactical Buttons (Active LOW - INPUT_PULLUP)
// Normal Mode:
//   BTN_1: Power / Deep Sleep Wake (GPIO 3, RTC Wake)
//   BTN_2: Push-To-Talk Mic (GPIO 4)
//   BTN_3: Enter Graphical Menu / Self-Test (GPIO 5)
// Menu Mode:
//   BTN_1: Previous Item / Scroll Up
//   BTN_2: Next Item / Select
//   BTN_3: Exit Menu -> Monolith HUD
#define PIN_BTN_1             3   // Power / RTC Wake / Menu Prev
#define PIN_BTN_2             4   // Push-To-Talk / Menu Next & Select
#define PIN_BTN_3             5   // Menu Toggle / Exit / Self-Test

// ==================== AUDIO DSP & NOISE FILTER ====================
#define I2S_SAMPLE_RATE       16000
#define I2S_BUFFER_COUNT      6
#define I2S_BUFFER_LEN        512
#define AUDIO_CAPTURE_MAX_MS  4000
#define AUDIO_GAIN_SHIFT      14
#define BUTTON_DEBOUNCE_MS    40
#define NOISE_GATE_THRESHOLD  (-42)
#define HPF_CUTOFF_HZ         (80)

// ==================== TARS PARAMETERS ====================
#define TARS_HUMOR_LEVEL      (75)
#define TARS_HONESTY_LEVEL    (90)
#define IDLE_SLEEP_TIMEOUT_MS (40000)
#define BATTERY_DIVIDER_RATIO (2.00)
#define WIFI_CONNECT_TIMEOUT_MS 15000

#endif // CONFIG_H