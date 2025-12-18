#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "AsyncJson.h"
#include <SPIFFS.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <ADS1X15.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Ethernet.h> //ethernet w5500
#include <esp_wifi.h>
#include "driver/spi_master.h"
#include <time.h>
#include <config.hpp>
#include <ModbusIP_ESP8266.h>
#include <ModbusRTU.h>
#include <DNSServer.h>
#include "NetworkFunctions.hpp"

// #define DEBUG
// ============================================================================
// TASK HANDLES
// ============================================================================
TaskHandle_t Task_Core0_Network = NULL;
TaskHandle_t Task_Core1_DataAcquisition = NULL;
TaskHandle_t Task_Core1_ModbusClient = NULL;
TaskHandle_t Task_Core1_DataLogger = NULL;

// ============================================================================
// QUEUE HANDLES untuk komunikasi antar task
// ============================================================================
QueueHandle_t queueSensorData = NULL;
QueueHandle_t queueModbusData = NULL;
QueueHandle_t queueLogData = NULL;

// ============================================================================
// MUTEX untuk resource sharing
// ============================================================================
SemaphoreHandle_t spiMutex;
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t sdMutex;
SemaphoreHandle_t jsonMutex;

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
ModbusIP mbIP;
ModbusRTU mbRTU;
RTC_DS3231 rtc;
AsyncWebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
bool dnsStarted = false;
WiFiClient esp32;
PubSubClient mqtt;
// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

unsigned long timeElapsed;
unsigned long lastSDSaveTime = 0;
ADS1115 ads;
ErrorBlinker errorBlinker(SIG_LED_PIN, 800);
ErrorMessages errorMessages("/debugStream");

String stringParam, sendString;
int numOfParam, modbusCount;
unsigned long printTime, checkTime, sendTime, sendTimeModbus;
HardwareSerial SerialModbus(2);

DynamicJsonDocument doc(4096), jsonParam(4096), jsonSend(4096);
bool flagGetJobNum = 1;
String jobNum;

// ============================================================================
// STRUKTUR DATA untuk Queue
// ============================================================================
struct SensorDataPacket
{
  float analogValues[jumlahInputAnalog + 1];
  float digitalValues[jumlahInputDigital + 1];
  unsigned long timestamp;
};

struct ModbusDataPacket
{
  String paramName;
  float value;
  unsigned long timestamp;
};

struct LogDataPacket
{
  String jsonData;
  unsigned long timestamp;
};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void readConfig();
void saveToJson(const char *dir, const char *configType);
void saveToSDConfig(const char *dir, const char *configType);
void updateJson(const char *dir, const char *jsonKey, int jsonValue);
void updateJson(const char *dir, const char *jsonKey, const char *jsonValue);
void handleFileRequest(AsyncWebServerRequest *request, const char *filePath, const char *mimeType);
String getTimeDateNow();
String getTimeNow();
void modbusSlaveSetup();
void setupWebServer();
void setupInterrupts();
bool isAuthenticated(AsyncWebServerRequest *request);
void authenthicateUser(AsyncWebServerRequest *request);
void handleFormSubmit(AsyncWebServerRequest *request);

int countJsonKeys(const JsonDocument &doc);
float filterSensor(float filterVar, float filterResult_1, float fc);
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
unsigned int readModbus(unsigned int modbusAddress, unsigned int funCode, unsigned int regAddress);
unsigned int crcModbus(unsigned int crc[], byte start, byte sizeArray);
unsigned int parseByte(unsigned int bytes, bool byteOrder);

// ============================================================================
// ISR DECLARATIONS
// ============================================================================
void IRAM_ATTR isrDI1()
{
  digitalInput[1].millisNow = millis();
  digitalInput[1].flagInt = 1;
}
void IRAM_ATTR isrDI2()
{
  digitalInput[2].millisNow = millis();
  digitalInput[2].flagInt = 1;
}
void IRAM_ATTR isrDI3()
{
  digitalInput[3].millisNow = millis();
  digitalInput[3].flagInt = 1;
}
void IRAM_ATTR isrDI4()
{
  digitalInput[4].millisNow = millis();
  digitalInput[4].flagInt = 1;
}

void (*isrArray[])(void) = {nullptr, isrDI1, isrDI2, isrDI3, isrDI4};

void attachDigitalInputInterrupt(int index)
{
  if (digitalInput[index].taskMode == "Cycle Time" || digitalInput[index].taskMode == "Counting")
  {
    if (index < sizeof(isrArray) / sizeof(isrArray[0]))
    {
      attachInterrupt(digitalPinToInterrupt(digitalInput[index].pin), isrArray[index], RISING);
    }
  }
  else if (digitalInput[index].taskMode == "Pulse Mode")
  {
    if (index < sizeof(isrPulseMode) / sizeof(isrPulseMode[0]))
    {
      attachInterrupt(digitalPinToInterrupt(digitalInput[index].pin), isrPulseMode[index], RISING);
    }
  }
  else
  {
    detachInterrupt(digitalPinToInterrupt(digitalInput[index].pin));
  }
}

void configureSendTriggerInterrupt(Network networkSettings)
{
  if (networkSettings.sendTrig != "Timer/interval")
  {
    int pinTrig = networkSettings.sendTrig.substring(2, 3).toInt();
    if (pinTrig >= 1 && pinTrig < sizeof(isrArray) / sizeof(isrArray[0]))
    {
      attachInterrupt(digitalPinToInterrupt(digitalInput[pinTrig].pin), isrArray[pinTrig], RISING);
    }
  }
}

// ============================================================================
// ETHERNET WEB SERVER HANDLER
// Menangani request website saat menggunakan kabel LAN (W5500)
// ============================================================================
String getContentType(String filename)
{
  if (filename.endsWith(".html"))
    return "text/html";
  else if (filename.endsWith(".css"))
    return "text/css";
  else if (filename.endsWith(".js"))
    return "application/javascript";
  else if (filename.endsWith(".png"))
    return "image/png";
  else if (filename.endsWith(".jpg"))
    return "image/jpeg";
  else if (filename.endsWith(".ico"))
    return "image/x-icon";
  return "text/plain";
}

// void handleEthernetClient()
// {
//   EthernetClient client = ethServer.available();
//   if (client)
//   {
//     Serial.println("New Ethernet Client");
//     boolean currentLineIsBlank = true;
//     String req = "";

//     while (client.connected())
//     {
//       if (client.available())
//       {
//         char c = client.read();
//         req += c;
//         if (c == '\n' && currentLineIsBlank)
//         {
//           // Request Header selesai, parsing URL
//           String url = req.substring(req.indexOf("GET") + 4, req.indexOf(" HTTP/1.1"));
//           url.trim();

//           // 1. ROUTING HALAMAN UTAMA
//           if (url == "/")
//             url = "/home.html";
//           else if (url == "/network")
//             url = "/network.html";
//           else if (url == "/analog_input")
//             url = "/analog_input.html";
//           else if (url == "/digital_IO")
//             url = "/digital_IO.html";
//           else if (url == "/system_settings")
//             url = "/system_settings.html";
//           else if (url == "/modbus_setup")
//             url = "/modbus_setup.html";
//           else if (url == "/updateOTA")
//             url = "/UpdateOTA.html";
//           else if (url == "/debug")
//             url = "/debug-monitor.html";
//           // 2. ROUTING API JSON (Replikasi logika dari AsyncWebServer)
//           if (url == "/getCurrentValue")
//           {
//             // Logika sama dengan server.on("/getCurrentValue"...)
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(100)))
//             {
//               DynamicJsonDocument docTemp(1024);
//               for (int i = 1; i <= jumlahInputDigital; i++)
//               { // Koreksi index loop dari 1
//                 // Pastikan analogInput diakses dengan benar sesuai config
//                 docTemp["rawValue" + String(i)] = analogInput[i].adcValue;
//                 docTemp["scaledValue" + String(i)] = analogInput[i].mapValue;
//               }
//               String dataLoad;
//               serializeJson(docTemp, dataLoad);
//               xSemaphoreGive(jsonMutex);

//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.println(dataLoad);
//             }
//             break;
//           }
//           else if (url == "/networkLoad")
//           {
//             // API Network Info
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(100)))
//             {
//               DynamicJsonDocument docTemp(1024);
//               // Isi data sesuai variabel global networkSettings
//               docTemp["networkMode"] = networkSettings.networkMode;
//               docTemp["ipAddress"] = (networkSettings.networkMode == "Ethernet") ? Ethernet.localIP().toString() : networkSettings.ipAddress;
//               docTemp["macAddress"] = networkSettings.macAddress;
//               docTemp["connStatus"] = networkSettings.connStatus;
//               docTemp["protocolMode"] = networkSettings.protocolMode;
//               docTemp["sendInterval"] = networkSettings.sendInterval;

//               String dataLoad;
//               serializeJson(docTemp, dataLoad);
//               xSemaphoreGive(jsonMutex);

//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.println(dataLoad);
//             }
//             break;
//           }

//           // 3. ROUTING FILE STATIC (SPIFFS)
//           if (SPIFFS.exists(url))
//           {
//             File file = SPIFFS.open(url, "r");
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: " + getContentType(url));
//             client.println("Connection: close");
//             client.println();

//             while (file.available())
//             {
//               client.write(file.read());
//             }
//             file.close();
//           }
//           else
//           {
//             // 404 Not Found
//             client.println("HTTP/1.1 404 Not Found");
//             client.println("Content-Type: text/plain");
//             client.println("Connection: close");
//             client.println();
//             client.println("404: File Not Found");
//           }
//           break;
//         }
//         if (c == '\n')
//         {
//           currentLineIsBlank = true;
//         }
//         else if (c != '\r')
//         {
//           currentLineIsBlank = false;
//         }
//       }
//     }
//     delay(1);
//     client.stop();
//   }
// }

// ============================================================================
// ETHERNET WEB SERVER HANDLER (FULL FEATURE)
// Meniru logika ESPAsyncWebServer agar tampilan sama persis
// ============================================================================
// void handleEthernetClient()
// {
//   EthernetClient client = ethServer.available();
//   if (client)
//   {
//     boolean currentLineIsBlank = true;
//     String req = "";
//     String reqBody = "";
//     bool headerFinished = false;
//     unsigned long timeout = millis();
//     int contentLength = 0;

//     while (client.connected() && millis() - timeout < 2000)
//     {
//       if (client.available())
//       {
//         char c = client.read();

//         // 1. READ HEADER
//         if (!headerFinished)
//         {
//           req += c;
//           if (req.length() > 2048)
//             req = req.substring(req.length() - 2048); // Prevent overflow

//           if (c == '\n' && currentLineIsBlank)
//           {
//             headerFinished = true;

//             // Check Content-Length if needed (for POST future use)
//             String lowerReq = req;
//             lowerReq.toLowerCase();
//             int clIndex = lowerReq.indexOf("content-length:");
//             if (clIndex >= 0)
//             {
//               contentLength = lowerReq.substring(clIndex + 15, lowerReq.indexOf("\n", clIndex)).toInt();
//             }
//           }
//           if (c == '\n')
//             currentLineIsBlank = true;
//           else if (c != '\r')
//             currentLineIsBlank = false;
//         }
//         // 2. READ BODY (If needed for POST)
//         else
//         {
//           if (reqBody.length() < contentLength)
//             reqBody += c;
//         }

//         // If Header is finished and (no body or body read)
//         if (headerFinished && (contentLength == 0 || reqBody.length() >= contentLength))
//         {
//           // --- PARSING REQUEST LINE ---
//           int methodEnd = req.indexOf(" ");
//           int urlEnd = req.indexOf(" ", methodEnd + 1);
//           if (methodEnd == -1 || urlEnd == -1)
//             return;

//           String url = req.substring(methodEnd + 1, urlEnd);
//           url.trim();

//           // Separate URL Path and Query Param (e.g., /analogLoad?input=1)
//           String basePath = url;
//           String queryParams = "";
//           if (url.indexOf("?") > 0)
//           {
//             basePath = url.substring(0, url.indexOf("?"));
//             queryParams = url.substring(url.indexOf("?") + 1);
//           }

//           // ==================================================================
//           // PART A: PAGE ROUTING (URL -> FILE)
//           // ==================================================================
//           String filePath = basePath;

//           if (basePath == "/")
//             filePath = "/home.html";
//           else if (basePath == "/network")
//             filePath = "/network.html";
//           else if (basePath == "/analog_input")
//             filePath = "/analog_input.html";
//           else if (basePath == "/digital_IO")
//             filePath = "/digital_IO.html";
//           else if (basePath == "/system_settings")
//             filePath = "/system_settings.html";
//           else if (basePath == "/modbus_setup")
//             filePath = "/modbus_setup.html";
//           else if (basePath == "/updateOTA")
//             filePath = "/UpdateOTA.html";
//           else if (basePath == "/debug")
//             filePath = "/debug-monitor.html";

//           // ==================================================================
//           // PART B: API JSON (DATA LOADERS)
//           // ==================================================================

//           // 1. /homeLoad
//           if (basePath == "/homeLoad")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(2048);
//               // Parsing Input Parameter if exists
//               unsigned char id = 1;
//               if (queryParams.indexOf("input=") >= 0)
//               {
//                 String valStr = queryParams.substring(queryParams.indexOf("input=") + 6);
//                 id = valStr.toInt();
//               }

//               doc["networkMode"] = networkSettings.networkMode;
//               doc["ssid"] = networkSettings.ssid;
//               doc["ipAddress"] = Ethernet.localIP().toString();
//               doc["macAddress"] = networkSettings.macAddress;
//               doc["protocolMode"] = networkSettings.protocolMode;
//               doc["endpoint"] = networkSettings.endpoint;
//               doc["connStatus"] = networkSettings.connStatus;
//               doc["jobNumber"] = jobNum;
//               doc["sendInterval"] = networkSettings.sendInterval;
//               doc["datetime"] = getTimeDateNow();

//               JsonArray enableAI = doc.createNestedArray("enAI");
//               for (int i = 1; i <= jumlahInputAnalog; i++)
//                 enableAI.add(analogInput[i].name != "" ? 1 : 0);

//               JsonObject AI = doc.createNestedObject("AI");
//               JsonArray AIRawValue = AI.createNestedArray("rawValue");
//               JsonArray AIScaledValue = AI.createNestedArray("scaledValue");
//               for (int i = 1; i <= jumlahInputAnalog; i++)
//               {
//                 AIRawValue.add(analogInput[i].adcValue);
//                 AIScaledValue.add(analogInput[i].mapValue);
//               }

//               JsonObject DI = doc.createNestedObject("DI");
//               JsonArray DIValue = DI.createNestedArray("value");
//               JsonArray DITaskMode = DI.createNestedArray("taskMode");
//               for (int i = 1; i <= jumlahInputDigital; i++)
//               {
//                 DIValue.add(digitalInput[i].value);
//                 DITaskMode.add(digitalInput[i].taskMode);
//               }

//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.println(dataLoad);
//             }
//           }

//           // 2. /networkLoad
//           else if (basePath == "/networkLoad")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(4096);
//               doc["networkMode"] = networkSettings.networkMode;
//               doc["ssid"] = networkSettings.ssid;
//               doc["password"] = networkSettings.password;
//               doc["apSsid"] = networkSettings.apSsid;
//               doc["apPassword"] = networkSettings.apPassword;
//               doc["dhcpMode"] = networkSettings.dhcpMode;
//               doc["ipAddress"] = networkSettings.ipAddress;
//               doc["subnet"] = networkSettings.subnetMask;
//               doc["ipGateway"] = networkSettings.ipGateway;
//               doc["ipDNS"] = networkSettings.ipDNS;
//               doc["sendInterval"] = String(networkSettings.sendInterval, 2);
//               doc["protocolMode"] = networkSettings.protocolMode;
//               doc["endpoint"] = networkSettings.endpoint;
//               doc["port"] = networkSettings.port;
//               doc["pubTopic"] = networkSettings.pubTopic;
//               doc["subTopic"] = networkSettings.subTopic;
//               doc["mqttUsername"] = networkSettings.mqttUsername;
//               doc["mqttPass"] = networkSettings.mqttPassword;
//               doc["loggerMode"] = networkSettings.loggerMode;
//               doc["protocolMode2"] = networkSettings.protocolMode2;
//               doc["modbusMode"] = modbusParam.mode;
//               doc["modbusPort"] = modbusParam.port;
//               doc["modbusSlaveID"] = modbusParam.slaveID;
//               doc["sendTrig"] = networkSettings.sendTrig;
//               doc["erpUrl"] = networkSettings.erpUrl;
//               doc["erpUsername"] = networkSettings.erpUsername;
//               doc["erpPassword"] = networkSettings.erpPassword;

//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.println(dataLoad);
//             }
//           }

//           // 3. /analogLoad (Requires parsing ?input=X)
//           else if (basePath == "/analogLoad")
//           {
//             int id = 1;
//             if (queryParams.indexOf("input=") >= 0)
//             {
//               int start = queryParams.indexOf("input=") + 6;
//               int end = queryParams.indexOf("&", start);
//               if (end == -1)
//                 end = queryParams.length();
//               id = queryParams.substring(start, end).toInt();
//             }
//             if (id < 1)
//               id = 1;
//             if (id > jumlahInputAnalog)
//               id = jumlahInputAnalog;

//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(4096);
//               doc["inputType"] = analogInput[id].inputType;
//               doc["filter"] = analogInput[id].filter;
//               doc["filterPeriod"] = (String(analogInput[id].filterPeriod, 2));
//               doc["scaling"] = analogInput[id].scaling;
//               doc["lowLimit"] = analogInput[id].lowLimit;
//               doc["highLimit"] = analogInput[id].highLimit;
//               doc["name"] = analogInput[id].name;
//               doc["calibration"] = analogInput[id].calibration;
//               doc["mValue"] = analogInput[id].mValue;
//               doc["cValue"] = analogInput[id].cValue;

//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.println(dataLoad);
//             }
//           }

//           // 4. /digitalLoad (Handles input, reset, output)
//           else if (basePath == "/digitalLoad")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(4096);

//               // Case 1: Load Input Data
//               if (queryParams.indexOf("input=") >= 0)
//               {
//                 int start = queryParams.indexOf("input=") + 6;
//                 int end = queryParams.indexOf("&", start);
//                 if (end == -1)
//                   end = queryParams.length();
//                 int id = queryParams.substring(start, end).toInt();
//                 if (id >= 1 && id <= jumlahInputDigital)
//                 {
//                   doc["nameDI"] = digitalInput[id].name;
//                   doc["invDI"] = digitalInput[id].inv;
//                   doc["taskMode"] = digitalInput[id].taskMode;
//                   doc["inputState"] = digitalInput[id].inputState ? "High" : "Low";
//                   doc["intervalTime"] = (float)digitalInput[id].intervalTime / 1000;
//                   doc["conversionFactor"] = digitalInput[id].conversionFactor;
//                 }
//               }
//               // Case 2: Reset Value
//               if (queryParams.indexOf("reset=") >= 0)
//               {
//                 int start = queryParams.indexOf("reset=") + 6;
//                 int end = queryParams.indexOf("&", start);
//                 if (end == -1)
//                   end = queryParams.length();
//                 int id = queryParams.substring(start, end).toInt();
//                 if (id >= 1 && id <= jumlahInputDigital)
//                   digitalInput[id].value = 0;
//               }
//               // Case 3: Load Output Data
//               if (queryParams.indexOf("output=") >= 0)
//               {
//                 int start = queryParams.indexOf("output=") + 7;
//                 int end = queryParams.indexOf("&", start);
//                 if (end == -1)
//                   end = queryParams.length();
//                 int id = queryParams.substring(start, end).toInt();
//                 if (id >= 1 && id <= jumlahOutputDigital)
//                 {
//                   doc["nameDO"] = digitalOutput[id].name;
//                   doc["invDO"] = digitalOutput[id].inv;
//                 }
//               }
//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.println(dataLoad);
//             }
//           }

//           // 5. /settingsLoad (System Settings)
//           else if (basePath == "/settingsLoad")
//           {
//             String jsonAuth = "{";
//             jsonAuth += "\"username\":\"" + networkSettings.loginUsername + "\",";
//             jsonAuth += "\"password\":\"" + networkSettings.loginPassword + "\",";
//             jsonAuth += "\"sdInterval\":" + String(networkSettings.sdSaveInterval);
//             jsonAuth += "}";
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.println(jsonAuth);
//           }

