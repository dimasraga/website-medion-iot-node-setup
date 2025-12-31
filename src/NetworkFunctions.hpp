#ifndef NETWORK_FUNCTIONS_HPP
#define NETWORK_FUNCTIONS_HPP

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <DNSServer.h>
#include "config.hpp"
#include <Ethernet.h>
class MyEthernetServer : public EthernetServer
{
public:
  // Gunakan constructor dari parent
  MyEthernetServer(uint16_t port) : EthernetServer(port) {}

  // Implementasi begin() sesuai aturan ESP32 Server Class
  void begin(uint16_t port = 0) override
  {
    EthernetServer::begin(); // Panggil fungsi begin() asli milik library Ethernet
  }
};
// Forward declarations
extern PubSubClient mqtt;
extern WiFiClient esp32;
// EthernetServer ethServer(80);
MyEthernetServer ethServer(80);
extern DNSServer dnsServer;
extern const byte DNS_PORT;
extern bool dnsStarted;
extern unsigned long checkTime;
extern unsigned long sendTime;
extern bool flagGetJobNum;
extern String jobNum;
extern ErrorBlinker errorBlinker;
extern ErrorMessages errorMessages;

extern String getTimeNow();
extern String getTimeDateNow();

// WiFi Connection State
bool wifiConnected = false;
bool wifiConnecting = false;
bool apReady = false;
unsigned long wifiConnectStartTime = 0;
// =================================================================
// PERUBAHAN 1: Ubah timeout ke 10 detik (sesuai permintaan)
// =================================================================
const unsigned long WIFI_CONNECT_TIMEOUT = 10000; // 10 detik timeout (sebelumnya 30000)

// =================================================================
// PERUBAHAN 2: Tambahkan flag "menyerah"
// =================================================================
bool staConnectionAttemptFailed = false; // Flag untuk berhenti mencoba koneksi STA

// Function declarations
IpAddressSplit parsingIP(String data);
void mqttCallback(char *topic, byte *payload, unsigned int length);
void checkWiFi(int timeout);
void get_JobNum();
void startDNSServer();
void stopDNSServer();
void configNetwork();
void configProtocol();
void sendDataMQTT(String dataSend, String publishTopic, int intervalSend);
void sendDataHTTP(String data, String serverPath, String httpUsername, String httpPassword, int intervalSend);
void saveToSD(String data);
void sendBackupData();

// Implementation
IpAddressSplit parsingIP(String data)
{
  IpAddressSplit result;
  int start = 0, j = 0;
  int len = data.length();
  for (int i = 0; i <= len; i++)
  {
    if (i == len || data.charAt(i) == '.' || data.charAt(i) == ',')
    {
      if (j < 5)
      {
        result.ip[j] = data.substring(start, i).toInt();
        j++;
      }
      start = i + 1;
    }
  }
  return result;
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.println("MQTT Message Received");
  String command = "";
  for (int j = 0; j < length; j++)
    command += (char)payload[j];
  Serial.println(command);

  StaticJsonDocument<200> commandJson;
  DeserializationError error = deserializeJson(commandJson, command);
  if (error)
  {
    Serial.println("Failed to parse JSON");
    return;
  }

  for (byte i = 1; i < jumlahOutputDigital + 1; i++)
  {
    if (commandJson.containsKey(digitalOutput[i].name))
    {
      Serial.print(digitalOutput[i].name + ": ");
      bool statusDO = commandJson[digitalOutput[i].name];
      Serial.println(statusDO);
      digitalWrite(2, commandJson[digitalOutput[i].name]);
    }
  }
}

