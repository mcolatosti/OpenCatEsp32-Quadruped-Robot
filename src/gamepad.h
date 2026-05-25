#ifndef OPENCAT_GAMEPAD_H
#define OPENCAT_GAMEPAD_H

#ifdef BLUEPAD32

#include <Bluepad32.h>

// Maximum number of gamepads supported simultaneously
#define MAX_GAMEPADS 1

ControllerPtr myControllers[MAX_GAMEPADS];

// Gamepad state tracking to avoid repeated commands
static bool gamepadConnected = false;
static unsigned long lastGamepadUpdate = 0;
static const unsigned long GAMEPAD_UPDATE_INTERVAL = 100;  // ms between input reads

// Deadzone for analog sticks (Switch Pro sticks range roughly -512 to 512)
#define STICK_DEADZONE 80
// Threshold for triggering a direction
#define STICK_THRESHOLD 200

// Track previous direction to avoid spamming the same command
static int8_t prevLeftX = 0;
static int8_t prevLeftY = 0;
static uint16_t prevButtons = 0;
static bool isMoving = false;

// Forward declarations
void sendGamepadDisconnected();
static String lastGamepadCmd = "--";

// --- Callbacks ---

void onConnectedController(ControllerPtr ctl) {
  for (int i = 0; i < MAX_GAMEPADS; i++) {
    if (myControllers[i] == nullptr) {
      Serial.printf("[Gamepad] Connected, index=%d\n", i);
      myControllers[i] = ctl;
      gamepadConnected = true;
      printToAllPorts("[Gamepad] Nintendo Switch controller connected!");
      return;
    }
  }
  Serial.println("[Gamepad] No slot available, rejecting connection");
}

void onDisconnectedController(ControllerPtr ctl) {
  for (int i = 0; i < MAX_GAMEPADS; i++) {
    if (myControllers[i] == ctl) {
      Serial.printf("[Gamepad] Disconnected, index=%d\n", i);
      myControllers[i] = nullptr;
      gamepadConnected = false;
      isMoving = false;
      lastGamepadCmd = "--";
      printToAllPorts("[Gamepad] Controller disconnected");
      sendGamepadDisconnected();
      return;
    }
  }
}

// --- Input Processing ---

// Convert left stick to 8-direction movement
// Returns: 0=none, 1=forward, 2=backward, 3=left, 4=right,
//          5=forward-left, 6=forward-right, 7=backward-left, 8=backward-right
int8_t getStickDirection(int16_t x, int16_t y) {
  if (abs(x) < STICK_DEADZONE && abs(y) < STICK_DEADZONE)
    return 0;  // center/deadzone

  bool up = (y < -STICK_THRESHOLD);
  bool down = (y > STICK_THRESHOLD);
  bool left = (x < -STICK_THRESHOLD);
  bool right = (x > STICK_THRESHOLD);

  // Diagonals
  if (up && left) return 5;   // forward-left
  if (up && right) return 6;  // forward-right
  if (down && left) return 7; // backward-left
  if (down && right) return 8;// backward-right

  // Cardinals
  if (up) return 1;     // forward
  if (down) return 2;   // backward
  if (left) return 3;   // turn left
  if (right) return 4;  // turn right

  return 0;
}

void processGamepadStick(ControllerPtr ctl) {
  int16_t lx = ctl->axisX();     // Left stick X
  int16_t ly = ctl->axisY();     // Left stick Y

  int8_t dir = getStickDirection(lx, ly);

  // Only send command when direction changes
  int8_t mappedX = (abs(lx) > STICK_DEADZONE) ? (lx > 0 ? 1 : -1) : 0;
  int8_t mappedY = (abs(ly) > STICK_DEADZONE) ? (ly > 0 ? 1 : -1) : 0;

  if (mappedX == prevLeftX && mappedY == prevLeftY)
    return;  // No change

  prevLeftX = mappedX;
  prevLeftY = mappedY;

  switch (dir) {
    case 0:  // Neutral - stop
      if (isMoving) {
        tQueue->addTask(T_SKILL, "up");
        isMoving = false;
        lastGamepadCmd = "up";
      }
      break;
    case 1:  // Forward
      tQueue->addTask(T_SKILL, "wkF");
      isMoving = true;
      lastGamepadCmd = "wkF";
      break;
    case 2:  // Backward
      tQueue->addTask(T_SKILL, "bkF");
      isMoving = true;
      lastGamepadCmd = "bkF";
      break;
    case 3:  // Turn Left
      tQueue->addTask(T_SKILL, "trL");
      isMoving = true;
      lastGamepadCmd = "trL";
      break;
    case 4:  // Turn Right
      tQueue->addTask(T_SKILL, "trR");
      isMoving = true;
      lastGamepadCmd = "trR";
      break;
    case 5:  // Forward-Left (walk turning left)
      tQueue->addTask(T_SKILL, "wkL");
      isMoving = true;
      lastGamepadCmd = "wkL";
      break;
    case 6:  // Forward-Right (walk turning right)
      tQueue->addTask(T_SKILL, "wkR");
      isMoving = true;
      lastGamepadCmd = "wkR";
      break;
    case 7:  // Backward-Left
      tQueue->addTask(T_SKILL, "bkL");
      isMoving = true;
      lastGamepadCmd = "bkL";
      break;
    case 8:  // Backward-Right
      tQueue->addTask(T_SKILL, "bkR");
      isMoving = true;
      lastGamepadCmd = "bkR";
      break;
  }
}

