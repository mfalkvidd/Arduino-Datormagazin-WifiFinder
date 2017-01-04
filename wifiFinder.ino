#include "Arduino.h"
#include <EDB.h>

//Use SPIFFS FS as data storage
#include <FS.h>

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
extern "C" {
  #include <user_interface.h>
}

#define NUM_ROWS 4096 // Will we encounter more than 4096 access points?
#define RECHECK_INTERVAL (2*60*1000) // Retry AP if more than 2 minutes has passed sine we last tried.
#define LED_PIN D4
#define SLEEP_TIME (20*1000) // How often (in seconds) to scan for APs
#define ALERT_INTERVAL (5*60*1000) // Don't beep more often than every 5 minutes
#define DB_CLOSE_INTERVAL (6*60*1000) // Write out db every 6 minutes.
#define VERIFY_MESSAGE "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
#define AP_OK 0
#define AP_CONNECT_FAILED 1
#define AP_TEST_FAILED 2
#define AP_ENCRYPTED 4

const char* db_name = "/db/wifiAP2.db";
File dbFile;
unsigned long lastCloseTime = 0;
unsigned long lastAlertTime = 0;
// Arbitrary record definition for this table.
// This should be modified to reflect your record needs.
struct WifiAP {
  unsigned long id;
  char BSSIDstr[18];
  char SSIDstr[33];
  long lastChecked;
  byte lastStatus;
  uint8_t encryptionType;
}
wifiAP;

// Empty AP
const WifiAP noAP = {};

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
  pinMode(LED_PIN, OUTPUT);     // Initialize the LED_BUILTIN pin as an output
  Serial.begin(115200);
  randomSeed(analogRead(0));

  SPIFFS.begin();
  digitalWrite(LED_PIN, !HIGH);
  delay(5000);
  digitalWrite(LED_PIN, !LOW);

  openDB();
}

void loop()
{
  doScan();
  Serial.printf("Last close time: %lu Current time: %lu\n", lastCloseTime, millis());
  if (lastCloseTime + DB_CLOSE_INTERVAL < millis()) {
    Serial.printf("Last close time + DB: %lu\n", lastCloseTime + DB_CLOSE_INTERVAL);
    dbFile.close();
    openDB();
    lastCloseTime = millis();
  }
  wifi_set_sleep_type(LIGHT_SLEEP_T);
  delay(SLEEP_TIME);
}

// utility functions

void recordLimit() {
  Serial.print("Record Limit: ");
  Serial.println(db.limit());
}

void deleteOneRecord(int recno) {
  Serial.print("Deleting recno: ");
  Serial.println(recno);
  db.deleteRec(recno);
}

void deleteAll() {
  Serial.print("Truncating table... ");
  db.clear();
  Serial.println("DONE");
}

void countRecords() {
  Serial.print("Record Count: ");
  Serial.println(db.count());
}

void selectAll() {
  Serial.print("selectAll is displaying ");
  Serial.print(db.count());
  Serial.println(" records");
  for (int recno = 1; recno <= db.count(); recno++)
  {
    EDB_Status result = db.readRec(recno, EDB_REC wifiAP);
    if (result == EDB_OK)
    {
      printRecord(wifiAP);
    }
    else printError(result);
  }
}

WifiAP findByBSSID(String BSSIDstr) {
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
        return foundAP;
      }
    }
    else printError(result);
  }
  return noAP;
}

void insertOneRecord(int recno)
{
  Serial.print("Inserting record at recno: ");
  Serial.print(recno);
  Serial.print("... ");
  //wifiAP.id = recno;
  //wifiAP.temperature = random(1, 125);
  EDB_Status result = db.insertRec(recno, EDB_REC wifiAP);
  if (result != EDB_OK) printError(result);
  Serial.println("DONE");
}