// =================================================================
// PERUBAHAN 3: Modifikasi checkWiFi() secara keseluruhan
// =================================================================
void checkWiFi(int timeout)
{
  if (networkSettings.networkMode == "WiFi")
  {
    if (staConnectionAttemptFailed)
    {
      return; // Berhenti mencoba koneksi STA
    }

    networkSettings.macAddress = WiFi.macAddress();

    if (millis() - checkTime >= timeout)
    {
      wl_status_t status = WiFi.status();

      // Handle WiFi disconnection
      if (status != WL_CONNECTED && !wifiConnecting)
      {
        errorBlinker.trigger(5, 500);
        errorMessages.addMessage(getTimeNow() + " - WiFi Disconnected, attempting reconnect...");

        Serial.println("WiFi disconnected. Status: " + String(status));
        wifiConnecting = true;
        wifiConnectStartTime = millis();

        // Try reconnect (jangan disconnect AP!)
        WiFi.begin(networkSettings.ssid.c_str(), networkSettings.password.c_str());
      }

      // Check connection timeout
      if (wifiConnecting && (millis() - wifiConnectStartTime > WIFI_CONNECT_TIMEOUT))
      {
        // =================================================================
        // LOGIKA BARU: Timeout, "Menyerah"
        // =================================================================
        Serial.println("=================================");
        Serial.println("✗ WiFi connection timeout (10s).");
        Serial.println("  Stopping all future STA connection attempts.");
        Serial.println("  Access Point (AP) will remain active.");
        Serial.println("=================================");

        wifiConnecting = false;
        staConnectionAttemptFailed = true; // Set flag "menyerah"

        // Eksplisit matikan STA dan auto-reconnect
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(true); // Disconnect STA, biarkan AP tetap jalan
      }

      // Handle successful connection
      if (status == WL_CONNECTED)
      {
        if (!wifiConnected || wifiConnecting)
        {
          Serial.println("=================================");
          Serial.println("WiFi Connected Successfully!");
          Serial.print("Local IP: ");
          Serial.println(WiFi.localIP());
          Serial.print("AP IP: ");
          Serial.println(WiFi.softAPIP());
          Serial.print("RSSI: ");
          Serial.println(WiFi.RSSI());
          Serial.println("=================================");

          wifiConnected = true;
          wifiConnecting = false;
          staConnectionAttemptFailed = false; // Berhasil, reset flag "menyerah"

          // =================================================================
          // LOGIKA BARU: Hanya set AutoReconnect SETELAH berhasil
          // =================================================================
          WiFi.setAutoReconnect(true); // Jika koneksi drop, baru boleh auto-reconnect

          if (flagGetJobNum)
          {
#ifndef DEBUG
            get_JobNum();
#endif
          }
        }
      }
      else
      {
        wifiConnected = false;
      }

      checkTime = millis();
    }

    // Monitor AP status (tidak diubah)
    if (apReady && millis() % 30000 < 100)
    { // Setiap 30 detik
      Serial.println("=================================");
      Serial.println("AP Status Check:");
      Serial.printf("  AP SSID: %s\n", networkSettings.apSsid.c_str());
      Serial.printf("  AP IP: %s\n", WiFi.softAPIP().toString().c_str());
      Serial.printf("  Clients: %d\n", WiFi.softAPgetStationNum());
      Serial.println("=================================");
    }
  }
}

void get_JobNum()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Cannot get Job Number - WiFi not connected");
    return;
  }

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String serverPath = networkSettings.erpUrl;
  http.begin(client, serverPath.c_str());
  http.setAuthorization(networkSettings.erpUsername.c_str(), networkSettings.erpPassword.c_str());
  int httpResponseCode = http.GET();

  if (httpResponseCode > 0)
  {
    String takeData = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, takeData);

    if (error)
    {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      return;
    }

    JsonObject data = doc["value"][0];
    jobNum = data["LaborDtl_JobNum"].as<String>();
    Serial.println("Job Number: " + jobNum);
    flagGetJobNum = 0;
  }
  else
  {
    Serial.print(F("Error code: "));
    Serial.println(httpResponseCode);
    errorMessages.addMessage(getTimeNow() + " - Failed to get Job Number from ERP");
  }
  http.end();
}

void startDNSServer()
{
  if (!dnsStarted && apReady)
  {
    delay(500); // Tunggu AP fully ready
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    dnsStarted = true;
    ESP_LOGI("DNS", "DNS server started, redirecting to %s", WiFi.softAPIP().toString().c_str());
    Serial.println("DNS Server started on " + WiFi.softAPIP().toString());
  }
}

void stopDNSServer()
{
  if (dnsStarted)
  {
    dnsServer.stop();
    dnsStarted = false;
    ESP_LOGI("DNS", "DNS server stopped");
  }
}