//           // 6. /getTime
//           else if (basePath == "/getTime")
//           {
//             DynamicJsonDocument jsonDoc(256);
//             jsonDoc["datetime"] = getTimeDateNow();
//             String jsonTime;
//             serializeJson(jsonDoc, jsonTime);
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.println(jsonTime);
//           }

//           // 7. /modbusLoad
//           else if (basePath == "/modbusLoad")
//           {
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.println(stringParam);
//           }

//           // 8. /getCurrentValue (Realtime)
//           // else if (basePath == "/getCurrentValue")
//           // {
//           //   if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(500)))
//           //   {
//           //     DynamicJsonDocument docTemp(2048);
//           //     for (int i = 0; i < jumlahInputDigital; i++)
//           //     {
//           //       docTemp["rawValue" + String(i + 1)] = analogInput[i + 1].adcValue;
//           //       docTemp["scaledValue" + String(i + 1)] = analogInput[i + 1].mapValue;
//           //     }
//           //     String dataLoad;
//           //     serializeJson(docTemp, dataLoad);
//           //     xSemaphoreGive(jsonMutex);
//           //     client.println("HTTP/1.1 200 OK");
//           //     client.println("Content-Type: application/json");
//           //     client.println("Access-Control-Allow-Origin: *");
//           //     client.println("Cache-Control: no-store, no-cache, must-revalidate");
//           //     client.println("Connection: close");
//           //     client.println();
//           //     client.println(dataLoad);
//           //   }
//           //   else
//           //   {
//           //     // Jika Mutex sibuk, kirim error 503 biar browser tau (bukan diam saja)
//           //     client.println("HTTP/1.1 503 Service Unavailable");
//           //     client.println("Connection: close");
//           //     client.println();
//           //   }
//           // }

// else if (basePath == "/getCurrentValue")
//           {
//              // Kita naikkan timeout jadi 200ms agar kalau sistem sibuk, dia tetap nunggu data
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
//                 DynamicJsonDocument docTemp(1024);

//                 // Gunakan loop yang sama dengan WiFi, tapi akses array kita geser +1
//                 // Karena data sensor Anda tersimpan di index 1, 2, 3, 4 (bukan 0, 1, 2, 3)
//                 for (int i = 0; i < jumlahInputDigital; i++) {
//                    docTemp["rawValue" + String(i + 1)] = analogInput[i+1].adcValue;
//                    docTemp["scaledValue" + String(i + 1)] = analogInput[i+1].mapValue;
//                 }

//                 String dataLoad;
//                 serializeJson(docTemp, dataLoad);
//                 xSemaphoreGive(jsonMutex);

//                 // Header HTTP Lengkap (Penting agar browser tidak menolak data)
//                 client.println("HTTP/1.1 200 OK");
//                 client.println("Content-Type: application/json");
//                 client.println("Access-Control-Allow-Origin: *"); // Biar tidak diblokir browser
//                 client.println("Connection: close");
//                 client.println();

//                 // Kirim Data JSON
//                 client.print(dataLoad);
//              }
//              else {
//                 // Jika Mutex Sibuk, kirim JSON kosong agar browser tidak error
//                 client.println("HTTP/1.1 200 OK");
//                 client.println("Content-Type: application/json");
//                 client.println("Connection: close");
//                 client.println();
//                 client.println("{}");
//              }
//           }
//           // 9. /wifiStatus (Faked using Ethernet data for frontend compatibility)
//           else if (basePath == "/wifiStatus" || basePath == "/ethernetStatus")
//           {
//             DynamicJsonDocument statusDoc(512);
//             statusDoc["mode"] = networkSettings.networkMode;
//             statusDoc["ssid"] = "Ethernet (LAN)";
//             statusDoc["connected"] = (Ethernet.linkStatus() == LinkON);
//             statusDoc["ip"] = Ethernet.localIP().toString();
//             statusDoc["rssi"] = -50;
//             statusDoc["macAddress"] = networkSettings.macAddress;
//             statusDoc["apIP"] = WiFi.softAPIP().toString();
//             statusDoc["connectionStatus"] = networkSettings.connStatus;
//             statusDoc["signalQuality"] = "Wired";

//             String response;
//             serializeJson(statusDoc, response);
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.println(response);
//           }

//           // ==================================================================
//           // PART C: STATIC FILES (CSS, JS, IMAGES)
//           // ==================================================================
//           else
//           {
//             if (SPIFFS.exists(filePath))
//             {
//               File file = SPIFFS.open(filePath, "r");
//               String contentType = getContentType(filePath);

//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: " + contentType);
//               client.println("Connection: close");
//               client.println();

//               uint8_t buffer[512];
//               while (file.available())
//               {
//                 int bytesRead = file.read(buffer, sizeof(buffer));
//                 client.write(buffer, bytesRead);
//               }
//               file.close();
//             }
//             else
//             {
//               client.println("HTTP/1.1 404 Not Found");
//               client.println("Connection: close");
//               client.println();
//               client.print("404: File Not Found (" + filePath + ")");
//             }
//           }

//           break;
//         }
//       }
//     }
//     delay(5);
//     client.stop();
//   }
// }

// // ETHERNET WEB SERVER HANDLER (FIXED ROUTING)
// // ============================================================================
// // ============================================================================
// // ETHERNET WEB SERVER HANDLER (FIXED FOR MODBUS VALUE)
// // ============================================================================
// void handleEthernetClient()
// {
//   EthernetClient client = ethServer.available();
//   if (client)
//   {
//     boolean currentLineIsBlank = true;
//     String reqLine = "";
//     boolean readingFirstLine = true;
//     unsigned long timeout = millis();

//     while (client.connected() && millis() - timeout < 3000)
//     {
//       if (client.available())
//       {
//         char c = client.read();

//         if (readingFirstLine)
//         {
//           if (c == '\r' || c == '\n')
//           {
//             readingFirstLine = false;
//           }
//           else
//           {
//             if (reqLine.length() < 250)
//               reqLine += c;
//           }
//         }

//         if (c == '\n' && currentLineIsBlank)
//         {
//           int methodEnd = reqLine.indexOf(" ");
//           int urlEnd = reqLine.indexOf(" ", methodEnd + 1);
//           if (methodEnd == -1 || urlEnd == -1)
//           {
//             client.stop();
//             return;
//           }

//           String url = reqLine.substring(methodEnd + 1, urlEnd);
//           url.trim();

//           String basePath = url;
//           String queryParams = "";
//           if (url.indexOf("?") > 0)
//           {
//             basePath = url.substring(0, url.indexOf("?"));
//             queryParams = url.substring(url.indexOf("?") + 1);
//           }

//           // A. ROUTING HALAMAN HTML
//           String filePath = basePath;
//           if (basePath == "/" || basePath == "/home")
//             filePath = "/home.html";
//           else if (basePath == "/network")
//             filePath = "/network.html";
//           else if (basePath == "/analog_input")
//             filePath = "/analog_input.html";
//           else if (basePath == "/digital_IO")
//             filePath = "/digital_IO.html";
//           else if (basePath == "/system_settings")
//             filePath = "/system_settings.html";
//           else if (basePath == "/modbus_setup")
//             filePath = "/modbus_setup.html";
//           else if (basePath == "/updateOTA")
//             filePath = "/UpdateOTA.html";
//           else if (basePath == "/debug")
//             filePath = "/debug-monitor.html";

//           // B. API JSON

//           if (basePath == "/homeLoad")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(2048);
//               unsigned char id = 1;
//               if (queryParams.indexOf("input=") >= 0)
//               {
//                 String valStr = queryParams.substring(queryParams.indexOf("input=") + 6);
//                 id = valStr.toInt();
//               }
//               doc["networkMode"] = networkSettings.networkMode;
//               doc["ssid"] = networkSettings.ssid;
//               doc["ipAddress"] = Ethernet.localIP().toString();
//               doc["macAddress"] = networkSettings.macAddress;
//               doc["protocolMode"] = networkSettings.protocolMode;
//               doc["endpoint"] = networkSettings.endpoint;
//               doc["connStatus"] = networkSettings.connStatus;
//               doc["jobNumber"] = jobNum;
//               doc["sendInterval"] = networkSettings.sendInterval;
//               doc["datetime"] = getTimeDateNow();
//               JsonArray enableAI = doc.createNestedArray("enAI");
//               for (int i = 1; i <= jumlahInputAnalog; i++)
//               {
//                 if (analogInput[i].name != "")
//                   enableAI.add(1);
//                 else
//                   enableAI.add(0);
//               }
//               JsonObject AI = doc.createNestedObject("AI");
//               JsonArray AIRawValue = AI.createNestedArray("rawValue");
//               JsonArray AIScaledValue = AI.createNestedArray("scaledValue");
//               for (int i = 1; i <= jumlahInputAnalog; i++)
//               {
//                 AIRawValue.add(analogInput[i].adcValue);
//                 AIScaledValue.add(analogInput[i].mapValue);
//               }
//               JsonObject DI = doc.createNestedObject("DI");
//               JsonArray DIValue = DI.createNestedArray("value");
//               JsonArray DITaskMode = DI.createNestedArray("taskMode");
//               for (int i = 1; i <= jumlahInputDigital; i++)
//               {
//                 DIValue.add(digitalInput[i].value);
//                 DITaskMode.add(digitalInput[i].taskMode);
//               }
//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.print(dataLoad);
//             }
//           }
//           // else if (basePath == "/getValue")
//           // {
//           //   client.println("HTTP/1.1 200 OK");
//           //   client.println("Content-Type: application/json");
//           //   client.println("Access-Control-Allow-Origin: *");
//           //   client.println("Connection: close");
//           //   client.println();
//           //   //  if (sendString.length() > 0) client.print(sendString);
//           //   //  else client.print("{}");
//           //   if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(100)))
//           //   {
//           //     String realtimeJson;
//           //     serializeJson(jsonSend, realtimeJson); // Ambil jsonSend yg diupdate Task Modbus
//           //     client.print(realtimeJson);
//           //     xSemaphoreGive(jsonMutex);
//           //   }
//           //   else
//           //   {
//           //     client.print("{}");
//           //   }
//           // }

//           else if (basePath == "/getValue")
//           {
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Access-Control-Allow-Origin: *");
//             client.println("Connection: close");
//             client.println();

//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               String realtimeJson;
//               // Mengambil data dari jsonSend yang sudah diisi oleh Task_ModbusClient
//               serializeJson(jsonSend, realtimeJson); 
              
//               // --- DEBUG POINT 4: APA YANG DIKIRIM KE WEB? ---
//               Serial.print("🌐 [Web Eth] Sending /getValue: ");
//               Serial.println(realtimeJson);
//               // ----------------------------------------------
              
//               client.print(realtimeJson);
//               xSemaphoreGive(jsonMutex);
//             }
//             else
//             {
//               Serial.println("🌐 [Web Eth] Mutex Busy, sending empty {}");
//               client.print("{}");
//             }
//           }

//           else if (basePath == "/getCurrentValue")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               DynamicJsonDocument docTemp(1024);
//               for (int i = 0; i < jumlahInputDigital; i++)
//               {
//                 docTemp["rawValue" + String(i + 1)] = analogInput[i + 1].adcValue;
//                 docTemp["scaledValue" + String(i + 1)] = analogInput[i + 1].mapValue;
//               }
//               String dataLoad;
//               serializeJson(docTemp, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.print(dataLoad);
//             }
//             else
//             {
//               client.println("HTTP/1.1 200 OK");
//               client.println("Connection: close");
//               client.println();
//               client.print("{}");
//             }
//           }
//           else if (basePath == "/networkLoad")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(4096);
//               doc["networkMode"] = networkSettings.networkMode;
//               doc["ssid"] = networkSettings.ssid;
//               doc["password"] = networkSettings.password;
//               doc["apSsid"] = networkSettings.apSsid;
//               doc["apPassword"] = networkSettings.apPassword;
//               doc["dhcpMode"] = networkSettings.dhcpMode;
//               doc["ipAddress"] = networkSettings.ipAddress;
//               doc["subnet"] = networkSettings.subnetMask;
//               doc["ipGateway"] = networkSettings.ipGateway;
//               doc["ipDNS"] = networkSettings.ipDNS;
//               doc["sendInterval"] = String(networkSettings.sendInterval, 2);
//               doc["protocolMode"] = networkSettings.protocolMode;
//               doc["endpoint"] = networkSettings.endpoint;
//               doc["port"] = networkSettings.port;
//               doc["pubTopic"] = networkSettings.pubTopic;
//               doc["subTopic"] = networkSettings.subTopic;
//               doc["mqttUsername"] = networkSettings.mqttUsername;
//               doc["mqttPass"] = networkSettings.mqttPassword;
//               doc["loggerMode"] = networkSettings.loggerMode;
//               doc["protocolMode2"] = networkSettings.protocolMode2;
//               doc["modbusMode"] = modbusParam.mode;
//               doc["modbusPort"] = modbusParam.port;
//               doc["modbusSlaveID"] = modbusParam.slaveID;
//               doc["sendTrig"] = networkSettings.sendTrig;
//               doc["erpUrl"] = networkSettings.erpUrl;
//               doc["erpUsername"] = networkSettings.erpUsername;
//               doc["erpPassword"] = networkSettings.erpPassword;
//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.print(dataLoad);
//             }
//           }
//           else if (basePath == "/settingsLoad")
//           {
//             String jsonAuth = "{";
//             jsonAuth += "\"username\":\"" + networkSettings.loginUsername + "\",";
//             jsonAuth += "\"password\":\"" + networkSettings.loginPassword + "\",";
//             jsonAuth += "\"sdInterval\":" + String(networkSettings.sdSaveInterval);
//             jsonAuth += "}";
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.print(jsonAuth);
//           }
//           else if (basePath == "/getTime")
//           {
//             DynamicJsonDocument jsonDoc(256);
//             jsonDoc["datetime"] = getTimeDateNow();
//             String jsonTime;
//             serializeJson(jsonDoc, jsonTime);
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.print(jsonTime);
//           }
//           else if (basePath == "/analogLoad")
//           {
//             int id = 1;
//             if (queryParams.indexOf("input=") >= 0)
//             {
//               int start = queryParams.indexOf("input=") + 6;
//               int end = queryParams.indexOf("&", start);
//               if (end == -1)
//                 end = queryParams.length();
//               id = queryParams.substring(start, end).toInt();
//             }
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(4096);
//               doc["inputType"] = analogInput[id].inputType;
//               doc["filter"] = analogInput[id].filter;
//               doc["filterPeriod"] = (String(analogInput[id].filterPeriod, 2));
//               doc["scaling"] = analogInput[id].scaling;
//               doc["lowLimit"] = analogInput[id].lowLimit;
//               doc["highLimit"] = analogInput[id].highLimit;
//               doc["name"] = analogInput[id].name;
//               doc["calibration"] = analogInput[id].calibration;
//               doc["mValue"] = analogInput[id].mValue;
//               doc["cValue"] = analogInput[id].cValue;
//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.print(dataLoad);
//             }
//           }
//           else if (basePath == "/digitalLoad")
//           {
//             if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200)))
//             {
//               doc = DynamicJsonDocument(4096);
//               if (queryParams.indexOf("input=") >= 0)
//               {
//                 int start = queryParams.indexOf("input=") + 6;
//                 int end = queryParams.indexOf("&", start);
//                 if (end == -1)
//                   end = queryParams.length();
//                 int id = queryParams.substring(start, end).toInt();
//                 doc["nameDI"] = digitalInput[id].name;
//                 doc["invDI"] = digitalInput[id].inv;
//                 doc["taskMode"] = digitalInput[id].taskMode;
//                 doc["inputState"] = digitalInput[id].inputState ? "High" : "Low";
//                 doc["intervalTime"] = (float)digitalInput[id].intervalTime / 1000;
//                 doc["conversionFactor"] = digitalInput[id].conversionFactor;
//               }
//               String dataLoad;
//               serializeJson(doc, dataLoad);
//               xSemaphoreGive(jsonMutex);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: application/json");
//               client.println("Connection: close");
//               client.println();
//               client.print(dataLoad);
//             }
//           }
//           else if (basePath == "/modbusLoad")
//           {
//             client.println("HTTP/1.1 200 OK");
//             client.println("Content-Type: application/json");
//             client.println("Connection: close");
//             client.println();
//             client.print(stringParam);
//           }
//           // C. STATIC FILE HANDLER
//           else
//           {
//             if (SPIFFS.exists(filePath))
//             {
//               File file = SPIFFS.open(filePath, "r");
//               String contentType = getContentType(filePath);
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: " + contentType);
//               if (filePath.endsWith(".css") || filePath.endsWith(".js") || filePath.endsWith(".png"))
//               {
//                 client.println("Cache-Control: public, max-age=31536000");
//               }
//               client.println("Connection: close");
//               client.println();
//               uint8_t buffer[512];
//               while (file.available())
//               {
//                 int bytesRead = file.read(buffer, sizeof(buffer));
//                 client.write(buffer, bytesRead);
//               }
//               file.close();
//             }
//             else
//             {
//               client.println("HTTP/1.1 404 Not Found");
//               client.println("Connection: close");
//               client.println();
//             }
//           }

//           break;
//         }

//         if (c == '\n')
//           currentLineIsBlank = true;
//         else if (c != '\r')
//           currentLineIsBlank = false;
//       }
//     }
//     delay(2);
//     client.stop();
//   }
// }

// ============================================================================
// ETHERNET WEB SERVER HANDLER (TAMPILAN 100% SAMA DENGAN WIFI)
// ============================================================================
// void handleEthernetClient()
// {
//   EthernetClient client = ethServer.available();
//   if (client)
//   {
//     boolean currentLineIsBlank = true;
//     String reqLine = "";      
//     boolean readingFirstLine = true;
//     unsigned long timeout = millis();

//     while (client.connected() && millis() - timeout < 3000) 
//     {
//       if (client.available())
//       {
//         char c = client.read();

//         // 1. Tangkap Baris Pertama Request (GET /... HTTP/1.1)
//         if (readingFirstLine) {
//           if (c == '\r' || c == '\n') {
//             readingFirstLine = false;
//           } else {
//             if (reqLine.length() < 250) reqLine += c;
//           }
//         }

//         // 2. Deteksi Akhir Header
//         if (c == '\n' && currentLineIsBlank)
//         {
//           int methodEnd = reqLine.indexOf(" ");
//           int urlEnd = reqLine.indexOf(" ", methodEnd + 1);
//           if (methodEnd == -1 || urlEnd == -1) { client.stop(); return; }

//           String url = reqLine.substring(methodEnd + 1, urlEnd);
//           url.trim();

//           // Pisahkan URL Dasar dan Parameter (?input=1...)
//           String basePath = url;
//           String queryParams = "";
//           if (url.indexOf("?") > 0) {
//              basePath = url.substring(0, url.indexOf("?"));
//              queryParams = url.substring(url.indexOf("?") + 1);
//           }

//           // ==================================================================
//           // A. ROUTING HALAMAN HTML (URL -> FILE)
//           // ==================================================================
//           String filePath = basePath;
//           if (basePath == "/" || basePath == "/home") filePath = "/home.html"; 
//           else if (basePath == "/network") filePath = "/network.html";
//           else if (basePath == "/analog_input") filePath = "/analog_input.html";
//           else if (basePath == "/digital_IO") filePath = "/digital_IO.html";
//           else if (basePath == "/system_settings") filePath = "/system_settings.html";
//           else if (basePath == "/modbus_setup") filePath = "/modbus_setup.html";
//           else if (basePath == "/updateOTA") filePath = "/UpdateOTA.html";
//           else if (basePath == "/debug") filePath = "/debug-monitor.html";

//           // ==================================================================
//           // B. API JSON (WAJIB ADA AGAR TAMPILAN TIDAK KOSONG)
//           // ==================================================================
          
//           // 1. STATUS HEADER (Agar Tampilan Header Sama Persis WiFi)
//           if (basePath == "/wifiStatus" || basePath == "/ethernetStatus") 
//           {
//              DynamicJsonDocument statusDoc(512);
//              statusDoc["mode"] = networkSettings.networkMode;
//              statusDoc["ssid"] = "Ethernet LAN"; // Nama palsu biar header isi
//              statusDoc["connected"] = (Ethernet.linkStatus() == LinkON);
//              statusDoc["ip"] = Ethernet.localIP().toString();
//              statusDoc["rssi"] = -40; // Sinyal Full (Palsu)
//              statusDoc["macAddress"] = networkSettings.macAddress;
//              statusDoc["connectionStatus"] = networkSettings.connStatus;
//              statusDoc["signalQuality"] = "Wired"; // Indikator kabel
             
