#include "esp32-hal.h"
#include <WiFi.h>
#include <WebServer.h> // ESP32 built-in web server library
#include <WebSocketsServer.h> // download at https://github.com/Links2004/arduinoWebSockets/
#ifdef WIFI_MANAGER
#include <WiFiManager.h> // download at https://github.com/tzapu/WiFiManager
#endif
#ifndef WIFI_MANAGER
#include <esp_wifi.h>
#endif

#include <map>
#include <ArduinoJson.h>

// Web server debug level control
#define WEB_DEBUG_LEVEL 1               // 0=off, 1=error, 2=warning, 3=info, 4=verbose

// Debug print macros - controlled by level
#if WEB_DEBUG_LEVEL >= 1
  #define WEB_ERROR(msg, value) PTHL(msg, value)
  #define WEB_ERROR_F(msg) PTLN(msg)
#else
  #define WEB_ERROR(msg, value)
  #define WEB_ERROR_F(msg)
#endif

#if WEB_DEBUG_LEVEL >= 2
  #define WEB_WARN(msg, value) PTHL(msg, value)
  #define WEB_WARN_F(msg) PTLN(msg)
#else
  #define WEB_WARN(msg, value)
  #define WEB_WARN_F(msg)
#endif

#if WEB_DEBUG_LEVEL >= 3
  #define WEB_INFO(msg, value) PTHL(msg, value)
  #define WEB_INFO_F(msg) PTLN(msg)
#else
  #define WEB_INFO(msg, value)
  #define WEB_INFO_F(msg)
#endif

#if WEB_DEBUG_LEVEL >= 4
  #define WEB_DEBUG(msg, value) PTHL(msg, value)
  #define WEB_DEBUG_F(msg) PTLN(msg)
#else
  #define WEB_DEBUG(msg, value)
  #define WEB_DEBUG_F(msg)
#endif

#include <atomic>
extern std::atomic<bool> webShowAllOutput;
// Web server timeout configuration (milliseconds) - optimized for Bluetooth coexistence
#define HEARTBEAT_TIMEOUT 40000         // Heartbeat timeout: 40s (increased buffer time for BLE interference)
#define HEALTH_CHECK_INTERVAL 15000     // Health check interval: 15s (reduced frequency)
#define WEB_TASK_EXECUTION_TIMEOUT 45000 // Task execution timeout: 45s (increased execution time)
#define MAX_CLIENTS 2                   // Maximum connection limit

// WiFi configuration
String ssid = "MEC";
String password = "Myc@t1sm1st3r!";
WebSocketsServer webSocket = WebSocketsServer(81); // WebSocket server on port 81
WebServer httpServer(80); // HTTP server on port 80
long connectWebTime;
bool webServerConnected = false;

// WebSocket client management
std::map<uint8_t, bool> connectedClients;
std::map<uint8_t, unsigned long> lastHeartbeat; // Record last heartbeat time for each client

// Connection health check
unsigned long lastHealthCheckTime = 0;

// OPTION 1 EVENT-DRIVEN JOINT UPDATES - START
// Joint change detection variables
int previousJointAngles[DOF] = {0};  // Track previous joint positions
unsigned long lastJointUpdateTime = 0;  // Rate limiting
const unsigned long MIN_JOINT_UPDATE_INTERVAL = 50;  // Minimum 50ms between updates (20Hz max)
const int JOINT_CHANGE_THRESHOLD = 3;  // Degrees - only update if change > 3°
bool jointUpdatePending = false;  // Flag for pending update
// OPTION 1 EVENT-DRIVEN JOINT UPDATES - END

// Asynchronous task management
struct WebTask
{
  String taskId;
  String status; // "pending", "running", "completed", "error"
  unsigned long timestamp;
  unsigned long endTime;
  unsigned long startTime;
  bool resultReady;
  uint8_t clientId; // Client ID
  std::vector<String> commandGroup; // Command list in the command group
  std::vector<String> results; // Execution results from the command group
  size_t currentCommandIndex; // Current command index being executed
};

std::map<String, WebTask> webTasks;
String currentWebTaskId = "";
bool webTaskActive = false;

// Function declarations
String generateTaskId();
void startWebTask(String taskId);
void completeWebTask();
void errorWebTask(String errorMessage);
void processNextWebTask();
void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
#ifdef CAMERA
void sendCameraData(int xCoord, int yCoord, int width, int height);
#endif
void sendUltrasonicData(int distance);
String getJointAnglesJson();
// OPTION 1 EVENT-DRIVEN JOINT UPDATES - START
void sendJointUpdateIfChanged();
void notifyJointChange();
// OPTION 1 EVENT-DRIVEN JOINT UPDATES - END
void clearWebTask(String taskId);
void checkConnectionHealth();
void sendSocketResponse(uint8_t clientId, String message);

// HTTP Server function declarations
void setupHttpServer();
void handleRoot();
void handleConsole();
void handleCommand();
void handleNotFound();

// Enhanced output capture functions
void sendRobotOutput(String output);
void sendSystemInfo();

