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

// 网页服务器调试级别控制
#define WEB_DEBUG_LEVEL 1               // 0=关闭, 1=错误, 2=警告, 3=信息, 4=详细

// 调试打印宏 - 根据级别控制
#if WEB_DEBUG_LEVEL >= 1
  #define WEB_ERROR(msg, value) PTHL(msg, value)
  #define WEB_ERROR_F(msg) PTLF(msg)
#else
  #define WEB_ERROR(msg, value)
  #define WEB_ERROR_F(msg)
#endif

#if WEB_DEBUG_LEVEL >= 2
  #define WEB_WARN(msg, value) PTHL(msg, value)
  #define WEB_WARN_F(msg) PTLF(msg)
#else
  #define WEB_WARN(msg, value)
  #define WEB_WARN_F(msg)
#endif

#if WEB_DEBUG_LEVEL >= 3
  #define WEB_INFO(msg, value) PTHL(msg, value)
  #define WEB_INFO_F(msg) PTLF(msg)
#else
  #define WEB_INFO(msg, value)
  #define WEB_INFO_F(msg)
#endif

#if WEB_DEBUG_LEVEL >= 4
  #define WEB_DEBUG(msg, value) PTHL(msg, value)
  #define WEB_DEBUG_F(msg) PTLF(msg)
#else
  #define WEB_DEBUG(msg, value)
  #define WEB_DEBUG_F(msg)
#endif

// 网页服务器超时配置 (毫秒) - 针对蓝牙共存优化
#define HEARTBEAT_TIMEOUT 40000         // 心跳超时：40秒（增加缓冲时间应对BLE干扰）
#define HEALTH_CHECK_INTERVAL 15000     // 健康检查间隔：15秒（减少检查频率）
#define WEB_TASK_EXECUTION_TIMEOUT 45000 // 任务执行超时：45秒（增加执行时间）
#define MAX_CLIENTS 2                   // 最大连接数限制

// WiFi配置
String ssid = "";
String password = "";
WebSocketsServer webSocket = WebSocketsServer(81); // WebSocket服务器在81端口
WebServer httpServer(80); // HTTP服务器在80端口
long connectWebTime;
bool webServerConnected = false;

// WebSocket客户端管理
std::map<uint8_t, bool> connectedClients;
std::map<uint8_t, unsigned long> lastHeartbeat; // 记录每个客户端的最后心跳时间

// 连接健康检查
unsigned long lastHealthCheckTime = 0;