// =================================================================
// PERUBAHAN 4: Modifikasi configNetwork()
// =================================================================
void configNetwork()
{
  if (networkSettings.networkMode == "WiFi")
  {
    Serial.println("\n=================================");
    Serial.println("CONFIGURING WIFI (AP+STA MODE)");
    Serial.println("=================================\n");

    // =====================================================================
    // STEP 1: Complete WiFi Reset
    // =====================================================================
    Serial.println("[1/7] Resetting WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1000); // PENTING: Kasih waktu WiFi fully off

    // =====================================================================
    // STEP 2: Set MAC Address (SEBELUM WiFi ON)
    // =====================================================================
    Serial.println("[2/7] Setting MAC Address...");
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err == ESP_OK)
    {
      Serial.printf("  ✓ MAC Address set: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    else
    {
      Serial.println("  ✗ Failed to set MAC Address");
    }

    // =====================================================================
    // STEP 3: Configure WiFi Mode (AP+STA)
    // =====================================================================
    Serial.println("[3/7] Setting WiFi mode to AP+STA...");
    WiFi.mode(WIFI_AP_STA);
    delay(500);
    Serial.println("  ✓ Mode set to AP+STA");

    // =====================================================================
    // STEP 4: Setup WiFi Events
    // =====================================================================
    Serial.println("[4/7] Setting up WiFi events...");

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 { Serial.println("[WiFi Event] STA Started"); }, ARDUINO_EVENT_WIFI_STA_START);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        Serial.println("[WiFi Event] STA Connected to AP!");
        wifiConnecting = false;
        wifiConnected = true; }, ARDUINO_EVENT_WIFI_STA_CONNECTED);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
                   Serial.print("[WiFi Event] STA Disconnected. Reason: ");
                   Serial.println(info.wifi_sta_disconnected.reason);

                   switch (info.wifi_sta_disconnected.reason)
                   {
                   case WIFI_REASON_AUTH_EXPIRE:
                   case WIFI_REASON_AUTH_FAIL:
                     Serial.println("  → Authentication failed (wrong password?)");
                     break;
                   case WIFI_REASON_NO_AP_FOUND:
                     Serial.println("  → AP not found (wrong SSID?)");
                     break;
                   case WIFI_REASON_ASSOC_FAIL:
                     Serial.println("  → Association failed");
                     break;
                   case WIFI_REASON_HANDSHAKE_TIMEOUT:
                     Serial.println("  → 4-way handshake timeout");
                     break;
                   default:
                     Serial.printf("  → Error code: %d\n", info.wifi_sta_disconnected.reason);
                   }

                   wifiConnected = false;
                   // Jangan panggil reconnect di sini, biarkan checkWiFi() yang handle
                   // Serial.println("  → Will retry connection...");
                 },
                 ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        Serial.print("[WiFi Event] STA Got IP: ");
        Serial.println(WiFi.localIP());
        Serial.print("  → Subnet: ");
        Serial.println(WiFi.subnetMask());
        Serial.print("  → Gateway: ");
        Serial.println(WiFi.gatewayIP());
        Serial.print("  → DNS: ");
        Serial.println(WiFi.dnsIP()); }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    // AP Events
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        Serial.println("[WiFi Event] AP Started!");
        apReady = true; }, ARDUINO_EVENT_WIFI_AP_START);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        Serial.println("[WiFi Event] Client Connected to AP");
        Serial.printf("  Clients: %d\n", WiFi.softAPgetStationNum()); }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        Serial.println("[WiFi Event] Client Disconnected from AP");
        Serial.printf("  Clients: %d\n", WiFi.softAPgetStationNum()); }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

    Serial.println("  ✓ Events configured");

    // =====================================================================
    // STEP 5: Configure Access Point (AP) - PRIORITAS PERTAMA!
    // =====================================================================
    Serial.println("[5/7] Starting Access Point...");
    Serial.println("  AP Configuration:");
    Serial.printf("    SSID: %s\n", networkSettings.apSsid.c_str());
    Serial.printf("    Pass: %s\n", networkSettings.apPassword.c_str());
    Serial.println("    Channel: 6 (fixed, tidak bentrok)");
    Serial.println("    Hidden: No");
    Serial.println("    Max Clients: 4");

    // KUNCI: Gunakan channel yang berbeda dari STA
    bool apStarted = WiFi.softAP(
        networkSettings.apSsid.c_str(),     // SSID
        networkSettings.apPassword.c_str(), // Password
        6,                                  // Channel (fixed ke 6)
        0,                                  // SSID Hidden (0=visible, 1=hidden)
        4                                   // Max connections
    );

    if (apStarted)
    {
      delay(1000); // PENTING: Tunggu AP fully initialized

      // Verify AP is actually running
      if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0))
      {
        Serial.println("  ✗ AP Failed - IP is 0.0.0.0");
        Serial.println("  → Retrying AP start...");
        WiFi.softAPdisconnect(true);
        delay(500);
        apStarted = WiFi.softAP(networkSettings.apSsid.c_str(),
                                networkSettings.apPassword.c_str(), 6, 0, 4);
        delay(1000);
      }

      if (WiFi.softAPIP() != IPAddress(0, 0, 0, 0))
      {
        apReady = true;
        Serial.println("  ✓ AP Started Successfully!");
        Serial.printf("    AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("    AP MAC: %s\n", WiFi.softAPmacAddress().c_str());

        // Start DNS Server setelah AP fully ready
        startDNSServer();
      }
      else
      {
        Serial.println("  ✗ AP Failed to start properly");
        apReady = false;
      }
    }
    else
    {
      Serial.println("  ✗ Failed to start AP");
      apReady = false;
    }

    // =====================================================================
    // STEP 6: Configure Station (STA) IP Settings
    // =====================================================================
    Serial.println("[6/7] Configuring STA IP settings...");

    if (networkSettings.dhcpMode == "DHCP")
    {
      Serial.println("  Using DHCP");
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    else
    {
      Serial.println("  Using Static IP");
      IpAddressSplit hasilParsing;

      hasilParsing = parsingIP(networkSettings.ipAddress);
      IPAddress localIP(hasilParsing.ip[0], hasilParsing.ip[1], hasilParsing.ip[2], hasilParsing.ip[3]);

      hasilParsing = parsingIP(networkSettings.subnetMask);
      IPAddress subnetIP(hasilParsing.ip[0], hasilParsing.ip[1], hasilParsing.ip[2], hasilParsing.ip[3]);

      hasilParsing = parsingIP(networkSettings.ipGateway);
      IPAddress gatewayIP(hasilParsing.ip[0], hasilParsing.ip[1], hasilParsing.ip[2], hasilParsing.ip[3]);

      hasilParsing = parsingIP(networkSettings.ipDNS);
      IPAddress dnsIP(hasilParsing.ip[0], hasilParsing.ip[1], hasilParsing.ip[2], hasilParsing.ip[3]);

      if (WiFi.config(localIP, gatewayIP, subnetIP, dnsIP))
      {
        Serial.println("  ✓ Static IP configured");
        Serial.printf("    IP: %s\n", localIP.toString().c_str());
        Serial.printf("    Gateway: %s\n", gatewayIP.toString().c_str());
      }
      else
      {
        Serial.println("  ✗ Failed to configure static IP");
      }
    }

    // =====================================================================
    // STEP 7: Connect to WiFi Network (STA)
    // =====================================================================
    Serial.println("[7/7] Connecting to WiFi network...");
    Serial.printf("  Target SSID: %s\n", networkSettings.ssid.c_str());

    mqtt = PubSubClient(esp32);
    checkTime = millis();

    // =================================================================
    // LOGIKA BARU: Jangan auto-reconnect di awal!
    // =================================================================
    WiFi.setAutoReconnect(false); // <--- DIUBAH DARI true
    WiFi.persistent(false);       // Jangan save ke flash terus-menerus

    wifiConnecting = true;
    wifiConnectStartTime = millis();
    WiFi.begin(networkSettings.ssid.c_str(), networkSettings.password.c_str());

    networkSettings.macAddress = WiFi.macAddress();

    Serial.println("\n=================================");
    Serial.println("CONFIGURATION COMPLETE");
    Serial.println("=================================");
    Serial.println("AP Status:");
    Serial.printf("  SSID: %s\n", networkSettings.apSsid.c_str());
    Serial.printf("  IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("  Ready: %s\n", apReady ? "YES" : "NO");
    Serial.println("\nSTA Status:");
    Serial.printf("  Target: %s\n", networkSettings.ssid.c_str());
    Serial.println("  Status: Connecting...");
    Serial.println("=================================\n");
  }
  // Di dalam fungsi configNetwork(), bagian else if (networkSettings.networkMode == "Ethernet")
  // GANTI BLOK ETHERNET YANG LAMA DENGAN KODE INI:

  if (networkSettings.networkMode == "Ethernet")
  {
    Serial.println("\n=================================");
    Serial.println("CONFIGURING ETHERNET (W5500)");
    Serial.println("=================================\n");

    // Disconnect WiFi completely
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);

    // Start Access Point untuk konfigurasi
    Serial.println("[1/5] Starting Access Point...");
    bool apStarted = WiFi.softAP(networkSettings.apSsid.c_str(),
                                 networkSettings.apPassword.c_str(), 6, 0, 4);

    if (apStarted)
    {
      delay(1000);
      apReady = true;
      Serial.println("  ✓ AP Started");
      Serial.printf("    AP IP: %s\n", WiFi.softAPIP().toString().c_str());
      startDNSServer();
    }
    else
    {
      Serial.println("  ✗ Failed to start AP");
      apReady = false;
    }

    // =====================================================================
    // STEP 2: Hardware Reset W5500
    // =====================================================================
    Serial.println("[2/5] Resetting W5500 chip...");
    pinMode(ETH_RST, OUTPUT);
    digitalWrite(ETH_RST, LOW);
    delay(100);
    digitalWrite(ETH_RST, HIGH);
    delay(200);
    Serial.println("  ✓ W5500 reset complete");

    // =====================================================================
    // STEP 3: Initialize SPI for W5500
    // =====================================================================
    Serial.println("[3/5] Initializing SPI...");
    // SPI sudah di-init di setup(), tapi pastikan dengan pinout yang benar
    SPI.begin(ETH_CLK, ETH_MISO, ETH_MOSI, -1); // -1 karena CS manual
    SPI.setDataMode(SPI_MODE0);
    Serial.printf("  ✓ SPI initialized (CLK:%d, MISO:%d, MOSI:%d, CS:%d)\n",
                  ETH_CLK, ETH_MISO, ETH_MOSI, ETH_CS);

    // =====================================================================
    // STEP 4: Initialize Ethernet with W5500
    // =====================================================================
    Serial.println("[4/5] Initializing Ethernet...");
    Ethernet.init(ETH_CS); // Set CS pin untuk W5500
    // ip static
    // IPAddress localIP(10, 22, 7, 3);      // IP Requested
    // IPAddress gatewayIP(10, 22, 7, 1);    // Asumsi Gateway default
    // IPAddress subnetIP(255, 255, 255, 0); // Subnet standard
    // IPAddress dnsIP(8, 8, 8, 8);          // DNS Google

    // // Update variable global agar tampilan di web/serial benar
    // networkSettings.ipAddress = localIP.toString();
    // networkSettings.ipGateway = gatewayIP.toString();
    // networkSettings.subnetMask = subnetIP.toString();
    // networkSettings.ipDNS = dnsIP.toString();
    // networkSettings.dhcpMode = "Static (Forced)";

    // Serial.println("  [FORCE] Starting Ethernet with Static IP: 10.22.7.3 ...");

    // // Syntax: begin(mac, ip, dns, gateway, subnet)
    // Ethernet.begin(mac, localIP, dnsIP, gatewayIP, subnetIP);
    // delay(1000); // Beri waktu untuk inisialisasi PHY

    // Serial.println("  ✓ Ethernet configured manually");
    // ini dhcp
    // Parse IP addresses
    IpAddressSplit hasilParsing;
    hasilParsing = parsingIP(networkSettings.ipAddress);
    IPAddress localIP(hasilParsing.ip[0], hasilParsing.ip[1],
                      hasilParsing.ip[2], hasilParsing.ip[3]);

    hasilParsing = parsingIP(networkSettings.subnetMask);
    IPAddress subnetIP(hasilParsing.ip[0], hasilParsing.ip[1],
                       hasilParsing.ip[2], hasilParsing.ip[3]);

    hasilParsing = parsingIP(networkSettings.ipGateway);
    IPAddress gatewayIP(hasilParsing.ip[0], hasilParsing.ip[1],
                        hasilParsing.ip[2], hasilParsing.ip[3]);

    hasilParsing = parsingIP(networkSettings.ipDNS);
    IPAddress dnsIP(hasilParsing.ip[0], hasilParsing.ip[1],
                    hasilParsing.ip[2], hasilParsing.ip[3]);

    if (networkSettings.dhcpMode == "DHCP")
    {
      Serial.println("  Starting Ethernet with DHCP...");
      // 10s timeout, 4s response timeout
      if (Ethernet.begin(mac, 10000, 4000) == 0)
      {
        ESP_LOGE("Ethernet", "  ✗ Failed to configure Ethernet using DHCP");
        errorMessages.addMessage(getTimeNow() + " - Failed to start Ethernet (DHCP)");
      }
      else
      {
        Serial.println("  ✓ Ethernet started with DHCP");
        Serial.printf("    IP: %s\n", Ethernet.localIP().toString().c_str());
      }
    }
    else
    {
      Serial.println("  Starting Ethernet with Static IP...");
      Ethernet.begin(mac, localIP, dnsIP, gatewayIP, subnetIP);
      delay(1000);
      Serial.println("  ✓ Ethernet configured with static IP");
    }
    // */ //batas dhcp dikomen saja ini
    // =====================================================================
    // STEP 5: Verify Ethernet Hardware & Link Status
    // =====================================================================
    Serial.println("[5/5] Verifying Ethernet connection...");

    if (Ethernet.hardwareStatus() == EthernetNoHardware)
    {
      ESP_LOGE("Ethernet", "  ✗ W5500 chip not found!");
      errorMessages.addMessage(getTimeNow() + " - W5500 hardware not found");
      errorBlinker.trigger(5, 200);
    }
    else
    {
      Serial.println("  ✓ W5500 chip detected");

      if (Ethernet.linkStatus() == LinkOFF)
      {
        ESP_LOGW("Ethernet", "  ⚠ Ethernet cable not connected");
        errorMessages.addMessage(getTimeNow() + " - Ethernet cable disconnected");
      }
      else if (Ethernet.linkStatus() == LinkON)
      {
        Serial.println("  ✓ Ethernet cable connected");
      }
    }

    // Set MAC Address string
    networkSettings.macAddress = "";
    for (int i = 0; i < 6; i++)
    {
      if (mac[i] < 0x10)
        networkSettings.macAddress += "0";
      networkSettings.macAddress += String(mac[i], HEX);
      if (i < 5)
        networkSettings.macAddress += ":";
    }

    // Print final Ethernet status
    Serial.println("\n=================================");
    Serial.println("ETHERNET CONFIGURATION COMPLETE");
    Serial.println("=================================");
    Serial.printf("MAC Address: %s\n", networkSettings.macAddress.c_str());
    Serial.printf("IP Address:  %s\n", Ethernet.localIP().toString().c_str());
    Serial.printf("Subnet Mask: %s\n", Ethernet.subnetMask().toString().c_str());
    Serial.printf("Gateway:     %s\n", Ethernet.gatewayIP().toString().c_str());
    Serial.printf("DNS Server:  %s\n", Ethernet.dnsServerIP().toString().c_str());
    Serial.printf("Link Status: %s\n",
                  Ethernet.linkStatus() == LinkON ? "CONNECTED" : "DISCONNECTED");
    Serial.println("=================================\n");
    ethServer.begin();
    Serial.println("[INFO] AsyncWebServer will handle Ethernet requests");
    Serial.printf("  Access via Ethernet: http://%s\n", Ethernet.localIP().toString().c_str());
    Serial.printf("  Access via WiFi AP:  http://%s\n", WiFi.softAPIP().toString().c_str());
  }
}