// Simple Base64 decoding function
String base64Decode(String input) {
  const char PROGMEM b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String result = "";
  int val = 0, valb = -8;
  
  for (char c : input) {
    if (c == '=') break;
    
    int index = -1;
    for (int i = 0; i < 64; i++) {
      if (pgm_read_byte(&b64_alphabet[i]) == c) {
        index = i;
        break;
      }
    }
    
    if (index == -1) continue;
    
    val = (val << 6) | index;
    valb += 6;
    
    if (valb >= 0) {
      result += char((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  
  return result;
}

// Generate task ID
String generateTaskId()
{
  return String(millis()) + "_" + String(esp_random() % 1000);
}

// Send response to specified client
void sendSocketResponse(uint8_t clientId, String message) {
  if (connectedClients.find(clientId) != connectedClients.end() && connectedClients[clientId]) {
    webSocket.sendTXT(clientId, message);
  }
}

// Check connection health status
void checkConnectionHealth() {
  unsigned long currentTime = millis();
  
  // Check if there is BLE activity, if so relax heartbeat timeout
  bool bleActive = false;
#ifdef BT_CLIENT
  extern boolean doScan;
  extern boolean btConnected;
  bleActive = doScan || btConnected;
#endif
  
  unsigned long effectiveTimeout = bleActive ? (HEARTBEAT_TIMEOUT + 15000) : HEARTBEAT_TIMEOUT;
  
  // Check heartbeat timeout
  for (auto it = lastHeartbeat.begin(); it != lastHeartbeat.end();) {
    uint8_t clientId = it->first;
    unsigned long lastHeartbeatTime = it->second;
    
    if (currentTime - lastHeartbeatTime > effectiveTimeout) {
      if (bleActive) {
        WEB_WARN("Client heartbeat timeout during BLE activity: ", clientId);
      } else {
        WEB_ERROR("Client heartbeat timeout, disconnecting: ", clientId);
      }
      
      // Send timeout notification (including BLE status information)
      String timeoutMsg = bleActive ? 
        "{\"type\":\"error\",\"error\":\"Heartbeat timeout during BLE scan\"}" :
        "{\"type\":\"error\",\"error\":\"Heartbeat timeout\"}";
      sendSocketResponse(clientId, timeoutMsg);
      
  // Disconnect connection (handled by library on timeout)
  // webSocket.disconnect(clientId); // Disabled to avoid double-disconnect heap error

      // If current task belongs to this client, need to handle it
      if (webTaskActive && currentWebTaskId != "" && 
          webTasks.find(currentWebTaskId) != webTasks.end() && 
          webTasks[currentWebTaskId].clientId == clientId) {
        errorWebTask("Client disconnected due to heartbeat timeout");
      }

      // Clean up client state AFTER all messaging and cleanup
      connectedClients.erase(clientId);
      it = lastHeartbeat.erase(it);
    } else {
      ++it;
    }
  }
}

#ifdef CAMERA
// Send camera data to all connected clients
void sendCameraData(int xCoord, int yCoord, int width, int height) {
  if (!webServerConnected || connectedClients.empty()) {
    return;
  }

  JsonDocument cameraDoc;
  cameraDoc["type"] = "event_cam";
  cameraDoc["x"] = xCoord - imgRangeX / 2.0;  // Consistent with showRecognitionResult
  cameraDoc["y"] = yCoord - imgRangeY / 2.0;  // Consistent with showRecognitionResult
  cameraDoc["width"] = width;
  cameraDoc["height"] = height;
  cameraDoc["timestamp"] = millis();

  String cameraData;
  serializeJson(cameraDoc, cameraData);

  // Send data to all connected clients
  for (auto &client : connectedClients) {
    if (client.second) { // If client is still connected
      webSocket.sendTXT(client.first, cameraData);
    }
  }
}
#endif

// Send ultrasonic data to all connected clients
void sendUltrasonicData(int distance) {
  if (!webServerConnected || connectedClients.empty()) {
    return;
  }

  JsonDocument ultrasonicDoc;
  ultrasonicDoc["type"] = "event_us";
  ultrasonicDoc["distance"] = distance;
  ultrasonicDoc["timestamp"] = millis();

  String ultrasonicData;
  serializeJson(ultrasonicDoc, ultrasonicData);

  // Send data to all connected clients
  for (auto &client : connectedClients) {
    if (client.second) { // If client is still connected
      webSocket.sendTXT(client.first, ultrasonicData);
    }
  }
}

// Get current joint angles as JSON string
String getJointAnglesJson() {
  JsonDocument jointDoc;
  JsonArray angles = jointDoc["angles"].to<JsonArray>();
  for (int i = 0; i < DOF; i++) {
    angles.add((int)currentAng[i]);
  }
  String result;
  serializeJson(jointDoc, result);
  return result;
}

// OPTION 1 EVENT-DRIVEN JOINT UPDATES - START
// Check if joints have changed significantly and send update if needed
void sendJointUpdateIfChanged() {
  if (!webServerConnected || connectedClients.empty()) {
    return;
  }
  
  // Rate limiting - don't send updates too frequently
  unsigned long currentTime = millis();
  if (currentTime - lastJointUpdateTime < MIN_JOINT_UPDATE_INTERVAL) {
    jointUpdatePending = true;  // Mark for later
    return;
  }
  
  // Check if any joint has changed significantly
  bool hasSignificantChange = false;
  for (int i = 0; i < DOF; i++) {
    if (abs(currentAng[i] - previousJointAngles[i]) >= JOINT_CHANGE_THRESHOLD) {
      hasSignificantChange = true;
      break;
    }
  }
  
  if (hasSignificantChange) {
    // Update previous angles
    for (int i = 0; i < DOF; i++) {
      previousJointAngles[i] = currentAng[i];
    }
    
    // Send joint data to all connected clients
    String jointData = "{\"type\":\"joint_data\"," + getJointAnglesJson().substring(1);
    for (auto &client : connectedClients) {
      if (client.second) { // If client is still connected
        webSocket.sendTXT(client.first, jointData);
      }
    }
    
    lastJointUpdateTime = currentTime;
    jointUpdatePending = false;
  }
}

// Call this function whenever joints are moved
void notifyJointChange() {
  sendJointUpdateIfChanged();
}
// OPTION 1 EVENT-DRIVEN JOINT UPDATES - END

void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {

    case WStype_DISCONNECTED:
      Serial.println("[DEBUG] Entering WStype_DISCONNECTED handler");
      WEB_ERROR("WebSocket client disconnected: ", num);

      // If current task belongs to this client, need to handle it
      if (webTaskActive && currentWebTaskId != "" && 
          webTasks.find(currentWebTaskId) != webTasks.end() && 
          webTasks[currentWebTaskId].clientId == num) {
        Serial.println("[DEBUG] Calling errorWebTask from disconnect");
        errorWebTask("Client disconnected");
        Serial.println("[DEBUG] Returned from errorWebTask");
      }

      // Clean up client state at the very end
      connectedClients.erase(num);
      lastHeartbeat.erase(num);
      Serial.println("[DEBUG] Cleaned up client state");
      Serial.println("[DEBUG] Exiting WStype_DISCONNECTED handler");
      break;
      
    case WStype_CONNECTED:
      // Check connection limit
      if (connectedClients.size() >= MAX_CLIENTS) {
        WEB_ERROR("Max clients reached, rejecting: ", num);
        sendSocketResponse(num, "{\"type\":\"error\",\"error\":\"Max clients reached\"}");
        webSocket.disconnect(num);
        return;
      }
      
      connectedClients[num] = true;
      lastHeartbeat[num] = millis();
              WEB_DEBUG("WebSocket client connected: ", num);
      
      // Send connection success response
      sendSocketResponse(num, "{\"type\":\"connected\",\"clientId\":\"" + String(num) + "\"}");
      break;
      
    case WStype_TEXT: {
      String message = String((char*)payload);
      // Parse JSON message
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, message);
      if (error) {
        // JSON parse error, send error response
        sendSocketResponse(num, "{\"type\":\"error\",\"error\":\"Invalid JSON format\"}");
        return;
      }
      String msgType = doc["type"].as<String>();
      WEB_DEBUG("msg type: ", msgType);
      // Handle heartbeat messages
      if (doc["type"] == "heartbeat") {
        lastHeartbeat[num] = millis();
        sendSocketResponse(num, "{\"type\":\"heartbeat\",\"timestamp\":" + String(millis()) + "}");
        return;
      }
      // Handle keep-alive ping messages
      if (doc["type"] == "ping") {
        lastHeartbeat[num] = millis();
        // No pong response needed
        return;
      }
      // Handle system information requests
      if (doc["type"] == "get_system_info") {
        lastHeartbeat[num] = millis();
        sendSystemInfo();
        return;
      }
      // Handle joint refresh requests
      if (doc["type"] == "get_joints") {
        lastHeartbeat[num] = millis();
        String jointData = "{\"type\":\"joint_data\"," + getJointAnglesJson().substring(1);
        sendSocketResponse(num, jointData);
        return;
      }
      // Handle command messages (unified command group format)
      if (doc["type"] == "command") {
        String taskId = doc["taskId"].as<String>();
        JsonArray commands;
        // If it's a single command, convert to command group format
        commands = doc["commands"].as<JsonArray>();
        // Update heartbeat time
        lastHeartbeat[num] = millis();
        // Create task record
        WebTask task;
        task.taskId = taskId;
        task.status = "pending";
        task.timestamp = millis();
        task.startTime = 0;
        task.resultReady = false;
        task.clientId = num;
        task.currentCommandIndex = 0;
        // Store command group
        for (JsonVariant cmd : commands) {
          task.commandGroup.push_back(cmd.as<String>());
        }
        // Debug information
                  WEB_DEBUG("Received command task: ", taskId);
          WEB_DEBUG("Command count: ", task.commandGroup.size());
                  #if WEB_DEBUG_LEVEL >= 4
          for (size_t i = 0; i < task.commandGroup.size(); i++) {
            WEB_DEBUG("Command " + String(i) + ": ", task.commandGroup[i]);
          }
          #endif
        
        // If there are currently no active web tasks, start execution immediately
        if (!webTaskActive) {
          // Store task
          webTasks[taskId] = task;
          startWebTask(taskId);
        } else {
          // If there is currently an active web task, discard and return error
          errorWebTask("Previous web task is still running");
          return;
        }
        
        // Send task start response
        sendSocketResponse(num, "{\"type\":\"response\",\"taskId\":\"" + taskId + "\",\"status\":\"running\"}");
        
        WEB_DEBUG("web command group async: ", taskId);
        WEB_DEBUG("command count: ", task.commandGroup.size());
      }

      // Handle output mode toggle
      if (doc["type"] == "toggle_output_mode") {
        webShowAllOutput = !webShowAllOutput.load();
        sendSocketResponse(num, String("{\"type\":\"output_mode\",\"all\":") + (webShowAllOutput ? "true" : "false") + "}");
        String msg = webShowAllOutput ? "[Web Console] Output mode: Show ALL output" : "[Web Console] Output mode: Show only web command output";
        sendRobotOutput(msg);
        // Debug: also print to serial
  PTLN("[DEBUG] Output mode toggled. webShowAllOutput=" + String(webShowAllOutput ? "true" : "false"));
        return;
      }
      break;
    }
  }
}

// Start executing web task
void startWebTask(String taskId)
{
  if (webTasks.find(taskId) == webTasks.end()) {
    return;
  }

  WebTask &task = webTasks[taskId];

  // Set global flags and commands
  cmdFromWeb = true;
  currentWebTaskId = taskId;
  webTaskActive = true;
  webResponse = "";  // Clear response buffer

      // Execute the next command in the command group
    if (task.currentCommandIndex < task.commandGroup.size()) {
      String webCmd = task.commandGroup[task.currentCommandIndex];
      
              WEB_DEBUG("Processing command: ", webCmd);
      
      // Check if it's a base64 encoded command
      if (webCmd.startsWith("b64:")) {
        String base64Cmd = webCmd.substring(4);
        String decodedString = base64Decode(base64Cmd);
        if (decodedString.length() > 0) {
          token = decodedString[0];
          for (int i = 1; i < decodedString.length(); i++) {
            int8_t param = (int8_t)decodedString[i];
            newCmd[i-1] = param;
          }
          // strcpy(newCmd, decodedString.c_str() + 1);
          cmdLen = decodedString.length() - 1;
          if (token >= 'A' && token <= 'Z') {
            newCmd[cmdLen] = '~';
          } else {
            newCmd[cmdLen] = '\0';
          }
                      WEB_DEBUG("base64 decode token: ", token);
            WEB_DEBUG("base64 decode args count: ", cmdLen);
        } else {
          WEB_ERROR("base64 decode failed: ", task.currentCommandIndex);
          // base64 decode failed, skip command
          task.currentCommandIndex++;
          startWebTask(taskId);
          return;
        }
      } else {
        // Parse command
        token = webCmd[0];
        strcpy(newCmd, webCmd.c_str() + 1);
        cmdLen = strlen(newCmd);
        newCmd[cmdLen + 1] = '\0';
        
                  WEB_DEBUG("Parsed token: ", token);
          WEB_DEBUG("Parsed command: ", newCmd);
          WEB_DEBUG("Command length: ", cmdLen);
      }
      newCmdIdx = 4;

    // Update task status
    task.status = "running";
    task.startTime = millis();

    // Notify client that task has started
    JsonDocument statusDoc;
    statusDoc["type"] = "response";
    statusDoc["taskId"] = taskId;
    statusDoc["status"] = "running";
    String statusMsg;
    serializeJson(statusDoc, statusMsg);
    webSocket.sendTXT(task.clientId, statusMsg);

    WEB_DEBUG("executing command group task: ", taskId);
    WEB_DEBUG("sub command Index: ", task.currentCommandIndex);
    WEB_DEBUG("sub command: ", webCmd);
    WEB_DEBUG("total commands: ", task.commandGroup.size());
  } else {
    // All commands execution completed
    completeWebTask();
  }
}

// Complete web task
void completeWebTask()
{
  if (!webTaskActive || currentWebTaskId == "") {
    return;
  }

  if (webTasks.find(currentWebTaskId) != webTasks.end()) {
    WebTask &task = webTasks[currentWebTaskId];
    task.results.push_back(webResponse);

    // Check if there are more commands to execute
    if (task.currentCommandIndex + 1 < task.commandGroup.size()) {
      // There are more commands, continue execution
      task.currentCommandIndex++;
      startWebTask(currentWebTaskId);
      return;
    }
    
    // All commands execution completed
    task.status = "completed";
    task.endTime = millis();
    task.resultReady = true;

    WEB_DEBUG("web task completed: ", currentWebTaskId);
    WEB_DEBUG("results length: ", task.results.size());

    // Send completion status to client
    JsonDocument completeDoc;
    completeDoc["type"] = "response";
    completeDoc["taskId"] = currentWebTaskId;
    completeDoc["status"] = "completed";
    JsonArray results = completeDoc["results"].to<JsonArray>();
    for (String result : task.results) {
      results.add(result);
    }
    String statusMsg;
    serializeJson(completeDoc, statusMsg);
    sendSocketResponse(task.clientId, statusMsg);
    WEB_DEBUG("web task response: ", statusMsg);
    clearWebTask(currentWebTaskId);
  }

  // Reset global state
  cmdFromWeb = false;
  webTaskActive = false;
  currentWebTaskId = "";

  // Check if there are waiting tasks
  processNextWebTask();
}

// Web task error handling
void errorWebTask(String errorMessage)
{
  Serial.println("[DEBUG] Entering errorWebTask");
  if (!webTaskActive || currentWebTaskId == "") {
    Serial.println("[DEBUG] errorWebTask: No active web task");
    return;
  }

  if (webTasks.find(currentWebTaskId) != webTasks.end()) {
    WebTask &task = webTasks[currentWebTaskId];
    task.status = "error";
    task.resultReady = true;

    // Send error status to client
    JsonDocument errorDoc;
    errorDoc["type"] = "response";
    errorDoc["taskId"] = currentWebTaskId;
    errorDoc["status"] = "error";
    errorDoc["error"] = errorMessage;
    String statusMsg;
    serializeJson(errorDoc, statusMsg);
    sendSocketResponse(task.clientId, statusMsg);
    Serial.println("[DEBUG] Sent error status to client");
    clearWebTask(currentWebTaskId);
    Serial.println("[DEBUG] Cleared web task");
  }

  // Reset state
  cmdFromWeb = false;
  webTaskActive = false;
  currentWebTaskId = "";

  // Process next task
  Serial.println("[DEBUG] Calling processNextWebTask");
  processNextWebTask();
  Serial.println("[DEBUG] Exiting errorWebTask");
}

void clearWebTask(String taskId)
{
  if (webTasks.find(taskId) != webTasks.end()) {
    WebTask &task = webTasks[taskId];
    WEB_DEBUG("clear web task: ", taskId);
    task.commandGroup.clear();
    task.results.clear();
    webTasks.erase(taskId);
  }
}

// Process next waiting task
void processNextWebTask()
{
  for (auto &pair : webTasks) {
    WebTask &task = pair.second;
    if (task.status == "pending") {
      startWebTask(task.taskId);
      break;
    }
  }
}

// WiFi configuration function - Enhanced version with retry mechanism
bool connectWifi(String ssid, String password, int maxRetries = 3)
{
  for (int retry = 0; retry < maxRetries; retry++) {
    if (retry > 0) {
      WEB_WARN("WiFi connection retry: ", retry);
      delay(2000); // Wait 2 seconds before retry
    }
    
    WiFi.begin(ssid.c_str(), password.c_str());
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 100) {
      delay(100);
      #if WEB_DEBUG_LEVEL >= 3
      PT('.');
      #endif
      timeout++;
    }
    #if WEB_DEBUG_LEVEL >= 3
  PTL();
    #endif
    
    if (WiFi.status() == WL_CONNECTED) {
      WEB_INFO("WiFi connected on attempt: ", retry + 1);
      return true;
    } else {
      WEB_ERROR("WiFi connection failed on attempt: ", retry + 1);
      WiFi.disconnect(true); // Completely disconnect to prepare for next attempt
    }
  }
  
  Serial.println("All WiFi connection attempts failed");
  return false;
}

#ifndef WIFI_MANAGER
// When WIFI_MANAGER is not enabled, try to read and use previously saved WiFi information to connect
bool connectWifiFromStoredConfig()
{
  // Check available memory
  size_t freeHeap = ESP.getFreeHeap();
  WEB_INFO("Free heap before WiFi init: ", freeHeap);
  
  if (freeHeap < 30000) { // If available memory less than 30KB
    WEB_ERROR("Insufficient memory for WiFi initialization: ", freeHeap);
    return false;
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  wifi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    WEB_ERROR_F("Failed to get stored WiFi config");
    return false;
  }

  String savedSsid = String(reinterpret_cast<char*>(cfg.sta.ssid));
  String savedPassword = String(reinterpret_cast<char*>(cfg.sta.password));


  if (savedSsid.length() == 0) {
    WEB_WARN_F("No stored SSID found, using default credentials");
    savedSsid = ssid;
    savedPassword = password;
  }

  webServerConnected = connectWifi(savedSsid, savedPassword);

  if (webServerConnected) {
    printToAllPorts("Successfully connected Wifi to IP Address: " + WiFi.localIP().toString());
    // Start WebSocket server
    webSocket.begin();
    webSocket.onEvent(handleWebSocketEvent);
    // Enable heartbeat with longer timeout (30 seconds)
  webSocket.enableHeartbeat(60000, 10000, 10); // ping every 60s, pong timeout 10s, allow 10 missed pongs
    WEB_INFO_F("WebSocket server started with heartbeat enabled");
    
    // Start HTTP server
    setupHttpServer();
    
    // Show memory status after connection
    size_t freeHeapAfter = ESP.getFreeHeap();
    WEB_INFO("Free heap after WiFi connection: ", freeHeapAfter);
  } else {
    WEB_ERROR_F("Timeout: Fail to connect web server!");
  }
  return webServerConnected;
}
#endif

#ifdef WIFI_MANAGER
void startWifiManager() {
#ifdef I2C_EEPROM_ADDRESS
  i2c_eeprom_write_byte(EEPROM_WIFI_MANAGER, false);
#else
  config.putBool("WifiManager", false);
#endif

  WiFiManager wm;
  wm.setConfigPortalTimeout(60);
  if (!wm.autoConnect((uniqueName + " WifiConfig").c_str())) {
    WEB_ERROR_F("Fail to connect Wifi. Rebooting.");
    delay(3000);
    ESP.restart();
  } else {
    webServerConnected = true;
    printToAllPorts("Successfully connected Wifi to IP Address: " + WiFi.localIP().toString());
  }

  if (webServerConnected) {
    // Start WebSocket server
    webSocket.begin();
    webSocket.onEvent(handleWebSocketEvent);
    // Enable heartbeat with longer timeout (30 seconds)
  webSocket.enableHeartbeat(60000, 10000, 10); // ping every 60s, pong timeout 10s, allow 10 missed pongs
    WEB_INFO_F("WebSocket server started with heartbeat enabled");
    
    // Start HTTP server
    setupHttpServer();
  } else {
    WEB_ERROR_F("Timeout: Fail to connect web server!");
  }

#ifdef I2C_EEPROM_ADDRESS
  i2c_eeprom_write_byte(EEPROM_WIFI_MANAGER, webServerConnected);
#else
  config.putBool("WifiManager", webServerConnected);
#endif
}
#endif

void resetWifiManager() {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  delay(2000);
  if (esp_wifi_restore() != ESP_OK) {
    WEB_ERROR_F("\nWiFi is not initialized by esp_wifi_init ");
  } else {
    WEB_INFO_F("\nWiFi Configurations Cleared!");
  }
  delay(2000);
  ESP.restart();
}

// Main loop function call
void WebServerLoop()
{
  if (webServerConnected) {
    webSocket.loop();
    
    // Monitor BLE activity impact on WebSocket
    static unsigned long lastBleStatusLog = 0;
    unsigned long currentTime = millis();
    
    if (currentTime - lastBleStatusLog > 30000) { // Log status every 30 seconds
#ifdef BT_CLIENT
      extern boolean doScan;
      extern boolean btConnected;
      if (doScan || btConnected) {
        WEB_INFO("BLE active - doScan: ", doScan);
        WEB_INFO("BLE connected: ", btConnected);
        WEB_INFO("Active WebSocket clients: ", connectedClients.size());
      }
#endif
      lastBleStatusLog = currentTime;
    }

    // Periodically check connection health
    if (currentTime - lastHealthCheckTime > HEALTH_CHECK_INTERVAL) {
      checkConnectionHealth();
      lastHealthCheckTime = currentTime;
    }

    // Check task timeout
    for (auto &pair : webTasks) {
      WebTask &task = pair.second;
      if (task.status == "running" && task.startTime > 0) {
        if (currentTime - task.startTime > WEB_TASK_EXECUTION_TIMEOUT) { // Use configured task execution timeout
          WEB_ERROR("web task timeout: ", task.taskId);
          task.status = "error";
          task.resultReady = true;

          // Send timeout status to client
          sendSocketResponse(task.clientId, "{\"taskId\":\"" + task.taskId + "\",\"status\":\"error\",\"error\":\"Task timeout\"}");

          if (task.taskId == currentWebTaskId) {
            cmdFromWeb = false;
            webTaskActive = false;
            currentWebTaskId = "";
            processNextWebTask();
          }
        }
      }
    }
  }
  
  // JOINT UPDATE: Process pending joint updates with rate limiting
  if (jointUpdatePending) {
    sendJointUpdateIfChanged();
  }
  
  // Handle HTTP server requests
  httpServer.handleClient();
}

// HTTP Server Implementation
void setupHttpServer() {
  // Check available memory before starting HTTP server
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 25000) { // Require at least 25KB free for HTTP server
    WEB_ERROR("Insufficient memory to start HTTP server. Free heap: ", freeHeap);
    return;
  }
  
  // Route handlers
  httpServer.on("/", handleRoot);
  httpServer.on("/console", handleConsole);
  httpServer.on("/command", HTTP_POST, handleCommand);
  httpServer.onNotFound(handleNotFound);
  
  // Start the server
  httpServer.begin();
  WEB_INFO_F("HTTP server started on port 80");
  WEB_INFO("Free heap after HTTP server start: ", ESP.getFreeHeap());
}