// 异步任务管理
struct WebTask
{
  String taskId;
  String status; // "pending", "running", "completed", "error"
  unsigned long timestamp;
  unsigned long endTime;
  unsigned long startTime;
  bool resultReady;
  uint8_t clientId; // 添加客户端ID
  std::vector<String> commandGroup; // 命令组中的命令列表
  std::vector<String> results; // 命令组中的执行结果
  size_t currentCommandIndex; // 当前执行的命令索引
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

// 简单的 Base64 解码函数
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

// 生成任务ID
String generateTaskId()
{
  return String(millis()) + "_" + String(esp_random() % 1000);
}

// 发送响应到指定客户端
void sendSocketResponse(uint8_t clientId, String message) {
  if (connectedClients.find(clientId) != connectedClients.end() && connectedClients[clientId]) {
    webSocket.sendTXT(clientId, message);
  }
}

// 检查连接健康状态
void checkConnectionHealth() {
  unsigned long currentTime = millis();
  
  // 检查是否有BLE活动，如果有则放宽心跳超时
  bool bleActive = false;
#ifdef BT_CLIENT
  extern boolean doScan;
  extern boolean btConnected;
  bleActive = doScan || btConnected;
#endif
  
  unsigned long effectiveTimeout = bleActive ? (HEARTBEAT_TIMEOUT + 15000) : HEARTBEAT_TIMEOUT;
  
  // 检查心跳超时
  for (auto it = lastHeartbeat.begin(); it != lastHeartbeat.end();) {
    uint8_t clientId = it->first;
    unsigned long lastHeartbeatTime = it->second;
    
    if (currentTime - lastHeartbeatTime > effectiveTimeout) {
      if (bleActive) {
        WEB_WARN("Client heartbeat timeout during BLE activity: ", clientId);
      } else {
        WEB_ERROR("Client heartbeat timeout, disconnecting: ", clientId);
      }
      
      // 发送超时通知（包含BLE状态信息）
      String timeoutMsg = bleActive ? 
        "{\"type\":\"error\",\"error\":\"Heartbeat timeout during BLE scan\"}" :
        "{\"type\":\"error\",\"error\":\"Heartbeat timeout\"}";
      sendSocketResponse(clientId, timeoutMsg);
      
      // 断开连接
      webSocket.disconnect(clientId);
      
      // 清理客户端状态
      connectedClients.erase(clientId);
      it = lastHeartbeat.erase(it);
      
      // 如果当前任务属于这个客户端，需要处理
      if (webTaskActive && currentWebTaskId != "" && 
          webTasks.find(currentWebTaskId) != webTasks.end() && 
          webTasks[currentWebTaskId].clientId == clientId) {
        errorWebTask("Client disconnected due to heartbeat timeout");
      }
    } else {
      ++it;
    }
  }
}

#ifdef CAMERA
// 发送摄像头数据到所有连接的客户端
void sendCameraData(int xCoord, int yCoord, int width, int height) {
  if (!webServerConnected || connectedClients.empty()) {
    return;
  }

  JsonDocument cameraDoc;
  cameraDoc["type"] = "event_cam";
  cameraDoc["x"] = xCoord - imgRangeX / 2.0;  // 与showRecognitionResult保持一致
  cameraDoc["y"] = yCoord - imgRangeY / 2.0;  // 与showRecognitionResult保持一致
  cameraDoc["width"] = width;
  cameraDoc["height"] = height;
  cameraDoc["timestamp"] = millis();

  String cameraData;
  serializeJson(cameraDoc, cameraData);

  // 向所有连接的客户端发送数据
  for (auto &client : connectedClients) {
    if (client.second) { // 如果客户端仍然连接
      webSocket.sendTXT(client.first, cameraData);
    }
  }
}
#endif

// 发送超声波数据到所有连接的客户端
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

  // 向所有连接的客户端发送数据
  for (auto &client : connectedClients) {
    if (client.second) { // 如果客户端仍然连接
      webSocket.sendTXT(client.first, ultrasonicData);
    }
  }
}

