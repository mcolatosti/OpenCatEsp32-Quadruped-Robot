# OpenCat ESP32 HTTP Server Documentation

## Overview
Added a comprehensive HTTP web server on port 80 alongside the existing WebSocket server (port 81) to provide a user-friendly console-type interface for interacting with the OpenCat robot.

## Features Implemented

### 1. **Dual Server Architecture**
- **HTTP Server (Port 80)**: Web-based user interface with console interaction
- **WebSocket Server (Port 81)**: Real-time bidirectional communication (existing)

### 2. **Web Pages Available**

#### Home Page (`/`)
- **URL**: `http://robot-ip/`
- **Features**:
  - Robot status display
  - System information (model, software version, IP address, free heap)
  - Navigation to console interface
  - Overview of available interfaces

#### Console Interface (`/console`)
- **URL**: `http://robot-ip/console`
- **Features**:
  - Terminal-style interface with dark theme
  - Real-time command input/output
  - WebSocket integration for live responses
  - Command history (up/down arrows)
  - Quick command buttons for common actions
  - Auto-connecting WebSocket for real-time feedback

### 3. **Command Processing**
- **HTTP POST** (`/command`): Fallback command processing when WebSocket unavailable
- **WebSocket Integration**: Automatic connection to port 81 for real-time responses
- **Command Validation**: Input length and format validation
- **Built-in Help**: Type 'h' for available commands

### 4. **User Interface Features**
- **Responsive Design**: Works on desktop and mobile devices
- **Dark Terminal Theme**: Courier New font with syntax highlighting
- **Status Indicators**: Connection status display
- **Quick Commands**: One-click buttons for common robot actions
- **Command History**: Navigate previous commands with arrow keys
- **Auto-scroll**: Console output automatically scrolls to latest content

## Quick Commands Available
- **Sit**: `ksit`
- **Stand Up**: `kup`
- **Walk Forward**: `kwkF`
- **Walk Left**: `kwkL`
- **Walk Right**: `kwkR`
- **Walk Back**: `kwkB`
- **Rest**: `d`
- **Toggle Gyro**: `g`
- **Beep**: `b`
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

### Integration Points
- **WiFi Connection**: HTTP server starts automatically when WiFi connects
- **Main Loop**: `httpServer.handleClient()` called in `WebServerLoop()`
- **Command Processing**: Integrates with existing OpenCat command system
- **WebSocket Bridge**: Console can use both HTTP POST and WebSocket for commands

## Usage Instructions

### 1. **Access the Interface**
1. Connect robot to WiFi (automatic with saved credentials)
2. Find robot's IP address from Serial Monitor
3. Open web browser and go to `http://robot-ip/`
4. Click "Console Interface" to access the terminal

### 2. **Using the Console**
1. Type commands in the input field at the bottom
2. Press Enter or click "Send" to execute
3. Use quick command buttons for common actions
4. Use up/down arrows to navigate command history
5. WebSocket automatically connects for real-time responses

### 3. **Command Examples**
```
ksit          // Make robot sit
kup           // Make robot stand up
kwkF          // Walk forward
d             // Rest position
g             // Toggle gyro
b12 4 14 4    // Play melody
j             // Show joint angles
P             // Show battery voltage
h             // Show help
```

## System Requirements
- **ESP32 with WiFi**: BiBoard V1.0 or compatible
- **Available Memory**: ~50KB free heap minimum for stable operation
- **Web Browser**: Any modern browser (Chrome, Firefox, Safari, Edge)
- **Network**: Robot and client on same WiFi network

## Benefits
1. **User-Friendly**: No need for specialized software or WebSocket clients
2. **Universal Access**: Works from any device with a web browser
3. **Dual Interface**: Choose HTTP for simple commands or WebSocket for real-time
4. **Mobile-Friendly**: Responsive design works on phones and tablets
5. **Educational**: Easy to understand and modify for learning purposes
6. **Fallback Option**: HTTP commands work even if WebSocket fails

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