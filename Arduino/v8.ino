// WiFi credentials
const char* WIFI_SSID = "WIFI SSID";
const char* WIFI_PASSWORD = "WIFI PASSWORD";

// User Login
#define USER_EMAIL "USER EMAIL"
#define USER_PASSWORD "USER PASSWORD"

// Firestore document paths
const char* ROOM_ENDPOINT = "PATH TO DEVICE DOCUMENT OF ESP8266 IN FIRESTORE";
const char* SOCKET_ONE = "PATH TO RELAY ONE IN FIRESTORE";
const char* SOCKET_TWO = "PATH TO RELAY TWO IN FIRESTORE";
const char* SOCKET_THREE = "PATH TO RELAY THREE IN FIRESTORE";
const char* MERALCO_CONVERSION_PATH = "PATH TO MERALCO CONVERSION IN FIRESTORE";
const char* PHONE_NUMBER = "PATH TO USER PHONE NUMBER IN FIRESTORE";

// ------------------------ DO NOT CHANGE ANYTHING BELOW ------------------------

// Add EEPROM for non-volatile memory storage
#include <EEPROM.h>

// Define EEPROM addresses
#define ENERGY_RESET_FLAG_ADDR 0
#define KWH_TO_PESO_ADDR 1
#define PHONE_NUMBER_ADDR 10
#define MAX_PHONE_NUMBER_LENGTH 15

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Firebase_ESP_Client.h>
#include <PZEM004Tv30.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <addons/TokenHelper.h>
#include <time.h>
#include <ESP8266HTTPClient.h>  // For HTTP requests
#include <WiFiClient.h>         // WiFi client
#include <LiquidCrystal_I2C.h>

// Firebase configuration
#define API_KEY "FIREBASE API KE"
#define FIREBASE_PROJECT_ID "FIREBASE PROJECT ID"

// Semaphore credentials
#define SEMAPHORE_API_KEY "SEMAPHORE API KEY"
#define SEMAPHORE_SENDERNAME "SEMAPHORE SENDER NAME"
String phoneNumber = "";  // This will be retrieved from Firestore

// Pin definitions
const int relayOnePin = 14;
const int relayTwoPin = 12;
const int relayThreePin = 13;
const int PZEM_RX_PIN = 0;
const int PZEM_TX_PIN = 2;

// Global variables
bool relayOneState = false;
bool relayTwoState = false;
bool relayThreeState = false;
float kWhToPeso = 1.0;
bool energyResetFlag = false;
float cachedKWhToPeso = 1.0;
double currentLimit = 0;
double setPesoTimer = 0;
bool currentLimitExceeded = false;
bool pesoLimitExceeded = false;

// Update intervals in milliseconds
const unsigned long ENERGY_UPDATE_INTERVAL = 20000;  // 20 seconds
const unsigned long OTHER_UPDATE_INTERVAL = 6800;   // 6.8 seconds
const unsigned long RESET_TIMER_UPDATE_INTERVAL = 3140; // 3.14 seconds
const unsigned long KWH_TO_PESO_UPDATE_INTERVAL = 10800000; // 3 hours
const unsigned long TIME_CHECK_INTERVAL = 5000;     // 5 seconds
const unsigned long ALERT_INTERVAL = 60000;         // 1 minute

// Time trackers
unsigned long lastEnergyUpdateTime = 0;
unsigned long lastOtherUpdateTime = 0;
unsigned long lastResetTimerUpdate = 0;
unsigned long lastTimeCheck = 0;
unsigned long lastAlertTime = 0;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// PZEM004T setup
SoftwareSerial pzemSWSerial(PZEM_RX_PIN, PZEM_TX_PIN);
PZEM004Tv30 pzem(pzemSWSerial);

// LCD setup
LiquidCrystal_I2C lcd(0x27, 16, 2);

// NTP server
const char* ntpServer = "asia.pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;
const int daylightOffset_sec = 0;

// Struct definition for SocketCache
struct SocketCache {
  bool state;
  unsigned long lastUpdate;
};

struct SocketSchedule {
  int timeOnMinutes;
  int timeOffMinutes;
  bool scheduleEnabled;
};

// Socket cache instances
SocketCache socketOneCache = { false, 0 };
SocketCache socketTwoCache = { false, 0 };
SocketCache socketThreeCache = { false, 0 };
const unsigned long SOCKET_UPDATE_INTERVAL = 5000;

SocketSchedule socketOneSchedule = { 0, 0, false };
SocketSchedule socketTwoSchedule = { 0, 0, false };
SocketSchedule socketThreeSchedule = { 0, 0, false };