void configProtocol()
{
  if (networkSettings.protocolMode == "MQTT")
  {
    mqtt.setServer(networkSettings.endpoint.c_str(), networkSettings.port);
    mqtt.setCallback(mqttCallback);
  }
}

void sendDataMQTT(String dataSend, String publishTopic, int intervalSend)
{
  if (millis() - sendTime >= (intervalSend * 1000))
  {
    if (mqtt.publish(publishTopic.c_str(), dataSend.c_str()))
    {
      Serial.println("MQTT data sent successfully");
    }
    else
    {
      Serial.println("MQTT publish failed");
    }
    sendTime = millis();
  }
}

void sendDataHTTP(String data, String serverPath, String httpUsername, String httpPassword, int intervalSend)
{
  if (millis() - sendTime >= (intervalSend * 1000))
  {
    WiFiClientSecure client;
    HTTPClient https;
    client.setInsecure();

    if (networkSettings.connStatus == "Not Connected")
    {
#ifndef DEBUG
      saveToSD(data);
#endif
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      https.begin(client, serverPath);
      ESP_LOGI("HTTP", "Sending to: %s", serverPath.c_str());
      https.setAuthorization(httpUsername.c_str(), httpPassword.c_str());
      https.addHeader("Content-Type", "application/json");
      https.setTimeout(10000);
      int httpResponseSent = https.POST(data);
      ESP_LOGI("HTTP", "Response code: %d", httpResponseSent);

      if (httpResponseSent >= 200 && httpResponseSent <= 204)
      {
#ifdef DEBUG
        ESP_LOGI("HTTP", "✓ Data sent successfully!");
#endif
        networkSettings.connStatus = "Connected";
      }
      else
      {
        errorBlinker.trigger(4, 100);
        ESP_LOGW("HTTP", "✗ Failed to send data");
        errorMessages.addMessage(getTimeNow() + " - Failed to send data to API");
        networkSettings.connStatus = "Not Connected";
      }
      https.end();
    }
    else if (networkSettings.networkMode == "Ethernet")
    {
      https.begin(client, serverPath);
      https.setAuthorization(httpUsername.c_str(), httpPassword.c_str());
      https.addHeader("Content-Type", "application/json");
      https.setTimeout(10000);
      int httpResponseSent = https.POST(data);

      if (httpResponseSent >= 200 && httpResponseSent <= 204)
      {
#ifdef DEBUG
        Serial.println("Data successfully sent!");
#endif
        networkSettings.connStatus = "Connected";
      }
      else
      {
        errorBlinker.trigger(4, 100);
        errorMessages.addMessage(getTimeNow() + " - Failed to send data to API");
        networkSettings.connStatus = "Not Connected";
      }
      https.end();
    }
    else
    {
      errorBlinker.trigger(4, 100);
      ESP_LOGW("HTTP", "✗ WiFi not connected");
      errorMessages.addMessage(getTimeNow() + " - WiFi not connected");
      networkSettings.connStatus = "Not Connected";
    }
    sendTime = millis();
  }
}