//              // Info tambahan biar JS tidak error
//              statusDoc["freeHeap"] = ESP.getFreeHeap();
//              statusDoc["uptime"] = millis() / 1000;

//              String response; serializeJson(statusDoc, response);
//              client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//              client.println("Connection: close"); client.println(); client.print(response);
//           }

//           // 2. HOME LOAD (Dashboard)
//           else if (basePath == "/homeLoad") 
//           {
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
//                 doc = DynamicJsonDocument(2048);
//                 // Parsing Input Param jika ada
//                 unsigned char id = 1;
//                 if(queryParams.indexOf("input=") >= 0) {
//                    String valStr = queryParams.substring(queryParams.indexOf("input=") + 6);
//                    id = valStr.toInt();
//                 }

//                 doc["networkMode"] = networkSettings.networkMode;
//                 doc["ssid"] = networkSettings.ssid;
//                 doc["ipAddress"] = Ethernet.localIP().toString();
//                 doc["macAddress"] = networkSettings.macAddress;
//                 doc["protocolMode"] = networkSettings.protocolMode;
//                 doc["endpoint"] = networkSettings.endpoint;
//                 doc["connStatus"] = networkSettings.connStatus;
//                 doc["jobNumber"] = jobNum;
//                 doc["sendInterval"] = networkSettings.sendInterval;
//                 doc["datetime"] = getTimeDateNow();

//                 JsonArray enableAI = doc.createNestedArray("enAI");
//                 for (int i = 1; i <= jumlahInputAnalog; i++) {
//                    enableAI.add(analogInput[i].name != "" ? 1 : 0);
//                 }
//                 JsonObject AI = doc.createNestedObject("AI");
//                 JsonArray AIRawValue = AI.createNestedArray("rawValue");
//                 JsonArray AIScaledValue = AI.createNestedArray("scaledValue");
//                 for (int i = 1; i <= jumlahInputAnalog; i++) {
//                    AIRawValue.add(analogInput[i].adcValue);
//                    AIScaledValue.add(analogInput[i].mapValue);
//                 }
//                 JsonObject DI = doc.createNestedObject("DI");
//                 JsonArray DIValue = DI.createNestedArray("value");
//                 JsonArray DITaskMode = DI.createNestedArray("taskMode");
//                 for (int i = 1; i <= jumlahInputDigital; i++) {
//                    DIValue.add(digitalInput[i].value);
//                    DITaskMode.add(digitalInput[i].taskMode);
//                 }
//                 String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
//                 client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//                 client.println("Connection: close"); client.println(); client.print(dataLoad);
//              }
//           }
          
//           // 3. GET VALUE (MODBUS REALTIME - DENGAN BYPASS)
// else if (basePath == "/getValue") 
//           {
//              client.println("HTTP/1.1 200 OK"); 
//              client.println("Content-Type: application/json"); 
//              client.println("Access-Control-Allow-Origin: *");
//              client.println("Connection: close"); 
//              client.println(); 
             
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(100))) {
//                  String realtimeJson;
//                  // Ambil data dari variabel global jsonSend
//                  serializeJson(jsonSend, realtimeJson); 
                 
//                  // --- DEBUG PRINT: Lihat apa yang dikirim ke browser ---
//                  Serial.print("Kirim ke Web: ");
//                  Serial.println(realtimeJson);
//                  // -----------------------------------------------------

//                  client.print(realtimeJson);
//                  xSemaphoreGive(jsonMutex);
//              } else {
//                  client.print("{}"); // Kirim kosong jika sistem sibuk
//              }
//           }
//           // 4. GET CURRENT VALUE (IO REALTIME)
//           else if (basePath == "/getCurrentValue") 
//           {
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
//                 DynamicJsonDocument docTemp(1024);
//                 for (int i = 0; i < jumlahInputDigital; i++) {
//                    docTemp["rawValue" + String(i + 1)] = analogInput[i+1].adcValue; 
//                    docTemp["scaledValue" + String(i + 1)] = analogInput[i+1].mapValue;
//                 }
//                 String dataLoad; serializeJson(docTemp, dataLoad); xSemaphoreGive(jsonMutex);
//                 client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//                 client.println("Connection: close"); client.println(); client.print(dataLoad);
//              }
//           }

//           // 5. NETWORK LOAD
//           else if (basePath == "/networkLoad") {
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
//                 doc = DynamicJsonDocument(4096);
//                 doc["networkMode"] = networkSettings.networkMode;
//                 doc["ssid"] = networkSettings.ssid;
//                 doc["password"] = networkSettings.password;
//                 doc["apSsid"] = networkSettings.apSsid;
//                 doc["apPassword"] = networkSettings.apPassword;
//                 doc["dhcpMode"] = networkSettings.dhcpMode;
//                 doc["ipAddress"] = networkSettings.ipAddress;
//                 doc["subnet"] = networkSettings.subnetMask;
//                 doc["ipGateway"] = networkSettings.ipGateway;
//                 doc["ipDNS"] = networkSettings.ipDNS;
//                 doc["sendInterval"] = String(networkSettings.sendInterval, 2);
//                 doc["protocolMode"] = networkSettings.protocolMode;
//                 doc["endpoint"] = networkSettings.endpoint;
//                 doc["port"] = networkSettings.port;
//                 doc["pubTopic"] = networkSettings.pubTopic;
//                 doc["subTopic"] = networkSettings.subTopic;
//                 doc["mqttUsername"] = networkSettings.mqttUsername;
//                 doc["mqttPass"] = networkSettings.mqttPassword;
//                 doc["loggerMode"] = networkSettings.loggerMode;
//                 doc["protocolMode2"] = networkSettings.protocolMode2;
//                 doc["modbusMode"] = modbusParam.mode;
//                 doc["modbusPort"] = modbusParam.port;
//                 doc["modbusSlaveID"] = modbusParam.slaveID;
//                 doc["sendTrig"] = networkSettings.sendTrig;
//                 doc["erpUrl"] = networkSettings.erpUrl;
//                 doc["erpUsername"] = networkSettings.erpUsername;
//                 doc["erpPassword"] = networkSettings.erpPassword;
//                 String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
//                 client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//                 client.println("Connection: close"); client.println(); client.print(dataLoad);
//              }
//           }

//           // 6. SETTINGS LOAD
//           else if (basePath == "/settingsLoad") {
//              String jsonAuth = "{";
//              jsonAuth += "\"username\":\"" + networkSettings.loginUsername + "\",";
//              jsonAuth += "\"password\":\"" + networkSettings.loginPassword + "\",";
//              jsonAuth += "\"sdInterval\":" + String(networkSettings.sdSaveInterval);
//              jsonAuth += "}";
//              client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//              client.println("Connection: close"); client.println(); client.print(jsonAuth);
//           }

//           // 7. GET TIME (Jam di Header)
//           else if (basePath == "/getTime") {
//              DynamicJsonDocument jsonDoc(256);
//              jsonDoc["datetime"] = getTimeDateNow();
//              String jsonTime; serializeJson(jsonDoc, jsonTime);
//              client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//              client.println("Connection: close"); client.println(); client.print(jsonTime);
//           }
          
//           // 8. ANALOG LOAD
//           else if (basePath == "/analogLoad") {
//              int id = 1;
//              if(queryParams.indexOf("input=") >= 0) {
//                 int start = queryParams.indexOf("input=") + 6;
//                 int end = queryParams.indexOf("&", start); if(end == -1) end = queryParams.length();
//                 id = queryParams.substring(start, end).toInt();
//              }
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
//                 doc = DynamicJsonDocument(4096);
//                 doc["inputType"] = analogInput[id].inputType;
//                 doc["filter"] = analogInput[id].filter;
//                 doc["filterPeriod"] = (String(analogInput[id].filterPeriod, 2));
//                 doc["scaling"] = analogInput[id].scaling;
//                 doc["lowLimit"] = analogInput[id].lowLimit;
//                 doc["highLimit"] = analogInput[id].highLimit;
//                 doc["name"] = analogInput[id].name;
//                 doc["calibration"] = analogInput[id].calibration;
//                 doc["mValue"] = analogInput[id].mValue;
//                 doc["cValue"] = analogInput[id].cValue;
//                 String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
//                 client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//                 client.println("Connection: close"); client.println(); client.print(dataLoad);
//              }
//           }

//           // 9. DIGITAL LOAD
//           else if (basePath == "/digitalLoad") {
//              if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
//                 doc = DynamicJsonDocument(4096);
//                 if(queryParams.indexOf("input=") >= 0) {
//                    int start = queryParams.indexOf("input=") + 6;
//                    int end = queryParams.indexOf("&", start); if(end == -1) end = queryParams.length();
//                    int id = queryParams.substring(start, end).toInt();
//                    doc["nameDI"] = digitalInput[id].name;
//                    doc["invDI"] = digitalInput[id].inv;
//                    doc["taskMode"] = digitalInput[id].taskMode;
//                    doc["inputState"] = digitalInput[id].inputState ? "High" : "Low";
//                    doc["intervalTime"] = (float)digitalInput[id].intervalTime/1000;
//                    doc["conversionFactor"] = digitalInput[id].conversionFactor;
//                 }
//                 String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
//                 client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//                 client.println("Connection: close"); client.println(); client.print(dataLoad);
//              }
//           }
          
//           // 10. MODBUS LOAD
//           else if (basePath == "/modbusLoad") {
//              client.println("HTTP/1.1 200 OK"); client.println("Content-Type: application/json"); 
//              client.println("Connection: close"); client.println(); client.print(stringParam); 
//           }

//           // ==================================================================
//           // C. STATIC FILE HANDLER (CSS, JS, IMAGES) - DENGAN BUFFER
//           // ==================================================================
//           else 
//           {
//             if (SPIFFS.exists(filePath))
//             {
//               File file = SPIFFS.open(filePath, "r");
//               String contentType = getContentType(filePath);
              
//               client.println("HTTP/1.1 200 OK");
//               client.println("Content-Type: " + contentType);
              
//               // Cache Control agar browser tidak reload asset statis (Load Cepat)
//               if (filePath.endsWith(".css") || filePath.endsWith(".js") || filePath.endsWith(".png")) {
//                 client.println("Cache-Control: public, max-age=31536000");
//               }
//               client.println("Connection: close");
//               client.println();

//               // KIRIM DENGAN BUFFER 512 BYTE (Stabil & Cepat)
//               uint8_t buffer[512];
//               while (file.available()) {
//                 int bytesRead = file.read(buffer, sizeof(buffer));
//                 client.write(buffer, bytesRead);
//               }
//               file.close();
//             }
//             else
//             {
//               client.println("HTTP/1.1 404 Not Found");
//               client.println("Connection: close");
//               client.println();
//             }
//           }
          
//           break; 
//         }
        
//         if (c == '\n') currentLineIsBlank = true;
//         else if (c != '\r') currentLineIsBlank = false;
//       }
//     }
//     delay(2);
//     client.stop();
//   }
// }


// ============================================================================
// 1. HELPER FUNCTIONS (WAJIB ADA UNTUK MEMBACA DATA SAVE)
// ============================================================================
unsigned char h2int(char c) {
    if (c >= '0' && c <= '9') return ((unsigned char)c - '0');
    if (c >= 'a' && c <= 'f') return ((unsigned char)c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return ((unsigned char)c - 'A' + 10);
    return 0;
}

String urlDecode(String str) {
    String encodedString = "";
    char c, code0, code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == '+') {
            encodedString += ' ';
        } else if (c == '%') {
            i++; code0 = str.charAt(i);
            i++; code1 = str.charAt(i);
            c = (h2int(code0) << 4) | h2int(code1);
            encodedString += c;
        } else {
            encodedString += c;
        }
    }
    return encodedString;
}

String getPostValue(String data, String key) {
    String keyParam = key + "=";
    int keyStart = data.indexOf(keyParam);
    if (keyStart == -1) return "";
    int valStart = keyStart + keyParam.length();
    int valEnd = data.indexOf("&", valStart);
    if (valEnd == -1) valEnd = data.length();
    return urlDecode(data.substring(valStart, valEnd));
}