void handleRoot() {
  // Redirect to console interface for direct access
  httpServer.sendHeader("Location", "/console");
  httpServer.send(302, "text/plain", "Redirecting to console interface...");
}

void handleConsole() {
  // Check available memory - minimal version
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 8000) { // Very low threshold for minimal page
    httpServer.send(503, "text/plain", "Low memory: " + String(freeHeap) + " bytes");
    return;
  }
  
  String html = R"rawliteral(<!DOCTYPE html><html><head><title>OpenCat Console</title>
<style>html,body{height:100%;margin:0;padding:10px;box-sizing:border-box;font-family:monospace;background:#000;color:#0f0}
#out{height:85vh;overflow-y:auto;border:1px solid #444;padding:5px;background:#111}
#cmd{width:60%;padding:5px;background:#222;color:#0f0;border:1px solid #444}
.btn{padding:4px 8px;margin:2px;background:#333;color:#0f0;border:1px solid #444;cursor:pointer}
.btn:hover{background:#444}
.help-pane{flex:1;min-width:180px;max-width:260px;background:#111;border:1px solid #444;padding:10px;margin-left:10px;font-size:14px;overflow-y:auto}
</style></head><body>
<h3>OpenCat Console <span id="status" style="color:#ff0">*</span></h3>
<div style="display:flex;gap:10px">
  <div style="flex:2">
    <div id="out">Ready. Free: )rawliteral" + String(freeHeap) + R"rawliteral( bytes<br></div>
    <input id="cmd" placeholder="Enter command...">
    <button class="btn" onclick="send()">Send</button>
    <button class="btn" onclick="clear()">Clear</button><br>
    <button class="btn" onclick="q('ksit')">Sit</button>
    <button class="btn" onclick="q('kup')">Up</button>
    <button class="btn" onclick="q('d')">Rest</button>
    <button class="btn" onclick="q('h')">Help</button>
  </div>
  <div class="help-pane">
    <b>Help & Commands</b><hr style="border:1px solid #222">
    <ul style="padding-left:18px;margin:0">
  <li><b>wkF</b>: Walk forward</li>
  <li><b>bk</b>: Walk backward</li>
  <li><b>tbl</b>: Turn body left</li>
  <li><b>str</b>: Stretch</li>
  <li><b>pu</b>: Push up</li>
  <li><b>rol</b>: Roll</li>
  <li><b>hi</b>: Say hi</li>
  <li><b>ksit</b>: Sit down</li>
  <li><b>kup</b>: Stand up</li>
      <li><b>d</b>: Rest (power off servos)</li>
      <li><b>h</b>: Show help</li>
      <li><b>g</b>: Toggle gyro/IMU</li>
      <li><b>j</b>: Show joint angles</li>
      <li><b>P</b>: Show battery voltage</li>
      <li><b>c</b>: Calibrate servos</li>
      <li><b>f</b>: Servo feedback</li>
      <li><b>l</b>: Adjust balance slope</li>
      <li><b>n</b>: Set Bluetooth name</li>
      <li><b>u</b>: Meow (sound)</li>
      <li><b>w</b>: WiFi info</li>
      <li><b>z</b>: Toggle random mind</li>
      <li><b>R</b>: Robot arm control</li>
      <li><b>s</b>: Save settings</li>
      <li><b>t</b>: Tilt</li>
      <li><b>?</b>: Query status</li>
      <li><b>!</b>: Reset</li>
      <li><b>X...</b>: Extension/module command</li>
    </ul>
    <div style="margin:10px 0 4px 0;font-size:13px;color:#0ff"><b>Active Modules:</b></div>
    <ul style="padding-left:18px;margin:0">
      <li>Voice</li>
      <li>BackTouch</li>
      <li>Ultrasonic</li>
      <!-- Add more modules as detected/activated in code -->
    </ul>
    <div style="margin-top:8px;font-size:12px;color:#aaa">Type a command or use the buttons.<br>See documentation for more.<br>Module commands: <b>X</b> + module code (see docs).</div>
  </div>
</div>

<script>
// Client-side keep-alive ping to keep WebSocket connection active
setInterval(() => {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify({type: "ping"}));
}, 10000); // every 10 seconds
let ws;
function log(msg,type){let out=document.getElementById('out');let color=type==='command'?'#ff0':type==='error'?'#f66':'#0f0';out.innerHTML+='<br><span style="color:'+color+'">'+msg+'</span>';out.scrollTop=999999}
function send(){let c=document.getElementById('cmd').value.trim();if(!c)return;log('> '+c,'command');
if(ws&&ws.readyState===1){ws.send(JSON.stringify({type:'command',commands:[c]}));log('> Sent via WebSocket','normal')}
else{fetch('/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'command='+encodeURIComponent(c)}).then(r=>r.text()).then(d=>log(d,'normal')).catch(e=>log('Error: '+e,'error'))}
document.getElementById('cmd').value=''}
function q(c){document.getElementById('cmd').value=c;send()}
function clear(){document.getElementById('out').innerHTML='Console cleared'}

function connect(){
ws=new WebSocket('ws://'+location.hostname+':81');
ws.onopen=()=>{log('WebSocket connected');document.getElementById('status').style.color='#0f0';patchWS();};
ws.onmessage=e=>{try{let d=JSON.parse(e.data);if(d.type==='joint_data'&&d.angles){updateJointDisplay(d.angles)}else if(d.type==='response'&&d.results){d.results.forEach(r=>{log('Result: '+r);if(r.includes('=')||r.match(/-?\d+,\s*-?\d+/)){parseJointAngles(r)}})}else if(d.type==='robot_output'){let msg=d.message.replace(/\n$/,'');log(msg);if(msg.includes('=')||msg.match(/-?\d+,\s*-?\d+/)){parseJointAngles(msg)}}else{log('WS: '+JSON.stringify(d))}}catch{log('WS: '+e.data)}};
ws.onclose=()=>{log('WebSocket closed');document.getElementById('status').style.color='#ff0';stopAutoJointUpdates();setTimeout(connect,3000)};
ws.onerror=()=>{log('WebSocket error');document.getElementById('status').style.color='#f00';stopAutoJointUpdates()}}
document.getElementById('cmd').onkeydown=e=>{if(e.key==='Enter')send()}
setTimeout(connect,1000)


</script></body></html>
)rawliteral";
  
  httpServer.send(200, "text/html", html);
}

void handleCommand() {
  if (!httpServer.hasArg("command")) {
    httpServer.send(400, "text/plain", "Error: No command provided");
    return;
  }
  
  String command = httpServer.arg("command");
  
  // Basic command validation
  if (command.length() == 0 || command.length() > 50) {
    httpServer.send(400, "text/plain", "Error: Invalid command length");
    return;
  }
  
  // Process the command and capture detailed output
  if (command.length() > 0) {
    // Clear previous web response
    webResponse = "";
    
    // Set up command processing
    token = command[0];
    strcpy(newCmd, command.c_str() + 1);
    cmdLen = command.length() - 1;
    newCmd[cmdLen] = '\0';
    newCmdIdx = 4; // Set to process command
    
    // Create verbose response with detailed information
    String response = ">>> Executing Command: '" + command + "'\n";
    response += "Token: '" + String(token) + "'\n";
    response += "Arguments: '" + String(newCmd) + "'\n";
    response += "Argument Length: " + String(cmdLen) + "\n";
    response += "Timestamp: " + String(millis()) + "ms\n";
    response += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n\n";
    
    // Add command-specific information and expected output
    response += "Expected Action:\n";
    if (command.startsWith("k")) {
      String skill = command.substring(1);
      response += "- Execute skill: '" + skill + "'\n";
      response += "- Robot will move to new posture\n";
      response += "- Check WebSocket for real-time feedback\n";
    } else if (command == "d") {
      response += "- Set robot to rest position\n";
      response += "- All servos will be turned off\n";
    } else if (command == "g") {
      response += "- Toggle gyro/IMU functionality\n";
      response += "- Balance control will be affected\n";
    } else if (command == "j") {
      response += "- Display all joint angles\n";
      response += "- Check serial output for detailed readings\n";
    } else if (command == "P") {
      response += "- Display battery voltage\n";
      response += "- Check serial output for voltage reading\n";
    } else if (command.startsWith("b")) {
      response += "- Play sound/beep sequence\n";
      response += "- Listen for audio feedback from robot\n";
    } else if (command.startsWith("i")) {
      response += "- Set joint positions individually\n";
      response += "- Servos will move to specified angles\n";
    } else if (command.startsWith("X")) {
      response += "- Execute extension module command\n";
      response += "- Module-specific functionality activated\n";
    } else {
      response += "- Execute custom command\n";
      response += "- Refer to OpenCat documentation for details\n";
    }
    
    response += "\n";
    
    // Add comprehensive help if requested
    if (command == "h" || command == "help") {
      if (ESP.getFreeHeap() < 20000) {
        response += "Help available - use WebSocket console for full help (insufficient memory for HTTP help)";
      } else {
        response += "=== OPENCAT ROBOT COMMANDS ===\n\n";
        // List all skills
        extern const char* skillNameWithType[];
        response += "SKILLS (use k<skill> to run):\n  ";
        int skillIdx = 0;
        while (true) {
          const char* skill = skillNameWithType[skillIdx];
          if (!skill) break;
          response += skill;
          skillIdx++;
          if (skillNameWithType[skillIdx]) response += ", ";
        }
        response += "\n\n";
        // List system commands
        response += "SYSTEM COMMANDS:\n  d (rest), g (gyro), j (joints), P (battery), i (head), c (calibrate)\n\n";
        // List sound/extension commands
        response += "SOUND: b (beep), u (meow)\n";
        response += "EXTENSIONS: XCP (camera), XCR (reactions)\n";
        response += "JOINTS: i0 45 (move joint 0 to 45deg)\n\n";
        // List custom/voice commands
        extern String customizedCmdList[];
        extern int listLength;
        response += "CUSTOM/VOICE COMMANDS:\n  ";
        for (int i = 0; i < listLength; i++) {
          response += customizedCmdList[i];
          if (i < listLength - 1) response += ", ";
        }
        response += "\n\nUse WebSocket for real-time feedback and detailed help.\n";
      }
    }
    
    response += "Status: Command queued for execution\n";
    response += "Use WebSocket connection for real-time response monitoring.";
    
    httpServer.send(200, "text/plain", response);
  } else {
    httpServer.send(400, "text/plain", "Error: Empty command");
  }
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "Available paths:\n";
  message += "/ - Robot home page\n";
  message += "/console - Interactive console\n";
  message += "\nWebSocket available at port 81";
  
  httpServer.send(404, "text/plain", message);
}

// Enhanced output capture functions
void sendRobotOutput(String output) {
  if (!webServerConnected || connectedClients.empty() || output.length() == 0) {
    return;
  }

  JsonDocument outputDoc;
  outputDoc["type"] = "robot_output";
  outputDoc["message"] = output;
  outputDoc["timestamp"] = millis();

  String outputData;
  serializeJson(outputDoc, outputData);

  // Send to all connected WebSocket clients
  for (auto &client : connectedClients) {
    if (client.second) { // If client is still connected
      webSocket.sendTXT(client.first, outputData);
    }
  }
}

void sendSystemInfo() {
  if (!webServerConnected || connectedClients.empty()) {
    return;
  }

  JsonDocument infoDoc;
  infoDoc["type"] = "system_info";
  infoDoc["model"] = MODEL;
  infoDoc["software_version"] = SoftwareVersion;
  infoDoc["ip_address"] = WiFi.localIP().toString();
  infoDoc["free_heap"] = ESP.getFreeHeap();
  infoDoc["uptime"] = millis();
#ifdef VOLTAGE
  extern float lastVoltage; // Declare as extern since it's defined in OpenCat.h
  infoDoc["battery_voltage"] = lastVoltage;
#endif
  infoDoc["wifi_rssi"] = WiFi.RSSI();
  infoDoc["timestamp"] = millis();

  String infoData;
  serializeJson(infoDoc, infoData);

  // Send to all connected WebSocket clients
  for (auto &client : connectedClients) {
    if (client.second) {
      webSocket.sendTXT(client.first, infoData);
    }
  }
}