// void saveToSD(String data)
// {
//   Serial.println("Saving to SD card...");

//   if (!SD.begin(5))
//   {
//     errorBlinker.trigger(4, 200);
//     ESP_LOGE("SD Card", "Initialization failed!");
//     errorMessages.addMessage(getTimeNow() + " - Failed to initialize SD card");
//     return;
//   }

//   ESP_LOGI("SD Card", "Initialized successfully");
//   delay(100);

//   File dataOffline = SD.open("/sensor_data.csv", FILE_APPEND);
//   if (!dataOffline)
//   {
//     errorBlinker.trigger(3, 200);
//     ESP_LOGE("SD Card", "Failed to open file!");
//     errorMessages.addMessage(getTimeNow() + " - Failed to open file for writing!");
//     SD.end();
//     return;
//   }

//   dataOffline.println(data);
//   dataOffline.close();
//   ESP_LOGI("SD Card", "✓ Data saved");
//   SD.end();
// }

void saveToSD(String data)
{
  Serial.println("Saving to SD card...");

  // HAPUS SD.begin(5)! Kita asumsikan sudah nyala dari setup()
  // HAPUS SD.end()! Biarkan jalur SPI tetap hidup untuk Ethernet

  // Cek apakah sistem file mounted (opsional, tapi aman)
  // if(!SD.totalBytes()) {
  //    Serial.println("SD Card not mounted!");
  //    return;
  // }

  // Gunakan Mutex jika perlu (agar tidak bentrok dengan task lain)
  // if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000))) {

  File dataOffline = SD.open("/sensor_data.csv", FILE_APPEND);
  if (!dataOffline)
  {
    errorBlinker.trigger(3, 200);
    ESP_LOGE("SD Card", "Failed to open file!");
    errorMessages.addMessage(getTimeNow() + " - Failed to open file for writing!");
    // JANGAN panggil SD.end() disini
    return;
  }

  dataOffline.println(data);
  dataOffline.close();
  ESP_LOGI("SD Card", "✓ Data saved");

  // xSemaphoreGive(sdMutex);
  // }
}

