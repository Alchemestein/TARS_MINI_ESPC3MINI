<img width="1280" height="960" alt="WhatsApp Image 2026-11-17 at 8 06 36 PM" src="https://github.com/user-attachments/assets/6b60eac7-d428-4c5d-8cd7-d19440f92a66" />
# TARS — ESP32-C3 Pocket AI Voice Assistant

A keychain-sized robotic companion inspired by **TARS from Interstellar**, built on an **ESP32-C3 Super Mini**. It combines animated OLED eyes, real-time audio waveform rendering, flash-spooled I2S voice recording, and cloud speech-to-text + LLM inference via **Groq** (`whisper-large-v3-turbo` + `qwen/qwen3.8-27b`).

---

## Features
- **Expressive OLED Eye Engine:** Smoothly eased rounded eyes that blink (including occasional double-blinks), glance around, squint and scan side-to-side while transcribing, shrink to the top bar when displaying replies, and smile (`^ ^`) after responding.
- **LittleFS Flash-Spooled Recording:** Streams 16-bit PCM audio through a 4 KB RAM staging buffer directly to LittleFS flash while Push-To-Talk is held—allowing long recordings limited only by free flash instead of ESP32-C3 SRAM.
- **I2S Audio DSP & Heap Optimization:**
  - Auto-detects the active INMP441 slot (`CH0` or `CH1`) during a 250 ms startup flush.
  - Filters DC offset and handling rumble with an **80 Hz 1st-order high-pass filter**.
  - Renders a live 120-column oscilloscope waveform on the OLED while recording.
  - Applies peak normalization (up to `4.0x` toward a `16000` peak) on the fly during upload.
  - Temporarily uninstalls the I2S driver during TLS transfers so Wi-Fi and mbedTLS have maximum contiguous heap.
- **Groq Cloud AI Pipeline:**
  - **Speech-to-Text:** Streams WAV audio from flash to `whisper-large-v3-turbo`.
  - **LLM Response:** Sends transcribed text to `qwen/qwen3.8-27b` (with automatic active-model fallback via `/openai/v1/models`).
- **Tactical Menu & Deep Sleep:** Built-in hardware self-test (live mic channel levels & button states), manual Wi-Fi reconnect, RAM/storage/personality diagnostics, and auto/manual deep sleep with GPIO wake.

---

## Hardware Required
- **Microcontroller:** ESP32-C3 Super Mini
- **Display:** 0.96" (`SSD1306`) or 1.3" (`SH1106`) 128×64 I2C OLED
- **Microphone:** INMP441 Omnidirectional I2S MEMS Microphone
- **Buttons:** 3× Tactile Pushbuttons (wired active-LOW to GND)
- **Portable Power (Optional):** 3.7V 500mAh LiPo Battery + TP4056 USB-C Charging Module (with protection IC)

---

## Pinout Wiring

### Core Components

| Component | Module Pin | ESP32-C3 Pin | Notes |
| :--- | :--- | :--- | :--- |
| **OLED Display** | `SDA` | **GPIO 8** | I2C Data (3.3V logic) |
| **OLED Display** | `SCL` | **GPIO 9** | I2C Clock (3.3V logic) |
| **OLED Display** | `VCC` / `GND` | **3V3** / **GND** | — |
| **INMP441 Mic** | `WS` | **GPIO 7** | Word Select (LRCLK) |
| **INMP441 Mic** | `SCK` | **GPIO 6** | Serial Clock (BCLK) |
| **INMP441 Mic** | `SD` | **GPIO 10** | Serial Data (DOUT) |
| **INMP441 Mic** | `L/R` | **GND** | Selects Left slot (auto-detected in firmware) |
| **INMP441 Mic** | `VDD` / `GND` | **3V3** / **GND** | — |
| **Button 1** | Signal | **GPIO 3** | HUD: Enter Deep Sleep / Wake • Menu: Move Up |
| **Button 2** | Signal | **GPIO 4** | HUD: Hold to Talk (PTT) • Menu: Select |
| **Button 3** | Signal | **GPIO 5** | HUD: Open Menu • Menu/Sub-screens: Exit to HUD |

> **Button Wiring:** Connect one pin of each pushbutton to its assigned GPIO and the opposite pin to **GND**. Internal pull-ups (`INPUT_PULLUP`) are enabled in firmware.

### Optional Portable Power (3.7V 500mAh LiPo + TP4056)

| From | To | Notes |
| :--- | :--- | :--- |
| **LiPo Red (`+`)** | **TP4056 `B+`** | Battery positive terminal |
| **LiPo Black (`-`)** | **TP4056 `B-`** | Battery negative terminal *(if using a 3-wire LiPo, leave the white thermistor wire disconnected and insulated)* |
| **TP4056 `OUT+`** | **ESP32-C3 `5V`** | Feeds the ESP32-C3 onboard 3.3V LDO regulator *(optional: wire a miniature SPDT slide switch in series to cut power completely)* |
| **TP4056 `OUT-`** | **ESP32-C3 `GND`** | Common ground |

> **Charging Safety Note:** Use a **TP4056 board with built-in battery protection** (DW01A + 8205A MOSFETs) and charge via the TP4056 USB-C port. Avoid plugging USB cables into both the TP4056 and the ESP32-C3 at the same time unless you place a Schottky diode (`1N5819`) in series between `OUT+` and `5V`.

---

## Tactical Menu & Controls
- **Hold Button 2 (`GPIO 4`):** Records voice until released (or until LittleFS storage reaches its safety limit), then transcribes and displays TARS's reply.
- **Press Button 1 (`GPIO 3`):** Puts the ESP32-C3 into deep sleep (`POWERING DOWN...`) to save battery. Press Button 1 again to wake.
- **Press Button 3 (`GPIO 5`):** Opens the 4-item Tactical Menu:
  1. **HW Self-Test:** Displays live button states (`BTN`), LittleFS mount status (`FS`), Left/Right I2S signal magnitudes, raw I2S word, and an audio level bar.
  2. **Wi-Fi Reconnect:** Scans 2.4 GHz networks and reconnects to your configured SSID.
  3. **TARS Diagnostics:** Shows Humor %, Honesty %, free heap (`RAM`), and maximum recording duration (`REC`) based on available flash.
  4. **Enter Deep Sleep:** Powers down OLED, I2S, and Wi-Fi until woken via `GPIO 3`.

---

## Project Structure

```text
esp32-c3-tars/
├── platformio.ini            # PlatformIO board, partition & library config
├── .gitignore                # Excludes .pio/ build artifacts and include/secrets.h
├── README.md
├── include/
│   ├── config.h              # GPIO pins, I2S sample rate, HPF cutoff, and UI settings
│   ├── secrets.example.h     # Template for Wi-Fi & Groq credentials
│   └── secrets.h             # Your private credentials (gitignored)
└── src/
    └── main.cpp              # State machine, LittleFS recorder, OLED eye engine & Groq API