void appendOneRecord(int id)
{
  Serial.print("Appending record... ");
  //wifiAP.id = id;
  //wifiAP.temperature = random(1, 125);
  EDB_Status result = db.appendRec(EDB_REC wifiAP);
  if (result != EDB_OK) printError(result);
  Serial.println("DONE");
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
  if (n == 0)
    Serial.println("no networks found");
  else
  {
    Serial.print(n);
    Serial.println(" networks found");
    for (int i = 0; i < n; ++i)
    {
      // Print SSID and RSSI for each network found
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.BSSIDstr(i));
      Serial.print(" - ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : "*");

      unsigned long currentTime = millis();
      WifiAP theAP = findByBSSID(WiFi.BSSIDstr(i));
      Serial.print("findByBSSID returned: ");
      printRecord(theAP);
      WiFi.SSID(i).toCharArray(theAP.SSIDstr, sizeof(theAP.SSIDstr));
      theAP.encryptionType = WiFi.encryptionType(i);

      if (WiFi.encryptionType(i) != ENC_TYPE_NONE) {
        Serial.print("Skipping encrypted AP ");
        Serial.println(WiFi.SSID(i));
        WiFi.BSSIDstr(i).toCharArray(theAP.BSSIDstr, sizeof(theAP.BSSIDstr));
        theAP.lastChecked = currentTime;
        theAP.lastStatus = AP_ENCRYPTED;
        saveAP(theAP);
      } else {
        // Found an open wifi hotspot
        // Check the database for when it was last checked
        // If the time in the database is in the future, we have rebooted so we need to check again.
        // If the time in the database is older than RECHECK_INTERVAL, check again
        // If there is no entry in the database (returns noAP which is all zeros), check again
        if (theAP.BSSIDstr != "") {
          // The AP did not exist earlier. Fill in the BSSID (the rest were already filled in above)
          WiFi.BSSIDstr(i).toCharArray(theAP.BSSIDstr, sizeof(theAP.BSSIDstr));
        }
        if (theAP.lastChecked + RECHECK_INTERVAL < currentTime || theAP.lastChecked > currentTime || theAP.lastChecked == 0) {
          Serial.println("Time to check the AP");
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
    }
  }
}

void edbDemo() {
  recordLimit();
  countRecords();
  countRecords();
  selectAll();
  countRecords();
  selectAll();
  countRecords();
  selectAll();
  countRecords();
  selectAll();
  selectAll();
  countRecords();
  deleteAll();
  Serial.println("Use insertRec() and deleteRec() carefully, they can be slow");
  countRecords();
  for (int i = 1; i <= 20; i++) insertOneRecord(1);  // inserting from the beginning gets slower and slower
  countRecords();
  for (int i = 1; i <= 20; i++) deleteOneRecord(1);  // deleting records from the beginning is slower than from the end
  countRecords();
}

void printRecord(struct WifiAP wifiAP) {
  Serial.print(" ID: ");
  Serial.print(wifiAP.id);
  Serial.print(" BSSID: ");
  Serial.print(wifiAP.BSSIDstr);
  Serial.print(" SSID: ");
  Serial.print(wifiAP.SSIDstr);
  Serial.print(" last checked: ");
  Serial.print(wifiAP.lastChecked);
  Serial.print(" status: ");
  Serial.print(wifiAP.lastStatus);
  Serial.print(" encryption type: ");
  Serial.println(wifiAP.encryptionType);
}

boolean testConnection() {
  boolean result = false;
  Serial.println("Testing connection");
  if ((WiFi.status() != WL_CONNECTED)) {
    Serial.println("Not connected!");
    return false;
  }

  HTTPClient http;

  Serial.print("[HTTP] begin...\n");
  //http.begin("https://192.168.1.12/test.html", "7a 9c f4 db 40 d3 62 5a 6e 21 bc 5c cc 66 c8 3e a1 45 59 38"); //HTTPS
  http.begin("http://captive.apple.com/hotspot-detect.html");

  Serial.print("[HTTP] GET...\n");
  // start connection and send HTTP header
  int httpCode = http.GET();

  // httpCode will be negative on error
  if (httpCode > 0) {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] GET... code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      if (payload == VERIFY_MESSAGE) {
        Serial.println("Success!");
        result = true;
      } else {
        Serial.println("Contents did not match: ");
        Serial.println(payload);
      }
    }
  } else {
    Serial.printf("[HTTP] GET... failed, error: % s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return result;
}

void connectToTestAP() {
  char ssid[] = "SAMUIBNB";
  char key[] = "samuibnbvilla117";
  connectToAP(ssid, key);
}

boolean connectToAP(char* ssid, char* key) {
  Serial.println("Adding AP");
  WiFi.begin(ssid, key);
  Serial.println("AP added");
  int status = WiFi.waitForConnectResult();
  if (status != WL_CONNECTED) {
    Serial.println("Connection Failed");
    return false;
  }
  return true;
}

void signalUser() {
  if (lastAlertTime == 0 || millis() > lastAlertTime + ALERT_INTERVAL) {
    analogWrite(D2, 512);
    delay(1000);
    analogWrite(D2, 0);
    lastAlertTime = millis();
  }
}

void saveAP(WifiAP theAP) {
  EDB_Status result;
  if (theAP.id == 0) {
    // 0 is reserved for "new"
    // First available record is count + 1
    theAP.id = db.count () + 1;
    Serial.print("Appending record at recno: ");
    Serial.println(theAP.id);
    result = db.appendRec(EDB_REC theAP);
  } else {
    Serial.print("Updating record: ");
    printRecord(theAP);
    result = db.updateRec(theAP.id, EDB_REC theAP);
  }
  if (result != EDB_OK) printError(result);
}

void openDB() {
  if (SPIFFS.exists(db_name)) {

    dbFile = SPIFFS.open(db_name, "r+");

    if (dbFile) {
      Serial.print("Opening current table... ");
      EDB_Status result = db.open(0);
      if (result == EDB_OK) {
        Serial.println("DONE");
      } else {
        Serial.println("ERROR");
        Serial.println("Did not find database in the file " + String(db_name));
        Serial.print("Creating new table... ");
        db.create(0, NUM_ROWS * (unsigned int)sizeof(wifiAP), (unsigned int)sizeof(wifiAP));
        Serial.println("DONE");
        return;
      }
    } else {
      Serial.println("Could not open file " + String(db_name));
      // TODO check error Serial.printf("errno %i\n", SPIFFS_errno(&dbFile));
      return;
    }
  } else {
    Serial.print("Creating table... ");
    // create table at with starting address 0
    dbFile = SPIFFS.open(db_name, "w+");
    db.create(0, NUM_ROWS * (unsigned int)sizeof(wifiAP), (unsigned int)sizeof(wifiAP));
    Serial.println("DONE");
  }
  recordLimit();
  selectAll();
}

void showScanDone(byte numAPs) {
  if (numAPs == 0) {
    digitalWrite(LED_PIN, !HIGH);
    delay(1000);
    digitalWrite(LED_PIN, !LOW);
  } else {
    for (byte n = 0; n < numAPs; n++) {
      digitalWrite(LED_PIN, !HIGH);
      delay(500);
      digitalWrite(LED_PIN, !LOW);
      delay(250);
    }
  }
}