// WebSocket事件处理
void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      WEB_ERROR("WebSocket client disconnected: ", num);
      
      // 清理客户端状态
      connectedClients.erase(num);
      lastHeartbeat.erase(num);
      
      // 如果当前任务属于这个客户端，需要处理
      if (webTaskActive && currentWebTaskId != "" && 
          webTasks.find(currentWebTaskId) != webTasks.end() && 
          webTasks[currentWebTaskId].clientId == num) {
        errorWebTask("Client disconnected");
      }
      break;
      
    case WStype_CONNECTED:
      // 检查连接数限制
      if (connectedClients.size() >= MAX_CLIENTS) {
        WEB_ERROR("Max clients reached, rejecting: ", num);
        sendSocketResponse(num, "{\"type\":\"error\",\"error\":\"Max clients reached\"}");
        webSocket.disconnect(num);
        return;
      }
      
      connectedClients[num] = true;
      lastHeartbeat[num] = millis();
              WEB_DEBUG("WebSocket client connected: ", num);
      
      // 发送连接成功响应
      sendSocketResponse(num, "{\"type\":\"connected\",\"clientId\":\"" + String(num) + "\"}");
      break;
      
    case WStype_TEXT: {
      String message = String((char*)payload);
      
      // 解析 JSON 消息
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, message);
      
      if (error) {
        // JSON 解析错误，发送错误响应
        sendSocketResponse(num, "{\"type\":\"error\",\"error\":\"Invalid JSON format\"}");
        return;
      }

      String msgType = doc["type"].as<String>();
              WEB_DEBUG("msg type: ", msgType);
      
      // 处理心跳消息
      if (doc["type"] == "heartbeat") {
        lastHeartbeat[num] = millis();
        sendSocketResponse(num, "{\"type\":\"heartbeat\",\"timestamp\":" + String(millis()) + "}");
        return;
      }

      // 处理系统信息请求
      if (doc["type"] == "get_system_info") {
        lastHeartbeat[num] = millis();
        sendSystemInfo();
        return;
      }

      // 处理命令消息（统一使用命令组格式）
      if (doc["type"] == "command") {
        String taskId = doc["taskId"].as<String>();
        JsonArray commands;
        
        // 如果是单个命令，转换为命令组格式
        commands = doc["commands"].as<JsonArray>();
        
        // 更新心跳时间
        lastHeartbeat[num] = millis();
        
        // 创建任务记录
        WebTask task;
        task.taskId = taskId;
        task.status = "pending";
        task.timestamp = millis();
        task.startTime = 0;
        task.resultReady = false;
        task.clientId = num;
        task.currentCommandIndex = 0;
        
        // 存储命令组
        for (JsonVariant cmd : commands) {
          task.commandGroup.push_back(cmd.as<String>());
        }
        
        // 调试信息
                  WEB_DEBUG("Received command task: ", taskId);
          WEB_DEBUG("Command count: ", task.commandGroup.size());
                  #if WEB_DEBUG_LEVEL >= 4
          for (size_t i = 0; i < task.commandGroup.size(); i++) {
            WEB_DEBUG("Command " + String(i) + ": ", task.commandGroup[i]);
          }
          #endif
        
        // 如果当前没有活跃的web任务，立即开始执行
        if (!webTaskActive) {
          // 存储任务
          webTasks[taskId] = task;
          startWebTask(taskId);
        } else {
          // 如果当前有活跃的web任务，丢弃并返回错误
          errorWebTask("Previous web task is still running");
          return;
        }
        
        // 发送任务开始响应
        sendSocketResponse(num, "{\"type\":\"response\",\"taskId\":\"" + taskId + "\",\"status\":\"running\"}");
        
        WEB_DEBUG("web command group async: ", taskId);
        WEB_DEBUG("command count: ", task.commandGroup.size());
      }
      break;
    }
  }
}

// 开始执行web任务
void startWebTask(String taskId)
{
  if (webTasks.find(taskId) == webTasks.end()) {
    return;
  }

  WebTask &task = webTasks[taskId];

  // 设置全局标志和命令
  cmdFromWeb = true;
  currentWebTaskId = taskId;
  webTaskActive = true;
  webResponse = "";  // Clear response buffer

      // 执行命令组中的下一个命令
    if (task.currentCommandIndex < task.commandGroup.size()) {
      String webCmd = task.commandGroup[task.currentCommandIndex];
      
              WEB_DEBUG("Processing command: ", webCmd);
      
      // 检查是否是base64编码的命令
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
          // base64 解码失败，跳过这个命令
          task.currentCommandIndex++;
          startWebTask(taskId);
          return;
        }
      } else {
        // 解析命令
        token = webCmd[0];
        strcpy(newCmd, webCmd.c_str() + 1);
        cmdLen = strlen(newCmd);
        newCmd[cmdLen + 1] = '\0';
        
                  WEB_DEBUG("Parsed token: ", token);
          WEB_DEBUG("Parsed command: ", newCmd);
          WEB_DEBUG("Command length: ", cmdLen);
      }
      newCmdIdx = 4;

    // 更新任务状态
    task.status = "running";
    task.startTime = millis();

    // 通知客户端任务开始
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
    // 所有命令执行完成
    completeWebTask();
  }
}

