# OpenCatESP32

OpenCat code running on [BiBoard, a high-performance ESP32 quadruped robot development board](https://www.petoi.com/products/biboard-esp32-development-board-for-quadruped-robot). The board is mainly designed for developers and engineers working on multi-degree-of-freedom (MDOF) Multi-legged robots with up to 12 servos. 

This fork adds **WiFi web control**, **real-time telemetry**, and **Nintendo Switch Pro Controller** support via BluePad32.

[User manual](https://docs.petoi.com/arduino-ide/upload-sketch-for-biboard)

---

## New Features

### WiFi Web Server & Real-Time Console
- **HTTP Server (port 80)** with a full terminal-style web console
- **WebSocket Server (port 81)** for real-time bidirectional communication
- Live servo position visualization (top-down robot body diagram)
- Real-time telemetry: battery voltage, IMU orientation (yaw/pitch/roll), uptime, heap
- Comprehensive command help panel built into the UI
- All serial output mirrored to the web console
- Works on desktop and mobile browsers

### Nintendo Switch Pro Controller (BluePad32)
- Wireless Bluetooth gamepad control using [BluePad32](https://github.com/nicoya/bluepad32)
- 8-direction left stick movement:
  - Up: walk forward (`wkF`)
  - Down: walk backward (`bkF`)
  - Left: turn left (`trL`)
  - Right: turn right (`trR`)
  - Diagonals: walk turning (`wkL`, `wkR`, `bkL`, `bkR`)
- Right stick: head pan/tilt control
- D-pad: discrete head up/down/left/right
- Buttons mapped to skills: A=stand, B=sit, X=stretch, Y=greeting, L=trot, R=crawl, ZL=push up, ZR=special
- Live gamepad status in web UI (stick positions, button highlights, last command)
- Automatic pairing — hold Sync button on controller

### Enhanced Web UI
- Interactive gamepad panel with SVG stick visualizations that update in real-time
- Button grid lights up when controller buttons are pressed
- Connection status indicators for both WiFi and gamepad
- Quick command buttons (Sit, Up, Rest, Help)
- Joint angle color coding (green/yellow/red by magnitude)

---

## Configuration

ESP32 Dev Module (or BluePad32 for Arduino → ESP32 Dev Module if using gamepad)

* Upload Speed: 921600

* CPU Frequency: 240MHz(WiFi/BT)

* Flash Frequency: 80MHz

* Flash Mode: QIO

* Flash Size: 16MB

* Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)

* Core Debug Level: None

* PSRAM: Disabled

### Build Options (in `OpenCatEsp32.ino` and `src/OpenCat.h`)

| Define | Purpose |
|--------|---------|
| `BLUEPAD32` | Enable Nintendo Switch Pro Controller via BluePad32 |
| `BT_BLE` | Standard BLE UART (disabled when using BluePad32) |
| `BT_SSP` | Classic Bluetooth SSP (disabled when using BluePad32) |
| `WEB_SERVER` | WiFi HTTP + WebSocket server |
| `VOICE` | Petoi Grove voice module |
| `DOUBLE_TOUCH` | Double touch sensor |
| `QUICK_DEMO` | Quick demo mode |

### BluePad32 Setup

1. Add board URL: `https://raw.githubusercontent.com/nicoya/bluepad32/main/package_bluepad32_index.json`
2. Install "BluePad32 boards" from Board Manager
3. Select **Tools → Board → BluePad32 for Arduino → ESP32 Dev Module**
4. Set partition scheme to "Huge APP (3MB No OTA/1MB SPIFFS)"


[![BittleESP32](https://github.com/PetoiCamp/NonCodeFiles/blob/master/gif/BiBoard.gif)](https://www.youtube.com/watch?v=GTgps_H990w)

[![BittleGap](https://github.com/PetoiCamp/NonCodeFiles/blob/master/gif/gap.gif)](https://youtu.be/1qhNRSQTcG4)

Click the GIF to open the YouTube demo.

