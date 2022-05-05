#include <Arduino.h>
#include <LilyGoWatch.h>
#include <CircularBuffer.h>
#include <BLEDevice.h>
#include <EEPROM.h>

#include "aes.h"
#include "rng.h"

#define SENSOR_BLE_NAME "CGM Sensor"
#define WATCH_BLE_NAME "CGM Watch"

#define CGM_SERVICE_UUID "87f617d1-6c21-461f-b0a5-b5fea2872392"
#define CGM_MEASUREMENT_CHARACTERISTIC_UUID "aa2c4908-64d5-4c89-8ae7-37932f15eadf"
#define CGM_TIME_CHARACTERISTIC_UUID "4f992bbe-675e-4950-9d6c-79acb1cdfa93"

#define SECURITY_SERVICE_UUID "c12f609d-c6b0-49f0-8f17-cba6c45adb8b"
#define SECURITY_VALUE_CHARACTERISTIC_UUID "13494b03-a2da-41f2-8e59-a824d0cee2a5"
#define SECURITY_ACTION_CHARACTERISTIC_UUID "2a1b5b74-97da-4b65-9bfb-dd155d69c8c3"

#define EEPROM_SIZE 32

#define DH_COMMON_G 2
#define DH_COMMON_P 19

struct CGMeasurement {
  int32_t timeOffset;
  int32_t glucoseValue;
};

enum SecurityState {PAIR_0, PAIR_1, AUTH_0, AUTH_1, READY};

TTGOClass *ttgo;
int32_t currentTime = 0;
int32_t ypos = 42;
char *messageLine;
SecurityState currentState;

CircularBuffer<CGMeasurement, 10> sensorBuffer;

BLEScan *pBLEScan;
BLEAddress *pServerAddress;
BLEClient *pClient;
BLERemoteService *cgmRemoteService;
BLERemoteCharacteristic *cgmMeasurementRemoteCharacteristic;
BLERemoteCharacteristic *cgmTimeRemoteCharacteristic;
BLERemoteService *securityRemoteService;
BLERemoteCharacteristic *securityValueRemoteCharacteristic;
BLERemoteCharacteristic *securityActionRemoteCharacteristic;

bool connected = false;
bool newValue = false;
bool readInitial = true;

char answer[64];

uint32_t private_key;
uint32_t server_public_key;
uint32_t client_public_key;
uint32_t shared_key = 0;
uint32_t aes_key[4] = { 0 };
uint32_t checkNum;

void drawMessage(char *message) {
  ttgo->tft->fillRect(0, 223, TFT_WIDTH, 17, TFT_BLACK);
  ttgo->tft->setTextSize(1);
  ttgo->tft->drawCentreString(message, (TFT_WIDTH / 2), 228, 1);
  ttgo->tft->setTextSize(2);
}

void drawMainScreen(int timeOffset, char *message) {
  int8_t hh, mm;
  char timeStr[9];

  hh = (timeOffset % 86400) / 3600;
  mm = ((timeOffset % 86400) % 3600) / 60;
  sprintf(timeStr, "%02d:%02d", hh, mm);
  ttgo->tft->setTextSize(3);
  ttgo->tft->drawCentreString(timeStr, (TFT_WIDTH / 2), 8, 1);
  ttgo->tft->drawFastHLine(0, 40, TFT_WIDTH, TFT_WHITE);
  ttgo->tft->drawFastVLine(40, 40, 182, TFT_WHITE);
  ttgo->tft->drawChar('<', 11, 122);
  ttgo->tft->drawFastVLine(200, 40, 182, TFT_WHITE);
  ttgo->tft->drawChar('>', 211, 122);
  ttgo->tft->drawFastHLine(0, 222, TFT_WIDTH, TFT_WHITE);
  ttgo->tft->setTextSize(2);
  if (message) drawMessage(message);
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getName() == SENSOR_BLE_NAME) {
      advertisedDevice.getScan()->stop();
      pServerAddress = new BLEAddress(advertisedDevice.getAddress());
      drawMessage("CGM Sensor found! Connecting...");
    }
  }
};

void glucoseNotifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  char *received = (char*)pData;
  int32_t time;
  int32_t val;

  sscanf(received, "%10d|%4d", &time, &val);

  sensorBuffer.push(CGMeasurement{time, val});

  Serial.print("Received: ");
  Serial.print(time);
  Serial.print(" | ");
  Serial.println(val);

  newValue = true;
}

