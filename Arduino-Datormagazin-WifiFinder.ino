#include <EDB.h>
#include <FS.h> // For SPIFFS
#include "limits.h"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
extern "C" { // For ESP sleep
#include <user_interface.h>
}

#define NUM_ROWS 8192 // Maximum umber of access points in the database
#define RECHECK_INTERVAL (2*60*1000) // Retry AP if more than 2 minutes has passed sine we last tried.
#define LED_PIN D4
#define SLEEP_TIME (1) // How often (in milliseconds) to scan for APs
#define ALERT_INTERVAL (5*60*1000) // Don't beep more often than every 5 minutes
#define DB_CLOSE_INTERVAL (6*60*1000) // Write out db every 6 minutes - increase to put less strain on the flash.
#define VERIFY_URL "http://captive.apple.com/hotspot-detect.html"
#define VERIFY_MESSAGE "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
#define AP_OK 0
#define AP_CONNECT_FAILED 1
#define AP_TEST_FAILED 2
#define AP_ENCRYPTED 4
#define ON LOW // The led on Wemos D1 Mini is lit when the pin is pulled low
#define OFF HIGH
#define WAIT_LIMIT 100
#define LRU_CACHE_SIZE 512

const char* db_name = "/db/wifiAP.db";
File dbFile;
unsigned long lastCloseTime = 0;
unsigned long lastAlertTime = 0;

// This is what we save to the DB
struct WifiAP {
  unsigned long id;
  char BSSIDstr[18];
  char SSIDstr[33];
  unsigned long lastChecked;
  byte lastStatus;
  uint8_t encryptionType;
}
wifiAP;

struct SeenAP {
  char BSSIDstr[18];
  unsigned long lastSeen;
};

struct SeenAP SeenAPs[LRU_CACHE_SIZE] = {};

// Empty AP
const WifiAP noAP = {};

boolean saveSeenAP(String BSSIDstr) {
  unsigned long startTime = millis();
  boolean retval = true;
  signed int LRUPos = getLRUPos(BSSIDstr);
  if (LRUPos == -1) {
    saveNewSeenAP(BSSIDstr);
  } else {
    SeenAPs[LRUPos].lastSeen = millis();
    retval = false;
  }
  Serial.print("saveSeenAP took ");
  Serial.println(millis() - startTime);
  return retval;
}

signed int saveNewSeenAP(String BSSIDstr) {
  // Saves the AP in the oldest position in the LRU cache and returns the position
  signed int pos = findOldestPos();
  SeenAPs[pos].lastSeen = millis();
  BSSIDstr.toCharArray(SeenAPs[pos].BSSIDstr, 18);
  return pos;
}

signed int findOldestPos() {
  unsigned long startTime = millis();
  unsigned long oldestTime = ULONG_MAX;
  signed int oldestPos = 0;
  for (signed int pos = 0; pos < LRU_CACHE_SIZE; pos++ ) {
    if (SeenAPs[pos].lastSeen < oldestTime) {
      oldestTime = SeenAPs[pos].lastSeen;
      oldestPos = pos;
    }
  }
  Serial.print("findOldest took ");
  Serial.println(millis() - startTime);
  return oldestPos;
}

signed int getLRUPos(String BSSIDstr) {
  for (signed int pos = 0; pos < LRU_CACHE_SIZE; pos++ ) {
    if (String(SeenAPs[pos].BSSIDstr) == BSSIDstr) {
      return pos;
    }
  }
  return -1;
}

void writer (unsigned long address, byte data) {
  dbFile.seek(address, SeekSet);
  dbFile.write(data);
  dbFile.flush();
}

byte reader (unsigned long address) {
  dbFile.seek(address, SeekSet);
  return dbFile.read();
}

// Create an EDB object with the appropriate write and read handlers
EDB db(&writer, &reader);

// Run the demo
void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  Serial.println("Starting in 15 seconds");
  randomSeed(analogRead(0));

  SPIFFS.begin();
  digitalWrite(LED_PIN, ON);
  delay(15000);
  digitalWrite(LED_PIN, OFF);

  openDB();
  showAll();
}

void loop()
{
  unsigned long startTime = millis();
  doScan();
  Serial.printf("Last close time: %lu Current time: %lu\n", lastCloseTime, millis());
  if (lastCloseTime + DB_CLOSE_INTERVAL < millis()) {
    Serial.printf("Last close time + DB: %lu\n", lastCloseTime + DB_CLOSE_INTERVAL);
    lastCloseTime = millis();
    dbFile.close();
    openDB();
  }
  unsigned long timeElapsed = millis() - startTime;
  unsigned long timeToSleep = SLEEP_TIME - timeElapsed;
  if (timeToSleep < SLEEP_TIME) {
    wifi_set_sleep_type(LIGHT_SLEEP_T);
    delay(timeToSleep);
  }
}

