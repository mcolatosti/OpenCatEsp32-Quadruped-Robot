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
void sendTelemetry();

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
      
      // Send current joint positions immediately on connect
      {
        String jointData = "{\"type\":\"joint_data\"," + getJointAnglesJson().substring(1);
        webSocket.sendTXT(num, jointData);
      }
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
        leftTrimSpaces(newCmd, &cmdLen);  // allow space between token and parameters, such as "k sit"
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
  
  // TELEMETRY: Send periodic IMU/battery data
  sendTelemetry();
  
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
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%;font-family:monospace;background:#000;color:#0f0}
.top-panel{height:40vh;display:flex;border-bottom:2px solid #444;padding:8px;gap:8px}
.bot-panel{height:60vh;display:flex;padding:8px;gap:8px}
.servo-view{flex:2;background:#111;border:1px solid #444;padding:10px;position:relative;overflow:hidden}
.telem-panel{flex:0 0 200px;min-width:200px;max-width:280px;background:#111;border:1px solid #444;padding:10px;overflow-y:auto;font-size:13px}
.gamepad-panel{flex:0 0 320px;min-width:320px;max-width:380px;background:#111;border:1px solid #444;padding:10px;overflow-y:auto}
.console-area{flex:2;display:flex;flex-direction:column}
#out{flex:1;overflow-y:auto;border:1px solid #444;padding:5px;background:#111;font-size:13px}
.cmd-bar{display:flex;gap:4px;margin-top:4px}
#cmd{flex:1;padding:5px;background:#222;color:#0f0;border:1px solid #444;font-family:monospace}
.btn{padding:4px 8px;margin:1px;background:#333;color:#0f0;border:1px solid #444;cursor:pointer;font-size:12px}
.btn:hover{background:#444}
.help-pane{flex:0 0 250px;min-width:250px;max-width:380px;background:#111;border:1px solid #444;padding:8px;overflow-y:auto;font-size:12px}
.robot-body{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);width:290px;height:290px}
.joint-box{position:absolute;background:#222;border:1px solid #555;padding:2px 4px;font-size:11px;text-align:center;border-radius:3px;min-width:42px}
.joint-box .lbl{color:#888;font-size:9px}
.joint-box .val{color:#0f0;font-weight:bold}
.body-rect{position:absolute;top:70px;left:95px;width:100px;height:180px;border:2px solid #555;border-radius:12px;background:#1a1a1a}
.head-circ{position:absolute;top:30px;left:125px;width:40px;height:40px;border:2px solid #555;border-radius:50%;background:#1a1a1a}
.leg-line{position:absolute;background:#444;height:2px}
.section-hdr{color:#0ff;font-size:12px;font-weight:bold;margin:8px 0 4px 0;border-bottom:1px solid #333;padding-bottom:2px}
.telem-row{display:flex;justify-content:space-between;padding:2px 0}
.telem-lbl{color:#888}
.telem-val{color:#0f0;font-weight:bold}
.telem-val.warn{color:#ff0}
.telem-val.crit{color:#f44}
.imu-bar{height:6px;background:#333;margin:2px 0;border-radius:3px;overflow:hidden}
.imu-fill{height:100%;background:#0f0;transition:width 0.2s}
</style></head><body>

<!-- TOP PANEL: Servo View + Telemetry -->
<div class="top-panel">
  <div class="servo-view">
    <div style="font-size:11px;color:#888;margin-bottom:4px">Servo Positions (top-down view) <span id="status" style="color:#ff0">&#9679;</span></div>
    <div class="robot-body">
      <!-- Head -->
      <div class="head-circ"></div>
      <div class="joint-box" style="top:10px;left:110px" id="j0"><div class="lbl">Pan</div><div class="val" id="v0">--</div></div>
      <div class="joint-box" style="top:48px;left:170px" id="j1"><div class="lbl">Tilt</div><div class="val" id="v1">--</div></div>
      <!-- Body outline -->
      <div class="body-rect"></div>
      <!-- Tail -->
      <div class="joint-box" style="top:260px;left:110px" id="j2"><div class="lbl">Tail</div><div class="val" id="v2">--</div></div>
      <!-- Left Front leg (top-left) -->
      <div class="joint-box" style="top:60px;left:0px" id="j4"><div class="lbl">LF Roll</div><div class="val" id="v4">--</div></div>
      <div class="joint-box" style="top:90px;left:0px" id="j8"><div class="lbl">LF Shldr</div><div class="val" id="v8">--</div></div>
      <div class="joint-box" style="top:120px;left:0px" id="j12"><div class="lbl">LF Knee</div><div class="val" id="v12">--</div></div>
      <!-- Right Front leg (top-right) -->
      <div class="joint-box" style="top:60px;left:210px" id="j5"><div class="lbl">RF Roll</div><div class="val" id="v5">--</div></div>
      <div class="joint-box" style="top:90px;left:210px" id="j9"><div class="lbl">RF Shldr</div><div class="val" id="v9">--</div></div>
      <div class="joint-box" style="top:120px;left:210px" id="j13"><div class="lbl">RF Knee</div><div class="val" id="v13">--</div></div>
      <!-- Left Back leg (bottom-left) -->
      <div class="joint-box" style="top:170px;left:0px" id="j6"><div class="lbl">LB Roll</div><div class="val" id="v6">--</div></div>
      <div class="joint-box" style="top:200px;left:0px" id="j10"><div class="lbl">LB Shldr</div><div class="val" id="v10">--</div></div>
      <div class="joint-box" style="top:230px;left:0px" id="j14"><div class="lbl">LB Knee</div><div class="val" id="v14">--</div></div>
      <!-- Right Back leg (bottom-right) -->
      <div class="joint-box" style="top:170px;left:210px" id="j7"><div class="lbl">RB Roll</div><div class="val" id="v7">--</div></div>
      <div class="joint-box" style="top:200px;left:210px" id="j11"><div class="lbl">RB Shldr</div><div class="val" id="v11">--</div></div>
      <div class="joint-box" style="top:230px;left:210px" id="j15"><div class="lbl">RB Knee</div><div class="val" id="v15">--</div></div>
    </div>
  </div>

  <!-- Telemetry Panel -->
  <div class="telem-panel">
    <div class="section-hdr">Battery</div>
    <div class="telem-row"><span class="telem-lbl">Voltage</span><span class="telem-val" id="bat-v">--</span></div>
    <div class="imu-bar"><div class="imu-fill" id="bat-bar" style="width:0%"></div></div>

    <div class="section-hdr">IMU Orientation</div>
    <div class="telem-row"><span class="telem-lbl">Yaw</span><span class="telem-val" id="imu-yaw">--</span></div>
    <div class="imu-bar"><div class="imu-fill" id="yaw-bar" style="width:50%;background:#08f"></div></div>
    <div class="telem-row"><span class="telem-lbl">Pitch</span><span class="telem-val" id="imu-pitch">--</span></div>
    <div class="imu-bar"><div class="imu-fill" id="pitch-bar" style="width:50%;background:#f80"></div></div>
    <div class="telem-row"><span class="telem-lbl">Roll</span><span class="telem-val" id="imu-roll">--</span></div>
    <div class="imu-bar"><div class="imu-fill" id="roll-bar" style="width:50%;background:#0f8"></div></div>

    <div class="section-hdr">Status</div>
    <div class="telem-row"><span class="telem-lbl">Skill</span><span class="telem-val" id="cur-skill">--</span></div>
    <div class="telem-row"><span class="telem-lbl">Uptime</span><span class="telem-val" id="uptime">--</span></div>
    <div class="telem-row"><span class="telem-lbl">Heap</span><span class="telem-val" id="heap">--</span></div>
  </div>

  <!-- Gamepad Status Panel -->
  <div class="gamepad-panel">
    <div style="font-size:11px;color:#888;margin-bottom:6px"><b style="color:#0ff">Gamepad</b> <span id="gp-status" style="color:#f44">&#9679; Disconnected</span></div>
    <div style="display:flex;gap:10px;align-items:flex-start">
      <!-- Left Stick Visualization -->
      <div style="text-align:center">
        <div style="font-size:9px;color:#888;margin-bottom:2px">Left Stick</div>
        <svg width="140" height="140" viewBox="0 0 140 140" style="display:block">
          <!-- Outer ring -->
          <circle cx="70" cy="70" r="65" fill="#1a1a1a" stroke="#444" stroke-width="2"/>
          <!-- Direction zones (subtle) -->
          <line x1="70" y1="5" x2="70" y2="135" stroke="#333" stroke-width="1"/>
          <line x1="5" y1="70" x2="135" y2="70" stroke="#333" stroke-width="1"/>
          <line x1="23" y1="23" x2="117" y2="117" stroke="#222" stroke-width="1"/>
          <line x1="117" y1="23" x2="23" y2="117" stroke="#222" stroke-width="1"/>
          <!-- Direction labels -->
          <text x="70" y="18" fill="#0f0" font-size="8" text-anchor="middle">wkF</text>
          <text x="70" y="132" fill="#0f0" font-size="8" text-anchor="middle">bkF</text>
          <text x="14" y="73" fill="#0f0" font-size="8" text-anchor="middle">trL</text>
          <text x="126" y="73" fill="#0f0" font-size="8" text-anchor="middle">trR</text>
          <text x="28" y="30" fill="#ff0" font-size="7" text-anchor="middle">wkL</text>
          <text x="112" y="30" fill="#ff0" font-size="7" text-anchor="middle">wkR</text>
          <text x="28" y="118" fill="#ff0" font-size="7" text-anchor="middle">bkL</text>
          <text x="112" y="118" fill="#ff0" font-size="7" text-anchor="middle">bkR</text>
          <!-- Stick position indicator -->
          <circle id="gp-lstick" cx="70" cy="70" r="8" fill="#0f0" opacity="0.8"/>
          <!-- Center dot -->
          <circle cx="70" cy="70" r="3" fill="#444"/>
        </svg>
        <div style="font-size:9px;color:#666;margin-top:2px" id="gp-lstick-val">0, 0</div>
      </div>
      <!-- Right Stick Visualization -->
      <div style="text-align:center">
        <div style="font-size:9px;color:#888;margin-bottom:2px">Right Stick</div>
        <svg width="100" height="100" viewBox="0 0 100 100" style="display:block">
          <circle cx="50" cy="50" r="45" fill="#1a1a1a" stroke="#444" stroke-width="2"/>
          <line x1="50" y1="5" x2="50" y2="95" stroke="#333" stroke-width="1"/>
          <line x1="5" y1="50" x2="95" y2="50" stroke="#333" stroke-width="1"/>
          <text x="50" y="16" fill="#888" font-size="7" text-anchor="middle">Tilt&#8593;</text>
          <text x="50" y="94" fill="#888" font-size="7" text-anchor="middle">Tilt&#8595;</text>
          <text x="10" y="53" fill="#888" font-size="7" text-anchor="middle">&#8592;Pan</text>
          <text x="90" y="53" fill="#888" font-size="7" text-anchor="middle">Pan&#8594;</text>
          <!-- Stick position indicator -->
          <circle id="gp-rstick" cx="50" cy="50" r="6" fill="#08f" opacity="0.8"/>
          <circle cx="50" cy="50" r="2" fill="#444"/>
        </svg>
        <div style="font-size:9px;color:#666;margin-top:2px" id="gp-rstick-val">0, 0</div>
      </div>
    </div>
    <!-- Buttons Status -->
    <div style="margin-top:8px">
      <div style="font-size:9px;color:#888;margin-bottom:4px">Buttons</div>
      <div style="display:grid;grid-template-columns:repeat(4,1fr);gap:3px;text-align:center">
        <div id="gp-btn-y" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">Y</span><br><span style="color:#555">Hi</span></div>
        <div id="gp-btn-x" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">X</span><br><span style="color:#555">Str</span></div>
        <div id="gp-btn-a" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">A</span><br><span style="color:#555">Up</span></div>
        <div id="gp-btn-b" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">B</span><br><span style="color:#555">Sit</span></div>
        <div id="gp-btn-l" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">L</span><br><span style="color:#555">Trot</span></div>
        <div id="gp-btn-r" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">R</span><br><span style="color:#555">Crawl</span></div>
        <div id="gp-btn-zl" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">ZL</span><br><span style="color:#555">PU</span></div>
        <div id="gp-btn-zr" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px"><span style="color:#888">ZR</span><br><span style="color:#555">Spcl</span></div>
      </div>
      <div style="display:grid;grid-template-columns:repeat(4,1fr);gap:3px;text-align:center;margin-top:3px">
        <div id="gp-dpad-l" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px;color:#555">&#9664;</div>
        <div id="gp-dpad-u" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px;color:#555">&#9650;</div>
        <div id="gp-dpad-d" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px;color:#555">&#9660;</div>
        <div id="gp-dpad-r" style="background:#222;border:1px solid #444;border-radius:3px;padding:2px;font-size:9px;color:#555">&#9654;</div>
      </div>
    </div>
    <div style="margin-top:6px;font-size:9px;color:#555">Last: <span id="gp-last-cmd">--</span></div>
  </div>
</div>

<!-- BOTTOM PANEL: Console + Help -->
<div class="bot-panel">
  <div class="console-area">
    <div id="out">Ready. Free: )rawliteral" + String(freeHeap) + R"rawliteral( bytes<br></div>
    <div class="cmd-bar">
      <input id="cmd" placeholder="Enter command...">
      <button class="btn" onclick="send()">Send</button>
      <button class="btn" onclick="clear()">Clear</button>
      <button class="btn" onclick="q('k sit')">Sit</button>
      <button class="btn" onclick="q('k up')">Up</button>
      <button class="btn" onclick="q('d')">Rest</button>
      <button class="btn" onclick="q('h')">Help</button>
    </div>
  </div>
  <div class="help-pane">
    <b>Help & Commands</b><hr style="border:1px solid #222">
    <div style="margin:6px 0 2px 0;font-size:12px;color:#0ff"><b>Movement (prefix k):</b></div>
    <ul style="padding-left:14px;margin:0;font-size:12px">
      <li><b>k wkF</b> walk forward, <b>k wkL</b> left, <b>k wkR</b> right</li>
      <li><b>k bk</b> backward, <b>k bkL</b> back left, <b>k bkR</b> back right</li>
      <li><b>k trF</b> trot forward, <b>k trL</b> trot left, <b>k trR</b> trot right</li>
      <li><b>k crF</b> crawl forward, <b>k crL</b> crawl left, <b>k crR</b> crawl right</li>
      <li><b>k vtF</b> step forward, <b>k vtL</b> step left</li>
      <li><b>k tbl</b> table</li>
    </ul>
    <div style="margin:6px 0 2px 0;font-size:12px;color:#0ff"><b>Postures (prefix k):</b></div>
    <ul style="padding-left:14px;margin:0;font-size:12px">
      <li><b>k sit</b> sit down</li>
      <li><b>k up</b> stand up</li>
      <li><b>k rest</b> rest posture</li>
      <li><b>k balance</b> balance</li>
      <li><b>k str</b> stretch</li>
      <li><b>k buttUp</b> butt up</li>
      <li><b>k zero</b> zero position</li>
      <li><b>k hi</b> wave hello</li>
      <li><b>k pu</b> push up</li>
    </ul>
    <div style="margin:6px 0 2px 0;font-size:12px;color:#0ff"><b>System:</b></div>
    <ul style="padding-left:14px;margin:0;font-size:12px">
      <li><b>d</b> rest (servos off)</li>
      <li><b>p</b> pause / resume</li>
      <li><b>g</b> toggle gyro</li>
      <li><b>j</b> joints, <b>P</b> battery, <b>c</b> calibrate</li>
      <li><b>f</b> feedback, <b>n</b> BT name, <b>w</b> WiFi</li>
      <li><b>b</b> beep, <b>u</b> meow, <b>z</b> random</li>
      <li><b>!</b> reset, <b>?</b> query, <b>h</b> help</li>
      <li><b>i0 45</b> move joint 0 to 45&deg;</li>
    </ul>
  </div>
</div>

<script>
setInterval(()=>{if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:"ping"}))},10000);
let ws;
function log(msg,type){let out=document.getElementById('out');let color=type==='command'?'#ff0':type==='error'?'#f66':'#0f0';out.innerHTML+='<br><span style="color:'+color+'">'+msg+'</span>';out.scrollTop=999999}
function send(){let c=document.getElementById('cmd').value.trim();if(!c)return;log('> '+c,'command');
if(ws&&ws.readyState===1){ws.send(JSON.stringify({type:'command',commands:[c]}))}
else{fetch('/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'command='+encodeURIComponent(c)}).then(r=>r.text()).then(d=>log(d,'normal')).catch(e=>log('Error: '+e,'error'))}
document.getElementById('cmd').value=''}
function q(c){document.getElementById('cmd').value=c;send()}
function clear(){document.getElementById('out').innerHTML='Console cleared'}

// Update servo position display
function updateJointDisplay(angles){
  for(let i=0;i<16;i++){
    let el=document.getElementById('v'+i);
    if(el){el.textContent=angles[i]+'°';
      let v=Math.abs(angles[i]);
      el.style.color=v>80?'#f44':v>50?'#ff0':'#0f0';
    }
  }
}

// Update telemetry panel
function updateTelemetry(d){
  if(d.battery!==undefined){
    let el=document.getElementById('bat-v');
    el.textContent=d.battery.toFixed(1)+'V';
    let pct=Math.max(0,Math.min(100,((d.battery-6.0)/2.4)*100));
    document.getElementById('bat-bar').style.width=pct+'%';
    document.getElementById('bat-bar').style.background=pct<20?'#f44':pct<40?'#ff0':'#0f0';
    el.className='telem-val'+(pct<20?' crit':pct<40?' warn':'');
  }
  if(d.yaw!==undefined){
    document.getElementById('imu-yaw').textContent=d.yaw.toFixed(1)+'°';
    document.getElementById('yaw-bar').style.width=((d.yaw+180)/360*100)+'%';
  }
  if(d.pitch!==undefined){
    document.getElementById('imu-pitch').textContent=d.pitch.toFixed(1)+'°';
    document.getElementById('pitch-bar').style.width=((d.pitch+90)/180*100)+'%';
  }
  if(d.roll!==undefined){
    document.getElementById('imu-roll').textContent=d.roll.toFixed(1)+'°';
    document.getElementById('roll-bar').style.width=((d.roll+90)/180*100)+'%';
  }
  if(d.skill)document.getElementById('cur-skill').textContent=d.skill;
  if(d.ts){
    let s=Math.floor(d.ts/1000);
    let m=Math.floor(s/60);s%=60;
    let h=Math.floor(m/60);m%=60;
    document.getElementById('uptime').textContent=h+'h'+m+'m'+s+'s';
  }
}

function parseJointAngles(msg){
  let m=msg.match(/-?\d+/g);
  if(m&&m.length>=16){updateJointDisplay(m.map(Number))}
}
function patchWS(){if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'get_system_info'}))}
function stopAutoJointUpdates(){}

// Gamepad UI update functions
function updateGamepad(d){
  // Connection status
  if(d.connected!==undefined){
    let el=document.getElementById('gp-status');
    el.innerHTML=d.connected?'&#9679; Connected':'&#9679; Disconnected';
    el.style.color=d.connected?'#0f0':'#f44';
  }
  // Left stick position (values -512 to 512, map to SVG coords 5-135)
  if(d.lx!==undefined&&d.ly!==undefined){
    let sx=70+(d.lx/512)*55;
    let sy=70+(d.ly/512)*55;
    let el=document.getElementById('gp-lstick');
    if(el){el.setAttribute('cx',sx);el.setAttribute('cy',sy)}
    document.getElementById('gp-lstick-val').textContent=d.lx+', '+d.ly;
  }
  // Right stick position (map to SVG coords 5-95)
  if(d.rx!==undefined&&d.ry!==undefined){
    let sx=50+(d.rx/512)*38;
    let sy=50+(d.ry/512)*38;
    let el=document.getElementById('gp-rstick');
    if(el){el.setAttribute('cx',sx);el.setAttribute('cy',sy)}
    document.getElementById('gp-rstick-val').textContent=d.rx+', '+d.ry;
  }
  // Buttons (bitmask)
  if(d.buttons!==undefined){
    let b=d.buttons;
    setBtn('gp-btn-a',b&0x0001);setBtn('gp-btn-b',b&0x0002);
    setBtn('gp-btn-x',b&0x0004);setBtn('gp-btn-y',b&0x0008);
    setBtn('gp-btn-l',b&0x0010);setBtn('gp-btn-r',b&0x0020);
    setBtn('gp-btn-zl',b&0x0040);setBtn('gp-btn-zr',b&0x0080);
  }
  // D-pad
  if(d.dpad!==undefined){
    let dp=d.dpad;
    setBtn('gp-dpad-u',dp&0x01);setBtn('gp-dpad-d',dp&0x02);
    setBtn('gp-dpad-r',dp&0x04);setBtn('gp-dpad-l',dp&0x08);
  }
  // Last command
  if(d.cmd){document.getElementById('gp-last-cmd').textContent=d.cmd;document.getElementById('gp-last-cmd').style.color='#0f0'}
}
function setBtn(id,active){let el=document.getElementById(id);if(el){el.style.background=active?'#0a3':'#222';el.style.borderColor=active?'#0f0':'#444'}}

function connect(){
ws=new WebSocket('ws://'+location.hostname+':81');
ws.onopen=()=>{log('Connected');document.getElementById('status').style.color='#0f0';patchWS()};
ws.onmessage=e=>{try{let d=JSON.parse(e.data);
  if(d.type==='joint_data'&&d.angles){updateJointDisplay(d.angles)}
  else if(d.type==='telemetry'){updateTelemetry(d);if(d.angles)updateJointDisplay(d.angles)}
  else if(d.type==='system_info'){document.getElementById('heap').textContent=Math.round(d.free_heap/1024)+'KB';if(d.battery_voltage)updateTelemetry({battery:d.battery_voltage,ts:d.uptime})}
  else if(d.type==='gamepad'){updateGamepad(d)}
  else if(d.type==='response'&&d.results){d.results.forEach(r=>{log('Result: '+r);parseJointAngles(r)})}
  else if(d.type==='robot_output'){let msg=d.message.replace(/\n$/,'');log(msg);parseJointAngles(msg)}
  else if(d.type!=='heartbeat'){log('WS: '+JSON.stringify(d))}
}catch(x){log('WS: '+e.data)}};
ws.onclose=()=>{log('Disconnected');document.getElementById('status').style.color='#ff0';setTimeout(connect,3000)};
ws.onerror=()=>{log('WS Error');document.getElementById('status').style.color='#f00'}}
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

// Periodic telemetry: IMU + battery data
unsigned long lastTelemetryTime = 0;
const unsigned long TELEMETRY_INTERVAL = 200;  // 200ms = 5Hz

void sendTelemetry() {
  if (!webServerConnected || connectedClients.empty()) {
    return;
  }
  unsigned long now = millis();
  if (now - lastTelemetryTime < TELEMETRY_INTERVAL) {
    return;
  }
  lastTelemetryTime = now;

  JsonDocument telDoc;
  telDoc["type"] = "telemetry";
#ifdef GYRO_PIN
  telDoc["yaw"] = ypr[0];
  telDoc["pitch"] = ypr[1];
  telDoc["roll"] = ypr[2];
#endif
#ifdef VOLTAGE
  extern float lastVoltage;
  telDoc["battery"] = lastVoltage;
#endif
  telDoc["skill"] = (skill != nullptr && skill->skillName[0] != '\0') ? skill->skillName : "none";
  telDoc["ts"] = now;
  // Include joint angles in telemetry for continuous display updates
  JsonArray angles = telDoc["angles"].to<JsonArray>();
  for (int i = 0; i < DOF; i++) {
    angles.add((int)currentAng[i]);
  }

  String telData;
  serializeJson(telDoc, telData);

  for (auto &client : connectedClients) {
    if (client.second) {
      webSocket.sendTXT(client.first, telData);
    }
  }
}


