# SIGNAL

**An off-grid emergency signaling device built on the ESP32.**

SIGNAL is a single-button, battery-powered device that helps you signal for help when public infrastructure — cell towers, wifi, the internet — isn't available. It paces out Morse code for you on an OLED display so you can flash a flashlight, bang on metal, or key a radio in correctly timed code without knowing Morse yourself, and it broadcasts a peerless 2.4GHz distress beacon over ESP-NOW to any other SIGNAL device nearby.

No pairing, no access point, no network required — it works because the grid isn't there, not in spite of it.

---

## Features

- **One-button interface** — single click, double click, and long press are all it takes to operate; no menus buried behind multiple controls
- **Morse pacer & torch guide** — converts preset emergency messages into precisely timed Morse, with a synchronized on-screen light guide so anyone can signal correctly under pressure
- **7 built-in presets** — SOS, MED, FIRE, LOST, HERE, OK, and a dedicated 2.4G Beacon mode
- **ESP-NOW distress beacon** — broadcasts a structured emergency packet every 2.5 seconds to any nearby ESP-NOW listener, with no router or internet connection needed
- **Optional buzzer** — audible tone synced to the Morse timing; fully optional, the visual guide works standalone
- **Non-blocking firmware** — button input, Morse playback, and beacon transmission are all driven by `millis()`-based state machines, so nothing locks up waiting on a `delay()`

---

## Hardware

| Component | Notes |
|---|---|
| ESP32 Dev Module | Any standard 30/38-pin dev board |
| 0.96" SSD1306 OLED, I2C, 128x64 | Address `0x3C` (some modules ship on `0x3D` — check yours) |
| Momentary tactile push button | The only input on the device |
| Passive or active buzzer | Optional — toggle in firmware, see below |
| LiPo battery + charge/boost module | Optional, for untethered operation |

### Wiring

| Component | Pin | ESP32 |
|---|---|---|
| OLED | VCC | 3V3 |
| OLED | GND | GND |
| OLED | SDA | GPIO 21 |
| OLED | SCL | GPIO 22 |
| Button | Leg 1 | GPIO 4 |
| Button | Leg 2 | GND |
| Buzzer | + | GPIO 19 |
| Buzzer | − | GND |

The button uses the ESP32's internal pull-up resistor — no external resistor needed.

---

## Getting started

### 1. Install the Arduino IDE
Download the latest version from [arduino.cc](https://www.arduino.cc/en/software).

### 2. Add ESP32 board support
`File → Preferences → Additional Boards Manager URLs`:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Then install the **esp32** package via `Tools → Board → Boards Manager`.

### 3. Install the U8g2 library
`Sketch → Include Library → Manage Libraries` → search **U8g2** → install the library by **oliver (olikraus)**.

### 4. Configure the buzzer type
Near the top of `SIGNAL.ino`:
```cpp
#define BUZZER_IS_PASSIVE true   // false = active buzzer
```

### 5. Flash it
Select your board under `Tools → Board`, select the correct port, and hit upload. On first reset, the OLED should show the SIGNAL splash screen followed by the main menu.

---

## Usage

| Action | Effect |
|---|---|
| **Single click** | Advance to the next menu item |
| **Long press (600ms+)** | Select and activate the highlighted preset or mode |
| **Double click** | Cancel playback/broadcast and return to the main menu |

**Morse pacer mode** loops the selected preset continuously. The OLED alternates between a filled "LIGHT ON" block and an outlined "LIGHT OFF" box, synced exactly to the Morse timing — hold a flashlight and toggle it in time with the display.

**2.4G Beacon mode** puts the device into continuous ESP-NOW broadcast, sending a distress packet (device name, status, packet counter) to `FF:FF:FF:FF:FF:FF` every 2.5 seconds. The OLED shows a live transmission counter and TX status.

---

## How it works

- **Button engine** — non-blocking debounce/state machine distinguishing single click, double click, and long press purely via `millis()` timing, so input is never locked out
- **Morse engine** — each preset is expanded once into a flat sequence of dot/dash/gap symbols using an ITU-standard lookup table, then played back on a timed loop that drives the buzzer and light guide in sync
- **Display** — rendered with U8g2 over I2C; all screens (splash, menu, Morse pacer, beacon status) redraw on a lightweight refresh loop rather than being static
- **ESP-NOW beacon** — initializes the ESP32 WiFi radio in station mode with no access point, and sends a packed struct to the broadcast address on a fixed interval
- **State machine** — a single central state (splash → menu → Morse playback → beacon) governs transitions, with each state owning its own update and render logic

---

## Extending SIGNAL

- Swap the enclosure — the same wiring and firmware work in a cardboard, PVC, or 3D-printed shell
- Add new presets by editing the message table — no restructuring required
- Build a dedicated receiver unit to listen for and log incoming beacon packets
- Add solar charging ahead of the LiPo boost module for multi-day off-grid use

---

## License

*(Add your chosen license here — e.g. MIT, GPLv3.)*