// 完成web任务
void completeWebTask()
{
  if (!webTaskActive || currentWebTaskId == "") {
    return;
  }

  if (webTasks.find(currentWebTaskId) != webTasks.end()) {
    WebTask &task = webTasks[currentWebTaskId];
    task.results.push_back(webResponse);

    // 检查是否还有下一个命令
    if (task.currentCommandIndex + 1 < task.commandGroup.size()) {
      // 还有下一个命令，继续执行
      task.currentCommandIndex++;
      startWebTask(currentWebTaskId);
      return;
    }
    
    // 所有命令执行完成
    task.status = "completed";
    task.endTime = millis();
    task.resultReady = true;

    WEB_DEBUG("web task completed: ", currentWebTaskId);
    WEB_DEBUG("results length: ", task.results.size());

    // 发送完成状态给客户端
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

// Web任务错误处理
void errorWebTask(String errorMessage)
{
  if (!webTaskActive || currentWebTaskId == "") {
    return;
  }

  if (webTasks.find(currentWebTaskId) != webTasks.end()) {
    WebTask &task = webTasks[currentWebTaskId];
    task.status = "error";
    task.resultReady = true;

    // 发送错误状态给客户端
    JsonDocument errorDoc;
    errorDoc["type"] = "response";
    errorDoc["taskId"] = currentWebTaskId;
    errorDoc["status"] = "error";
    errorDoc["error"] = errorMessage;
    String statusMsg;
    serializeJson(errorDoc, statusMsg);
    sendSocketResponse(task.clientId, statusMsg);
    clearWebTask(currentWebTaskId);
  }

  // Reset state
  cmdFromWeb = false;
  webTaskActive = false;
  currentWebTaskId = "";

  // Process next task
  processNextWebTask();
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

// 处理下一个等待的任务
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

// WiFi配置函数 - 增强版本，支持重试机制
bool connectWifi(String ssid, String password, int maxRetries = 3)
{
  for (int retry = 0; retry < maxRetries; retry++) {
    if (retry > 0) {
      WEB_WARN("WiFi connection retry: ", retry);
      delay(2000); // 重试前等待2秒
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
      WiFi.disconnect(true); // 完全断开连接，为下次尝试做准备
    }
  }
  
  Serial.println("All WiFi connection attempts failed");
  return false;
}

#ifndef WIFI_MANAGER
// 当未启用WIFI_MANAGER时，尝试读取并使用之前保存的WiFi信息连接
bool connectWifiFromStoredConfig()
{
  // 检查可用内存
  size_t freeHeap = ESP.getFreeHeap();
  WEB_INFO("Free heap before WiFi init: ", freeHeap);
  
  if (freeHeap < 50000) { // 如果可用内存少于50KB
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
    WEB_WARN_F("No stored SSID found");
    return false;
  }

  webServerConnected = connectWifi(savedSsid, savedPassword);

  if (webServerConnected) {
    printToAllPorts("Successfully connected Wifi to IP Address: " + WiFi.localIP().toString());
    // 启动WebSocket服务器
    webSocket.begin();
    webSocket.onEvent(handleWebSocketEvent);
    WEB_INFO_F("WebSocket server started");
    
    // 启动HTTP服务器
    setupHttpServer();
    
    // 显示连接后的内存状态
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
    // 启动WebSocket服务器
    webSocket.begin();
    webSocket.onEvent(handleWebSocketEvent);
    WEB_INFO_F("WebSocket server started");
    
    // 启动HTTP服务器
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

// 主循环调用函数
void WebServerLoop()
{
  if (webServerConnected) {
    webSocket.loop();
    
    // 监控BLE活动对WebSocket的影响
    static unsigned long lastBleStatusLog = 0;
    unsigned long currentTime = millis();
    
    if (currentTime - lastBleStatusLog > 30000) { // 每30秒记录一次状态
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

    // 定期检查连接健康状态
    if (currentTime - lastHealthCheckTime > HEALTH_CHECK_INTERVAL) {
      checkConnectionHealth();
      lastHealthCheckTime = currentTime;
    }

    // 检查任务超时
    for (auto &pair : webTasks) {
      WebTask &task = pair.second;
      if (task.status == "running" && task.startTime > 0) {
        if (currentTime - task.startTime > WEB_TASK_EXECUTION_TIMEOUT) { // 使用配置的任务执行超时
          WEB_ERROR("web task timeout: ", task.taskId);
          task.status = "error";
          task.resultReady = true;

          // 发送超时状态给客户端
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
  
  // Handle HTTP server requests
  httpServer.handleClient();
}

// HTTP Server Implementation
void setupHttpServer() {
  // Check available memory before starting HTTP server
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 40000) { // Require at least 40KB free for HTTP server
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
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>OpenCat Robot - Home</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }
        .container { max-width: 600px; margin: 0 auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; text-align: center; }
        .status { padding: 10px; margin: 10px 0; border-radius: 5px; }
        .online { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
        .links { text-align: center; margin: 20px 0; }
        .links a { display: inline-block; margin: 10px; padding: 10px 20px; background: #007bff; color: white; text-decoration: none; border-radius: 5px; }
        .links a:hover { background: #0056b3; }
        .info { background: #e2e3e5; padding: 15px; border-radius: 5px; margin: 10px 0; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🤖 OpenCat Robot Control</h1>
        <div class="status online">✅ Robot Online - WiFi Connected</div>
        
        <div class="links">
            <a href="/console">🖥️ Console Interface</a>
        </div>
        
        <div class="info">
            <h3>Available Interfaces:</h3>
            <p><strong>Console (Port 80):</strong> Simple web-based command interface</p>
            <p><strong>WebSocket (Port 81):</strong> Real-time bidirectional communication</p>
        </div>
        
        <div class="info">
            <h3>Robot Information:</h3>
            <p><strong>Model:</strong> )rawliteral" + String(MODEL) + R"rawliteral(</p>
            <p><strong>Software:</strong> )rawliteral" + SoftwareVersion + R"rawliteral(</p>
            <p><strong>IP Address:</strong> )rawliteral" + WiFi.localIP().toString() + R"rawliteral(</p>
            <p><strong>Free Heap:</strong> <span id="heap">)rawliteral" + String(ESP.getFreeHeap()) + R"rawliteral(</span> bytes</p>
        </div>
    </div>
</body>
</html>
)rawliteral";
  
  httpServer.send(200, "text/html", html);
}

void handleConsole() {
  // Check available memory before serving large HTML page
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 30000) { // Require at least 30KB free
    httpServer.send(503, "text/plain", "Service unavailable - insufficient memory. Free heap: " + String(freeHeap) + " bytes");
    return;
  }
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>OpenCat Robot - Console</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: 'Courier New', monospace; margin: 0; padding: 20px; background: #1e1e1e; color: #d4d4d4; }
        .container { max-width: 900px; margin: 0 auto; }
        h1 { color: #569cd6; text-align: center; margin-bottom: 20px; }
        .console-container { background: #2d2d30; border: 1px solid #3e3e42; border-radius: 8px; overflow: hidden; }
        .console-header { background: #007acc; color: white; padding: 10px 15px; font-size: 14px; font-weight: bold; }
        .console-output { height: 400px; overflow-y: auto; padding: 15px; background: #1e1e1e; font-size: 14px; line-height: 1.4; }
        .console-input { display: flex; padding: 10px; background: #2d2d30; border-top: 1px solid #3e3e42; }
        .console-input input { flex: 1; background: #1e1e1e; color: #d4d4d4; border: 1px solid #3e3e42; padding: 8px 12px; font-family: 'Courier New', monospace; font-size: 14px; border-radius: 4px; }
        .console-input button { margin-left: 10px; padding: 8px 16px; background: #0e639c; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; }
        .console-input button:hover { background: #1177bb; }
        .prompt { color: #4ec9b0; }
        .command { color: #d7ba7d; }
        .response { color: #ce9178; margin-left: 20px; }
        .error { color: #f44747; margin-left: 20px; }
        .info { color: #608b4e; margin-left: 20px; font-style: italic; }
        .timestamp { color: #808080; font-size: 12px; }
        .status { padding: 10px; margin: 10px 0; border-radius: 5px; text-align: center; }
        .online { background: #0f5132; color: #75b798; border: 1px solid #0a3622; }
        .controls { margin: 15px 0; text-align: center; }
        .controls button { margin: 0 5px; padding: 6px 12px; background: #0e639c; color: white; border: none; border-radius: 4px; cursor: pointer; }
        .controls button:hover { background: #1177bb; }
        .quick-commands { margin: 15px 0; }
        .quick-commands h3 { color: #569cd6; margin-bottom: 10px; }
        .quick-commands button { margin: 2px; padding: 4px 8px; background: #3c3c3c; color: #d4d4d4; border: 1px solid #5a5a5a; border-radius: 3px; cursor: pointer; font-size: 12px; }
        .quick-commands button:hover { background: #464647; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🤖 OpenCat Robot Console</h1>
        
        <div class="status online" id="status">
            ✅ Connected to Robot at )rawliteral" + WiFi.localIP().toString() + R"rawliteral(
        </div>
        
        <div class="console-container">
            <div class="console-header">
                Console Output - OpenCat Robot Terminal
            </div>
            <div class="console-output" id="output">
                <div class="info">OpenCat Robot Console Ready</div>
                <div class="info">Enter commands below or use quick commands. Type 'h' for help.</div>
                <div class="prompt">robot@opencat:~$</div>
            </div>
            <div class="console-input">
                <input type="text" id="commandInput" placeholder="Enter command (e.g., 'ksit', 'kup', 'h' for help)" maxlength="50">
                <button onclick="sendCommand()">Send</button>
            </div>
        </div>
        
        <div class="controls">
            <button onclick="clearConsole()">Clear</button>
            <button onclick="connectWebSocket()">Connect WebSocket</button>
            <button onclick="disconnectWebSocket()">Disconnect</button>
        </div>
        
        <div class="quick-commands">
            <h3>Quick Commands:</h3>
            <button onclick="quickCommand('ksit')">Sit</button>
            <button onclick="quickCommand('kup')">Stand Up</button>
            <button onclick="quickCommand('kwkF')">Walk Forward</button>
            <button onclick="quickCommand('kwkL')">Walk Left</button>
            <button onclick="quickCommand('kwkR')">Walk Right</button>
            <button onclick="quickCommand('kwkB')">Walk Back</button>
            <button onclick="quickCommand('d')">Rest</button>
            <button onclick="quickCommand('g')">Toggle Gyro</button>
            <button onclick="quickCommand('b')">Beep</button>
            <button onclick="quickCommand('j')">Joint Angles</button>
            <button onclick="quickCommand('P')">Battery</button>
            <button onclick="getSystemInfo()">System Info</button>
            <button onclick="quickCommand('h')">Help</button>
        </div>
    </div>

    <script>
        let ws = null;
        let wsConnected = false;
        let commandHistory = [];
        let historyIndex = -1;
        
        function addToConsole(text, className = '') {
            const output = document.getElementById('output');
            const timestamp = new Date().toLocaleTimeString();
            const div = document.createElement('div');
            div.innerHTML = `<span class="timestamp">[${timestamp}]</span> <span class="${className}">${text}</span>`;
            output.appendChild(div);
            output.scrollTop = output.scrollHeight;
        }
        
        function clearConsole() {
            const output = document.getElementById('output');
            output.innerHTML = '<div class="info">Console cleared</div><div class="prompt">robot@opencat:~$</div>';
        }
        
        function connectWebSocket() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                addToConsole('WebSocket already connected', 'info');
                return;
            }
            
            const wsUrl = `ws://${window.location.hostname}:81`;
            addToConsole(`Connecting to WebSocket at ${wsUrl}...`, 'info');
            
            ws = new WebSocket(wsUrl);
            
            ws.onopen = function() {
                wsConnected = true;
                addToConsole('✅ WebSocket connected successfully', 'info');
                document.getElementById('status').innerHTML = '✅ Connected to Robot (HTTP + WebSocket)';
                document.getElementById('status').className = 'status online';
            };
            
            ws.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    if (data.type === 'response') {
                        // Detailed response handling
                        if (data.status === 'completed' && data.results) {
                            addToConsole(`✅ Command completed (Task: ${data.taskId})`, 'info');
                            data.results.forEach((result, index) => {
                                if (result && result.trim()) {
                                    addToConsole(`Result ${index + 1}: ${result}`, 'response');
                                }
                            });
                        } else if (data.status === 'running') {
                            addToConsole(`🔄 Command executing (Task: ${data.taskId})`, 'info');
                        } else if (data.status === 'error') {
                            addToConsole(`❌ Error: ${data.error || 'Unknown error'}`, 'error');
                        } else {
                            addToConsole(`Response: ${JSON.stringify(data, null, 2)}`, 'response');
                        }
                    } else if (data.type === 'event_cam') {
                        addToConsole(`📷 Camera: Object detected at (${data.x}, ${data.y}) size: ${data.width}×${data.height}`, 'info');
                    } else if (data.type === 'event_us') {
                        addToConsole(`📏 Ultrasonic: Distance ${data.distance}cm`, 'info');
                    } else if (data.type === 'connected') {
                        addToConsole(`🔗 WebSocket connected (Client ID: ${data.clientId})`, 'info');
                    } else if (data.type === 'robot_output') {
                        addToConsole(`🤖 Robot: ${data.message}`, 'response');
                    } else if (data.type === 'system_info') {
                        addToConsole(`ℹ️ System Info:`, 'info');
                        addToConsole(`   Model: ${data.model}`, 'info');
                        addToConsole(`   Software: ${data.software_version}`, 'info');
                        addToConsole(`   Free Heap: ${data.free_heap} bytes`, 'info');
                        addToConsole(`   Uptime: ${Math.floor(data.uptime/1000)}s`, 'info');
                        if (data.battery_voltage) {
                            addToConsole(`   Battery: ${data.battery_voltage}V`, 'info');
                        }
                        addToConsole(`   WiFi RSSI: ${data.wifi_rssi}dBm`, 'info');
                    } else if (data.type === 'heartbeat') {
                        // Don't log heartbeats to avoid spam, just update status silently
                        return;
                    } else {
                        addToConsole(`📡 WebSocket: ${event.data}`, 'response');
                    }
                } catch (e) {
                    // Handle non-JSON messages (raw text responses)
                    const message = event.data.toString().trim();
                    if (message.length > 0) {
                        addToConsole(`📡 Robot Output: ${message}`, 'response');
                    }
                }
            };
            
            ws.onclose = function() {
                wsConnected = false;
                addToConsole('❌ WebSocket disconnected', 'error');
                document.getElementById('status').innerHTML = '⚠️ Connected to Robot (HTTP only)';
                document.getElementById('status').style.background = '#664d03';
            };
            
            ws.onerror = function(error) {
                addToConsole(`WebSocket error: ${error}`, 'error');
            };
        }
        
        function disconnectWebSocket() {
            if (ws) {
                ws.close();
                wsConnected = false;
                addToConsole('WebSocket disconnected', 'info');
            }
        }
        
        function sendCommand() {
            const input = document.getElementById('commandInput');
            const command = input.value.trim();
            if (!command) return;
            
            commandHistory.push(command);
            historyIndex = commandHistory.length;
            
            addToConsole(`> ${command}`, 'command');
            
            if (wsConnected && ws.readyState === WebSocket.OPEN) {
                // Send via WebSocket for real-time response
                const message = {
                    type: 'command',
                    commands: [command],
                    taskId: Date.now().toString()
                };
                ws.send(JSON.stringify(message));
                addToConsole('Command sent via WebSocket', 'info');
            } else {
                // Fallback to HTTP POST
                fetch('/command', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: `command=${encodeURIComponent(command)}`
                })
                .then(response => response.text())
                .then(data => {
                    addToConsole(data, 'response');
                })
                .catch(error => {
                    addToConsole(`Error: ${error}`, 'error');
                });
            }
            
            input.value = '';
        }
        
        function quickCommand(cmd) {
            document.getElementById('commandInput').value = cmd;
            sendCommand();
        }
        
        function getSystemInfo() {
            if (wsConnected && ws.readyState === WebSocket.OPEN) {
                addToConsole('> Requesting system information...', 'command');
                // Send a special message to request system info
                const message = {
                    type: 'get_system_info',
                    timestamp: Date.now()
                };
                ws.send(JSON.stringify(message));
            } else {
                addToConsole('WebSocket not connected - system info requires WebSocket', 'error');
            }
        }
        
        // Handle Enter key and command history
        document.getElementById('commandInput').addEventListener('keydown', function(event) {
            if (event.key === 'Enter') {
                sendCommand();
            } else if (event.key === 'ArrowUp') {
                event.preventDefault();
                if (historyIndex > 0) {
                    historyIndex--;
                    this.value = commandHistory[historyIndex];
                }
            } else if (event.key === 'ArrowDown') {
                event.preventDefault();
                if (historyIndex < commandHistory.length - 1) {
                    historyIndex++;
                    this.value = commandHistory[historyIndex];
                } else {
                    historyIndex = commandHistory.length;
                    this.value = '';
                }
            }
        });
        
        // Auto-connect WebSocket on page load
        setTimeout(connectWebSocket, 1000);
    </script>
</body>
</html>
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
      response += "• Execute skill: '" + skill + "'\n";
      response += "• Robot will move to new posture\n";
      response += "• Check WebSocket for real-time feedback\n";
    } else if (command == "d") {
      response += "• Set robot to rest position\n";
      response += "• All servos will be turned off\n";
    } else if (command == "g") {
      response += "• Toggle gyro/IMU functionality\n";
      response += "• Balance control will be affected\n";
    } else if (command == "j") {
      response += "• Display all joint angles\n";
      response += "• Check serial output for detailed readings\n";
    } else if (command == "P") {
      response += "• Display battery voltage\n";
      response += "• Check serial output for voltage reading\n";
    } else if (command.startsWith("b")) {
      response += "• Play sound/beep sequence\n";
      response += "• Listen for audio feedback from robot\n";
    } else if (command.startsWith("i")) {
      response += "• Set joint positions individually\n";
      response += "• Servos will move to specified angles\n";
    } else if (command.startsWith("X")) {
      response += "• Execute extension module command\n";
      response += "• Module-specific functionality activated\n";
    } else {
      response += "• Execute custom command\n";
      response += "• Refer to OpenCat documentation for details\n";
    }
    
    response += "\n";
    
    // Add comprehensive help if requested
    if (command == "h") {
      // Check memory before building large help string
      if (ESP.getFreeHeap() < 20000) {
        response += "Help available - use WebSocket console for full help (insufficient memory for HTTP help)";
      } else {
        response += "=== OPENCAT ROBOT COMMANDS ===\n\n";
        response += "MOVEMENT SKILLS:\n";
        response += "  ksit, kup, krest, kwkF, kwkB, kwkL, kwkR, ktr, kcr, kpd\n\n";
        response += "SYSTEM COMMANDS:\n";
        response += "  d (rest), g (gyro), j (joints), P (battery), i (head), c (calibrate)\n\n";
        response += "SOUND: b (beep), u (meow)\n";
        response += "EXTENSIONS: XCP (camera), XCR (reactions)\n";
        response += "JOINTS: i0 45 (move joint 0 to 45°)\n\n";
        response += "Use WebSocket for real-time feedback and detailed help.\n";
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