// Function to send SMS via Semaphore API
bool sendSemaphoreSMS(const String& message, const String& number) {
  Serial.println("Sending Message...");

  WiFiClient client;  // Use regular WiFiClient for ESP8266

  HTTPClient http;

  if (http.begin(client, "http://semaphore.co/api/v4/messages")) {  // Initialize connection to Semaphore API
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");  // Set content type

    // Prepare the POST data (query parameters)
    String postData = "apikey=" + String(SEMAPHORE_API_KEY) +
                      "&number=" + urlEncode(number) +
                      "&message=" + urlEncode(message) +
                      "&sendername=" + urlEncode(String(SEMAPHORE_SENDERNAME));

    Serial.println("POST Data: " + postData);

    // Send the POST request
    int httpCode = http.POST(postData);  // Perform the POST request

    if (httpCode > 0) {  // Check HTTP response code
      Serial.printf("HTTP Response code: %d\n", httpCode);
      String payload = http.getString();  // Get the response payload
      Serial.println("Response payload: " + payload);

      // Parse response to check for success (simple check based on HTTP 200)
      if (httpCode == 200) {
        Serial.println("Message Sent Successfully!");
        return true;
      } else {
        Serial.println("Failed to send message. Check response payload for details.");
        return false;
      }
    } else {
      Serial.printf("HTTP POST failed, error: %s\n", http.errorToString(httpCode).c_str());
      return false;
    }

    http.end();  // Close the connection
  } else {
    Serial.println("HTTP connection failed.");
    return false;
  }
}

// URL Encode function (Arduino-friendly version)
String urlEncode(const String& str) {
  String encoded = "";
  char c;

  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      encoded += '%';
      encoded += String(c, HEX);  // Convert to percent-encoded hex
    }
  }

  return encoded;
}

// Function to retrieve phone_number from Firestore
void retrievePhoneNumber() {
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", PHONE_NUMBER, "phone_number")) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, fbdo.payload().c_str());

    if (!error) {
      const char* retrievedNumber = doc["fields"]["phone_number"]["stringValue"];
      if (retrievedNumber && strlen(retrievedNumber) > 0) {
        phoneNumber = String(retrievedNumber);  // Update the global phoneNumber variable
        savePhoneNumberToEEPROM(retrievedNumber);
        Serial.print("Phone Number retrieved and saved: ");
        Serial.println(phoneNumber);
      } else {
        Serial.println("Retrieved phone number is empty or null");
      }
    } else {
      Serial.println("Failed to deserialize phone number");
    }
  } else {
    Serial.println("Failed to retrieve phone number from Firestore");
  }
}

// Function to retrieve currentLimit and setPesoTimer
void retrieveFirestoreLimits() {
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", ROOM_ENDPOINT, "currentLimit,setPesoTimer")) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, fbdo.payload().c_str());

    if (!error) {
      currentLimit = doc["fields"]["currentLimit"]["doubleValue"];
      setPesoTimer = doc["fields"]["setPesoTimer"]["doubleValue"];
      Serial.println("Limits updated - Current: " + String(currentLimit) + ", Peso: " + String(setPesoTimer));
    } else {
      Serial.println("Failed to deserialize currentLimit/setPesoTimer");
    }
  } else {
    Serial.println("Failed to retrieve limits from Firestore");
  }
}

void checkConditionsAndSendAlerts() {
  float current = pzem.current();  // Retrieve current from sensor
  float pesoValue = pzem.energy() * cachedKWhToPeso;  // Peso value based on energy

  bool shouldSendAlert = false;
  String alertMessage = "";
  retrieveFirestoreLimits();

  // Check if current exceeds limit
  if (current > currentLimit && !currentLimitExceeded) {
    currentLimitExceeded = true;
    shouldSendAlert = true;
    alertMessage = "Current Has Exceeded Your Set Current Limit. ";
    delay(100);
    sendSemaphoreSMS(alertMessage, phoneNumber);
    alertMessage = "";
  } else if (current <= currentLimit && currentLimitExceeded) {
    currentLimitExceeded = false;
  }

  // Check if pesoValue exceeds setPesoTimer
  if (pesoValue > setPesoTimer && !pesoLimitExceeded) {
    pesoLimitExceeded = true;
    shouldSendAlert = true;
    alertMessage = "Device Consumption Has Exceeded Your Set Peso Limit. ";
    delay(100);
    sendSemaphoreSMS(alertMessage, phoneNumber);
    alertMessage = "";
  } else if (pesoValue <= setPesoTimer && pesoLimitExceeded) {
    pesoLimitExceeded = false;
  }

  // Send alert if needed and enough time has passed since the last alert
  unsigned long currentTime = millis();
  if (shouldSendAlert && (currentTime - lastAlertTime >= ALERT_INTERVAL)) {
    sendSemaphoreSMS(alertMessage, phoneNumber);
    lastAlertTime = currentTime;
  }

  // Debug output
  Serial.println("Current: " + String(current) + ", Limit: " + String(currentLimit));
  Serial.println("Peso Value: " + String(pesoValue) + ", Limit: " + String(setPesoTimer));
}