void showAll() {
  Serial.print("showAll is displaying ");
  Serial.print(db.count());
  Serial.print(" records. Max is ");
  Serial.println(db.limit());
  for (int recno = 1; recno <= db.count(); recno++)
  {
    EDB_Status result = db.readRec(recno, EDB_REC wifiAP);
    if (result == EDB_OK)
    {
      printRecord(wifiAP);
    } else {
      printError(result);
    }
  }
}

WifiAP findByBSSID(String BSSIDstr) {
  unsigned long startTime = millis();
  WifiAP foundAP;
  Serial.print("findByBSSID is searching for ");
  Serial.println(BSSIDstr);
  for (int recno = 1; recno <= db.count(); recno++)
  {
    EDB_Status result = db.readRec(recno, EDB_REC foundAP);
    if (result == EDB_OK)
    {
      if (String(foundAP.BSSIDstr) == BSSIDstr) {
        foundAP.id = recno;
        unsigned long searchTime = millis() - startTime;
        Serial.printf("Find took %lu\n", searchTime);
        return foundAP;
      }
    }
    else printError(result);
  }
  return noAP;
}

void printError(EDB_Status err)
{
  Serial.print("ERROR: ");
  switch (err)
  {
    case EDB_OUT_OF_RANGE:
      Serial.println("Recno out of range");
      break;
    case EDB_TABLE_FULL:
      Serial.println("Table full");
      break;
    case EDB_OK:
    default:
      Serial.println("OK");
      break;
  }
}

void doScan() {
  Serial.print("scan start...");

  // WiFi.scanNetworks will return the number of networks found
  int n = WiFi.scanNetworks();
  showScanDone(n);
  if (n == 0) {
    return;
  }

  for (int i = 0; i < n; ++i)
  {
    // Print SSID and RSSI for each network found
    Serial.print(WiFi.BSSIDstr(i));
    Serial.print(" - ");
    Serial.print(WiFi.SSID(i));
    Serial.print(" (");
    Serial.print(WiFi.RSSI(i));
    Serial.print(")");
    Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : "*");

    unsigned long currentTime = millis();
    if (!saveSeenAP(WiFi.BSSIDstr(i))) {
      // We have already seen this AP recently. Skip it.
      continue;
    }
    WifiAP theAP = findByBSSID(WiFi.BSSIDstr(i));
    Serial.print("findByBSSID returned:");
    printRecord(theAP);
    WiFi.SSID(i).toCharArray(theAP.SSIDstr, sizeof(theAP.SSIDstr)); // Copy SSID to theAP
    WiFi.BSSIDstr(i).toCharArray(theAP.BSSIDstr, sizeof(theAP.BSSIDstr)); // Copy BSSID to theAP
    theAP.encryptionType = WiFi.encryptionType(i);

    if (WiFi.encryptionType(i) != ENC_TYPE_NONE) {
      Serial.print("Skipping encrypted AP ");
      Serial.println(WiFi.SSID(i));
      if (theAP.lastChecked == 0) {
        // Only write to flash the first time we see this AP, to not wear out the flash
        theAP.lastChecked = currentTime;
        theAP.lastStatus = AP_ENCRYPTED;
        saveAP(theAP);
      }
      continue;
    }

    // Found an open wifi hotspot
    if (!timeToCheck(theAP.lastChecked, currentTime)) {
      Serial.printf("Not time to check %s yet\n", theAP.SSIDstr);
      continue;
    }

    Serial.printf("Time to check %s\n", theAP.SSIDstr);
    theAP.lastChecked = currentTime;
    if (!connectToAP(theAP.SSIDstr, NULL)) {
      Serial.printf("Connection to %s failed\n", theAP.SSIDstr);
      theAP.lastStatus = AP_CONNECT_FAILED;
      saveAP(theAP);
      continue;
    }

    if (!testConnection()) {
      Serial.println("Connection test failed!");
      theAP.lastStatus = AP_TEST_FAILED;
      saveAP(theAP);
      continue;
    }

    Serial.printf("Successfully connected to %s!\n", theAP.SSIDstr);
    theAP.lastStatus = AP_OK;
    saveAP(theAP);
    signalUser();
  }
}

boolean timeToCheck(unsigned long lastChecked, unsigned long currentTime) {
  return (lastChecked + RECHECK_INTERVAL < currentTime || lastChecked > currentTime || lastChecked == 0);
}

