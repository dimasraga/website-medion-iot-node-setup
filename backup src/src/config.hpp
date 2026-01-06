#ifndef CONFIG_HPP
#define CONFIG_HPP

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
// #define DEBUG

#include <Arduino.h>
#include <esp_log.h>
#include <ESPAsyncWebServer.h>

byte mac[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

// W5500
#define jumlahInputDigital 4
#define jumlahOutputDigital 4
#define jumlahInputAnalog 4

// Digital Input Pins as an array
const int DI_PINS[] = {32, 33, 25, 26};

#define SIG_LED_PIN 2

// SD Card Pins
#define SD_CS_PIN 5

// Network Variable and Type Declaration
#define ETH_INT 34
#define ETH_MISO 19 // 12
#define ETH_MOSI 23 // 13
#define ETH_CLK 18  // 14
#define ETH_CS 4
#define ETH_RST 12

struct Network
{
  String ssid, password, networkMode, protocolMode, endpoint, pubTopic, subTopic, protocolMode2;
  String apSsid, apPassword, dhcpMode, mqttUsername, mqttPassword, loginUsername = "admin", loginPassword = "admin";
  String ipAddress = "192.168.4.1";  
  String subnetMask = "255.255.255.0"; 
  String ipGateway = "192.168.1.1";    
  String ipDNS = "192.168.1.1";        
  String macAddress, connStatus = "Not Connected", sendTrig;
  String erpUsername, erpPassword, erpUrl;
  bool loggerMode;
  int port;
  float sendInterval;
  int sdSaveInterval = 5; // <-- TAMBAHAN: Default 5 menit
} networkSettings;

// struct Network
// {
//   String ssid, password, networkMode, protocolMode, endpoint, pubTopic, subTopic, protocolMode2;
//   String apSsid, apPassword, dhcpMode, mqttUsername, mqttPassword, loginUsername = "admin", loginPassword = "admin";
//   String ipAddress, subnetMask, ipGateway, ipDNS, macAddress, connStatus = "Not Connected", sendTrig;
//   String erpUsername, erpPassword, erpUrl;
//   bool loggerMode;
//   int port;
//   float sendInterval;
// } networkSettings;

struct AnalogInput
{
  String name;
  String inputType;
  float adcValue;
  float mapValue;
  bool filter;
  bool scaling;
  bool calibration;
  float mValue, cValue, lowLimit, highLimit, filterPeriod;
  byte pin;
} analogInput[jumlahInputAnalog + 1];

struct DigitalInput
{
  String name;
  String taskMode;
  bool inv;
  bool inputState;
  float value;
  int sumValue = 0;
  unsigned long millis_1;
  unsigned long millisNow;
  bool flagInt;
  unsigned long intervalTime, lastMillisPulseMode;
  float conversionFactor;
  byte pin;
} digitalInput[jumlahInputDigital + 1];

struct DigialOutput
{
  String name;
  bool inv, value;
} digitalOutput[jumlahOutputDigital + 1];

struct ModbusParam
{
  int baudrate;
  float scanRate;
  unsigned char dataBit, stopBit;
  String parity;
  int port, slaveID;
  bool mode;
} modbusParam;

struct IpAddressSplit
{
  int ip[5];
};

void IRAM_ATTR isrPulseMode1()
{
  digitalInput[1].sumValue++;
}

void IRAM_ATTR isrPulseMode2()
{
  digitalInput[2].sumValue++;
}

void IRAM_ATTR isrPulseMode3()
{
  digitalInput[3].sumValue++;
}

void IRAM_ATTR isrPulseMode4()
{
  digitalInput[4].sumValue++;
}

void (*isrPulseMode[])(void) = {nullptr, isrPulseMode1, isrPulseMode2, isrPulseMode3, isrPulseMode4};

class ErrorBlinker
{
public:
  struct BlinkRequest
  {
    int blinkCount; // total on/off cycles
    int delayMs;    // both on and off durations in ms
  };

  ErrorBlinker(uint8_t pin, int queueDelay)
      : _pin(pin), _queueDelay(queueDelay)
  {
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
  }

  // Call to queue an error blink request.
  void trigger(int blinkCount, int delayMs)
  {
    BlinkRequest req = {blinkCount, delayMs};
    // You can choose to overwrite if queue full, here we simply ignore.
    enqueue(req);
  }

  // Must be called repeatedly (e.g. in loop()) to update blinking state.
  void update()
  {
    unsigned long currentMillis = millis();

    // If we're in an idle period between sequences, wait for the idle delay.
    if (!_active && !isQueueEmpty() && _idle)
    {
      if (currentMillis - _idleStart >= _queueDelay)
      {
        startNextBlink();
      }
      return;
    }

    // If no active blink and queue is available, trigger idle period.
    if (!_active && !isQueueEmpty() && !_idle)
    {
      _idle = true;
      _idleStart = currentMillis;
      return;
    }

    if (!_active)
      return;

    if (_ledState)
    {
      // LED is ON: wait for onDuration and turn it OFF.
      if (currentMillis - _prevMillis >= _onDuration)
      {
        digitalWrite(_pin, LOW);
        _ledState = false;
        _prevMillis = currentMillis;
        _currentBlink++; // Completed one on/off cycle
        if (_currentBlink >= _totalBlinks)
        {
          _active = false;
          _idle = true;
          _idleStart = currentMillis; // Start idle delay between sequences.
        }
      }
    }
    else
    {
      // LED is OFF: wait for offDuration and turn it ON if more cycles remain.
      if (_currentBlink < _totalBlinks && currentMillis - _prevMillis >= _offDuration)
      {
        digitalWrite(_pin, HIGH);
        _ledState = true;
        _prevMillis = currentMillis;
      }
    }
  }

private:
  uint8_t _pin;

  // Simple circular queue for blink requests.
  static const int QUEUE_SIZE = 5;
  BlinkRequest _queue[QUEUE_SIZE];
  int _queueHead = 0;
  int _queueTail = 0;

  // Current blink sequence state.
  bool _active = false;
  int _currentBlink = 0;
  int _totalBlinks = 0;
  unsigned long _onDuration = 0;
  unsigned long _offDuration = 0;
  bool _ledState = false; // true when LED is ON
  unsigned long _prevMillis = 0;

  // Delay between sequences.
  int _queueDelay;
  bool _idle = false;
  unsigned long _idleStart = 0;

  bool isQueueEmpty()
  {
    return (_queueHead == _queueTail);
  }

  bool enqueue(const BlinkRequest &req)
  {
    int nextPos = (_queueTail + 1) % QUEUE_SIZE;
    if (nextPos != _queueHead)
    { // queue not full
      _queue[_queueTail] = req;
      _queueTail = nextPos;
      return true;
    }
    return false; // queue full, ignore or handle overflow
  }

  bool dequeue(BlinkRequest &req)
  {
    if (isQueueEmpty())
      return false;
    req = _queue[_queueHead];
    _queueHead = (_queueHead + 1) % QUEUE_SIZE;
    return true;
  }

  void startNextBlink()
  {
    BlinkRequest req;
    if (dequeue(req))
    {
#ifdef DEBUG
      Serial.println("Starting new blink sequence: " + String(req.blinkCount) + " blinks, delay: " + String(req.delayMs) + " ms");
#endif
      _active = true;
      _idle = false;
      _totalBlinks = req.blinkCount;
      _currentBlink = 0;
      _onDuration = req.delayMs;
      _offDuration = req.delayMs;
      _ledState = false; // start with LED off
      _prevMillis = millis();
      digitalWrite(_pin, LOW);
    }
  }
};

class ErrorMessages
{
public:
  AsyncEventSource _eventSource;
  ErrorMessages(String path)
      : _eventSource(path) {}

  void addMessage(const String &msg)
  {
    _eventSource.send(msg.c_str());
#ifdef DEBUG
    Serial.println("Added message: " + msg);
#endif
  }
};

#endif