// void sendBackupData()
// {
//   Serial.println("Checking SD card for backup data...");

//   if (!SD.begin(5))
//   {
//     errorBlinker.trigger(4, 200);
//     ESP_LOGE("SD Card", "Initialization failed!");
//     errorMessages.addMessage(getTimeNow() + " - Failed to initialize SD card");
//     return;
//   }

//   File dataOffline = SD.open("/sensor_data.csv", FILE_READ);
//   if (!dataOffline)
//   {
//     errorBlinker.trigger(3, 200);
//     ESP_LOGE("SD Card", "Failed to open file for reading!");
//     errorMessages.addMessage(getTimeNow() + " - Failed to open file for reading!");
//     return;
//   }

//   size_t fileSize = dataOffline.size();
//   ESP_LOGI("SD Card", "Backup file size: %d bytes", fileSize);

//   if (fileSize < 10)
//   {
//     Serial.println("Backup file empty");
//     dataOffline.close();
//     return;
//   }

//   String jsonData = "";
//   bool firstLine = true;
//   const int MAX_LENGTH = 5000;
//   WiFiClientSecure client;
//   HTTPClient https;
//   client.setInsecure();

//   while (dataOffline.available())
//   {
//     String line = dataOffline.readStringUntil('\n');
//     if (line.length() < 10)
//       continue;