void handleEthernetClient() {
  EthernetClient client = ethServer.available();
  if (!client) return;

  // -----------------------------------------------------------
  // A. BUFFER REQUEST (Baca seluruh data masuk ke String)
  // -----------------------------------------------------------
  String req = "";
  unsigned long timeout = millis();
  
  while (client.connected() && millis() - timeout < 2000) {
    if (client.available()) {
      char c = client.read();
      req += c;
      // Batasi buffer maks 3000 karakter agar RAM aman
      if (req.length() > 3000) break; 
    }
  }

  if (req.length() == 0) { client.stop(); return; }

  // -----------------------------------------------------------
  // B. PARSING HEADER (Method & URL)
  // -----------------------------------------------------------
  int firstSpace = req.indexOf(' ');
  int secondSpace = req.indexOf(' ', firstSpace + 1);
  
  if (firstSpace == -1 || secondSpace == -1) { client.stop(); return; }

  String method = req.substring(0, firstSpace);
  String fullUrl = req.substring(firstSpace + 1, secondSpace);
  fullUrl.trim();

  // Pisahkan Path dan Query Parameter (jika ada tanda ?)
  String basePath = fullUrl;
  String queryParams = "";
  if (fullUrl.indexOf("?") > 0) {
     basePath = fullUrl.substring(0, fullUrl.indexOf("?"));
     queryParams = fullUrl.substring(fullUrl.indexOf("?") + 1);
  }

  // -----------------------------------------------------------
  // C. AMBIL BODY (Payload POST)
  // -----------------------------------------------------------
  String postData = "";
  if (method == "POST") {
    int bodyStart = req.indexOf("\r\n\r\n");
    if (bodyStart != -1) {
      postData = req.substring(bodyStart + 4);
    }
    // Debug: Lihat data yang masuk
    Serial.println("📥 [ETH] " + method + " " + basePath);
    // if(postData.length() > 0) Serial.println("📦 [BODY] " + postData);
  }

  // -----------------------------------------------------------
  // D. HANDLE "OPTIONS" (Pre-flight CORS Request) - WAJIB ADA
  // -----------------------------------------------------------
  if (method == "OPTIONS") {
      client.println("HTTP/1.1 204 No Content");
      client.println("Access-Control-Allow-Origin: *");
      client.println("Access-Control-Allow-Methods: POST, GET, OPTIONS");
      client.println("Access-Control-Allow-Headers: Content-Type");
      client.println("Connection: close");
      client.println();
      client.stop();
      return;
  }

  // -----------------------------------------------------------
  // E. HANDLE POST (SAVE DATA)
  // -----------------------------------------------------------
  if (method == "POST") {
      // Header Response Standar
      client.println("HTTP/1.1 200 OK");
      client.println("Access-Control-Allow-Origin: *"); // Fix CORS
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();

      // 1. SAVE MODBUS SETUP (JSON BODY)
      if (basePath == "/modbus_setup") {
         DeserializationError error = deserializeJson(jsonParam, postData);
         if (!error) {
             modbusParam.baudrate = jsonParam["baudrate"];
             modbusParam.parity = jsonParam["parity"].as<String>();
             modbusParam.stopBit = jsonParam["stopBit"];
             modbusParam.dataBit = jsonParam["dataBit"];
             modbusParam.scanRate = jsonParam["scanRate"];
             
             // Refresh stringParam agar saat di-GET datanya baru
             stringParam = "";
             serializeJson(jsonParam, stringParam); 

             saveToJson("/modbusSetup.json", "modbusSetup");
             client.print("Modbus Saved");
         } else {
             client.print("JSON Error");
         }
      }
      
      // 2. SAVE NETWORK CONFIG
      else if (basePath == "/network") {
          // Jika setting ERP
          if (postData.indexOf("erpUrl=") >= 0) {
             networkSettings.erpUrl = getPostValue(postData, "erpUrl");
             networkSettings.erpUsername = getPostValue(postData, "erpUsername");
             networkSettings.erpPassword = getPostValue(postData, "erpPassword");
          } 
          // Jika setting Jaringan Utama
          else {
             networkSettings.networkMode = getPostValue(postData, "networkMode");
             networkSettings.dhcpMode = getPostValue(postData, "dhcpMode");
             networkSettings.ssid = getPostValue(postData, "ssid");
             networkSettings.password = getPostValue(postData, "password");
             networkSettings.apSsid = getPostValue(postData, "apSsid");
             networkSettings.apPassword = getPostValue(postData, "apPassword");
             networkSettings.ipAddress = getPostValue(postData, "ipAddress");
             networkSettings.subnetMask = getPostValue(postData, "subnet");
             networkSettings.ipGateway = getPostValue(postData, "ipGateway");
             networkSettings.ipDNS = getPostValue(postData, "ipDNS");
             
             networkSettings.protocolMode = getPostValue(postData, "protocolMode");
             networkSettings.endpoint = getPostValue(postData, "endpoint");
             networkSettings.port = getPostValue(postData, "port").toInt();
             networkSettings.pubTopic = getPostValue(postData, "pubTopic");
             networkSettings.subTopic = getPostValue(postData, "subTopic");
             networkSettings.mqttUsername = getPostValue(postData, "mqttUsername");
             networkSettings.mqttPassword = getPostValue(postData, "mqttPass");
             networkSettings.sendInterval = getPostValue(postData, "sendInterval").toFloat();
             networkSettings.sendTrig = getPostValue(postData, "sendTrig");
             networkSettings.protocolMode2 = getPostValue(postData, "protocolMode2");
             
             if(getPostValue(postData, "modbusPort") != "") modbusParam.port = getPostValue(postData, "modbusPort").toInt();
             if(getPostValue(postData, "slaveID") != "") modbusParam.slaveID = getPostValue(postData, "slaveID").toInt();
             
             configureSendTriggerInterrupt(networkSettings);
          }
          saveToJson("/configNetwork.json", "network");
          client.print("Network Saved");
      }

      // 3. SAVE DIGITAL IO
      else if (basePath == "/digital_IO") {
          String pinName = getPostValue(postData, "inputPin"); // misal "DI1"
          if(pinName.startsWith("DI")) {
              int id = pinName.substring(2).toInt();
              if (id >= 1 && id <= jumlahInputDigital) {
                  digitalInput[id].name = getPostValue(postData, "nameDI");
                  digitalInput[id].taskMode = getPostValue(postData, "taskMode");
                  digitalInput[id].inv = (postData.indexOf("inputInversion=") != -1);
                  digitalInput[id].inputState = (getPostValue(postData, "inputState") == "High") ? 1 : 0;
                  digitalInput[id].intervalTime = (long)(getPostValue(postData, "intervalTime").toFloat() * 1000);
                  digitalInput[id].conversionFactor = getPostValue(postData, "conversionFactor").toFloat();
                  
                  attachDigitalInputInterrupt(id);
                  saveToJson("/configDigital.json", "digital");
                  client.print("Digital Saved");
              }
          }
      }

      // 4. SAVE ANALOG INPUT
      else if (basePath == "/analog_input") {
          String pinName = getPostValue(postData, "inputPin"); // misal "AI1"
          if(pinName.startsWith("AI")) {
              int id = pinName.substring(2).toInt();
              if (id >= 1 && id <= jumlahInputAnalog) {
                  analogInput[id].name = getPostValue(postData, "name");
                  analogInput[id].inputType = getPostValue(postData, "inputType");
                  analogInput[id].filter = (postData.indexOf("filter=") != -1);
                  analogInput[id].scaling = (postData.indexOf("scaling=") != -1);
                  analogInput[id].calibration = (postData.indexOf("calibration=") != -1);
                  analogInput[id].filterPeriod = getPostValue(postData, "filterPeriod").toFloat();
                  analogInput[id].lowLimit = getPostValue(postData, "lowLimit").toFloat();
                  analogInput[id].highLimit = getPostValue(postData, "highLimit").toFloat();
                  analogInput[id].mValue = getPostValue(postData, "mValue").toFloat();
                  analogInput[id].cValue = getPostValue(postData, "cValue").toFloat();
                  
                  saveToJson("/configAnalog.json", "analog");
                  client.print("Analog Saved");
              }
          }
      }

      // 5. SAVE SYSTEM SETTINGS
      else if (basePath == "/system_settings") {
          if(getPostValue(postData, "username") != "") {
              // Update RTC jika ada data waktu
              if(getPostValue(postData, "datetime") != "") {
                  String dt = getPostValue(postData, "datetime");
                  // Format: YYYY-MM-DD HH:MM
                  int yy = dt.substring(0,4).toInt();
                  int mm = dt.substring(5,7).toInt();
                  int dd = dt.substring(8,10).toInt();
                  int hh = dt.substring(11,13).toInt();
                  int mn = dt.substring(14,16).toInt();
                  rtc.adjust(DateTime(yy, mm, dd, hh, mn, 0));
              }

              networkSettings.loginUsername = getPostValue(postData, "username");
              networkSettings.loginPassword = getPostValue(postData, "password");
              networkSettings.sdSaveInterval = getPostValue(postData, "sdInterval").toInt();
              saveToJson("/systemSettings.json", "systemSettings");
              client.print("Settings Saved");
          }
      }
      else {
          client.print("OK");
      }
  }

  // -----------------------------------------------------------
  // F. HANDLE GET (LOAD DATA & HTML)
  // -----------------------------------------------------------
  else if (method == "GET") {
      
      // 1. MODBUS CONFIG LOAD
      if (basePath == "/modbusLoad") {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *"); 
          client.println("Connection: close");
          client.println();
          
          if (stringParam.length() == 0) client.print("{}"); 
          else client.print(stringParam);
      }

      // 2. GET VALUE (Realtime Data Modbus)
      else if (basePath == "/getValue") 
      {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Connection: close");
          client.println();
          
          // Ambil Mutex agar tidak bentrok dengan Task Lain
          if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
              
              // 1. Buat Doc Sementara untuk menampung format Array
              DynamicJsonDocument docTemp(4096); // Size agak besar untuk array
              JsonArray arr = docTemp.to<JsonArray>();

              // 2. Ambil root object dari jsonSend (sumber data realtime)
              JsonObject root = jsonSend.as<JsonObject>();

              // 3. Konversi format: {Key:Val} -> [{"KodeSensor":Key, "Value":Val}]
              for (JsonPair kv : root) {
                  // Filter jika ada key dummy "-"
                  if (String(kv.key().c_str()) == "-") continue;

                  JsonObject item = arr.createNestedObject();
                  item["KodeSensor"] = kv.key().c_str();
                  
                  // Pastikan value diambil sebagai String agar aman
                  item["Value"] = kv.value().as<String>();

                  // Opsional: Tambahkan JobNum jika diperlukan frontend
                  if (jobNum.length() > 2) {
                     JsonObject add = item.createNestedObject("additional");
                     add["jobnum"] = jobNum;
                  }
              }

              String realtimeJson; 
              serializeJson(arr, realtimeJson); 
              
              // Debugging: Cek format yang dikirim (harus diawali kurung siku [ ... ])
              // Serial.print("Web Sending: "); Serial.println(realtimeJson);

              client.print(realtimeJson);
              xSemaphoreGive(jsonMutex);
          } else {
              // Jika sibuk, kirim array kosong agar web tidak error parsing
              client.print("[]");
          }
      }

      // 3. GET CURRENT VALUE (Realtime IO Dashboard)
      else if (basePath == "/getCurrentValue") {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Connection: close");
          client.println();

          if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
              DynamicJsonDocument docTemp(1024);
              for (int i = 1; i <= jumlahInputAnalog; i++) {
                 docTemp["rawValue" + String(i)] = analogInput[i].adcValue;
                 docTemp["scaledValue" + String(i)] = analogInput[i].mapValue;
              }
              String dataLoad; serializeJson(docTemp, dataLoad); xSemaphoreGive(jsonMutex);
              client.print(dataLoad);
          } else {
              client.print("{}");
          }
      }
      
      // 4. HOME LOAD (Dashboard Config)
      else if (basePath == "/homeLoad") {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *"); 
          client.println("Connection: close");
          client.println();
          
          if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
             doc = DynamicJsonDocument(2048);
             doc["networkMode"] = networkSettings.networkMode;
             doc["ssid"] = networkSettings.ssid;
             doc["ipAddress"] = Ethernet.localIP().toString();
             doc["connStatus"] = networkSettings.connStatus;
             doc["jobNumber"] = jobNum;
             doc["datetime"] = getTimeDateNow();
             
             JsonArray enableAI = doc.createNestedArray("enAI");
             for (int i = 1; i <= jumlahInputAnalog; i++) enableAI.add(analogInput[i].name != "" ? 1 : 0);
             
             JsonObject AI = doc.createNestedObject("AI");
             JsonArray AIRawValue = AI.createNestedArray("rawValue");
             JsonArray AIScaledValue = AI.createNestedArray("scaledValue");
             for (int i = 1; i <= jumlahInputAnalog; i++) {
                AIRawValue.add(analogInput[i].adcValue); AIScaledValue.add(analogInput[i].mapValue);
             }
             JsonObject DI = doc.createNestedObject("DI");
             JsonArray DIValue = DI.createNestedArray("value");
             JsonArray DITaskMode = DI.createNestedArray("taskMode");
             for (int i = 1; i <= jumlahInputDigital; i++) {
                DIValue.add(digitalInput[i].value); DITaskMode.add(digitalInput[i].taskMode);
             }
             String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
             client.print(dataLoad);
          } else {
             client.print("{}");
          }
      }

      // 5. NETWORK LOAD
      else if (basePath == "/networkLoad") {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *"); 
          client.println("Connection: close");
          client.println();
          
          if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
             doc = DynamicJsonDocument(4096);
             doc["networkMode"] = networkSettings.networkMode;
             doc["ssid"] = networkSettings.ssid;
             doc["password"] = networkSettings.password;
             doc["apSsid"] = networkSettings.apSsid;
             doc["apPassword"] = networkSettings.apPassword;
             doc["dhcpMode"] = networkSettings.dhcpMode;
             doc["ipAddress"] = networkSettings.ipAddress;
             doc["subnet"] = networkSettings.subnetMask;
             doc["ipGateway"] = networkSettings.ipGateway;
             doc["ipDNS"] = networkSettings.ipDNS;
             doc["sendInterval"] = String(networkSettings.sendInterval, 2);
             doc["protocolMode"] = networkSettings.protocolMode;
             doc["endpoint"] = networkSettings.endpoint;
             doc["port"] = networkSettings.port;
             doc["pubTopic"] = networkSettings.pubTopic;
             doc["subTopic"] = networkSettings.subTopic;
             doc["mqttUsername"] = networkSettings.mqttUsername;
             doc["mqttPass"] = networkSettings.mqttPassword;
             doc["loggerMode"] = networkSettings.loggerMode;
             doc["protocolMode2"] = networkSettings.protocolMode2;
             doc["modbusMode"] = modbusParam.mode;
             doc["modbusPort"] = modbusParam.port;
             doc["modbusSlaveID"] = modbusParam.slaveID;
             doc["sendTrig"] = networkSettings.sendTrig;
             doc["erpUrl"] = networkSettings.erpUrl;
             doc["erpUsername"] = networkSettings.erpUsername;
             doc["erpPassword"] = networkSettings.erpPassword;
             String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
             client.print(dataLoad);
          } else {
             client.print("{}");
          }
      }

      // 6. ANALOG LOAD (Settings)
      else if (basePath == "/analogLoad") {
          int id = 1;
          if(queryParams.indexOf("input=") >= 0) {
              int start = queryParams.indexOf("input=") + 6;
              int end = queryParams.indexOf("&", start); 
              if(end == -1) end = queryParams.length();
              id = queryParams.substring(start, end).toInt();
          }

          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Connection: close");
          client.println();

          if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
              doc = DynamicJsonDocument(4096);
              doc["inputType"] = analogInput[id].inputType;
              doc["filter"] = analogInput[id].filter;
              doc["filterPeriod"] = (String(analogInput[id].filterPeriod, 2));
              doc["scaling"] = analogInput[id].scaling;
              doc["lowLimit"] = analogInput[id].lowLimit;
              doc["highLimit"] = analogInput[id].highLimit;
              doc["name"] = analogInput[id].name;
              doc["calibration"] = analogInput[id].calibration;
              doc["mValue"] = analogInput[id].mValue;
              doc["cValue"] = analogInput[id].cValue;
              String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
              client.print(dataLoad);
          } else { client.print("{}"); }
      }

      // 7. DIGITAL LOAD (Settings)
      else if (basePath == "/digitalLoad") {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Connection: close");
          client.println();

          if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(200))) {
             doc = DynamicJsonDocument(4096);
             // Handle Input
             if(queryParams.indexOf("input=") >= 0) {
                int start = queryParams.indexOf("input=") + 6;
                int end = queryParams.indexOf("&", start); if(end == -1) end = queryParams.length();
                int id = queryParams.substring(start, end).toInt();
                doc["nameDI"] = digitalInput[id].name;
                doc["invDI"] = digitalInput[id].inv;
                doc["taskMode"] = digitalInput[id].taskMode;
                doc["inputState"] = digitalInput[id].inputState ? "High" : "Low";
                doc["intervalTime"] = (float)digitalInput[id].intervalTime/1000;
                doc["conversionFactor"] = digitalInput[id].conversionFactor;
             }
             // Handle Reset
             if(queryParams.indexOf("reset=") >= 0) {
                 int start = queryParams.indexOf("reset=") + 6;
                 int end = queryParams.indexOf("&", start); if(end == -1) end = queryParams.length();
                 int id = queryParams.substring(start, end).toInt();
                 digitalInput[id].value = 0;
             }
             String dataLoad; serializeJson(doc, dataLoad); xSemaphoreGive(jsonMutex);
             client.print(dataLoad);
          } else { client.print("{}"); }
      }

      // 8. SETTINGS LOAD
      else if (basePath == "/settingsLoad") {
          String jsonAuth = "{";
          jsonAuth += "\"username\":\"" + networkSettings.loginUsername + "\",";
          jsonAuth += "\"password\":\"" + networkSettings.loginPassword + "\",";
          jsonAuth += "\"sdInterval\":" + String(networkSettings.sdSaveInterval);
          jsonAuth += "}";
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Connection: close");
          client.println();
          client.print(jsonAuth);
      }

      // 9. WIFI/ETHERNET STATUS
      else if (basePath == "/wifiStatus" || basePath == "/ethernetStatus") {
          DynamicJsonDocument statusDoc(512);
          statusDoc["mode"] = networkSettings.networkMode;
          statusDoc["ssid"] = "Ethernet LAN";
          statusDoc["connected"] = (Ethernet.linkStatus() == LinkON);
          statusDoc["ip"] = Ethernet.localIP().toString();
          statusDoc["rssi"] = -40;
          statusDoc["macAddress"] = networkSettings.macAddress;
          statusDoc["connectionStatus"] = networkSettings.connStatus;
          statusDoc["signalQuality"] = "Wired";
          statusDoc["freeHeap"] = ESP.getFreeHeap();
          
          String response; serializeJson(statusDoc, response);
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/json");
          client.println("Access-Control-Allow-Origin: *");
          client.println("Connection: close");
          client.println();
          client.print(response);
      }

      // 10. STATIC FILES (HTML, CSS, JS)
      else {
          String filePath = basePath;
          if (basePath == "/" || basePath == "/home") filePath = "/home.html";
          else if (basePath == "/network") filePath = "/network.html";
          else if (basePath == "/analog_input") filePath = "/analog_input.html";
          else if (basePath == "/digital_IO") filePath = "/digital_IO.html";
          else if (basePath == "/system_settings") filePath = "/system_settings.html";
          else if (basePath == "/modbus_setup") filePath = "/modbus_setup.html";
          else if (basePath == "/updateOTA") filePath = "/UpdateOTA.html";
          else if (basePath == "/debug") filePath = "/debug-monitor.html";

          if (SPIFFS.exists(filePath)) {
               File file = SPIFFS.open(filePath, "r");
               String contentType = getContentType(filePath);
               
               client.println("HTTP/1.1 200 OK");
               client.println("Content-Type: " + contentType);
               client.println("Access-Control-Allow-Origin: *");
               if (filePath.endsWith(".css") || filePath.endsWith(".js") || filePath.endsWith(".png")) {
                 client.println("Cache-Control: public, max-age=31536000");
               }
               client.println("Connection: close");
               client.println();
               
               while (file.available()) client.write(file.read());
               file.close();
           } else {
               client.println("HTTP/1.1 404 Not Found");
               client.println("Connection: close");
               client.println();
           }
      }
  }

  delay(5);
  client.stop();
}


// CORE 0 TASK: Network Management, WiFi, TCP/IP, Web Server


