# OpenCat ESP32 HTTP Server Documentation

## Overview
The OpenCat ESP32 project provides a comprehensive HTTP web server (port 80) and a WebSocket server (port 81) for a user-friendly, real-time console interface to interact with the robot.

## Features Implemented

### 1. **Dual Server Architecture**
- **HTTP Server (Port 80)**: Web-based console interface
- **WebSocket Server (Port 81)**: Real-time bidirectional communication

### 2. **Web Pages Available**

#### Home Page (`/`)
- **URL**: `http://robot-ip/`
- **Features**:
  - Robot status display
  - System information (model, software version, IP address, free heap)
  - Navigation to console interface

#### Console Interface (`/console`)
- **URL**: `http://robot-ip/console`
- **Features**:
  - Terminal-style interface with dark theme
  - Real-time command input/output
  - WebSocket integration for live responses
  - Minimal UI: command input, output log, and status indicator
  - Auto-connecting WebSocket for real-time feedback
  - All serial output (including system/process events) is mirrored to the web console

### 3. **Command Processing**
- **HTTP POST** (`/command`): Fallback command processing when WebSocket unavailable
- **WebSocket Integration**: Automatic connection to port 81 for real-time responses
- **Command Validation**: Input length and format validation
- **Built-in Help**: Type 'h' for available commands

### 4. **User Interface Features**
- **Responsive Design**: Works on desktop and mobile devices
- **Dark Terminal Theme**: Monospace font
- **Status Indicators**: Connection status display
- **Auto-scroll**: Console output automatically scrolls to latest content

## Quick Commands Available (via input)
- **Sit**: `ksit`
- **Stand Up**: `kup`
- **Rest**: `d`
- **Help**: `h`

## Technical Implementation

### Code Changes Made
1. **Added WebServer Library**: `#include <WebServer.h>`
2. **Created WebServer Instance**: `WebServer httpServer(80);`
3. **HTTP Handler Functions**:
   - `setupHttpServer()` - Initialize routes and start server
   - `handleRoot()` - Serve home page
   - `handleConsole()` - Serve console interface
   - `handleCommand()` - Process HTTP POST commands
   - `handleNotFound()` - Handle 404 errors
4. **Serial Output Forking**: All Serial output is now mirrored to the web console using a custom Print subclass.

### Integration Points
- **WiFi Connection**: HTTP and WebSocket servers start automatically when WiFi connects
- **Main Loop**: `httpServer.handleClient()` and `webSocket.loop()` called in `WebServerLoop()`
- **Command Processing**: Integrates with existing OpenCat command system
- **WebSocket Bridge**: Console uses both HTTP POST and WebSocket for commands and output

## Usage Instructions

### 1. **Access the Interface**
1. Connect robot to WiFi (automatic with saved credentials)
2. Find robot's IP address from Serial Monitor
3. Open web browser and go to `http://robot-ip/`

### 2. **Using the Console**
1. Type commands in the input field
2. Press Enter or click "Send" to execute
3. WebSocket automatically connects for real-time responses
4. All system and process output will appear in the console log

### 3. **Command Examples**
```
ksit          // Make robot sit
kup           // Make robot stand up
d             // Rest position
g             // Toggle gyro
b12 4 14 4    // Play melody
j             // Show joint angles
P             // Show battery voltage
h             // Show help
```

## System Requirements
- **ESP32 with WiFi**: BiBoard V1.0 or compatible
- **Available Memory**: ~25KB free heap minimum for stable operation
- **Web Browser**: Any modern browser (Chrome, Firefox, Safari, Edge)
- **Network**: Robot and client on same WiFi network

## Benefits
1. **User-Friendly**: No need for specialized software or WebSocket clients
2. **Universal Access**: Works from any device with a web browser
3. **Dual Interface**: Choose HTTP for simple commands or WebSocket for real-time
4. **Mobile-Friendly**: Responsive design works on phones and tablets
5. **Educational**: Easy to understand and modify for learning purposes
6. **Fallback Option**: HTTP commands work even if WebSocket fails
7. **All Serial Output Mirrored**: All system/process events are visible in the web console

## Security Considerations
- **Local Network Only**: Server only accessible on local WiFi network
- **No Authentication**: Open access (suitable for educational/personal use)
- **Command Validation**: Basic input validation to prevent malformed commands
- **Rate Limiting**: Natural rate limiting through user interface

## Future Enhancements
- Add authentication for secure access
- Implement command scheduling/macros
- Add sensor data visualization
- Create mobile app interface
- Add file upload for skills/configurations