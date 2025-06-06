#include <HX711_ADC.h>
#include <Wire.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#if defined(ESP8266) || defined(ESP32) || defined(AVR)
#include <EEPROM.h>
#endif

// --- Pins ---
const int HX711_dout1 = 4;
const int HX711_sck1  = 5;
const int HX711_dout2 = 6;
const int HX711_sck2  = 7;
const int chipSelect  = 10;
const int led         = 8;

// --- HX711 Instances ---
HX711_ADC LoadCell1(HX711_dout1, HX711_sck1);
HX711_ADC LoadCell2(HX711_dout2, HX711_sck2);

RTC_DS1307 rtc;

// --- EEPROM Calibration Addresses ---
const int calVal1_eepromAddress = 0;
const int calVal2_eepromAddress = 10;

// --- Other Variables ---
char filename[15];  // 8.3 format
int selectedCell = 1;
unsigned long lastLogTime = 0;
const unsigned long logInterval = 60000; // adjust for logging interval between measurements (milliseconds)

// --- Function Prototypes ---
void processSerialCommands();
void calibrateSelectedLoadCell();
void handleCalibrationInput();

void setup() {
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);

  Serial.begin(57600);
  delay(100);
  Serial.println(F("Starting..."));

  // --- RTC Init ---
  if (!rtc.begin()) {
    Serial.println(F("Couldn't find RTC"));
    while (1);
  }

  DateTime now = rtc.now();
  Serial.print(F("RTC reported time: "));
  Serial.println(now.timestamp(DateTime::TIMESTAMP_FULL));

  if (!rtc.isrunning()) {
    Serial.println(F("RTC is NOT running, setting to compile time."));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  } else {
    Serial.println(F("RTC is running."));
  }

  if (now.year() < 2020) {
    Serial.println(F("Warning: RTC time seems invalid. Battery may be missing or dead."));
  }

  // --- Create Filename (8.3 format) ---
  snprintf(filename, sizeof(filename), "L%02d%02d%02d.TXT", now.year() % 100, now.month(), now.day());
  Serial.print(F("Logging to file: "));
  Serial.println(filename);

  // --- SD Init ---
  Serial.print(F("Initializing SD card..."));
  if (!SD.begin(chipSelect)) {
    Serial.println(F("SD card initialization failed!"));
    while (1);
  }
  Serial.println(F("SD card ready."));

  if (!SD.exists(filename)) {
    File dataFile = SD.open(filename, FILE_WRITE);
    if (dataFile) {
      dataFile.println(F("Timestamp, weight 1, weight 2"));
      dataFile.close();
      Serial.println(F("Created new log file with header."));
    } else {
      Serial.println(F("Failed to create file."));
    }
  }

  // --- Load Cell 1 Init ---
  float cal1;
EEPROM.get(calVal1_eepromAddress, cal1);
if (isnan(cal1) || cal1 == 0) {
  Serial.println("Invalid cal factor in EEPROM. Using default.");
  cal1 = 696.0;  // replace with your known good value
}
Serial.print("Using LoadCell1 cal factor: ");
Serial.println(cal1);
LoadCell1.begin();
LoadCell1.setCalFactor(cal1);
  LoadCell1.setSamplesInUse(3);
  LoadCell1.start(2000, true);
  if (LoadCell1.getTareTimeoutFlag()) {
    Serial.println(F("Tare timeout on LoadCell1"));
    while (1);
  }
  LoadCell1.setCalFactor(cal1);

  // --- Load Cell 2 Init ---
  float cal2 = 345.0;
  EEPROM.get(calVal2_eepromAddress, cal2);
  LoadCell2.begin();
  LoadCell2.setSamplesInUse(3);
  LoadCell2.start(2000, true);
  if (LoadCell2.getTareTimeoutFlag()) {
    Serial.println(F("Tare timeout on LoadCell2"));
    while (1);
  }
  LoadCell2.setCalFactor(cal2);

  Serial.println(F("System ready. Send '1' or '2' to calibrate LoadCell1 or LoadCell2."));
  Serial.println(F("To set RTC time, use: set YYYY MM DD HH MM SS"));
  Serial.println(F("To view current time, send: time"));
}