void processGamepadButtons(ControllerPtr ctl) {
  uint16_t buttons = ctl->buttons();

  // Only process on button press (not hold)
  uint16_t pressed = buttons & ~prevButtons;  // newly pressed
  prevButtons = buttons;

  if (!pressed) return;

  // Nintendo Switch Pro Controller button mapping:
  // A (east)  = 0x0001   B (south) = 0x0002
  // X (north) = 0x0004   Y (west)  = 0x0008
  // L = 0x0010   R = 0x0020
  // ZL = 0x0040  ZR = 0x0080
  // Minus = 0x0100  Plus = 0x0200
  // L-Stick press = 0x0400  R-Stick press = 0x0800
  // Home = 0x1000  Capture = 0x2000

  if (pressed & 0x0001) {  // A - stand up
    tQueue->addTask(T_SKILL, "up");
    isMoving = false;
    printToAllPorts("[Gamepad] A: Stand up");
  }
  if (pressed & 0x0002) {  // B - sit
    tQueue->addTask(T_SKILL, "sit");
    isMoving = false;
    printToAllPorts("[Gamepad] B: Sit");
  }
  if (pressed & 0x0004) {  // X - stretch
    tQueue->addTask(T_SKILL, "str");
    isMoving = false;
    printToAllPorts("[Gamepad] X: Stretch");
  }
  if (pressed & 0x0008) {  // Y - greeting
    tQueue->addTask(T_SKILL, "hi");
    isMoving = false;
    printToAllPorts("[Gamepad] Y: Greeting");
  }
  if (pressed & 0x0010) {  // L shoulder - trot mode
    tQueue->addTask(T_SKILL, "trF");
    isMoving = true;
    printToAllPorts("[Gamepad] L: Trot");
  }
  if (pressed & 0x0020) {  // R shoulder - crawl mode
    tQueue->addTask(T_SKILL, "crF");
    isMoving = true;
    printToAllPorts("[Gamepad] R: Crawl");
  }
  if (pressed & 0x0040) {  // ZL - push up
    tQueue->addTask(T_SKILL, "pu");
    isMoving = false;
    printToAllPorts("[Gamepad] ZL: Push up");
  }
  if (pressed & 0x0080) {  // ZR - check around
#ifdef BITTLE
    tQueue->addTask(T_SKILL, "ck");
#else
    tQueue->addTask(T_SKILL, "wsf");
#endif
    isMoving = false;
    printToAllPorts("[Gamepad] ZR: Special");
  }
  if (pressed & 0x0100) {  // Minus - rest (servos off)
    tQueue->addTask(T_SKILL, "rest");
    isMoving = false;
    printToAllPorts("[Gamepad] Minus: Rest");
  }
  if (pressed & 0x0200) {  // Plus - toggle gyro
    tQueue->addTask(T_GYRO, "");
    printToAllPorts("[Gamepad] Plus: Toggle gyro");
  }

  // D-pad for head/pan control
  if (ctl->dpad() & 0x01) {  // D-pad Up - head up
    tQueue->addTask(T_INDEXED_SIMULTANEOUS_ASC, "0 -30");
    printToAllPorts("[Gamepad] DPad Up: Head up");
  }
  if (ctl->dpad() & 0x02) {  // D-pad Down - head down
    tQueue->addTask(T_INDEXED_SIMULTANEOUS_ASC, "0 30");
    printToAllPorts("[Gamepad] DPad Down: Head down");
  }
  if (ctl->dpad() & 0x04) {  // D-pad Right - head right
    tQueue->addTask(T_INDEXED_SIMULTANEOUS_ASC, "1 -30");
    printToAllPorts("[Gamepad] DPad Right");
  }
  if (ctl->dpad() & 0x08) {  // D-pad Left - head left
    tQueue->addTask(T_INDEXED_SIMULTANEOUS_ASC, "1 30");
    printToAllPorts("[Gamepad] DPad Left");
  }
}