void Task_NetworkManagement(void *parameter)
{
  ESP_LOGI("Core0", "Network Management Task started on core %d", xPortGetCoreID());

  unsigned long lastNetCheck = 0;
  unsigned long lastDNSProcess = 0;
  unsigned long lastMQTTCheck = 0;

  // =========================================================================
  // TAMBAHAN BARU: Variabel untuk monitoring Ethernet
  // =========================================================================
  unsigned long lastEthernetCheck = 0;
  EthernetLinkStatus lastLinkStatus = Unknown;
  bool ethernetInitialized = false;

  while (true)
  {
    // DNS Server Processing (setiap 10ms)
    if (millis() - lastDNSProcess >= 10)
    {
      dnsServer.processNextRequest();
      lastDNSProcess = millis();
    }

    // WiFi Connection Check (setiap 3 detik)
    if (millis() - lastNetCheck >= 3000)
    {
      checkWiFi(3000);
      lastNetCheck = millis();
    }

    if (networkSettings.networkMode == "Ethernet")
    {
      handleEthernetClient();
    }
    // =========================================================================
    // BLOK BARU: Ethernet Link Status Monitoring (setiap 5 detik)
    // =========================================================================
    // =========================================================================
    // AKHIR BLOK ETHERNET MONITORING
    // =========================================================================
    // MQTT Keep Alive (setiap 1 detik)
    if (networkSettings.protocolMode == "MQTT")
    {
      if (millis() - lastMQTTCheck >= 1000)
      {
        if (!mqtt.connected())
        {
          // Reconnect MQTT
          if (mqtt.connect("ESP32Client", networkSettings.mqttUsername.c_str(),
                           networkSettings.mqttPassword.c_str()))
          {
            mqtt.subscribe(networkSettings.subTopic.c_str());
            ESP_LOGI("MQTT", "Reconnected to MQTT broker");
          }
        }
        mqtt.loop();
        lastMQTTCheck = millis();
      }
    }

    // DNS Server Processing (setiap 10ms) - DUPLICATE, bisa dihapus salah satu
    if (dnsStarted && millis() - lastDNSProcess >= 10)
    {
      dnsServer.processNextRequest();
      lastDNSProcess = millis();
    }

    // Modbus TCP/IP Task
    bool useTCP = (networkSettings.protocolMode2 == "Modbus TCP/IP" ||
                   networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");
    if (useTCP)
    {
      mbIP.task();
    }

    // Error blinker update
    errorBlinker.update();

    // Yield untuk task lain
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// CORE 1 TASK: Data Acquisition (Analog & Digital Input)
// ============================================================================
void Task_DataAcquisition(void *parameter)
{
  ESP_LOGI("Core1", "Data Acquisition Task started on core %d", xPortGetCoreID());

  SensorDataPacket sensorData;
  unsigned long lastReadAnalog = 0;
  unsigned long lastReadDigital = 0;
  unsigned long lastRunTimeCheck = 0;

  while (true)
  {
    // ========================================================================
    // READ ANALOG INPUTS (setiap 100ms)
    // ========================================================================
    if (millis() - lastReadAnalog >= 100)
    {
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)))
      {
        for (byte i = 1; i < jumlahInputAnalog + 1; i++)
        {
          int16_t valueADC = ads.readADC(i - 1);

          if (analogInput[i].filter)
            analogInput[i].adcValue = filterSensor(valueADC, analogInput[i].adcValue,
                                                   analogInput[i].filterPeriod);
          else
            analogInput[i].adcValue = valueADC;

          // Scaling berdasarkan input type
          if (analogInput[i].inputType == "4-20 mA")
          {
            if (analogInput[i].scaling)
              analogInput[i].mapValue = mapFloat(analogInput[i].adcValue, 5333.33, 26666.67,
                                                 analogInput[i].lowLimit, analogInput[i].highLimit);
            else
              analogInput[i].mapValue = mapFloat(analogInput[i].adcValue, 5333.33, 26666.67,
                                                 4.0, 20.0);
          }
          else if (analogInput[i].inputType == "0-20 mA")
          {
            if (analogInput[i].scaling)
              analogInput[i].mapValue = mapFloat(analogInput[i].adcValue, 0.0, 26666.67,
                                                 analogInput[i].lowLimit, analogInput[i].highLimit);
            else
              analogInput[i].mapValue = mapFloat(analogInput[i].adcValue, 0.0, 26666.67,
                                                 0.0, 20.0);
          }
          else
          {
            if (analogInput[i].scaling)
              analogInput[i].mapValue = mapFloat(analogInput[i].adcValue, 0.0, 26666.67,
                                                 analogInput[i].lowLimit, analogInput[i].highLimit);
            else
              analogInput[i].mapValue = mapFloat(analogInput[i].adcValue, 0.0, 26666.67,
                                                 0.0, 10.0);
          }

          sensorData.analogValues[i] = analogInput[i].mapValue;

          // Update Modbus registers
          bool useTCP = (networkSettings.protocolMode2 == "Modbus TCP/IP" ||
                         networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");
          bool useRTU = (networkSettings.protocolMode2 == "Modbus RTU" ||
                         networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");

          if (useTCP)
          {
            mbIP.Ireg(i + 9, analogInput[i].adcValue);
            mbIP.Ireg(i - 1, analogInput[i].mapValue * 100);
          }
          if (useRTU)
          {
            mbRTU.Ireg(i + 9, analogInput[i].adcValue);
            mbRTU.Ireg(i - 1, analogInput[i].mapValue * 100);
          }
        }
        xSemaphoreGive(i2cMutex);
      }
      lastReadAnalog = millis();
    }

    // ========================================================================
    // READ DIGITAL INPUTS (setiap 50ms)
    // ========================================================================
    if (millis() - lastReadDigital >= 50)
    {
      for (byte i = 1; i < jumlahInputDigital + 1; i++)
      {
        if (digitalInput[i].taskMode == "Cycle Time")
        {
          if (digitalInput[i].flagInt)
          {
            digitalInput[i].value = (digitalInput[i].millisNow - digitalInput[i].millis_1) / 1000.0;
            digitalInput[i].millis_1 = digitalInput[i].millisNow;
            digitalInput[i].flagInt = 0;
          }
        }
        else if (digitalInput[i].taskMode == "Counting")
        {
          if (digitalInput[i].flagInt)
          {
            digitalInput[i].value++;
            digitalInput[i].flagInt = 0;
          }
        }
        else if (digitalInput[i].taskMode == "Pulse Mode")
        {
          if (millis() - digitalInput[i].lastMillisPulseMode > digitalInput[i].intervalTime)
          {
            digitalInput[i].value = (float)digitalInput[i].sumValue * digitalInput[i].conversionFactor;
            digitalInput[i].sumValue = 0;
            digitalInput[i].lastMillisPulseMode = millis();
          }
        }
        else
        {
          digitalInput[i].value = digitalInput[i].inv ? !digitalRead(digitalInput[i].pin) : digitalRead(digitalInput[i].pin);
        }

        sensorData.digitalValues[i] = digitalInput[i].value;

        // Update Modbus registers
        bool useTCP = (networkSettings.protocolMode2 == "Modbus TCP/IP" ||
                       networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");
        bool useRTU = (networkSettings.protocolMode2 == "Modbus RTU" ||
                       networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");

        if (useTCP)
          mbIP.Ireg(i + 19, digitalInput[i].value);
        if (useRTU)
          mbRTU.Ireg(i + 19, digitalInput[i].value);
      }
      lastReadDigital = millis();
    }

    // ========================================================================
    // RUN TIME CHECK (setiap 60 detik)
    // ========================================================================
    if (millis() - lastRunTimeCheck >= 60000)
    {
      for (byte i = 1; i < jumlahInputDigital + 1; i++)
      {
        if (digitalInput[i].taskMode == "Run Time")
        {
          if (digitalRead(digitalInput[i].pin) == digitalInput[i].inputState)
          {
            digitalInput[i].value++;
            updateJson("/runtimeData.json", String(i).c_str(), digitalInput[i].value);
          }
        }
      }
      lastRunTimeCheck = millis();
    }

    // Send data ke queue untuk task lain
    sensorData.timestamp = millis();
    xQueueSend(queueSensorData, &sensorData, 0);

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================================
// CORE 1 TASK: Modbus Client (Master)
// ============================================================================
// void Task_ModbusClient(void *parameter)
// {
//   ESP_LOGI("Core1", "Modbus Client Task started on core %d", xPortGetCoreID());

//   unsigned long lastModbusRead = 0;
//   ModbusDataPacket modbusData;

//   while (true)
//   {
//     if (millis() - lastModbusRead >= (modbusParam.scanRate * 1000))
//     {
//       if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(100)))
//       {
//         deserializeJson(jsonParam, stringParam);
//         JsonArray nameData = jsonParam["nameData"];
//         numOfParam = nameData.size();

//         if (numOfParam > 0)
//         {
//           const char *paramName = nameData[modbusCount].as<const char *>();
//           JsonArray paramArray = jsonParam[paramName];

//           float modbusVal = readModbus(paramArray[0], paramArray[1], paramArray[2]);
//           modbusVal *= paramArray[3].as<float>();

//           // Update Modbus registers
//           int tempReg = paramArray[4];
//           bool useTCP = (networkSettings.protocolMode2 == "Modbus TCP/IP" ||
//                          networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");
//           bool useRTU = (networkSettings.protocolMode2 == "Modbus RTU" ||
//                          networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");

//           if (useTCP)
//             mbIP.Ireg(tempReg - 1, modbusVal);
//           if (useRTU)
//             mbRTU.Ireg(tempReg - 1, modbusVal);

//           // Kirim ke queue
//           modbusData.paramName = String(paramName);
//           modbusData.value = modbusVal;
//           modbusData.timestamp = millis();
//           xQueueSend(queueModbusData, &modbusData, 0);

//           modbusCount++;
//           if (modbusCount >= numOfParam)
//             modbusCount = 0;
//         }

//         xSemaphoreGive(jsonMutex);
//       }
//       lastModbusRead = millis();
//     }

//     // Modbus RTU task
//     bool useRTU = (networkSettings.protocolMode2 == "Modbus RTU" ||
//                    networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");
//     if (useRTU)
//     {
//       mbRTU.task();
//     }

//     vTaskDelay(pdMS_TO_TICKS(100));
//   }
// }
// ============================================================================
// CORE 1 TASK: Modbus Client (Master) - DEBUG VERSION
// ============================================================================
void Task_ModbusClient(void *parameter)
{
  ESP_LOGI("Core1", "Modbus Client Task started");
  unsigned long lastModbusRead = 0;

  while (true)
  {
    // Cek Timer sesuai Scan Rate (Misal tiap 1 detik)
    if (millis() - lastModbusRead >= (modbusParam.scanRate * 1000))
    {
        // 1. Cek berapa banyak parameter yang harus dibaca
        int totalParamsToRead = 0;
        
        if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000))) {
            deserializeJson(jsonParam, stringParam);
            JsonArray nameData = jsonParam["nameData"];
            totalParamsToRead = nameData.size();
            xSemaphoreGive(jsonMutex);
        }

        // 2. LOOPING SEMUA PARAMETER (BURST READ)
        // Kita habiskan semua daftar sensor dalam satu siklus ini
        for (int i = 0; i < totalParamsToRead; i++) 
        {
            String currentParamName = "";
            unsigned int addr = 0, fc = 0, reg = 0;
            float multiplier = 1.0;
            bool validParam = false;

            // A. Ambil Konfigurasi Sensor ke-i (CEPAT)
            if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(50))) 
            {
                JsonArray nameData = jsonParam["nameData"];
                currentParamName = nameData[i].as<String>(); // Ambil nama berdasarkan index loop
                
                JsonArray paramArray = jsonParam[currentParamName];
                addr = paramArray[0];       // Slave ID
                fc = paramArray[1];         // Function Code
                reg = paramArray[2];        // Register
                multiplier = paramArray[3]; // Scaling
                
                validParam = true;
                xSemaphoreGive(jsonMutex);
            }

            // B. Baca Sensor (LAMBAT - I/O Hardware)
            // Lakukan di luar mutex agar Web Server tidak macet
            if (validParam) 
            {
                // Serial.printf("Reading [%d/%d]: %s... ", i+1, totalParamsToRead, currentParamName.c_str());
                
                unsigned int rawValue = readModbus(addr, fc, reg);
                float finalValue = rawValue * multiplier;

                // C. Simpan Hasil (CEPAT)
                if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(50))) {
                    jsonSend[currentParamName] = String(finalValue, 2);
                    xSemaphoreGive(jsonMutex);
                    // Serial.printf("Val: %.2f\n", finalValue);
                }
            }

            // D. Jeda Pendek Antar Sensor (Agar RS485 Stabil)
            // Jangan terlalu cepat membombardir sensor, beri nafas 50ms
            vTaskDelay(pdMS_TO_TICKS(50)); 
        }

        // Setelah semua sensor terbaca, reset timer utama
        lastModbusRead = millis();
    }
    
    // Yield task agar CPU tidak hang
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// CORE 1 TASK: Data Logger & HTTP Sender
// ============================================================================
// ============================================================================
// CORE 1 TASK: Data Logger (KHUSUS MODBUS REALTIME)
// ============================================================================
// ============================================================================
// CORE 1 TASK: Data Logger (PERBAIKAN: Parsing Lokal)
// ============================================================================
void Task_DataLogger(void *parameter)
{
  ESP_LOGI("Core1", "Data Logger Task started");

  SensorDataPacket sensorData;
  ModbusDataPacket modbusData;
  
  unsigned long lastSendTime = 0;
  unsigned long lastSDSave = 0;
  unsigned long lastPrint = 0;

  while (true)
  {
    // ------------------------------------------------------------------------
    // 1. UPDATE DATA DARI QUEUE (Opsional, jika masih dipakai)
    // ------------------------------------------------------------------------
    if (xQueueReceive(queueSensorData, &sensorData, 0) == pdTRUE) {
      if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(100))) {
        for (byte i = 1; i < jumlahInputAnalog + 1; i++) {
           if (analogInput[i].name != "") jsonSend[analogInput[i].name] = String(sensorData.analogValues[i], 2);
        }
        for (byte i = 1; i < jumlahInputDigital + 1; i++) {
           if (digitalInput[i].name != "") jsonSend[digitalInput[i].name] = sensorData.digitalValues[i];
        }
        xSemaphoreGive(jsonMutex);
      }
    }

    // ------------------------------------------------------------------------
    // 2. SIMPAN DATA MODBUS KE SD CARD (Setiap Interval)
    // ------------------------------------------------------------------------
    // Gunakan interval config, atau hardcode 10000 (10 detik) untuk test
    if (millis() - lastSDSave >= (networkSettings.sdSaveInterval * 60000UL)) 
    {
      String csvLine = "";
      bool hasData = false;

      // Ambil Mutex untuk membaca data global dengan aman
      if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(2000))) // Timeout diperpanjang biar pasti dapat
      {
          // A. Parse Ulang Konfigurasi ke Objek LOKAL (PENTING!)
          // Kita tidak pakai jsonParam global agar tidak bentrok dengan Task Modbus
          DynamicJsonDocument localDoc(4096); 
          DeserializationError error = deserializeJson(localDoc, stringParam);

          if (!error && localDoc.containsKey("nameData")) 
          {
             // Tambahkan Waktu
             csvLine += getTimeDateNow(); 
             csvLine += ",";

             JsonArray nameData = localDoc["nameData"];
             
             // B. Loop Nama Sensor dari Config Lokal
             for (JsonVariant v : nameData) 
             {
                String key = v.as<String>();
                
                // Tambahkan Nama Sensor ke CSV
                csvLine += key;
                csvLine += ":";
                
                // C. Ambil Nilai dari jsonSend (Data Realtime)
                if (jsonSend.containsKey(key)) {
                   String val = jsonSend[key].as<String>();
                   csvLine += val;
                   hasData = true; // Tandai bahwa kita berhasil dapat data
                } else {
                   csvLine += "null"; // Data belum masuk ke jsonSend
                }
                csvLine += "|"; // Pemisah antar sensor
             }
          }
          else 
          {
             Serial.println("⚠️ [Logger] Gagal parse stringParam atau Config Kosong!");
             csvLine = "Error_Config_Empty";
          }
          
          xSemaphoreGive(jsonMutex);
      }
      else {
          Serial.println("⚠️ [Logger] JSON Mutex Busy/Timeout!");
      }

      // D. Simpan ke SD Card jika ada data valid
      if (hasData && csvLine.length() > 10) {
         saveToSD(csvLine); 
      } else {
         Serial.println("⚠️ [Logger] Tidak ada data valid untuk disimpan. (Line: " + csvLine + ")");
      }
      
      lastSDSave = millis();
    }

    // ------------------------------------------------------------------------
    // 3. Keep Alive HTTP/MQTT (Logic Lama)
    // ------------------------------------------------------------------------
    if (millis() - lastSendTime >= (networkSettings.sendInterval * 1000)) {
        lastSendTime = millis();
    }
    
    // Backup logic
    if (millis() - lastPrint >= 10000) {
       // sendBackupData(); // Aktifkan jika perlu
       lastPrint = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================================
// SETUP FUNCTION
// ============================================================================
// ============================================================================
// SETUP FUNCTION (FULL REVISION)
// ============================================================================
void setup()
{
  Serial.begin(115200);
  delay(1000); // Tunggu sebentar biar serial stabil

  Serial.println("\n=================================");
  Serial.println("SYSTEM BOOT START");
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println("=================================");

  // Set CPU frequency
  setCpuFrequencyMhz(240);
  
  // Reset Pin Ethernet Hardware
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, HIGH);

  // Setup digital input pins
  for (byte i = 1; i < jumlahInputDigital + 1; i++)
  {
    digitalInput[i].pin = DI_PINS[i - 1];
    pinMode(digitalInput[i].pin, INPUT_PULLDOWN);
  }

  // ========================================================================
  // 1. CREATE MUTEXES & QUEUES
  // ========================================================================
  spiMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();
  sdMutex = xSemaphoreCreateMutex();
  jsonMutex = xSemaphoreCreateMutex();

  queueSensorData = xQueueCreate(10, sizeof(SensorDataPacket));
  queueModbusData = xQueueCreate(10, sizeof(ModbusDataPacket));
  queueLogData = xQueueCreate(10, sizeof(LogDataPacket));

  if (!spiMutex || !jsonMutex || !queueSensorData) {
    Serial.println("❌ Critical Error: Failed to create Mutex/Queue!");
    while(1) delay(1000);
  }

  // ========================================================================
  // 2. READ CONFIG & INIT BASIC HARDWARE
  // ========================================================================
  // Read configuration (SPIFFS)
  readConfig();
  
  // Force Ethernet mode (sesuai request Anda)
  networkSettings.networkMode = "Ethernet";
  Serial.println("[FORCE] Mode set to Ethernet");

  modbusSlaveSetup();
  setupInterrupts();

  // Initialize I2C
  Wire.begin();

  // Initialize Time & RTC
  configTime(7 * 3600, 0, "pool.ntp.org");
  if (!rtc.begin()) {
    Serial.println("❌ RTC Not Found");
    errorMessages.addMessage("RTC Failed");
  } else {
    Serial.println("✅ RTC Initialized");
    // Sync time logic here if needed...
  }

  // ========================================================================
  // 3. INIT SPI BUS (BAGIAN YANG ANDA TANYAKAN)
  // ========================================================================
  // Ini menggantikan blok "SD CARD" yang lama.
  // Tujuannya: Menyalakan SD Card dan Ethernet dengan urutan yang benar (Anti-Bentrok)
  
  Serial.println("[Init] Configuring SPI Hardware...");
  
  // A. Matikan (Deselect) kedua chip dulu agar tidak tabrakan di awal
  pinMode(ETH_CS, OUTPUT);
  digitalWrite(ETH_CS, HIGH);    // Matikan Ethernet (CS HIGH = OFF)
  
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH); // Matikan SD Card (CS HIGH = OFF)
  
  delay(50); // Beri waktu jeda

  // B. Mulai Jalur SPI (Shared Bus)
  // Kita set pin manual agar yakin W5500 dan SD Card pakai jalur yg sama
  // Parameter terakhir -1 artinya "tidak ada CS default", karena kita atur manual
  SPI.begin(ETH_CLK, ETH_MISO, ETH_MOSI, -1); 

  // C. Nyalakan SD Card Duluan (Sekali Saja!)
  Serial.print("[Init] Mounting SD Card... ");
  
  // Gunakan Mutex untuk keamanan
  if (xSemaphoreTake(spiMutex, portMAX_DELAY))
  {
    // Coba mount SD Card
    // Speed 4MHz lebih stabil untuk kabel jumper/panjang
    if (!SD.begin(SD_CS_PIN, SPI, 4000000)) 
    {
      Serial.println("❌ SD Card Mount Failed!");
      Serial.println("   -> Cek kabel atau format kartu (FAT32)");
      errorMessages.addMessage("SD Init Failed");
    }
    else
    {
      Serial.println("✅ SD Card Mounted Successfully!");
      Serial.print("   -> Size: ");
      Serial.print(SD.totalBytes() / (1024 * 1024));
      Serial.println(" MB");
    }
    xSemaphoreGive(spiMutex);
  }

  // Catatan: Ethernet W5500 belum dinyalakan disini.
  // Ethernet akan dinyalakan nanti di dalam fungsi configNetwork().

  // ========================================================================
  // 4. INIT SENSORS & NETWORK
  // ========================================================================

  // Initialize ADS1115
  if (!ads.begin()) {
    Serial.println("❌ ADS1115 Failed");
    errorMessages.addMessage("ADS1115 Failed");
  } else {
    Serial.println("✅ ADS1115 Initialized");
    ads.setGain(0);
  }

  // Init Variables
  printTime = millis();
  sendTimeModbus = millis();
  modbusCount = 0;
  jsonSend = DynamicJsonDocument(4096); 

  ESP_LOGI("Network", "Configuring network interface...");
  
  // Di dalam fungsi ini, Ethernet.begin() akan dipanggil dan W5500 akan aktif
  configNetwork(); 
  
  configProtocol();

  delay(1000);

  // Start Web Server
  ESP_LOGI("WebServer", "Starting web server...");
  setupWebServer();
  
  if (networkSettings.networkMode == "Ethernet") {
    ethServer.begin();
    Serial.println("✅ Ethernet Web Server STARTED");
  }

  // Print Access Info
  Serial.println("\n=================================");
  Serial.println("SYSTEM READY");
  if(networkSettings.networkMode == "Ethernet") {
     Serial.printf("Ethernet IP: %s\n", Ethernet.localIP().toString().c_str());
  }
  Serial.printf("WiFi AP IP:  %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("=================================\n");

  // ========================================================================
  // 5. CREATE TASKS
  // ========================================================================
  xTaskCreatePinnedToCore(Task_NetworkManagement, "NetworkTask", 8192, NULL, 2, &Task_Core0_Network, 0);
  xTaskCreatePinnedToCore(Task_DataAcquisition, "DataAcqTask", 4096, NULL, 3, &Task_Core1_DataAcquisition, 1);
  xTaskCreatePinnedToCore(Task_ModbusClient, "ModbusTask", 4096, NULL, 2, &Task_Core1_ModbusClient, 1);
  xTaskCreatePinnedToCore(Task_DataLogger, "LoggerTask", 8192, NULL, 1, &Task_Core1_DataLogger, 1);
}