//     line.replace("[", "");
//     line.replace("]", "");

//     if (!firstLine)
//       jsonData += ",";
//     else
//       firstLine = false;

//     jsonData += line;

//     if (jsonData.length() >= MAX_LENGTH)
//     {
//       String fullData = "[" + jsonData + "]";
//       if (!(WiFi.status() == WL_CONNECTED || networkSettings.networkMode == "Ethernet"))
//       {
//         dataOffline.close();
//         return;
//       }

//       https.begin(client, "https://sensor-logger-trial.medionindonesia.com/api/v1/AddBackupList");
//       https.setAuthorization(networkSettings.mqttUsername.c_str(), networkSettings.mqttPassword.c_str());
//       https.addHeader("Content-Type", "application/json");
//       https.setTimeout(10000);
//       int httpResponseSent = https.POST(fullData);

//       if (httpResponseSent >= 200 && httpResponseSent <= 204)
//       {
// #ifdef DEBUG
//         Serial.println("Backup data sent successfully!");
// #endif
//         networkSettings.connStatus = "Connected";
//       }
//       else
//       {
//         Serial.println("Failed to send backup data");
//         networkSettings.connStatus = "Not Connected";
//       }
//       https.end();

//       jsonData = "";
//       firstLine = true;
//     }
//   }