void loop() {
  // --- Continuously update sensors for responsive data ---
  LoadCell1.update();
  LoadCell2.update();

  // --- Log once per minute ---
  if (millis() - lastLogTime >= logInterval) {
    float weight1 = LoadCell1.getData();
    float weight2 = LoadCell2.getData();
    DateTime now = rtc.now();

    // --- LED ON during SD write (1-second blink) ---
    digitalWrite(led, HIGH);

    // --- Serial print ---
    Serial.print(now.timestamp(DateTime::TIMESTAMP_FULL));
    Serial.print(F(" - W1: "));
    Serial.print(weight1, 2);
    Serial.print(F(" g, W2: "));
    Serial.println(weight2, 2);

    // --- Write to SD ---
    File dataFile = SD.open(filename, FILE_WRITE);
    if (dataFile) {
      dataFile.print(now.year(), DEC); dataFile.print("-");
      dataFile.print(now.month(), DEC); dataFile.print("-");
      dataFile.print(now.day(), DEC); dataFile.print(" ");
      dataFile.print(now.hour(), DEC); dataFile.print(":");
      dataFile.print(now.minute(), DEC); dataFile.print(":");
      dataFile.print(now.second(), DEC); dataFile.print(",");
      dataFile.print(weight1, 2); dataFile.print(",");
      dataFile.println(weight2, 2);
      dataFile.close();
    } else {
      Serial.println(F("SD write error."));
    }

    delay(1000);  // keep LED on for visibility
    digitalWrite(led, LOW);  // signal safe to remove card

    lastLogTime = millis();
  }

  // Optional: check for commands
  processSerialCommands();
  handleCalibrationInput();

  // Show tare status
  if (LoadCell1.getTareStatus()) Serial.println(F("Tare complete on LoadCell1"));
  if (LoadCell2.getTareStatus()) Serial.println(F("Tare complete on LoadCell2"));
}

void handleCalibrationInput() {
  if (Serial.available()) {
    char cmd = Serial.peek();
    if (cmd == '1' || cmd == '2' || cmd == 't') {
      char c = Serial.read();
      if (c == '1' || c == '2') {
        selectedCell = c - '0';
        Serial.print(F("Selected LoadCell"));
        Serial.println(selectedCell);
        calibrateSelectedLoadCell();
      } else if (c == 't') {
        if (selectedCell == 1) LoadCell1.tareNoDelay();
        else if (selectedCell == 2) LoadCell2.tareNoDelay();
      }
    }
  }
}

void calibrateSelectedLoadCell() {
  HX711_ADC* cell;
  int eepromAddr;

  if (selectedCell == 1) {
    cell = &LoadCell1;
    eepromAddr = calVal1_eepromAddress;
  } else {
    cell = &LoadCell2;
    eepromAddr = calVal2_eepromAddress;
  }

  Serial.println(F("*** Calibration ***"));
  Serial.println(F("Remove weight and send 't' to tare."));
  while (true) {
    cell->update();
    if (Serial.available()) {
      char inByte = Serial.read();
      if (inByte == 't') {
        cell->tareNoDelay();
        Serial.println(F("Taring..."));
      }
    }
    if (cell->getTareStatus()) {
      Serial.println(F("Tare complete."));
      break;
    }
  }

  Serial.println(F("Place known mass and send its value (e.g., 100.0):"));
  float knownMass = 0.0;
  while (knownMass == 0.0) {
    if (Serial.available()) {
      knownMass = Serial.parseFloat();
    }
  }
  Serial.print(F("Known mass entered: "));
  Serial.println(knownMass);

  cell->refreshDataSet();
  float newCal = cell->getNewCalibration(knownMass);
  Serial.print(F("New calibration factor: "));
  Serial.println(newCal, 5);

  Serial.print(F("Save to EEPROM at address "));
  Serial.print(eepromAddr);
  Serial.println(F("? (y/n)"));

  while (true) {
    if (Serial.available()) {
      char confirm = Serial.read();
      if (confirm == 'y') {
#if defined(ESP8266) || defined(ESP32)
        EEPROM.begin(512);
#endif
        EEPROM.put(eepromAddr, newCal);
#if defined(ESP8266) || defined(ESP32)
        EEPROM.commit();
#endif
        Serial.println(F("Calibration saved."));
        break;
      } else if (confirm == 'n') {
        Serial.println(F("Calibration not saved."));
        break;
      }
    }
  }
}

void processSerialCommands() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("set")) {
      int yr, mo, dy, hr, min, sec;
      int count = sscanf(input.c_str(), "set %d %d %d %d %d %d", &yr, &mo, &dy, &hr, &min, &sec);
      if (count == 6) {
        rtc.adjust(DateTime(yr, mo, dy, hr, min, sec));
        Serial.println(F("RTC time set successfully."));
      } else {
        Serial.println(F("Invalid format. Use: set YYYY MM DD HH MM SS"));
      }
    } else if (input == "time") {
      DateTime now = rtc.now();
      Serial.print(F("Current RTC time: "));
      Serial.println(now.timestamp(DateTime::TIMESTAMP_FULL));
    }
  }
}