// ============================================================================
// LOOP FUNCTION (akan menjadi idle task)
// ============================================================================
void loop()
{
  // Loop kosong karena semua pekerjaan dilakukan di FreeRTOS tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ============================================================================
// SETUP WEB SERVER
// ============================================================================
void setupWebServer()
{
  // Handle OTA update
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request)
            {
      bool shouldReboot = !Update.hasError();
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", 
        shouldReboot ? "Update Successful. Rebooting..." : "Update Failed. Check error logs.");
      response->addHeader("Connection", "close");
      request->send(response);
      
      if (shouldReboot) {
        ESP_LOGI("OTA", "Update successful, rebooting...");
        delay(2000);
        ESP.restart();
      } else {
        ESP_LOGE("OTA", "Update failed");
        errorMessages.addMessage(getTimeNow() + " - OTA Update Failed");
      } }, [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
            {
      if (!index) {
        ESP_LOGI("OTA", "Update Start: %s", filename.c_str());
        Serial.printf("Update Start: %s\n", filename.c_str());
        
        int updateType = U_FLASH;
        if (filename.indexOf("spiffs") > -1) {
          updateType = U_SPIFFS;
          ESP_LOGI("OTA", "Update Type: SPIFFS");
        } else {
          ESP_LOGI("OTA", "Update Type: Firmware");
        }
        
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
          Update.printError(Serial);
          ESP_LOGE("OTA", "Update.begin failed");
          errorMessages.addMessage(getTimeNow() + " - OTA Update.begin failed");
        } else {
          ESP_LOGI("OTA", "Update started successfully");
        }
      }
      
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
        ESP_LOGE("OTA", "Update.write failed");
      } else {
        static unsigned long lastProgress = 0;
        if (millis() - lastProgress > 1000) {
          ESP_LOGI("OTA", "Progress: %d bytes written", index + len);
          lastProgress = millis();
        }
      }
      
      if (final) {
        if (Update.end(true)) {
          ESP_LOGI("OTA", "Update Success: %uB written", index + len);
          Serial.printf("Update Success: %uB\n", index + len);
        } else {
          Update.printError(Serial);
          ESP_LOGE("OTA", "Update.end failed");
          errorMessages.addMessage(getTimeNow() + " - OTA Update.end failed");
        }
      } });

  // OTA status endpoint
  server.on("/updateStatus", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    DynamicJsonDocument statusDoc(256);
    
    if (Update.hasError()) {
      statusDoc["status"] = "error";
      statusDoc["error"] = Update.errorString();
    } else if (Update.isRunning()) {
      statusDoc["status"] = "updating";
      statusDoc["progress"] = Update.progress();
      statusDoc["size"] = Update.size();
    } else {
      statusDoc["status"] = "idle";
    }
    
    statusDoc["freeHeap"] = ESP.getFreeHeap();
    statusDoc["sketchSize"] = ESP.getSketchSize();
    statusDoc["freeSketchSpace"] = ESP.getFreeSketchSpace();
    
    String response;
    serializeJson(statusDoc, response);
    request->send(200, "application/json", response); });

  // Web server routes
  // [3] Root handler untuk captive portal
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/home.html", "text/html"); });
  // [4] Redirect semua request ke root
  server.onNotFound([](AsyncWebServerRequest *request)
                    { request->redirect("/"); });

  server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request)
            { handleFileRequest(request, "/debug-monitor.html", "text/html"); });

  server.addHandler(&errorMessages._eventSource);

  server.on("/system_settings", HTTP_GET, [](AsyncWebServerRequest *request)
            { handleFileRequest(request, "/system_settings.html", "text/html"); });
  server.on("/system_settings", HTTP_POST, [](AsyncWebServerRequest *request)
            { handleFormSubmit(request); });

  server.on("/settingsLoad", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String jsonAuth = "{";
    jsonAuth += "\"username\":\"" + networkSettings.loginUsername + "\",";
    jsonAuth += "\"password\":\"" + networkSettings.loginPassword + "\",";
    jsonAuth += "\"sdInterval\":" + String(networkSettings.sdSaveInterval);
    jsonAuth += "}";
    request->send(200, "application/json", jsonAuth); });

  server.on("/getTime", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              DynamicJsonDocument jsonDoc(256);
              jsonDoc["datetime"] = getTimeDateNow();
              String jsonTime;
              serializeJson(jsonDoc, jsonTime);
              request->send(200, "application/json", jsonTime); });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { authenthicateUser(request);
              handleFileRequest(request, "/home.html", "text/html"); });
  server.on("/updateOTA", HTTP_GET, [](AsyncWebServerRequest *request)
            { authenthicateUser(request);
              handleFileRequest(request, "/UpdateOTA.html", "text/html"); });
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request)
            { handleFormSubmit(request); });
  server.on("/network", HTTP_GET, [](AsyncWebServerRequest *request)
            { authenthicateUser(request);
              handleFileRequest(request, "/network.html", "text/html"); });

  server.on("/networkLoad", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (request->hasArg("restart")){
      request->send(200, "text/plain", "OKKK");
      ESP_LOGI("ESP", "ESP will restart");
      delay(2000);
      ESP.restart();
    }
    doc = DynamicJsonDocument(4096);
    doc["networkMode"] = networkSettings.networkMode;
    doc["ssid"] = networkSettings.ssid;
    doc["password"] = networkSettings.password;
    doc["apSsid"] = networkSettings.apSsid;
    doc["apPassword"] = networkSettings.apPassword;
    doc["dhcpMode"] = networkSettings.dhcpMode;
    doc["ipAddress"] = networkSettings.ipAddress;
    doc["subnet"] = networkSettings.subnetMask;
    doc["ipGateway"] = networkSettings.ipGateway;
    doc["ipDNS"] = networkSettings.ipDNS;
    doc["sendInterval"] = String(networkSettings.sendInterval,2);
    doc["protocolMode"] = networkSettings.protocolMode;
    doc["endpoint"] = networkSettings.endpoint;
    doc["port"] = networkSettings.port;
    doc["pubTopic"] = networkSettings.pubTopic;
    doc["subTopic"] = networkSettings.subTopic;
    doc["mqttUsername"] = networkSettings.mqttUsername;
    doc["mqttPass"] = networkSettings.mqttPassword;
    doc["loggerMode"] = networkSettings.loggerMode;
    doc["protocolMode2"] = networkSettings.protocolMode2;
    doc["modbusMode"] = modbusParam.mode;
    doc["modbusPort"] = modbusParam.port;
    doc["modbusSlaveID"] = modbusParam.slaveID;
    doc["sendTrig"] = networkSettings.sendTrig;
    doc["erpUrl"] = networkSettings.erpUrl;
    doc["erpUsername"] = networkSettings.erpUsername;
    doc["erpPassword"] = networkSettings.erpPassword;

    String dataLoad;
    serializeJson(doc, dataLoad);
    request->send(200, "application/json", dataLoad); });

  server.on("/network", HTTP_POST, [](AsyncWebServerRequest *request)
            { handleFormSubmit(request); });

  server.on("/analog_input", HTTP_GET, [](AsyncWebServerRequest *request)
            { authenthicateUser(request);
              handleFileRequest(request, "/analog_input.html", "text/html"); });
  server.on("/analog_input", HTTP_POST, [](AsyncWebServerRequest *request)
            { handleFormSubmit(request); });

  server.on("/analogLoad", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              doc = DynamicJsonDocument(4096);
              if (request->hasArg("input"))
              {
                Serial.println(request->arg("input"));
                unsigned char id = request->arg("input").toInt();
                doc["inputType"] = analogInput[id].inputType;
                doc["filter"] = analogInput[id].filter;
                doc["filterPeriod"] = (String(analogInput[id].filterPeriod, 2));
                doc["scaling"] = analogInput[id].scaling;
                doc["lowLimit"] = analogInput[id].lowLimit;
                doc["highLimit"] = analogInput[id].highLimit;
                doc["name"] = analogInput[id].name;
                doc["calibration"] = analogInput[id].calibration;
                doc["mValue"] = analogInput[id].mValue;
                doc["cValue"] = analogInput[id].cValue;
              }
              String dataLoad;
              serializeJson(doc, dataLoad);
              request->send(200, "application/json", dataLoad); });

  server.on("/digital_IO", HTTP_GET, [](AsyncWebServerRequest *request)
            { authenthicateUser(request);
              handleFileRequest(request, "/digital_IO.html", "text/html"); });
  server.on("/digital_IO", HTTP_POST, [](AsyncWebServerRequest *request)
            { handleFormSubmit(request); });

  server.on("/digitalLoad", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    doc = DynamicJsonDocument(4096);
    if(request->hasArg("input")){
      Serial.println(request->arg("input"));
      unsigned char id = request->arg("input").toInt();
      doc["nameDI"] = digitalInput[id].name;
      doc["invDI"] = digitalInput[id].inv;
      doc["taskMode"] = digitalInput[id].taskMode;
      doc["inputState"] = digitalInput[id].inputState ? "High" : "Low";
      doc["intervalTime"] = (float)digitalInput[id].intervalTime/1000;
      doc["conversionFactor"] = digitalInput[id].conversionFactor;
    }
    if(request->hasArg("reset")){
      unsigned char id = request->arg("reset").toInt();
      digitalInput[id].value = 0;
    }
    if(request->hasArg("output")){
      Serial.println(request->arg("output"));
      unsigned char id = request->arg("output").toInt();
      doc["nameDO"] = digitalOutput[id].name;
      doc["invDO"] = digitalOutput[id].inv;
    }
    String dataLoad;
    serializeJson(doc, dataLoad);
    Serial.println(dataLoad);
    request->send(200, "application/json", dataLoad); });

  server.on("/modbus_setup", HTTP_GET, [](AsyncWebServerRequest *request)
            { authenthicateUser(request);
              handleFileRequest(request, "/modbus_setup.html", "text/html"); });

  AsyncCallbackJsonWebHandler *handler = new AsyncCallbackJsonWebHandler("/modbus_setup", [](AsyncWebServerRequest *request, JsonVariant &json)
                                                                         {
    Serial.println("Masuk JSON");
    jsonParam = DynamicJsonDocument(1024);
    if (json.is<JsonArray>())
    {
      jsonParam = json.as<JsonArray>();
    }
    else if (json.is<JsonObject>())
    {
      jsonParam = json.as<JsonObject>();
    }
    stringParam = "";
    serializeJson(jsonParam, stringParam);
    jsonSend = DynamicJsonDocument(1024);
    request->send(200, "text/plain", "Succesfull");
    saveToJson("/modbusSetup.json","modbusSetup");
    Serial.println(stringParam); });
  server.addHandler(handler);

  server.on("/modbusLoad", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", stringParam); });

  server.on("/getValue", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", sendString); });

  server.on("/getCurrentValue", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              doc = DynamicJsonDocument(4096);
              for (int i = 0; i < jumlahInputDigital; i++) {
                doc["rawValue" + String(i + 1)] = analogInput[i].adcValue;
                doc["scaledValue" + String(i + 1)] = analogInput[i].mapValue;
              }
              String dataLoad;
              serializeJson(doc, dataLoad);
              request->send(200, "application/json", dataLoad); });

  server.on("/homeLoad", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              doc = DynamicJsonDocument(4096);
              Serial.println(request->arg("input"));
              unsigned char id = request->arg("input").toInt();
              doc["networkMode"] = networkSettings.networkMode;
              doc["ssid"] = networkSettings.ssid;
              doc["ipAddress"] = networkSettings.networkMode == "WiFi" ? WiFi.localIP().toString() : networkSettings.ipAddress;
              doc["macAddress"] = networkSettings.networkMode == "WiFi" ? WiFi.macAddress() : networkSettings.macAddress;
              doc["protocolMode"] = networkSettings.protocolMode;
              doc["endpoint"] = networkSettings.endpoint;
              doc["connStatus"] = networkSettings.connStatus;
              doc["jobNumber"] = jobNum;
              doc["sendInterval"] = networkSettings.sendInterval;
              doc["datetime"] = getTimeDateNow();

              JsonArray enableAI = doc.createNestedArray("enAI");
              for (int i = 1; i <= jumlahInputAnalog; i++) {
                if (analogInput[i].name != "") {
                  enableAI.add(1);
                } else {
                  enableAI.add(0);
                }
              }

              JsonObject AI = doc.createNestedObject("AI");
              JsonArray AIRawValue = AI.createNestedArray("rawValue");
              JsonArray AIScaledValue = AI.createNestedArray("scaledValue");
              for (int i = 1; i <= jumlahInputAnalog; i++) {
                AIRawValue.add(analogInput[i].adcValue);
                AIScaledValue.add(analogInput[i].mapValue);
              }

              JsonObject DI = doc.createNestedObject("DI");
              JsonArray DIValue = DI.createNestedArray("value");
              JsonArray DITaskMode = DI.createNestedArray("taskMode");

              for (int i = 1; i <= jumlahInputDigital; i++) {
                DIValue.add(digitalInput[i].value);
                DITaskMode.add(digitalInput[i].taskMode);
              }

              String dataLoad;
              serializeJson(doc, dataLoad);
              Serial.println("");
              Serial.println("MASUK");
              Serial.println(dataLoad);
              request->send(200, "application/json", dataLoad); });
  // WiFi Status and Diagnostic Endpoint
  server.on("/wifiStatus", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    DynamicJsonDocument statusDoc(512);
    
    statusDoc["mode"] = networkSettings.networkMode;
    statusDoc["ssid"] = networkSettings.ssid;
    statusDoc["connected"] = (WiFi.status() == WL_CONNECTED);
    statusDoc["ip"] = WiFi.localIP().toString();
    statusDoc["rssi"] = WiFi.RSSI();
    statusDoc["channel"] = WiFi.channel();
    statusDoc["macAddress"] = WiFi.macAddress();
    statusDoc["apIP"] = WiFi.softAPIP().toString();
    statusDoc["apClients"] = WiFi.softAPgetStationNum();
    
    int rssi = WiFi.RSSI();
    String quality;
    if (rssi > -50) quality = "Excellent";
    else if (rssi > -60) quality = "Good";
    else if (rssi > -70) quality = "Fair";
    else if (rssi > -80) quality = "Weak";
    else quality = "Very Weak";
    statusDoc["signalQuality"] = quality;
    
    statusDoc["freeHeap"] = ESP.getFreeHeap();
    statusDoc["cpuFreq"] = getCpuFrequencyMhz();
    statusDoc["uptime"] = millis() / 1000;
    statusDoc["connectionStatus"] = networkSettings.connStatus;
    
    String response;
    serializeJson(statusDoc, response);
    request->send(200, "application/json", response); });
  // LETAKKAN TEPAT SETELAH ENDPOINT /wifiStatus (baris 353)
  // SEBELUM bagian "Check connected interface" (baris 372)

  // =========================================================================
  // ENDPOINT BARU: Ethernet Status and Diagnostic
  // =========================================================================
  server.on("/ethernetStatus", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  DynamicJsonDocument statusDoc(512);
  
  statusDoc["mode"] = networkSettings.networkMode;
  
  if (networkSettings.networkMode == "Ethernet")
  {
    // Hardware Status
    auto hwStatus = Ethernet.hardwareStatus();
    switch (hwStatus)
    {
      case EthernetNoHardware:
        statusDoc["hardware"] = "Not Found";
        statusDoc["hardwareStatus"] = false;
        break;
      case EthernetW5100:
        statusDoc["hardware"] = "W5100";
        statusDoc["hardwareStatus"] = true;
        break;
      case EthernetW5200:
        statusDoc["hardware"] = "W5200";
        statusDoc["hardwareStatus"] = true;
        break;
      case EthernetW5500:
        statusDoc["hardware"] = "W5500";
        statusDoc["hardwareStatus"] = true;
        break;
      default:
        statusDoc["hardware"] = "Unknown";
        statusDoc["hardwareStatus"] = false;
        break;
    }
    
    // Link Status
    auto linkStatus = Ethernet.linkStatus();
    switch (linkStatus)
    {
      case Unknown:
        statusDoc["link"] = "Unknown";
        statusDoc["connected"] = false;
        break;
      case LinkON:
        statusDoc["link"] = "Connected";
        statusDoc["connected"] = true;
        break;
      case LinkOFF:
        statusDoc["link"] = "Disconnected";
        statusDoc["connected"] = false;
        break;
    }
    
    // Network Information
    statusDoc["ip"] = Ethernet.localIP().toString();
    statusDoc["subnet"] = Ethernet.subnetMask().toString();
    statusDoc["gateway"] = Ethernet.gatewayIP().toString();
    statusDoc["dns"] = Ethernet.dnsServerIP().toString();
    statusDoc["macAddress"] = networkSettings.macAddress;
    
    // DHCP Mode
    statusDoc["dhcpMode"] = networkSettings.dhcpMode;
    
    // Connection quality indicator
    if (linkStatus == LinkON)
    {
      statusDoc["quality"] = "Excellent";
      statusDoc["qualityPercent"] = 100;
    }
    else if (linkStatus == LinkOFF)
    {
      statusDoc["quality"] = "No Cable";
      statusDoc["qualityPercent"] = 0;
    }
    else
    {
      statusDoc["quality"] = "Unknown";
      statusDoc["qualityPercent"] = 50;
    }
  }
  else
  {
    statusDoc["error"] = "Not in Ethernet mode";
    statusDoc["connected"] = false;
  }
  
  // AP Status (jika AP aktif)
  statusDoc["apIP"] = WiFi.softAPIP().toString();
  statusDoc["apClients"] = WiFi.softAPgetStationNum();
  statusDoc["apSSID"] = networkSettings.apSsid;
  
  // System Information
  statusDoc["freeHeap"] = ESP.getFreeHeap();
  statusDoc["cpuFreq"] = getCpuFrequencyMhz();
  statusDoc["uptime"] = millis() / 1000;
  statusDoc["connectionStatus"] = networkSettings.connStatus;
  
  String response;
  serializeJson(statusDoc, response);
  request->send(200, "application/json", response); });

  // =========================================================================
  // ENDPOINT BARU: Network Interface Information (kombinasi WiFi + Ethernet)
  // =========================================================================
  server.on("/networkInfo", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  DynamicJsonDocument infoDoc(768);
  
  infoDoc["currentMode"] = networkSettings.networkMode;
  infoDoc["timestamp"] = getTimeDateNow();
  
  // Ethernet Information
  JsonObject ethInfo = infoDoc.createNestedObject("ethernet");
  if (networkSettings.networkMode == "Ethernet")
  {
    ethInfo["active"] = true;
    ethInfo["hardware"] = "W5500";
    ethInfo["link"] = (Ethernet.linkStatus() == LinkON) ? "Connected" : "Disconnected";
    ethInfo["ip"] = Ethernet.localIP().toString();
    ethInfo["mac"] = networkSettings.macAddress;
  }
  else
  {
    ethInfo["active"] = false;
  }
  
  // WiFi Information
  JsonObject wifiInfo = infoDoc.createNestedObject("wifi");
  if (networkSettings.networkMode == "WiFi")
  {
    wifiInfo["active"] = true;
    wifiInfo["ssid"] = networkSettings.ssid;
    wifiInfo["connected"] = (WiFi.status() == WL_CONNECTED);
    wifiInfo["ip"] = WiFi.localIP().toString();
    wifiInfo["rssi"] = WiFi.RSSI();
    wifiInfo["mac"] = WiFi.macAddress();
  }
  else
  {
    wifiInfo["active"] = false;
  }
  
  // Access Point Information
  JsonObject apInfo = infoDoc.createNestedObject("accessPoint");
  apInfo["ssid"] = networkSettings.apSsid;
  apInfo["ip"] = WiFi.softAPIP().toString();
  apInfo["clients"] = WiFi.softAPgetStationNum();
  apInfo["active"] = apReady;
  
  // Protocol Information
  JsonObject protocolInfo = infoDoc.createNestedObject("protocol");
  protocolInfo["primary"] = networkSettings.protocolMode;
  protocolInfo["endpoint"] = networkSettings.endpoint;
  protocolInfo["port"] = networkSettings.port;
  protocolInfo["modbus"] = networkSettings.protocolMode2;
  protocolInfo["status"] = networkSettings.connStatus;
  
  String response;
  serializeJson(infoDoc, response);
  request->send(200, "application/json", response); });

  // =========================================================================
  // ENDPOINT BARU: Test Ethernet Connectivity
  // =========================================================================
  server.on("/testEthernet", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  DynamicJsonDocument testDoc(256);
  
  if (networkSettings.networkMode != "Ethernet")
  {
    testDoc["success"] = false;
    testDoc["error"] = "Not in Ethernet mode";
    String response;
    serializeJson(testDoc, response);
    request->send(400, "application/json", response);
    return;
  }
  
  // Test hardware
  testDoc["hardwareDetected"] = (Ethernet.hardwareStatus() != EthernetNoHardware);
  
  // Test link
  testDoc["cableConnected"] = (Ethernet.linkStatus() == LinkON);
  
  // Test IP configuration
  testDoc["ipConfigured"] = (Ethernet.localIP() != IPAddress(0, 0, 0, 0));
  
  // Overall test result
  bool allTestsPassed = testDoc["hardwareDetected"].as<bool>() && 
                        testDoc["cableConnected"].as<bool>() && 
                        testDoc["ipConfigured"].as<bool>();
  
  testDoc["success"] = allTestsPassed;
  testDoc["message"] = allTestsPassed ? "All tests passed" : "Some tests failed";
  
  if (allTestsPassed)
  {
    testDoc["ip"] = Ethernet.localIP().toString();
  }
  
  String response;
  serializeJson(testDoc, response);
  request->send(allTestsPassed ? 200 : 500, "application/json", response); });

  // =========================================================================
  // AKHIR ENDPOINT ETHERNET - LANJUT KE "Check connected interface"
  // =========================================================================
  // Check connected interface
  tcpip_adapter_ip_info_t ipInfo;
  if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo) == ESP_OK)
  {
    ESP_LOGI("Wi-Fi STA", "Wi-Fi STA IP: %s", ip4addr_ntoa(&ipInfo.ip));
  }
  if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ipInfo) == ESP_OK)
  {
    ESP_LOGI("Wi-Fi AP", "Wi-Fi AP IP: %s", ip4addr_ntoa(&ipInfo.ip));
  }
  if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_ETH, &ipInfo) == ESP_OK)
  {
    ESP_LOGI("Ethernet", "Ethernet IP: %s", ip4addr_ntoa(&ipInfo.ip));
  }
  else
  {
    ESP_LOGW("Ethernet", "Ethernet interface not initialized or no IP assigned.");
  }
  // ============================================================
  // KODE BARU: STATIC FILES DENGAN CACHE (Agar Cepat)
  // ============================================================

  // 1. Cache Folder CSS & JS (Disimpan browser selama 1 tahun)
  server.serveStatic("/css", SPIFFS, "/css").setCacheControl("max-age=31536000");
  server.serveStatic("/js", SPIFFS, "/js").setCacheControl("max-age=31536000");

  // 2. Cache File Libraries & Gambar Satuan (Disimpan browser selama 1 tahun)
  server.serveStatic("/bootstrap.min.css", SPIFFS, "/bootstrap.min.css").setCacheControl("max-age=31536000");
  server.serveStatic("/bootstrap.bundle.min.js", SPIFFS, "/bootstrap.bundle.min.js").setCacheControl("max-age=31536000");
  server.serveStatic("/jquery-3.5.1.min.js", SPIFFS, "/jquery-3.5.1.min.js").setCacheControl("max-age=31536000");
  server.serveStatic("/logoMeditech32.png", SPIFFS, "/logoMeditech32.png").setCacheControl("max-age=31536000");
  server.serveStatic("/logoMeditech256.png", SPIFFS, "/logoMeditech256.png").setCacheControl("max-age=31536000");

  // ============================================================
  // Start the server
  // ============================================================
  // KODE BARU: STATIC FILES DENGAN CACHE (Agar Cepat)
  // ============================================================

  // 1. Cache Folder CSS & JS (Disimpan browser selama 1 tahun)
  server.serveStatic("/css", SPIFFS, "/css").setCacheControl("max-age=31536000");
  server.serveStatic("/js", SPIFFS, "/js").setCacheControl("max-age=31536000");

  // 2. Cache File Libraries & Gambar Satuan (Disimpan browser selama 1 tahun)
  server.serveStatic("/bootstrap.min.css", SPIFFS, "/bootstrap.min.css").setCacheControl("max-age=31536000");
  server.serveStatic("/bootstrap.bundle.min.js", SPIFFS, "/bootstrap.bundle.min.js").setCacheControl("max-age=31536000");
  server.serveStatic("/jquery-3.5.1.min.js", SPIFFS, "/jquery-3.5.1.min.js").setCacheControl("max-age=31536000");
  server.serveStatic("/logoMeditech32.png", SPIFFS, "/logoMeditech32.png").setCacheControl("max-age=31536000");
  server.serveStatic("/logoMeditech256.png", SPIFFS, "/logoMeditech256.png").setCacheControl("max-age=31536000");

  // 3. Halaman HTML Utama (JANGAN di-cache, agar data selalu update)
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("home.html").setCacheControl("no-cache");

  // ============================================================
  server.begin();
  if (networkSettings.networkMode == "Ethernet")
  {
    ethServer.begin();
    Serial.println("✅ Ethernet Web Server STARTED on Port 80");
  }
  ESP_LOGI("WebServer", "Web server started successfully");

  // [1] Start DNS Server untuk captive portal
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  dnsStarted = true;
  Serial.println("DNS Server started, redirecting all domains to " + WiFi.softAPIP().toString());
}

