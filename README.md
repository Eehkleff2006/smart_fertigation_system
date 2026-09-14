# Eehkleff Smart Fertigation System — v4

Dual-mode (sensor / time / combined) fertigation controller for ESP32, with
Firebase Realtime Database sync, OLED status display, and an MIT App
Inventor companion app.

## Hardware
- ESP32 Dev Board
- Capacitive soil moisture sensor → GPIO34
- DHT11 temp/humidity → GPIO4
- SSD1306 OLED 128x64 (I2C) → GPIO21 (SDA), GPIO22 (SCL)
- DS1302 RTC → CLK=GPIO18, DAT=GPIO19, RST=GPIO5
- 2-channel relay module
  - Relay 1 → Pump 1 (water tank outlet) → GPIO25
  - Relay 2 → Pump 2 (fertilizer tank outlet) → GPIO26
- 2x 18650 Li-ion cells in series → buck converter (min. 2A) → 5V

## Libraries (Arduino Library Manager)
- Firebase ESP32 Client (Mobizt) v4.4.17
- DHT sensor library (Adafruit)
- Adafruit GFX Library
- Adafruit SSD1306
- Rtc by Makuna (DS1302)
- NTPClient (Fabrice Weinberg)

## Setup
1. Copy `secrets.h.example` to `secrets.h`.
2. Fill in your real WiFi SSID/password and Firebase host/auth token in `secrets.h`.
3. `secrets.h` is gitignored — it will stay local and never get pushed.
4. Open `fertigation_v4.ino` in Arduino IDE and flash to the ESP32.

## Status
Work in progress — MIT App Inventor companion app blocks phase still incomplete.