// Function to save phone number to EEPROM
void savePhoneNumberToEEPROM(const char* phoneNumber) {
  int len = strlen(phoneNumber);
  if (len > MAX_PHONE_NUMBER_LENGTH - 1) {
    Serial.println("Phone number is too long to store in EEPROM!");
    return;
  }

  // Write each character to EEPROM
  for (int i = 0; i < len; i++) {
    EEPROM.write(PHONE_NUMBER_ADDR + i, phoneNumber[i]);
  }

  // Add null terminator
  EEPROM.write(PHONE_NUMBER_ADDR + len, '\0');

  // Commit the changes to EEPROM
  if (EEPROM.commit()) {
    Serial.println("Phone number saved to EEPROM!");
  } else {
    Serial.println("Failed to commit phone number to EEPROM.");
  }
}

// Function to read phone number from EEPROM
void readPhoneNumberFromEEPROM(char* phoneNumber) {
  for (int i = 0; i < MAX_PHONE_NUMBER_LENGTH; i++) {
    phoneNumber[i] = EEPROM.read(PHONE_NUMBER_ADDR + i);
    if (phoneNumber[i] == '\0') {
      break;
    }
  }

  Serial.print("Phone number retrieved from EEPROM: ");
  Serial.println(phoneNumber);
}

void initialKWhToPesoFetch() {
  Serial.println("Performing kWhToPeso fetch from Firestore...");
  bool fetchSuccess = false;
  int retryCount = 0;
  const int maxRetries = 5;  // Increased retries for initial fetch

  while (!fetchSuccess && retryCount < maxRetries) {
    if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", MERALCO_CONVERSION_PATH, "kWhToPeso")) {
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, fbdo.payload().c_str());

      if (!error) {
        float newKWhToPeso = doc["fields"]["kWhToPeso"]["doubleValue"];

        // Validate the new value
        if (newKWhToPeso > 0 && !isnan(newKWhToPeso)) {
          cachedKWhToPeso = newKWhToPeso;
          EEPROM.put(KWH_TO_PESO_ADDR, cachedKWhToPeso);
          if (EEPROM.commit()) {
            Serial.printf("kWhToPeso set and saved to EEPROM: %.4f\n", cachedKWhToPeso);
          } else {
            Serial.println("EEPROM commit failed!");
          }
          fetchSuccess = true;
        } else {
          Serial.println("Invalid kWhToPeso value received");
        }
      }
    }
    if (!fetchSuccess) {
      retryCount++;
      if (retryCount < maxRetries) {
        Serial.printf("Retry attempt %d for initial kWhToPeso fetch\n", retryCount + 1);
        delay(100);  // Longer delay for initial fetch retries
      }
    }
  }

  if (!fetchSuccess) {
    Serial.println("Failed to fetch initial kWhToPeso from Firestore. Using cached value from EEPROM.");
  }
}

void setupWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nConnected with IP: " + WiFi.localIP().toString());
  delay(1000);
}

void setupFirebase() {
    // Set Firebase configuration and authentication
    config.api_key = API_KEY;
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;

    // Set the token status callback directly within the setup function
    config.token_status_callback = [](TokenInfo info) {
        // Check if the token status indicates an error with code -4
        if (info.status == token_status_error && info.error.code == -4) {
            Serial.println("Token error detected: code -4, connection lost. Restarting ESP...");
            lcd.setCursor(0, 0);
            lcd.println("Token error detected");
            lcd.setCursor(0, 1);
            lcd.println("Restarting ESP");
            delay(2000); // Short delay to allow serial message to be printed
            lcd.clear();
            ESP.restart(); // Restart the ESP device
        }
    };

    // Reconnect to the network and set buffer sizes
    Firebase.reconnectNetwork(true);
    fbdo.setBSSLBufferSize(1024, 256);
    fbdo.setResponseSize(512);

    // Begin Firebase
    Firebase.begin(&config, &auth);

    // Print confirmation of Firebase initialization
    Serial.println("Firebase initialized successfully!");
}

void setupPZEM() {
  pzemSWSerial.begin(9600);
  Serial.print("PZEM address: ");
  Serial.println(pzem.readAddress(), HEX);
}

void setupTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("\nWaiting for NTP time sync");
  delay(2000);
}

void printLocalTime() {
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);

  // Print current date and time in YYYY-MM-DD HH:MM:SS format
  Serial.printf("Current date and time: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeInfo->tm_year + 1900, timeInfo->tm_mon + 1, timeInfo->tm_mday,
                timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
}

void updateEnergyData() {

float voltage = pzem.voltage();
float current = pzem.current();
current = isnan(current) ? 0 : current;
float power = pzem.power();
float energy = pzem.energy();
float frequency = pzem.frequency();
float pf = pzem.pf();

  // Use cached kWhToPeso value
  float pesoValue = isnan(energy) ? 0 : energy * cachedKWhToPeso;

  // Get current time and date
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);
  char timeDate[30];  // Increased buffer size to accommodate AM/PM
  strftime(timeDate, sizeof(timeDate), "%a %I:%M:%S %p %m/%d", timeInfo);

  FirebaseJson content;
  content.set("fields/voltageState/doubleValue", voltage);
  content.set("fields/amperageState/doubleValue", current);
  content.set("fields/wattageState/doubleValue", power);
  content.set("fields/kWhState/doubleValue", energy);
  content.set("fields/pesoState/doubleValue", pesoValue);
  content.set("fields/pFState/doubleValue", pf);
  content.set("fields/lastUpdate/stringValue", timeDate);

    if (Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", ROOM_ENDPOINT, content.raw(),
                                         "voltageState,amperageState,wattageState,kWhState,pesoState,pFState,lastUpdate")) {
        Serial.println("Energy data updated successfully");
    } else {
        Serial.println("Failed to update energy data: " + fbdo.errorReason());
    }
}

void updateSocketStates() {
  updateSingleSocketState(SOCKET_ONE, "Socket One", relayOnePin, relayOneState, socketOneCache);
  updateSingleSocketState(SOCKET_TWO, "Socket Two", relayTwoPin, relayTwoState, socketTwoCache);
  updateSingleSocketState(SOCKET_THREE, "Socket Three", relayThreePin, relayThreeState, socketThreeCache);
}

void updateSingleSocketState(const char* socketPath, const char* socketName, int relayPin, bool& relayState, SocketCache& cache) {
  unsigned long currentTime = millis();
  if (currentTime - cache.lastUpdate >= SOCKET_UPDATE_INTERVAL) {
    if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", socketPath, "state")) {
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, fbdo.payload().c_str());

      if (!error) {
        bool newState = doc["fields"]["state"]["booleanValue"];
        if (newState != cache.state) {
          cache.state = newState;
          relayState = newState;
          digitalWrite(relayPin, relayState ? HIGH : LOW);
        }
        cache.lastUpdate = currentTime;
      }
    }
  }
}

void checkAndUpdateSocketState(const char* socketPath, const char* socketName) {
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", socketPath, "timeOn,timeOff,state")) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, fbdo.payload().c_str());
    ESP.wdtFeed();

    if (!error) {
      const char* timeOnStr = doc["fields"]["timeOn"]["stringValue"] | "(not set)";
      const char* timeOffStr = doc["fields"]["timeOff"]["stringValue"] | "(not set)";
      bool currentState = doc["fields"]["state"]["booleanValue"];

      // Get current time
      time_t now = time(nullptr);
      struct tm* currentTimeInfo = localtime(&now);
      char currentTimeStr[6];
      strftime(currentTimeStr, sizeof(currentTimeStr), "%H:%M", currentTimeInfo);

      bool stateChanged = false;
      bool newState = currentState;

      // Check if current time matches timeOn or timeOff
      if (strcmp(currentTimeStr, timeOnStr) == 0) {
        newState = true;
        stateChanged = true;
      } else if (strcmp(currentTimeStr, timeOffStr) == 0) {
        newState = false;
        stateChanged = true;
      }

      // If state needs to be changed, update Firestore
      if (stateChanged) {
        FirebaseJson content;
        content.set("fields/state/booleanValue", newState);
        if (Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", socketPath, content.raw(), "state")) {
          Serial.printf("%s state updated to %s at %s\n", socketName, newState ? "ON" : "OFF", currentTimeStr);
        } else {
          Serial.printf("Failed to update %s state in Firestore\n", socketName);
        }
      }

      // Debug logging
      Serial.printf("Socket: %s, TimeOn: %s, TimeOff: %s, Current: %s, State: %s\n",
                    socketName, timeOnStr, timeOffStr, currentTimeStr, currentState ? "ON" : "OFF");
    } else {
      Serial.printf("Failed to deserialize Firestore data for %s\n", socketName);
    }
  } else {
    Serial.printf("Failed to retrieve data for %s\n", socketName);
  }
}