// ============================================================================
// SETUP INTERRUPTS
// ============================================================================
void setupInterrupts()
{
  for (byte i = 1; i <= jumlahInputDigital; i++)
    attachDigitalInputInterrupt(i);
  configureSendTriggerInterrupt(networkSettings);
}

// ============================================================================
// MODBUS SLAVE SETUP
// ============================================================================
void modbusSlaveSetup()
{
  bool useTCP = (networkSettings.protocolMode2 == "Modbus TCP/IP" || networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");
  bool useRTU = (networkSettings.protocolMode2 == "Modbus RTU" || networkSettings.protocolMode2 == "Modbus RTU + TCP/IP");

  if (useTCP)
    mbIP.server(502);

  if (useRTU)
    mbRTU.begin(&SerialModbus);

  for (byte i = 0; i < 100; i++)
  {
    if (useTCP)
      mbIP.addIreg(i);
    if (useRTU)
      mbRTU.addIreg(i);
  }
}

// ============================================================================
// SUPPORT FUNCTIONS
// ============================================================================
float filterSensor(float filterVar, float filterResult_1, float fc)
{
  float Ts, RC, a, filterResult;
  Ts = 0.01;
  RC = 1 / (2 * 3.14 * fc);
  a = RC / 0.01;
  filterResult = (filterVar + a * filterResult_1) / (a + 1);
  return filterResult;
}

// unsigned int readModbus(unsigned int modbusAddress, unsigned int funCode, unsigned int regAddress)
// {
//   unsigned int buffSend[8], crcValue, returnValue;
//   int resIndex = 0;
//   byte buffModbus[20];

//   buffSend[0] = modbusAddress;
//   buffSend[1] = funCode;
//   buffSend[2] = parseByte(regAddress, 1);
//   buffSend[3] = parseByte(regAddress, 0);
//   buffSend[4] = 0;
//   buffSend[5] = 1;
//   crcValue = crcModbus(buffSend, 0, 6);
//   buffSend[6] = parseByte(crcValue, 1);
//   buffSend[7] = parseByte(crcValue, 0);

//   for (byte j = 0; j < 8; j++)
//   {
//     SerialModbus.write(buffSend[j]);
//     delayMicroseconds(100);
//   }

//   while (!(SerialModbus.available()))
//   {
//     for (int timeout = 0; timeout < 500; timeout++)
//     {
//       delay(1);
//     }
//     break;
//   }

//   while (SerialModbus.available())
//   {
//     buffModbus[resIndex] = SerialModbus.read();
//     resIndex++;
//   }

//   if (funCode == 1 or funCode == 2)
//   {
//     if (buffModbus[2] == 1)
//     {
//       returnValue = buffModbus[3];
//     }
//     else if (buffModbus[2] == 2)
//     {
//       returnValue = buffModbus[3] << 8 | buffModbus[4];
//     }
//   }
//   else if (funCode == 3)
//   {
//     returnValue = (buffModbus[3] << 8) | buffModbus[4];
//   }
//   else if (funCode == 4)
//   {
//     returnValue = (buffModbus[3] << 8) | buffModbus[4];
//   }
//   else if (funCode == 6)
//   {
//     if (buffModbus[4] == buffSend[4] and buffModbus[5] == buffSend[5])
//     {
//       returnValue = 1;
//     }
//     else
//     {
//       returnValue = 0;
//     }
//   }

//   return returnValue;
// }

// unsigned int readModbus(unsigned int modbusAddress, unsigned int funCode, unsigned int regAddress)
// {
//   unsigned int buffSend[8];
//   unsigned int crcValue;
//   // [FIX 1] Inisialisasi returnValue ke 0. Jika gagal baca, hasilnya 0 (bukan sampah 10/100)
//   unsigned int returnValue = 0;

//   byte buffModbus[32]; // Perbesar buffer sedikit
//   int resIndex = 0;

//   // Bersihkan buffer sebelum dipakai
//   memset(buffModbus, 0, sizeof(buffModbus));

//   // 1. Susun Frame Request
//   buffSend[0] = modbusAddress;
//   buffSend[1] = funCode;
//   buffSend[2] = parseByte(regAddress, 1); // High Byte Address
//   buffSend[3] = parseByte(regAddress, 0); // Low Byte Address
//   buffSend[4] = 0;
//   buffSend[5] = 1; // Jumlah data yg diminta (1 Register)

//   crcValue = crcModbus(buffSend, 0, 6);
//   buffSend[6] = parseByte(crcValue, 1);
//   buffSend[7] = parseByte(crcValue, 0);

//   // 2. Bersihkan RX Buffer sebelum kirim request baru
//   while(SerialModbus.available()) SerialModbus.read();

//   // 3. Kirim Request ke RS485
//   for (byte j = 0; j < 8; j++)
//   {
//     SerialModbus.write(buffSend[j]);
//     // delayMicroseconds(100); // Opsional, tergantung hardware
//   }
//   SerialModbus.flush(); // Pastikan data terkirim semua

//   // 4. Tunggu Respon (Timeout Logic yang Benar)
//   unsigned long startTime = millis();
//   bool dataReceived = false;

//   // Tunggu sampai ada data pertama masuk (Timeout 1 detik)
//   while ((millis() - startTime) < 1000)
//   {
//     if (SerialModbus.available()) {
//       dataReceived = true;
//       break;
//     }
//     delay(10);
//   }

//   // 5. Jika Timeout (Tidak ada balasan dari TK4S)
//   if (!dataReceived) {
//     // Return 0 agar ketahuan kalau error/putus (JANGAN return sampah)
//     return 0;
//   }

//   // 6. Baca Data Balasan
//   // Beri jeda sedikit untuk memastikan seluruh byte sampai
//   delay(20);

//   while (SerialModbus.available() && resIndex < 30)
//   {
//     buffModbus[resIndex] = SerialModbus.read();
//     resIndex++;
//   }

//   // 7. Parsing Data (Validasi Function Code)
//   // Respon Input Register (FC 04) atau Holding Register (FC 03)
//   // Format: [Addr] [FC] [ByteCount] [DataHigh] [DataLow] [CRC] [CRC]
//   // Index:    0      1       2           3         4

//   if (funCode == 3 || funCode == 4)
//   {
//     // Validasi dasar: Address & Function code harus sesuai request
//     if (buffModbus[0] == modbusAddress && buffModbus[1] == funCode && buffModbus[2] == 2)
//     {
//        returnValue = (buffModbus[3] << 8) | buffModbus[4];
//     }
//   }
//   // Respon Coil/Status (FC 01 / 02)
//   else if (funCode == 1 || funCode == 2)
//   {
//     // Cek Byte Count (biasanya 1 byte data)
//     if (buffModbus[2] == 1)
//     {
//       returnValue = buffModbus[3];
//     }
//   }

//   return returnValue;
// }

// ============================================================================
// FUNGSI BACA MODBUS (DENGAN SERIAL PRINT DEBUG LENGKAP)
// ============================================================================
unsigned int readModbus(unsigned int modbusAddress, unsigned int funCode, unsigned int regAddress)
{
  unsigned int buffSend[8];
  unsigned int crcValue;
  unsigned int returnValue = 0;
  byte buffModbus[64];
  int resIndex = 0;

  // 1. Bersihkan Buffer
  while (SerialModbus.available()) SerialModbus.read();

  // 2. Siapkan Request
  buffSend[0] = modbusAddress;
  buffSend[1] = funCode;
  buffSend[2] = parseByte(regAddress, 1);
  buffSend[3] = parseByte(regAddress, 0);
  buffSend[4] = 0;
  buffSend[5] = 1;
  crcValue = crcModbus(buffSend, 0, 6);
  buffSend[6] = parseByte(crcValue, 1);
  buffSend[7] = parseByte(crcValue, 0);

  // Debug Request
  // Serial.print("[TX]: ");
  for (byte j = 0; j < 8; j++) {
    SerialModbus.write(buffSend[j]);
    // Serial.printf("%02X ", buffSend[j]);
  }
  // Serial.println();
  SerialModbus.flush();

  // 3. Tunggu Respon
  unsigned long startTime = millis();
  while (millis() - startTime < 1000) {
    if (SerialModbus.available()) break;
    delay(10);
  }

  if (!SerialModbus.available()) {
    Serial.println("❌ [Modbus] Timeout! Tidak ada balasan dari TK4S.");
    return 0; 
  }

  delay(50); // Tunggu byte lengkap

  // 4. Baca Balasan
  Serial.print("✅ [Modbus RX Raw]: "); // <--- DEBUG POINT 1
  while (SerialModbus.available() && resIndex < 60) {
    buffModbus[resIndex] = SerialModbus.read();
    Serial.printf("%02X ", buffModbus[resIndex]); // Print HEX (Contoh: 01 04 02 00 F7 ...)
    resIndex++;
  }
  Serial.println();

  // 5. Validasi
  if (resIndex >= 3) {
    if (buffModbus[0] == modbusAddress && buffModbus[1] == funCode) {
      if (funCode == 3 || funCode == 4) {
        returnValue = (buffModbus[3] << 8) | buffModbus[4];
        Serial.printf("   -> Data Decimal: %d\n", returnValue); // <--- DEBUG POINT 2
      }
    } else {
      Serial.println("   -> ⚠️ Salah Alamat/Function Code!");
    }
  } else {
    Serial.println("   -> ⚠️ Paket data tidak lengkap/rusak.");
  }
  
  return returnValue;
}

unsigned int crcModbus(unsigned int crc[], byte start, byte sizeArray)
{
  unsigned int crcReg = 0xffff;
  for (byte j = start; j < sizeArray; j++)
  {
    crcReg ^= crc[j];
    for (byte i = 0; i < 8; i++)
    {
      if (crcReg & 1)
      {
        crcReg >>= 1;
        crcReg ^= 0xa001;
        continue;
      }
      crcReg >>= 1;
    }
  }
  unsigned int temp = 0;
  temp = crcReg & 0xff;
  crcReg = ((crcReg & 0xff00) >> 8) | (temp << 8);
  return crcReg;
}

unsigned int parseByte(unsigned int bytes, bool byteOrder)
{
  unsigned int parseResult;
  if (byteOrder == 0)
    parseResult = bytes & 0xff;
  else
    parseResult = (bytes & 0xff00) >> 8;
  return parseResult;
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  float mappedValue = (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
  return roundf(mappedValue * 100.0) / 100.0;
}

String getTimeDateNow()
{
  DateTime now;
  if (rtc.begin())
    now = rtc.now();
  else
  {
    struct tm timeInfo;
    if (!getLocalTime(&timeInfo))
    {
      ESP_LOGW("NTP", "Failed to obtain time from NTP server");
      now = DateTime(0, 0, 0, 0, 0, 0);
    }
    else
    {
      now = DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    }
  }

  char timeBuffer[20];
  sprintf(timeBuffer, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  return String(timeBuffer);
}

String getTimeNow()
{
  String dateTime = getTimeDateNow();
  int spaceIndex = dateTime.indexOf(' ');
  if (spaceIndex != -1)
  {
    return dateTime.substring(spaceIndex + 1);
  }
  return dateTime;
}

void authenthicateUser(AsyncWebServerRequest *request)
{
  if (!request->authenticate(networkSettings.loginUsername.c_str(), networkSettings.loginPassword.c_str()))
    return request->requestAuthentication();
}

// ============================================================================
// FILE & CONFIG HANDLING FUNCTIONS
// ============================================================================

void readConfig()
{
  ESP_LOGI("SPIFFS", "Mounting file system");

  if (SPIFFS.begin())
  {
    ESP_LOGI("SPIFFS", "File system mounted");

    // Read Network Config
    if (SPIFFS.exists("/configNetwork.json"))
    {
      doc = DynamicJsonDocument(4096);
      ESP_LOGI("SPIFFS", "Reading network config file...");
      File configFile = SPIFFS.open("/configNetwork.json");

      if (configFile)
      {
        ESP_LOGI("SPIFFS", "Opened network config file");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        auto error = deserializeJson(doc, buf.get());

        if (error)
        {
          ESP_LOGE("SPIFFS NETWORK", "Failed to parse JSON file");
          return;
        }

        const char *temp;
        temp = doc["networkMode"];
        networkSettings.networkMode = String(temp);
        temp = doc["ssid"];
        networkSettings.ssid = String(temp);
        temp = doc["password"];
        networkSettings.password = String(temp);
        temp = doc["apSsid"];
        networkSettings.apSsid = String(temp);
        temp = doc["apPassword"];
        networkSettings.apPassword = String(temp);
        temp = doc["dhcpMode"];
        networkSettings.dhcpMode = String(temp);
        temp = doc["ipAddress"];
        networkSettings.ipAddress = String(temp);
        temp = doc["subnet"];
        networkSettings.subnetMask = String(temp);
        temp = doc["ipGateway"];
        networkSettings.ipGateway = String(temp);
        temp = doc["ipDNS"];
        networkSettings.ipDNS = String(temp);
        temp = doc["protocolMode"];
        networkSettings.protocolMode = String(temp);
        temp = doc["endpoint"];
        networkSettings.endpoint = String(temp);
        temp = doc["pubTopic"];
        networkSettings.pubTopic = String(temp);
        temp = doc["subTopic"];
        networkSettings.subTopic = String(temp);
        temp = doc["mqttUsername"];
        networkSettings.mqttUsername = String(temp);
        temp = doc["mqttPass"];
        networkSettings.mqttPassword = String(temp);
        networkSettings.sendInterval = doc["sendInterval"];
        temp = doc["sendTrig"];
        networkSettings.sendTrig = String(temp);
        networkSettings.port = doc["port"];
        networkSettings.loggerMode = doc["loggerMode"];
        temp = doc["protocolMode2"];
        networkSettings.protocolMode2 = String(temp);
        modbusParam.mode = doc["modbusMode"];
        modbusParam.port = doc["modbusPort"];
        modbusParam.slaveID = doc["modbusSlaveID"];
        temp = doc["erpUrl"];
        networkSettings.erpUrl = String(temp);
        temp = doc["erpUsername"];
        networkSettings.erpUsername = String(temp);
        temp = doc["erpPassword"];
        networkSettings.erpPassword = String(temp);

        configNetwork();
        configProtocol();
      }
    }

    // Read Digital Config
    if (SPIFFS.exists("/configDigital.json"))
    {
      doc = DynamicJsonDocument(4096);
      ESP_LOGI("SPIFFS", "Reading digital input config file...");
      File configFile = SPIFFS.open("/configDigital.json");

      if (configFile)
      {
        ESP_LOGI("SPIFFS", "Opened digital input config file");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        auto error = deserializeJson(doc, buf.get());

        if (error)
        {
          ESP_LOGE("SPIFFS DIGITAL", "Failed to parse JSON file");
          return;
        }

        const char *temp;
        for (unsigned char i = 1; i < jumlahInputDigital + 1; i++)
        {
          temp = doc["DI" + String(i)]["name"];
          digitalInput[i].name = String(temp);
          digitalInput[i].inv = doc["DI" + String(i)]["invers"];
          temp = doc["DI" + String(i)]["taskMode"];
          digitalInput[i].taskMode = String(temp);
          temp = doc["DI" + String(i)]["inputState"];
          digitalInput[i].inputState = String(temp) == "High" ? 1 : 0;
          digitalInput[i].intervalTime = doc["DI" + String(i)]["intervalTime"];
          digitalInput[i].conversionFactor = doc["DI" + String(i)]["conversionFactor"];
        }
      }
    }

    // Read Analog Config
    if (SPIFFS.exists("/configAnalog.json"))
    {
      doc = DynamicJsonDocument(4096);
      Serial.println("Reading analog input config file...");
      File configFile = SPIFFS.open("/configAnalog.json");

      if (configFile)
      {
        ESP_LOGI("SPIFFS", "Opened analog input config file");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        auto error = deserializeJson(doc, buf.get());

        if (error)
        {
          ESP_LOGE("SPIFFS ANALOG", "Failed to parse JSON file");
          return;
        }

        const char *temp;
        for (unsigned char i = 1; i < jumlahInputAnalog + 1; i++)
        {
          temp = doc["AI" + String(i)]["name"];
          analogInput[i].name = String(temp);
          temp = doc["AI" + String(i)]["inputType"];
          analogInput[i].inputType = String(temp);
          analogInput[i].filter = doc["AI" + String(i)]["filter"];
          analogInput[i].filterPeriod = doc["AI" + String(i)]["filterPeriod"];
          analogInput[i].scaling = doc["AI" + String(i)]["scaling"];
          analogInput[i].lowLimit = doc["AI" + String(i)]["lowLimit"];
          analogInput[i].highLimit = doc["AI" + String(i)]["highLimit"];
          analogInput[i].calibration = doc["AI" + String(i)]["calibration"];
          analogInput[i].mValue = doc["AI" + String(i)]["mValue"];
          analogInput[i].cValue = doc["AI" + String(i)]["cValue"];
        }
      }
    }

    // Read Modbus Config
    // Read Modbus Config
    if (SPIFFS.exists("/modbusSetup.json"))
    {
      ESP_LOGI("SPIFFS", "Reading Modbus config file...");
      File configFile = SPIFFS.open("/modbusSetup.json");

      if (configFile)
      {
        ESP_LOGI("SPIFFS", "Opened Modbus config file");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        
        // Parsing data ke object global jsonParam
        auto error = deserializeJson(jsonParam, buf.get());

        if (error)
        {
          ESP_LOGE("SPIFFS MODBUS", "Failed to parse JSON file");
          return;
        }

        const char *temp;
        temp = jsonParam["parity"];
        modbusParam.parity = String(temp);
        modbusParam.baudrate = jsonParam["baudrate"];
        modbusParam.stopBit = jsonParam["stopBit"];
        modbusParam.dataBit = jsonParam["dataBit"];
        modbusParam.scanRate = jsonParam["scanRate"];

        // Configure Serial Modbus sesuai parameter
        if (modbusParam.dataBit == 8 and modbusParam.stopBit == 1 and modbusParam.parity == "None")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_8N1, 17, 16);
        else if (modbusParam.dataBit == 8 and modbusParam.stopBit == 2 and modbusParam.parity == "None")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_8N2, 17, 16);
        else if (modbusParam.dataBit == 8 and modbusParam.stopBit == 1 and modbusParam.parity == "Odd")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_8O1, 17, 16);
        else if (modbusParam.dataBit == 8 and modbusParam.stopBit == 2 and modbusParam.parity == "Odd")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_8O2, 17, 16);
        else if (modbusParam.dataBit == 8 and modbusParam.stopBit == 1 and modbusParam.parity == "Even")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_8E1, 17, 16);
        else if (modbusParam.dataBit == 8 and modbusParam.stopBit == 2 and modbusParam.parity == "Even")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_8E2, 17, 16);
        else if (modbusParam.dataBit == 7 and modbusParam.stopBit == 1 and modbusParam.parity == "None")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_7N1, 17, 16);
        else if (modbusParam.dataBit == 7 and modbusParam.stopBit == 2 and modbusParam.parity == "None")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_7N2, 17, 16);
        else if (modbusParam.dataBit == 7 and modbusParam.stopBit == 1 and modbusParam.parity == "Odd")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_7O1, 17, 16);
        else if (modbusParam.dataBit == 7 and modbusParam.stopBit == 2 and modbusParam.parity == "Odd")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_7O2, 17, 16);
        else if (modbusParam.dataBit == 7 and modbusParam.stopBit == 1 and modbusParam.parity == "Even")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_7E1, 17, 16);
        else if (modbusParam.dataBit == 7 and modbusParam.stopBit == 2 and modbusParam.parity == "Even")
          SerialModbus.begin(modbusParam.baudrate, SERIAL_7E2, 17, 16);

        // Update jumlah parameter untuk looping task
        JsonArray nameData = jsonParam["nameData"];
        numOfParam = nameData.size();

        // ==========================================================
        // PENTING: Update stringParam agar Logger bisa membacanya
        // ==========================================================
        stringParam = "";
        serializeJson(jsonParam, stringParam);
        
        // Debugging (Opsional)
        // Serial.println("Modbus Config Loaded: " + stringParam);
      }
    }

    // Read Runtime Data
    if (SPIFFS.exists("/runtimeData.json"))
    {
      File runtimeFile = SPIFFS.open("/runtimeData.json");
      doc = DynamicJsonDocument(4096);

      if (runtimeFile)
      {
        size_t size = runtimeFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        runtimeFile.readBytes(buf.get(), size);
        auto error = deserializeJson(doc, buf.get());

        if (!error)
        {
          for (int i = 1; i < jumlahInputDigital + 1; i++)
          {
            if (digitalInput[i].taskMode == "Run Time")
            {
              digitalInput[i].value = doc[String(i)];
            }
          }
        }
      }
    }

    // Read System Settings
    if (SPIFFS.exists("/systemSettings.json"))
    {
      File runtimeFile = SPIFFS.open("/systemSettings.json");
      doc = DynamicJsonDocument(4096);

      if (runtimeFile)
      {
        size_t size = runtimeFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        runtimeFile.readBytes(buf.get(), size);
        auto error = deserializeJson(doc, buf.get());

        if (!error)
        {
          const char *temp;
          temp = doc["username"];
          networkSettings.loginUsername = temp;
          temp = doc["password"];
          networkSettings.loginPassword = temp;

          if (doc.containsKey("sdInterval"))
          {
            networkSettings.sdSaveInterval = doc["sdInterval"];
          }
        }
      }
    }
  }
  else
  {
    ESP_LOGE("SPIFFS", "Failed to mount FS");
    errorMessages.addMessage(getTimeNow() + " - Failed to mount FS");
  }
}