void printRecord(struct WifiAP wifiAP) {
  Serial.print(" ID: ");
  Serial.print(wifiAP.id);
  Serial.print(" BSSID: ");
  Serial.print(wifiAP.BSSIDstr);
  Serial.print(" SSID: ");
  Serial.print(wifiAP.SSIDstr);
  Serial.print(" timestamp: ");
  Serial.print(wifiAP.lastChecked);
  Serial.print(" status: ");
  Serial.print(statusToText(wifiAP.lastStatus));
  Serial.print(" encryption type: ");
  Serial.println(encryptionTypeToText(wifiAP.encryptionType));
}

String encryptionTypeToText(byte encType) {
  switch (encType) {
    case ENC_TYPE_WEP:
      return "WEP";
    case ENC_TYPE_TKIP:
      return "WPA / PSK (TKIP)";
    case ENC_TYPE_CCMP:
      return "WPA2 / PSK (CCMP)";
    case ENC_TYPE_NONE:
      return "open network";
    case ENC_TYPE_AUTO:
      return "WPA / WPA2 / PSK (AUTO)";
    default:
      return "0";
  }
}

String statusToText(byte status) {
  switch (status) {
    case AP_OK:
      return "OK";
    case AP_CONNECT_FAILED:
      return "Failed to connect to AP";
    case AP_TEST_FAILED:
      return "Connection test failed";
    case AP_ENCRYPTED:
      return "Encrypted";
  }
}

boolean testConnection() {
  boolean result = false;
  Serial.println("Testing connection");
  if ((WiFi.status() != WL_CONNECTED)) {
    Serial.println("Not connected!");
    return false;
  }

  HTTPClient http;
  http.begin(VERIFY_URL);

  Serial.print("[HTTP] GET...\n");
  // start connection and send HTTP header
  int httpCode = http.GET();

  // httpCode will be negative on error
  if (httpCode <= 0) {
    Serial.printf("[HTTP] GET... failed, error: % s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("[HTTP] GET... returned: %d\n", httpCode);
  }

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    if (payload == VERIFY_MESSAGE) {
      Serial.println("Success!");
      result = true;
    } else {
      Serial.println("Contents did not match:");
      Serial.println(payload);
    }
  }

  http.end();
  return result;
}

boolean connectToAP(char* ssid, char* key) {
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, key);
  byte waitCount = 0;
  int status;
  while (status = WiFi.status() == WL_DISCONNECTED && waitCount < WAIT_LIMIT) {
    delay(100);
    waitCount++;
  }
  if (status != WL_CONNECTED) {
    Serial.println("Connection Failed");
    return false;
  }
  Serial.println("Connection successful!");
  return true;
}

void signalUser() {
  if (lastAlertTime == 0 || millis() > lastAlertTime + ALERT_INTERVAL) {
    analogWrite(D2, 512);
    delay(2000);
    analogWrite(D2, 0);
    lastAlertTime = millis();
  }
}

void saveAP(WifiAP theAP) {
  EDB_Status result;
  if (theAP.id == 0) {
    // 0 is reserved for "new"
    // First available record is count + 1
    theAP.id = db.count() + 1;
    Serial.print("Appending record at recno: ");
    Serial.println(theAP.id);
    result = db.appendRec(EDB_REC theAP);
  } else {
    Serial.print("Updating record: ");
    result = db.updateRec(theAP.id, EDB_REC theAP);
  }
  printRecord(theAP);
  if (result != EDB_OK) printError(result);
}

void stopEverything() {
  Serial.println("Halting");
  while (true);
}

void openDB() {
  if (SPIFFS.exists(db_name)) {
    dbFile = SPIFFS.open(db_name, "r+");
  } else {
    dbFile = SPIFFS.open(db_name, "w+");
    createTable();
  }

  if (!dbFile) {
    Serial.println("Could not open file " + String(db_name));
    stopEverything();
  }

  Serial.print("Opening table... ");
  EDB_Status result = db.open(0);
  if (result == EDB_OK) {
    Serial.println("DONE");
    return;
  } else {
    Serial.println("ERROR");
    Serial.println("Did not find database in the file " + String(db_name));
  }
  createTable();
}

void createTable() {
  Serial.print("Creating new table... ");
  EDB_Status result = db.create(0, NUM_ROWS * (unsigned int)sizeof(wifiAP), (unsigned int)sizeof(wifiAP));
  if (result != EDB_OK) {
    Serial.println("ERROR");
    stopEverything();
  }
  Serial.println("DONE");
}

void showScanDone(byte numAPs) {
  return;
  if (numAPs == 0) {
    Serial.println("No networks found");
    digitalWrite(LED_PIN, ON);
    delay(1000);
    digitalWrite(LED_PIN, OFF);
    return;
  }

  Serial.printf("%d networks found\n", numAPs);
  for (byte n = 0; n < numAPs; n++) {
    digitalWrite(LED_PIN, ON);
    delay(400);
    digitalWrite(LED_PIN, OFF);
    delay(150);
  }
}