void checkAndUpdateSocketStates() {
  checkAndUpdateSocketState(SOCKET_ONE, "Socket One");
  checkAndUpdateSocketState(SOCKET_TWO, "Socket Two");
  checkAndUpdateSocketState(SOCKET_THREE, "Socket Three");
}

void printResetTimers() {
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);

  // Calculate time until daily reset
  int hoursUntilReset = 23 - timeInfo->tm_hour;
  int minutesUntilReset = 59 - timeInfo->tm_min;
  int secondsUntilReset = 59 - timeInfo->tm_sec;

  Serial.println("Reset Timers:");
  Serial.printf("Time until daily reset: %02d:%02d:%02d\n", hoursUntilReset, minutesUntilReset, secondsUntilReset);
  Serial.println();
  // Print current date and time in YYYY-MM-DD HH:MM:SS format
  Serial.printf("Current date and time: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeInfo->tm_year + 1900, timeInfo->tm_mon + 1, timeInfo->tm_mday,
                timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
}

// Function to display kWh and pesoValue on the LCD
void displayKWhAndPeso() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("kWh: ");
  lcd.print(pzem.energy(), 2);

  lcd.setCursor(0, 1);
  lcd.print("Peso: ");
  lcd.print(pzem.energy() * cachedKWhToPeso, 2);
}

void performDailyReset() {
  pzem.resetEnergy();
  EEPROM.write(ENERGY_RESET_FLAG_ADDR, 0);
  EEPROM.commit();
  initialKWhToPesoFetch();
  energyResetFlag = true;
  Serial.println("Daily reset executed at 00:00");
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  lcd.init();
  lcd.backlight();

  // Retrieve the stored energyResetFlag from EEPROM
  energyResetFlag = EEPROM.read(ENERGY_RESET_FLAG_ADDR) == 1;

  // Retrieve kWhToPeso from EEPROM
  EEPROM.get(KWH_TO_PESO_ADDR, cachedKWhToPeso);

  // If the retrieved value is NaN or 0, set a default value
  if (isnan(cachedKWhToPeso) || cachedKWhToPeso == 0) {
    cachedKWhToPeso = 1.0;
  }

  Serial.printf("Loaded kWhToPeso from EEPROM: %.4f\n", cachedKWhToPeso);

  pinMode(relayOnePin, OUTPUT);
  pinMode(relayTwoPin, OUTPUT);
  pinMode(relayThreePin, OUTPUT);
  digitalWrite(relayOnePin, LOW);
  digitalWrite(relayTwoPin, LOW);
  digitalWrite(relayThreePin, LOW);

  setupWiFi();
  setupFirebase();
  setupPZEM();
  setupTime();
  printLocalTime();
  initialKWhToPesoFetch();
  retrievePhoneNumber();

  char retrievedPhoneNumber[MAX_PHONE_NUMBER_LENGTH];
  readPhoneNumberFromEEPROM(retrievedPhoneNumber);
}

void loop() {
  WiFiClient client;
  ESP.wdtFeed();
  if (client.connect("www.google.com", 80)) {
    unsigned long currentTime = millis();

    // Check current system time
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);

    // Daily reset check (runs once per day)
    if (timeInfo->tm_hour == 0 && timeInfo->tm_min == 0 && !energyResetFlag) {
      performDailyReset();
    } else if (timeInfo->tm_hour == 0 && timeInfo->tm_min == 1) {
      energyResetFlag = false;  // Reset the flag at 00:01
    }

    // Other updates every 6.8 seconds
    if (currentTime - lastOtherUpdateTime >= OTHER_UPDATE_INTERVAL) {
        lastOtherUpdateTime = currentTime;
        updateEnergyData();
        yield();
    }

    // Reset timer update and LCD display every 3.14 seconds
    if (currentTime - lastResetTimerUpdate >= RESET_TIMER_UPDATE_INTERVAL) {
        lastResetTimerUpdate = currentTime;
        printResetTimers();
        displayKWhAndPeso();
        updateSocketStates();
        yield();
    }

    // Check time and update other states every 5 seconds
    if (currentTime - lastTimeCheck >= TIME_CHECK_INTERVAL) {
        lastTimeCheck = currentTime;
        checkConditionsAndSendAlerts();
        checkAndUpdateSocketStates();
        yield();
    }

    client.stop();  // Close the connection
  } else {
    // Handle offline mode
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("  Offline Mode");
    delay(1000);
    displayKWhAndPeso();
  }
}