void updateJson(const char *dir, const char *jsonKey, const char *jsonValue)
{
  if (SPIFFS.begin())
  {
    File file = SPIFFS.open(dir);
    if (file)
    {
      size_t size = file.size();
      std::unique_ptr<char[]> buf(new char[size]);
      file.readBytes(buf.get(), size);
      file.close();
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, buf.get());
      if (!error)
      {
        doc[jsonKey] = jsonValue;
        File updatedFile = SPIFFS.open(dir, "w");
        if (updatedFile)
        {
          serializeJson(doc, updatedFile);
          updatedFile.close();
        }
      }
    }
  }
}

void updateJson(const char *dir, const char *jsonKey, int jsonValue)
{
  if (SPIFFS.begin())
  {
    File file = SPIFFS.open(dir);
    if (file)
    {
      size_t size = file.size();
      std::unique_ptr<char[]> buf(new char[size]);
      file.readBytes(buf.get(), size);
      file.close();
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, buf.get());
      if (!error)
      {
        doc[jsonKey] = jsonValue;
        File updatedFile = SPIFFS.open(dir, "w");
        if (updatedFile)
        {
          serializeJson(doc, updatedFile);
          updatedFile.close();
        }
      }
    }
  }
}

void handleFileRequest(AsyncWebServerRequest *request, const char *filePath, const char *mimeType)
{
  if (SPIFFS.exists(filePath))
  {
    File file = SPIFFS.open(filePath, "r");
    request->send(SPIFFS, filePath, mimeType);
    file.close();
  }
  else
  {
    request->send(404, "text/plain", "File Not Found");
  }
}

void handleFormSubmit(AsyncWebServerRequest *request)
{
  Serial.println("Masuk Submit Form");

  // ========================================================================
  // 1. SAVE NETWORK CONFIGURATION (Mode, IP, MQTT, dll)
  // ========================================================================
  if (request->hasArg("networkMode") && request->hasArg("dhcpMode"))
  {
    networkSettings.networkMode = request->arg("networkMode");
    networkSettings.dhcpMode = request->arg("dhcpMode");
    networkSettings.apSsid = request->arg("apSsid");
    networkSettings.apPassword = request->arg("apPassword");
    networkSettings.sendTrig = request->arg("sendTrig");
    networkSettings.sendInterval = request->arg("sendInterval").toFloat();

    if (request->hasArg("ssid"))
    {
      networkSettings.ssid = request->arg("ssid");
      networkSettings.password = request->arg("password");
    }
    if (request->hasArg("ipAddress"))
    {
      networkSettings.ipAddress = request->arg("ipAddress");
      networkSettings.subnetMask = request->arg("subnet");
      networkSettings.ipGateway = request->arg("ipGateway");
      networkSettings.ipDNS = request->arg("ipDNS");
    }

    networkSettings.protocolMode = request->arg("protocolMode");
    networkSettings.endpoint = request->arg("endpoint");
    networkSettings.port = request->arg("port").toInt();

    if (request->hasArg("pubTopic"))
    {
      networkSettings.pubTopic = request->arg("pubTopic");
      networkSettings.subTopic = request->arg("subTopic");
    }
    if (request->hasArg("mqttUsername"))
      networkSettings.mqttUsername = request->arg("mqttUsername");
    if (request->hasArg("mqttPassword"))
      networkSettings.mqttPassword = request->arg("mqttPass");
    if (request->hasArg("loggerMode"))
      networkSettings.loggerMode = request->arg("loggerMode");
    if (request->hasArg("modbusMode"))
    {
      networkSettings.protocolMode2 = request->arg("protocolMode2");
      if (request->hasArg("modbusPort"))
        modbusParam.port = request->arg("modbusPort").toInt();
      if (request->hasArg("slaveID"))
        modbusParam.slaveID = request->arg("slaveID").toInt();
    }

    configureSendTriggerInterrupt(networkSettings);

    request->send(200, "text/plain", "Form data received");
    
    // Simpan ke Internal & SD Card
    saveToJson("/configNetwork.json", "network");
    saveToSDConfig("/configNetwork.json", "network"); // <--- TAMBAHAN
  }

  // ========================================================================
  // 2. SAVE ERP CONFIGURATION (URL, User, Pass)
  // ========================================================================
  else if (request->hasArg("erpUrl"))
  {
    networkSettings.erpUrl = request->arg("erpUrl");
    networkSettings.erpUsername = request->arg("erpUsername");
    networkSettings.erpPassword = request->arg("erpPassword");

    request->send(200, "text/plain", "Form data received");
    
    // Simpan ke Internal & SD Card
    saveToJson("/configNetwork.json", "network");
    saveToSDConfig("/configNetwork.json", "network"); // <--- TAMBAHAN
  }

  // ========================================================================
  // 3. SAVE DIGITAL INPUT CONFIGURATION
  // ========================================================================
  else if (request->hasArg("nameDI"))
  {
    for (unsigned char i = 1; i < jumlahInputDigital + 1; i++)
    {
      if (request->arg("inputPin") == ("DI" + String(i)))
      {
        digitalInput[i].name = request->arg("nameDI");
        if (digitalInput[i].taskMode != request->arg("taskMode"))
          digitalInput[i].value = 0;
        digitalInput[i].taskMode = request->arg("taskMode");

        attachDigitalInputInterrupt(i);

        if (digitalInput[i].taskMode == "Run Time")
        {
          if (SPIFFS.begin())
          {
            File file = SPIFFS.open("/runtimeData.json");
            if (file)
            {
              size_t size = file.size();
              std::unique_ptr<char[]> buf(new char[size]);
              file.readBytes(buf.get(), size);
              file.close();
              DynamicJsonDocument runtime(128);
              DeserializationError error = deserializeJson(runtime, buf.get());
              if (!error)
              {
                digitalInput[i].value = runtime[String(i)];
              }
            }
          }
        }

        if (request->arg("inputState") == "High")
          digitalInput[i].inputState = 1;
        else
          digitalInput[i].inputState = 0;

        if (request->hasArg("inputInversion"))
          digitalInput[i].inv = 1;
        else
          digitalInput[i].inv = 0;

        if (request->hasArg("intervalTime"))
          digitalInput[i].intervalTime = static_cast<long>(request->arg("intervalTime").toFloat() * 1000);
        if (request->hasArg("conversionFactor"))
          digitalInput[i].conversionFactor = request->arg("conversionFactor").toFloat();
      }
    }

    configureSendTriggerInterrupt(networkSettings);

    request->send(200, "text/plain", "Form data received");
    
    // Simpan ke Internal & SD Card
    saveToJson("/configDigital.json", "digital");
    saveToSDConfig("/configDigital.json", "digital"); // <--- TAMBAHAN
  }

  // ========================================================================
  // 4. SAVE ANALOG INPUT CONFIGURATION
  // ========================================================================
  else if (request->hasArg("inputType"))
  {
    for (unsigned char i = 1; i < jumlahInputAnalog + 1; i++)
    {
      if (request->arg("inputPin") == ("AI" + String(i)))
      {
        analogInput[i].name = request->arg("name");
        analogInput[i].filter = request->hasArg("filter") ? 1 : 0;
        analogInput[i].scaling = request->hasArg("scaling") ? 1 : 0;
        analogInput[i].calibration = request->arg("calibration") ? 1 : 0;
        analogInput[i].mValue = request->arg("mValue").toFloat();
        analogInput[i].cValue = request->arg("cValue").toFloat();
        analogInput[i].filterPeriod = request->arg("filterPeriod").toFloat();
        analogInput[i].lowLimit = request->arg("lowLimit").toFloat();
        analogInput[i].highLimit = request->arg("highLimit").toFloat();
        analogInput[i].inputType = request->arg("inputType");
      }
    }
    request->send(200, "text/plain", "Form data received");
    
    // Simpan ke Internal & SD Card
    saveToJson("/configAnalog.json", "analog");
    saveToSDConfig("/configAnalog.json", "analog"); // <--- TAMBAHAN
  }

  // ========================================================================
  // 5. SAVE MODBUS SETUP (Baudrate, Parity, dll)
  // ========================================================================
  else if (request->hasArg("baudrate"))
  {
    modbusParam.scanRate = request->arg("scanRate").toFloat();
    modbusParam.stopBit = request->arg("stopBit").toInt();
    modbusParam.dataBit = request->arg("dataBit").toInt();
    modbusParam.baudrate = request->arg("baudrate").toInt();
    modbusParam.parity = request->arg("parity");

    request->send(200, "text/plain", "Form data received");

    // Update JSON global 'jsonParam' agar sinkron
    jsonParam["baudrate"] = modbusParam.baudrate;
    jsonParam["parity"] = modbusParam.parity;
    jsonParam["stopBit"] = modbusParam.stopBit;
    jsonParam["dataBit"] = modbusParam.dataBit;
    jsonParam["scanRate"] = modbusParam.scanRate;
    
    // Simpan ke Internal & SD Card
    saveToJson("/modbusSetup.json", "modbusSetup");
    saveToSDConfig("/modbusSetup.json", "modbusSetup"); // <--- TAMBAHAN
  }

  // ========================================================================
  // 6. SAVE SYSTEM SETTINGS (Username, Pass, Date)
  // ========================================================================
  else if (request->hasArg("username"))
  {
    if (request->arg("datetime") != "")
    {
      String dateTimeUpdate = request->arg("datetime");
      unsigned int updatedYear = dateTimeUpdate.substring(0, 4).toInt();
      unsigned int updatedMonth = dateTimeUpdate.substring(5, 7).toInt();
      unsigned int updatedDay = dateTimeUpdate.substring(8, 10).toInt();
      unsigned int updatedHour = dateTimeUpdate.substring(11, 13).toInt();
      unsigned int updatedMinute = dateTimeUpdate.substring(14).toInt();
      rtc.adjust(DateTime(updatedYear, updatedMonth, updatedDay, updatedHour, updatedMinute, 0));
    }

    networkSettings.loginUsername = request->arg("username");
    networkSettings.loginPassword = request->arg("password");

    if (request->hasArg("sdInterval"))
    {
      networkSettings.sdSaveInterval = request->arg("sdInterval").toInt();
    }

    request->send(200, "text/plain", "Form data received");
    
    // Simpan ke Internal & SD Card
    saveToJson("/systemSettings.json", "systemSettings");
    saveToSDConfig("/systemSettings.json", "systemSettings"); // <--- TAMBAHAN
  }
  else
  {
    request->send(400, "text/plain", "Bad Request");
  }
}

void saveToJson(const char *dir, const char *configType)
{
  if (!SPIFFS.begin(true))
  {
    Serial.println("Failed to mount file system");
    errorMessages.addMessage(getTimeNow() + " - Failed to mount file system");
    return;
  }

  doc = DynamicJsonDocument(1024);

  if (configType == "network")
  {
    doc["networkMode"] = networkSettings.networkMode;
    doc["ssid"] = networkSettings.ssid;
    doc["password"] = networkSettings.password;
    doc["apSsid"] = networkSettings.apSsid;
    doc["apPassword"] = networkSettings.apPassword;
    doc["dhcpMode"] = networkSettings.dhcpMode;
    doc["ipAddress"] = networkSettings.ipAddress;
    doc["subnet"] = networkSettings.subnetMask;
    doc["ipGateway"] = networkSettings.ipGateway;
    doc["ipDNS"] = networkSettings.ipDNS;
    doc["sendInterval"] = networkSettings.sendInterval;
    doc["protocolMode"] = networkSettings.protocolMode;
    doc["endpoint"] = networkSettings.endpoint;
    doc["port"] = networkSettings.port;
    doc["pubTopic"] = networkSettings.pubTopic;
    doc["subTopic"] = networkSettings.subTopic;
    doc["mqttUsername"] = networkSettings.mqttUsername;
    doc["mqttPass"] = networkSettings.mqttPassword;
    doc["loggerMode"] = networkSettings.loggerMode;
    doc["protocolMode2"] = networkSettings.protocolMode2;
    doc["modbusMode"] = modbusParam.mode;
    doc["modbusPort"] = modbusParam.port;
    doc["modbusSlaveID"] = modbusParam.slaveID;
    doc["sendTrig"] = networkSettings.sendTrig;
    doc["erpUrl"] = networkSettings.erpUrl;
    doc["erpUsername"] = networkSettings.erpUsername;
    doc["erpPassword"] = networkSettings.erpPassword;
  }
  else if (configType == "digital")
  {
    for (unsigned char i = 1; i < jumlahInputDigital + 1; i++)
    {
      doc["DI" + String(i)]["name"] = digitalInput[i].name;
      doc["DI" + String(i)]["invers"] = digitalInput[i].inv;
      doc["DI" + String(i)]["taskMode"] = digitalInput[i].taskMode;
      doc["DI" + String(i)]["inputState"] = digitalInput[i].inputState ? "High" : "Low";
      doc["DI" + String(i)]["intervalTime"] = digitalInput[i].intervalTime;
      doc["DI" + String(i)]["conversionFactor"] = digitalInput[i].conversionFactor;
    }
  }
  else if (configType == "analog")
  {
    for (unsigned char i = 1; i < jumlahInputAnalog + 1; i++)
    {
      doc["AI" + String(i)]["name"] = analogInput[i].name;
      doc["AI" + String(i)]["inputType"] = analogInput[i].inputType;
      doc["AI" + String(i)]["filter"] = analogInput[i].filter;
      doc["AI" + String(i)]["filterPeriod"] = analogInput[i].filterPeriod;
      doc["AI" + String(i)]["scaling"] = analogInput[i].scaling;
      doc["AI" + String(i)]["lowLimit"] = analogInput[i].lowLimit;
      doc["AI" + String(i)]["highLimit"] = analogInput[i].highLimit;
    }
  }
  else if (configType == "modbusSetup")
  {
    deserializeJson(jsonParam, stringParam);
    doc = jsonParam;
  }
  else if (configType == "systemSettings")
  {
    doc["username"] = networkSettings.loginUsername;
    doc["password"] = networkSettings.loginPassword;
    doc["sdInterval"] = networkSettings.sdSaveInterval;
  }

  File file = SPIFFS.open(dir, "w");
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    errorMessages.addMessage(getTimeNow() + " - Failed to open file for writing");
    return;
  }

  if (serializeJson(doc, file) == 0)
  {
    Serial.println("Failed to write JSON to file");
    file.close();
    return;
  }

  file.close();
  Serial.println("JSON object saved to file");
}

void saveToSDConfig(const char *dir, const char *configType)
{
  // Cek apakah SD Card siap (gunakan flag global jika ada, atau mutex)
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)))
  {
    Serial.printf("[SD] Saving config: %s ...\n", dir);

    // Hapus file lama agar data bersih (overwrite)
    if (SD.exists(dir)) {
      SD.remove(dir);
    }

    DynamicJsonDocument docSD(4096);

    // 1. NETWORK CONFIG
    if (String(configType) == "network")
    {
      docSD["networkMode"] = networkSettings.networkMode;
      docSD["ssid"] = networkSettings.ssid;
      docSD["password"] = networkSettings.password;
      docSD["apSsid"] = networkSettings.apSsid;
      docSD["apPassword"] = networkSettings.apPassword;
      docSD["dhcpMode"] = networkSettings.dhcpMode;
      docSD["ipAddress"] = networkSettings.ipAddress;
      docSD["subnet"] = networkSettings.subnetMask;
      docSD["ipGateway"] = networkSettings.ipGateway;
      docSD["ipDNS"] = networkSettings.ipDNS;
      docSD["sendInterval"] = networkSettings.sendInterval;
      docSD["protocolMode"] = networkSettings.protocolMode;
      docSD["endpoint"] = networkSettings.endpoint;
      docSD["port"] = networkSettings.port;
      docSD["pubTopic"] = networkSettings.pubTopic;
      docSD["subTopic"] = networkSettings.subTopic;
      docSD["mqttUsername"] = networkSettings.mqttUsername;
      docSD["mqttPass"] = networkSettings.mqttPassword;
      docSD["loggerMode"] = networkSettings.loggerMode;
      docSD["protocolMode2"] = networkSettings.protocolMode2;
      docSD["modbusMode"] = modbusParam.mode;
      docSD["modbusPort"] = modbusParam.port;
      docSD["modbusSlaveID"] = modbusParam.slaveID;
      docSD["sendTrig"] = networkSettings.sendTrig;
      docSD["erpUrl"] = networkSettings.erpUrl;
      docSD["erpUsername"] = networkSettings.erpUsername;
      docSD["erpPassword"] = networkSettings.erpPassword;
    }
    // 2. DIGITAL INPUT CONFIG
    else if (String(configType) == "digital")
    {
      for (unsigned char i = 1; i < jumlahInputDigital + 1; i++)
      {
        docSD["DI" + String(i)]["name"] = digitalInput[i].name;
        docSD["DI" + String(i)]["invers"] = digitalInput[i].inv;
        docSD["DI" + String(i)]["taskMode"] = digitalInput[i].taskMode;
        docSD["DI" + String(i)]["inputState"] = digitalInput[i].inputState ? "High" : "Low";
        docSD["DI" + String(i)]["intervalTime"] = digitalInput[i].intervalTime;
        docSD["DI" + String(i)]["conversionFactor"] = digitalInput[i].conversionFactor;
      }
    }
    // 3. ANALOG INPUT CONFIG
    else if (String(configType) == "analog")
    {
      for (unsigned char i = 1; i < jumlahInputAnalog + 1; i++)
      {
        docSD["AI" + String(i)]["name"] = analogInput[i].name;
        docSD["AI" + String(i)]["inputType"] = analogInput[i].inputType;
        docSD["AI" + String(i)]["filter"] = analogInput[i].filter;
        docSD["AI" + String(i)]["filterPeriod"] = analogInput[i].filterPeriod;
        docSD["AI" + String(i)]["scaling"] = analogInput[i].scaling;
        docSD["AI" + String(i)]["lowLimit"] = analogInput[i].lowLimit;
        docSD["AI" + String(i)]["highLimit"] = analogInput[i].highLimit;
        docSD["AI" + String(i)]["calibration"] = analogInput[i].calibration;
        docSD["AI" + String(i)]["mValue"] = analogInput[i].mValue;
        docSD["AI" + String(i)]["cValue"] = analogInput[i].cValue;
      }
    }
    // 4. MODBUS SETUP (Menggunakan jsonParam global yang sudah diupdate)
    else if (String(configType) == "modbusSetup")
    {
      docSD = jsonParam;
    }
    // 5. SYSTEM SETTINGS
    else if (String(configType) == "systemSettings")
    {
      docSD["username"] = networkSettings.loginUsername;
      docSD["password"] = networkSettings.loginPassword;
      docSD["sdInterval"] = networkSettings.sdSaveInterval;
    }

    // Tulis ke SD Card
    File file = SD.open(dir, FILE_WRITE);
    if (file)
    {
      if (serializeJson(docSD, file) == 0) {
        Serial.println("  ✗ Failed to write config to SD");
      } else {
        Serial.println("  ✓ Config saved to SD Card");
      }
      file.close();
    }
    else
    {
      Serial.println("  ✗ Failed to open file on SD Card");
    }

    xSemaphoreGive(sdMutex);
  }
  else {
    Serial.println("  ⚠ SD Card Busy (Mutex), Config NOT saved to SD.");
  }
}