// --- Right stick for fine head/joint control ---
void processRightStick(ControllerPtr ctl) {
  int16_t rx = ctl->axisRX();  // Right stick X
  int16_t ry = ctl->axisRY();  // Right stick Y

  if (abs(rx) < STICK_DEADZONE && abs(ry) < STICK_DEADZONE)
    return;

  // Map right stick to head pan/tilt (joints 0 and 1)
  // Scale from ~-512..512 to -45..45 degrees
  int8_t headTilt = constrain(map(ry, -512, 512, -45, 45), -45, 45);
  int8_t headPan = constrain(map(rx, -512, 512, -45, 45), -45, 45);

  char cmd[20];
  snprintf(cmd, sizeof(cmd), "0 %d 1 %d", headTilt, headPan);
  tQueue->addTask(T_INDEXED_SIMULTANEOUS_ASC, cmd);
}

// --- Broadcast gamepad state to WebSocket clients ---
static unsigned long lastGamepadBroadcast = 0;
static const unsigned long GAMEPAD_BROADCAST_INTERVAL = 150;  // ms between WS updates

void sendGamepadState(ControllerPtr ctl) {
#ifdef WEB_SERVER
  extern WebSocketsServer webSocket;
  extern std::map<uint8_t, bool> connectedClients;
  extern bool webServerConnected;

  if (!webServerConnected || connectedClients.empty()) return;

  unsigned long now = millis();
  if (now - lastGamepadBroadcast < GAMEPAD_BROADCAST_INTERVAL) return;
  lastGamepadBroadcast = now;

  char json[256];
  snprintf(json, sizeof(json),
    "{\"type\":\"gamepad\",\"connected\":true,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"buttons\":%d,\"dpad\":%d,\"cmd\":\"%s\"}",
    ctl->axisX(), ctl->axisY(), ctl->axisRX(), ctl->axisRY(),
    ctl->buttons(), ctl->dpad(), lastGamepadCmd.c_str());

  for (auto &client : connectedClients) {
    webSocket.sendTXT(client.first, json);
  }
#endif
}

void sendGamepadDisconnected() {
#ifdef WEB_SERVER
  extern WebSocketsServer webSocket;
  extern std::map<uint8_t, bool> connectedClients;
  extern bool webServerConnected;

  if (!webServerConnected || connectedClients.empty()) return;

  const char* json = "{\"type\":\"gamepad\",\"connected\":false,\"lx\":0,\"ly\":0,\"rx\":0,\"ry\":0,\"buttons\":0,\"dpad\":0,\"cmd\":\"--\"}";
  for (auto &client : connectedClients) {
    webSocket.sendTXT(client.first, json);
  }
#endif
}

// --- Main gamepad setup and loop functions ---

void gamepadSetup() {
  Serial.println("[Gamepad] BluePad32 initializing...");
  BP32.setup(&onConnectedController, &onDisconnectedController);

  // Forget previously paired controllers (optional - remove if you want persistent pairing)
  // BP32.forgetBluetoothKeys();

  Serial.println("[Gamepad] Ready. Put Switch Pro controller in pairing mode (hold Sync button).");
  printToAllPorts("[Gamepad] BluePad32 ready - pair your Switch Pro controller");
}

void gamepadLoop() {
  // Must be called each frame to process Bluetooth events
  bool dataUpdated = BP32.update();

  if (!dataUpdated) return;

  // Rate-limit input processing
  unsigned long now = millis();
  if (now - lastGamepadUpdate < GAMEPAD_UPDATE_INTERVAL) return;
  lastGamepadUpdate = now;

  for (int i = 0; i < MAX_GAMEPADS; i++) {
    ControllerPtr ctl = myControllers[i];
    if (ctl && ctl->isConnected() && ctl->hasData()) {
      if (ctl->isGamepad()) {
        processGamepadButtons(ctl);
        processGamepadStick(ctl);
        processRightStick(ctl);
        sendGamepadState(ctl);  // Broadcast to web UI
      }
    }
  }
}

#endif  // BLUEPAD32
#endif  // OPENCAT_GAMEPAD_H
