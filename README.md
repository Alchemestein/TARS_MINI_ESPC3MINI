# TARS - ESP32-C3 Monolithic Tactical AI Assistant (Interstellar Edition)
//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

A keychain-sized tactical AI companion inspired by **TARS from Interstellar**. Features a 2-eye tactical robotic aperture, I2S microphone DSP noise reduction, 3-button tactical menu navigation, and direct Google Gemini 2.5 Flash REST API integration.

## ⚠️ Solving "21 Errors" in VS Code (Clang False Positives)
If the VS Code "Problems" tab shows errors like `clang: In included file: 'algorithm' file not found` or `Unknown type name 'U8G2...'`:
- **Why this happens:** The VS Code **Clang / clangd** extension is attempting to analyze embedded RISC-V code using your host computer's desktop compiler before PlatformIO downloads the ESP32 toolchain.
- **How to fix in 10 seconds:**
  1. Click the **PlatformIO: Build (✓)** icon in the bottom-left blue status bar (or press `Ctrl+Alt+B` / `Cmd+Shift+P` > `PlatformIO: Build`).
  2. PlatformIO will download the ESP32-C3 packages and automatically configure `.vscode/c_cpp_properties.json`.
  3. Ensure `.vscode/settings.json` has `"clangd.enabled": false` (pre-included in this project).

## 🛠️ Hardware Requirements
- **Microcontroller:** ESP32-C3 Super Mini
- **Display:** 1.3" OLED (SH1106 or SSD1306, I2C)
- **Microphone:** INMP441 Digital I2S Microphone (with DSP Noise Gate & DC filter)
- **Battery:** 3.7V 500mAh LiPo + TP4056 USB-C Charger
- **Resistors:** 2x 100kΩ (Voltage divider for battery monitoring)
- **Buttons:** 3x Tactical pushbuttons (Active LOW)

## 📌 Master Pinout Wiring
| Component | Pin | ESP32-C3 Super Mini | Role & Notes |
|-----------|-----|---------------------|--------------|
| **Battery ADC** | Voltage Divider Node | **GPIO 1** | 100kΩ to LiPo (+), 100kΩ to GND (≤ 2.1V) |
| **OLED (I2C)** | SDA | **GPIO 8** | Regulated 3.3V logic |
| **OLED (I2C)** | SCL | **GPIO 9** | Regulated 3.3V logic |
| **INMP441 Mic**| WS | **GPIO 7** | Word Select (LRCLK) |
| **INMP441 Mic**| SCK | **GPIO 6** | Serial Clock (BCLK) |
| **INMP441 Mic**| SD | **GPIO 10** | Serial Data (DIN) |
| **INMP441 Mic**| L/R | **GND** | Left audio channel |
| **Button 1** | Power / Wake / Prev | **GPIO 3** | Normal: Sleep/Wake | Menu: Prev Item |
| **Button 2** | Push-To-Talk / Next | **GPIO 4** | Normal: Mic PTT | Menu: Next / Select |
| **Button 3** | Tactical Menu / Exit | **GPIO 5** | Normal: Enter Menu | Menu: Exit to HUD |

## 📁 PlatformIO Project Folder Structure
You can place `config.h` and `secrets.h` in **either** the `include/` folder or the `src/` folder:

```text
TARS-Project/
├── platformio.ini          <-- PlatformIO configuration in root
├── .gitignore              <-- Protects secrets.h
├── README.md
├── .vscode/
│   └── settings.json       <-- Fixes Clang IntelliSense conflicts
├── include/                <-- Recommended for all .h headers!
│   ├── config.h            <-- Pinout & hardware constants
│   └── secrets.h           <-- Wi-Fi & Gemini API Key (KEEP PRIVATE)
└── src/
    └── main.cpp            <-- TARS state machine & firmware logic
```

## 🎙️ Digital Noise Filter Pipeline (INMP441)
The firmware implements real-time DSP routines on incoming I2S audio buffers:
1. **80Hz Single-Pole IIR High-Pass Filter**: Strips constant DC bias and low-frequency handling rumble.
2. **Spectral Noise Gate (-42dB)**: Silences ambient background noise and room hiss when the user is not actively speaking.
3. **Automatic Gain Control (AGC)**: Normalizes speech volume before serializing to Google Gemini.

## 🔑 How to Get & Configure Your Google Gemini API Token
1. Go to **Google AI Studio**: [https://aistudio.google.com/app/apikey](https://aistudio.google.com/app/apikey)
2. Click **Create API Key**.
3. Choose or create a Google Cloud Project and copy your API key (starts with `AIzaSy...`).
4. Paste it into `secrets.h` as `#define GEMINI_API_KEY "YOUR_KEY"`.
5. Enter your phone's Wi-Fi Hotspot credentials in `secrets.h`.

## 🚀 How to Build & Flash (PlatformIO)
1. Install **VS Code** with the **PlatformIO IDE** extension.
2. Open this folder in PlatformIO.
3. Connect the ESP32-C3 Super Mini via USB-C.
4. Click **PlatformIO: Build** (✓) and then **PlatformIO: Upload** (→).
5. Open Serial Monitor at 115200 baud to view debug logs.