//   // Send remaining data
//   if (jsonData.length() > 0)
//   {
//     String fullData = "[" + jsonData + "]";
//     if (WiFi.status() == WL_CONNECTED || networkSettings.networkMode == "Ethernet")
//     {
//       https.begin(client, "https://sensor-logger-trial.medionindonesia.com/api/v1/AddBackupList");
//       https.setAuthorization(networkSettings.mqttUsername.c_str(), networkSettings.mqttPassword.c_str());
//       https.addHeader("Content-Type", "application/json");
//       https.setTimeout(10000);
//       int httpResponseSent = https.POST(fullData);

//       if (httpResponseSent >= 200 && httpResponseSent <= 204)
//       {
// #ifdef DEBUG
//         Serial.println("Final backup data sent!");
// #endif
//         networkSettings.connStatus = "Connected";
//       }
//       https.end();
//     }
//   }

//   dataOffline.close();

//   if (networkSettings.connStatus == "Connected")
//   {
//     Serial.println("All backup data sent successfully");
//     File clearFile = SD.open("/sensor_data.csv", FILE_WRITE);
//     if (clearFile)
//     {
//       clearFile.close();
//       Serial.println("Backup file cleared!");
//     }
//   }
// }

// uji coba saja

void sendBackupData()
{
  if (!SD.exists("/sensor_data.csv"))
    return;

  Serial.println("Checking SD card for backup data...");

  // Gunakan Mutex SD jika memungkinkan
  File dataOffline = SD.open("/sensor_data.csv", FILE_READ);
  if (!dataOffline)
  {
    ESP_LOGE("SD", "Failed to open backup file");
    return;
  }

  size_t fileSize = dataOffline.size();
  if (fileSize < 10)
  {
    dataOffline.close();
    SD.remove("/sensor_data.csv"); // Hapus file kosong/sampah
    return;
  }

  // Definisikan Client secara lokal tapi perhatikan Stack (sudah diperbesar di main.cpp)
  WiFiClientSecure client;
  HTTPClient https;
  client.setInsecure(); // Bypass SSL cert check

  String jsonData = "";
  // Pre-allocate memori untuk mencegah fragmentasi
  jsonData.reserve(2048);

  bool firstLine = true;
  int count = 0;

  while (dataOffline.available())
  {
    String line = dataOffline.readStringUntil('\n');
    line.trim();
    if (line.length() < 5)
      continue;

    // Format JSON Array manual
    line.replace("[", "");
    line.replace("]", "");

    if (!firstLine)
      jsonData += ",";
    jsonData += line;
    firstLine = false;
    count++;

    // KIRIM PER 10 DATA (Chunking)
    // Jangan tunggu sampai 5000 karakter, terlalu berisiko
    if (count >= 10 || jsonData.length() > 2000)
    {
      String fullData = "[" + jsonData + "]";

      if (WiFi.status() == WL_CONNECTED || networkSettings.networkMode == "Ethernet")
      {
        // Mulai koneksi HTTP
        if (https.begin(client, "https://sensor-logger-trial.medionindonesia.com/api/v1/AddBackupList"))
        {
          https.setAuthorization(networkSettings.mqttUsername.c_str(), networkSettings.mqttPassword.c_str());
          https.addHeader("Content-Type", "application/json");
          https.setTimeout(5000); // Timeout jangan terlalu lama (5 detik)

          int httpCode = https.POST(fullData);

          if (httpCode > 0)
          {
            Serial.printf("Backup Chunk Sent: %d code\n", httpCode);
            networkSettings.connStatus = "Connected";
          }
          else
          {
            Serial.printf("Backup Send Failed: %s\n", https.errorToString(httpCode).c_str());
          }
          https.end(); // Wajib end untuk lepas memori SSL
        }
      }

      // Reset buffer
      jsonData = "";
      jsonData.reserve(2048);
      firstLine = true;
      count = 0;
      vTaskDelay(10); // Beri jeda agar Watchdog tidak marah
    }
  }

  // Kirim sisa data jika ada
  if (jsonData.length() > 5)
  {
    String fullData = "[" + jsonData + "]";
    if (https.begin(client, "https://sensor-logger-trial.medionindonesia.com/api/v1/AddBackupList"))
    {
      https.setAuthorization(networkSettings.mqttUsername.c_str(), networkSettings.mqttPassword.c_str());
      https.addHeader("Content-Type", "application/json");
      https.POST(fullData);
      https.end();
    }
  }

  dataOffline.close();

  // Hapus file setelah sukses (atau rename ke .bak jika ingin aman)
  SD.remove("/sensor_data.csv");
  Serial.println("Backup process finished & file cleared.");
}
#endif // NETWORK_FUNCTIONS_HPP