bool connectToServer(BLEAddress pAddress) {

  pClient = BLEDevice::createClient();
  pClient->connect(pAddress);

  cgmRemoteService = pClient->getService(CGM_SERVICE_UUID);
  if (cgmRemoteService == nullptr) {
    return false;
  }
  securityRemoteService = pClient->getService(SECURITY_SERVICE_UUID);
  if (securityRemoteService == nullptr) {
    return false;
  }
  cgmMeasurementRemoteCharacteristic = cgmRemoteService->getCharacteristic(CGM_MEASUREMENT_CHARACTERISTIC_UUID);
  if (cgmMeasurementRemoteCharacteristic == nullptr) {
    return false;
  }
  securityValueRemoteCharacteristic = securityRemoteService->getCharacteristic(SECURITY_VALUE_CHARACTERISTIC_UUID);
  if (securityValueRemoteCharacteristic == nullptr) {
    return false;
  }
  cgmTimeRemoteCharacteristic = cgmRemoteService->getCharacteristic(CGM_TIME_CHARACTERISTIC_UUID);
  if (cgmTimeRemoteCharacteristic == nullptr) {
    return false;
  }
  securityActionRemoteCharacteristic = securityRemoteService->getCharacteristic(SECURITY_ACTION_CHARACTERISTIC_UUID);
  if (securityActionRemoteCharacteristic == nullptr) {
    return false;
  }
  cgmMeasurementRemoteCharacteristic->registerForNotify(glucoseNotifyCallback);

  return true;
}


void setup() {
  Serial.begin(115200);

  ttgo = TTGOClass::getWatch();
  ttgo->begin();
  ttgo->openBL();

  ttgo->tft->setTextFont(1);
  ttgo->tft->fillScreen(TFT_BLACK);
  ttgo->tft->setTextColor(TFT_WHITE, TFT_BLACK);
  drawMainScreen(currentTime, "Starting...");
  delay(1500);

  drawMessage("Initializing BLE device...");
  BLEDevice::init("CGM Collector");
  delay(1500);

  drawMessage("Scanning for CGM sensor...");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(0);
  delay(1500);

  if (connectToServer(*pServerAddress)) {
    messageLine = "Connected to CGM Sensor!";
    connected = true;
  }
  else {
    messageLine = "Failed to connect. Something is wrong!";
  }
  drawMessage(messageLine);
  delay(1500);

  EEPROM.begin(EEPROM_SIZE);
  private_key = random_from_to(1, 100);
  client_public_key = ((int)pow(DH_COMMON_G, private_key)) % DH_COMMON_P;
  shared_key = EEPROM.readUInt(0);
  for (int i = 0; i < 4; ++i) {
    aes_key[i] = shared_key;
  }
}

void loop() {
  if (connected == false) while(1);

  currentState = static_cast<SecurityState>(atoi(securityActionRemoteCharacteristic->readValue().c_str()));

  switch (currentState) {
    case PAIR_0: 
      messageLine = "PAIR";

      server_public_key = atoi(securityValueRemoteCharacteristic->readValue().c_str());
      shared_key = ((int)pow(server_public_key, private_key)) % DH_COMMON_P;
      for (int i = 0; i < 4; ++i) {
        aes_key[i] = shared_key;
      }
      EEPROM.writeUInt(0, shared_key);
      EEPROM.commit();

      sprintf(answer, "%d", client_public_key);
      securityValueRemoteCharacteristic->writeValue(answer);
      sprintf(answer, "%d", PAIR_1);
      securityActionRemoteCharacteristic->writeValue(answer);
      break;
    
    case PAIR_1: 
      break;

    case AUTH_0: 
      messageLine = "AUTH";

      checkNum = atoi(securityValueRemoteCharacteristic->readValue().c_str());
      checkNum += shared_key;

      sprintf(answer, "%d", checkNum);
      securityValueRemoteCharacteristic->writeValue(answer);
      sprintf(answer, "%d", AUTH_1);
      securityActionRemoteCharacteristic->writeValue(answer);
      break;

    case AUTH_1: 
      break;

    case READY: 
      if (readInitial) {
        messageLine = "READ";

        std::string initial = cgmMeasurementRemoteCharacteristic->readValue();
        int32_t time;
        int32_t val;

        sscanf(initial.c_str(), "%10d|%4d", &time, &val);

        sensorBuffer.push(CGMeasurement{time, val});

        Serial.print("Read: ");
        Serial.print(time);
        Serial.print(" | ");
        Serial.println(val);

        sprintf(answer, "%10d", sensorBuffer.last().timeOffset);
        cgmTimeRemoteCharacteristic->writeValue(answer);

        readInitial = false; 
      }
      if (newValue) {
        messageLine = "NOTIFY";

        currentTime = sensorBuffer.last().timeOffset;

        ttgo->tft->fillRect(41, 41, 158, 180, TFT_BLACK);
        ypos = 42;
        char measurement[16];
        for (int i = 0; i < sensorBuffer.size(); ++i) {
          sprintf(measurement, "%02d:%02d|%.2f", ((sensorBuffer[i].timeOffset % 86400) / 3600), (((sensorBuffer[i].timeOffset % 86400) % 3600) / 60), (sensorBuffer[i].glucoseValue / 100.0));
          ttgo->tft->drawCentreString(measurement, (TFT_WIDTH / 2), ypos, 1);
          ypos += 18;
        }

        sprintf(answer, "%10d", sensorBuffer.last().timeOffset);
        cgmTimeRemoteCharacteristic->writeValue(answer);

        newValue = false;
      }
      break;
  }

  drawMainScreen(currentTime, messageLine);

  delay(1000);
}