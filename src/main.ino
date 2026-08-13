/*
  ============================================================
          RFID BASED ATTENDANCE SYSTEM
          Arduino UNO + MFRC522 + DS1307 + AT24C32 + LCD
  ============================================================

  Hardware:
  - Arduino UNO
  - MFRC522 RFID Reader
  - DS1307 RTC Module with AT24C32 EEPROM
  - 16x2 I2C LCD
  - 2 Push Buttons
  - MIFARE Classic 1K RFID Cards/Tags

  Communication:
  - MFRC522 -> SPI
  - DS1307 -> I2C
  - AT24C32 -> I2C
  - LCD -> I2C

  Pin Configuration:
  ------------------------------------------------------------
  MFRC522:
     SDA/SS -> D10
     MOSI   -> D11
     MISO   -> D12
     SCK    -> D13
     RST    -> D7
     3.3V   -> 3.3V
     GND    -> GND

  I2C:
     SDA -> A4
     SCL -> A5

  Buttons:
     MENU   -> D8
     SELECT -> D9

  LCD I2C Address:
     0x27
     Change to 0x3F if required

  AT24C32:
     Address -> 0x50
  ============================================================
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>

// ============================================================
// PIN DEFINITIONS
// ============================================================

#define RFID_SS_PIN       10
#define RFID_RST_PIN       7

#define MENU_BUTTON        8
#define SELECT_BUTTON      9

// ============================================================
// I2C ADDRESSES
// ============================================================

#define LCD_ADDRESS       0x27
#define EEPROM_ADDRESS    0x50

// ============================================================
// OBJECTS
// ============================================================

MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);
RTC_DS1307 rtc;
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);

// ============================================================
// EEPROM CONFIGURATION
// ============================================================

#define EEPROM_HEADER_ADDRESS   0
#define EEPROM_RECORD_START     16

#define MAX_LOGS                30
#define RECORD_SIZE             64

// ============================================================
// DATA STRUCTURES
// ============================================================

struct AttendanceRecord
{
  char uid[21];
  char name[21];
  char dateTime[21];
  byte status;          // 1 = IN, 0 = OUT
};

struct Student
{
  const char *uid;
  const char *name;
};

// ============================================================
// STUDENT DATABASE
// ============================================================
//
// IMPORTANT:
// These are PLACEHOLDER values for the public GitHub version.
//
// Replace them in your LOCAL copy with your actual RFID UIDs.
//
// Example:
// {"A3 B4 C5 D6", "Student1"}
//
// Do not publish real student names/UIDs in a public repository.
// ============================================================

Student students[] =
{
  {"UID_1", "Student1"},
  {"UID_2", "Student2"},
  {"UID_3", "Student3"},
  {"UID_4", "Student4"}
};

const int STUDENT_COUNT =
  sizeof(students) / sizeof(students[0]);

// ============================================================
// GLOBAL VARIABLES
// ============================================================

int logCount = 0;

enum MenuState
{
  HOME,
  MENU,
  CLEAR_CONFIRM
};

MenuState currentMenu = HOME;

// 0 = View Logs
// 1 = Clear Logs
byte menuOption = 0;

unsigned long lastButtonTime = 0;
unsigned long lastScanTime = 0;

const unsigned long BUTTON_DELAY = 250;
const unsigned long SCAN_DELAY = 1500;

// ============================================================
// EEPROM LOW LEVEL FUNCTIONS
// ============================================================

void eepromWriteByte(unsigned int address, byte data)
{
  Wire.beginTransmission(EEPROM_ADDRESS);

  Wire.write((byte)(address >> 8));
  Wire.write((byte)(address & 0xFF));
  Wire.write(data);

  Wire.endTransmission();

  delay(5);
}

// ------------------------------------------------------------

byte eepromReadByte(unsigned int address)
{
  Wire.beginTransmission(EEPROM_ADDRESS);

  Wire.write((byte)(address >> 8));
  Wire.write((byte)(address & 0xFF));

  Wire.endTransmission();

  Wire.requestFrom(EEPROM_ADDRESS, 1);

  if (Wire.available())
  {
    return Wire.read();
  }

  return 0xFF;
}

// ------------------------------------------------------------

void eepromWriteBlock(
  unsigned int address,
  const byte *data,
  unsigned int length)
{
  unsigned int remaining = length;
  unsigned int currentAddress = address;
  unsigned int index = 0;

  while (remaining > 0)
  {
    // AT24C32 page size = 32 bytes
    unsigned int pageOffset = currentAddress % 32;
    unsigned int spaceInPage = 32 - pageOffset;

    unsigned int chunk = remaining;

    if (chunk > spaceInPage)
    {
      chunk = spaceInPage;
    }

    Wire.beginTransmission(EEPROM_ADDRESS);

    Wire.write((byte)(currentAddress >> 8));
    Wire.write((byte)(currentAddress & 0xFF));

    for (unsigned int i = 0; i < chunk; i++)
    {
      Wire.write(data[index + i]);
    }

    Wire.endTransmission();

    delay(5);

    currentAddress += chunk;
    index += chunk;
    remaining -= chunk;
  }
}

// ------------------------------------------------------------

void eepromReadBlock(
  unsigned int address,
  byte *data,
  unsigned int length)
{
  for (unsigned int i = 0; i < length; i++)
  {
    data[i] = eepromReadByte(address + i);
  }
}

// ============================================================
// LOG COUNT FUNCTIONS
// ============================================================

void saveLogCount()
{
  eepromWriteByte(
    EEPROM_HEADER_ADDRESS,
    (byte)logCount
  );
}

// ------------------------------------------------------------

void loadLogCount()
{
  byte storedCount =
    eepromReadByte(EEPROM_HEADER_ADDRESS);

  if (storedCount == 0xFF || storedCount > MAX_LOGS)
  {
    logCount = 0;
    saveLogCount();
  }
  else
  {
    logCount = storedCount;
  }
}

// ============================================================
// UID FUNCTIONS
// ============================================================

String getUID()
{
  String uid = "";

  for (byte i = 0; i < mfrc522.uid.size; i++)
  {
    if (mfrc522.uid.uidByte[i] < 0x10)
    {
      uid += "0";
    }

    uid += String(
      mfrc522.uid.uidByte[i],
      HEX
    );

    if (i < mfrc522.uid.size - 1)
    {
      uid += " ";
    }
  }

  uid.toUpperCase();

  return uid;
}

// ============================================================
// FIND STUDENT
// ============================================================

int findStudent(String uid)
{
  for (int i = 0; i < STUDENT_COUNT; i++)
  {
    if (uid.equalsIgnoreCase(students[i].uid))
    {
      return i;
    }
  }

  return -1;
}

// ============================================================
// FORMAT DATE AND TIME
// ============================================================

void getDateTimeString(char *buffer)
{
  DateTime now = rtc.now();

  sprintf(
    buffer,
    "%02d/%02d/%04d %02d:%02d:%02d",
    now.day(),
    now.month(),
    now.year(),
    now.hour(),
    now.minute(),
    now.second()
  );
}

// ============================================================
// SAVE ATTENDANCE RECORD
// ============================================================

bool saveAttendance(
  String uid,
  String name,
  byte status)
{
  if (logCount >= MAX_LOGS)
  {
    return false;
  }

  AttendanceRecord record;

  memset(
    &record,
    0,
    sizeof(record)
  );

  uid.toCharArray(
    record.uid,
    sizeof(record.uid)
  );

  name.toCharArray(
    record.name,
    sizeof(record.name)
  );

  getDateTimeString(record.dateTime);

  record.status = status;

  unsigned int address =
    EEPROM_RECORD_START +
    (logCount * RECORD_SIZE);

  eepromWriteBlock(
    address,
    (byte *)&record,
    sizeof(record)
  );

  logCount++;

  saveLogCount();

  return true;
}

// ============================================================
// READ ATTENDANCE RECORD
// ============================================================

bool readAttendanceRecord(
  int index,
  AttendanceRecord &record)
{
  if (index < 0 || index >= logCount)
  {
    return false;
  }

  unsigned int address =
    EEPROM_RECORD_START +
    (index * RECORD_SIZE);

  memset(
    &record,
    0,
    sizeof(record)
  );

  eepromReadBlock(
    address,
    (byte *)&record,
    sizeof(record)
  );

  return true;
}

// ============================================================
// FIND LAST STATUS OF A STUDENT
// ============================================================
//
// Returns:
//   1 -> currently IN
//   0 -> currently OUT
//
// If no previous record exists:
//   returns 0
// ============================================================

byte getCurrentStatus(String uid)
{
  AttendanceRecord record;

  for (int i = logCount - 1; i >= 0; i--)
  {
    if (readAttendanceRecord(i, record))
    {
      if (uid.equalsIgnoreCase(record.uid))
      {
        return record.status;
      }
    }
  }

  return 0;
}

// ============================================================
// CLEAR ALL LOGS
// ============================================================

void clearAllLogs()
{
  logCount = 0;

  saveLogCount();

  // Clear the first byte of every record.
  // logCount controls record validity.
  for (int i = 0; i < MAX_LOGS; i++)
  {
    unsigned int address =
      EEPROM_RECORD_START +
      (i * RECORD_SIZE);

    eepromWriteByte(
      address,
      0xFF
    );
  }

  Serial.println();
  Serial.println("==============================");
  Serial.println("ALL ATTENDANCE LOGS CLEARED");
  Serial.println("==============================");
}

// ============================================================
// LCD HOME SCREEN
// ============================================================

void showHome()
{
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("RFID ATTENDANCE");

  lcd.setCursor(0, 1);
  lcd.print("Scan Your Card");
}

// ============================================================
// LCD MESSAGE
// ============================================================

void showMessage(
  const char *line1,
  const char *line2,
  unsigned long duration)
{
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1);

  lcd.setCursor(0, 1);
  lcd.print(line2);

  delay(duration);
}

// ============================================================
// PROCESS RFID CARD
// ============================================================

void processRFID()
{
  if (!mfrc522.PICC_IsNewCardPresent())
  {
    return;
  }

  if (!mfrc522.PICC_ReadCardSerial())
  {
    return;
  }

  String uid = getUID();

  Serial.println();
  Serial.println("==============================");
  Serial.println("RFID CARD DETECTED");
  Serial.print("UID : ");
  Serial.println(uid);

  int studentIndex = findStudent(uid);

  // ----------------------------------------------------------
  // INVALID CARD
  // ----------------------------------------------------------

  if (studentIndex == -1)
  {
    Serial.println("Status: ACCESS DENIED");

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("ACCESS DENIED");

    lcd.setCursor(0, 1);
    lcd.print("Unknown Card");

    delay(2000);

    Serial.println("==============================");

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    showHome();

    return;
  }

  // ----------------------------------------------------------
  // VALID STUDENT
  // ----------------------------------------------------------

  String name =
    students[studentIndex].name;

  Serial.print("Name : ");
  Serial.println(name);

  // ----------------------------------------------------------
  // CHECK PREVIOUS STATUS
  // ----------------------------------------------------------

  byte previousStatus =
    getCurrentStatus(uid);

  byte newStatus;

  if (previousStatus == 0)
  {
    newStatus = 1;       // IN
  }
  else
  {
    newStatus = 0;       // OUT
  }

  // ----------------------------------------------------------
  // SAVE RECORD
  // ----------------------------------------------------------

  bool saved =
    saveAttendance(
      uid,
      name,
      newStatus
    );

  if (!saved)
  {
    Serial.println("ERROR: EEPROM FULL");

    showMessage(
      "MEMORY FULL",
      "Clear Logs",
      2000
    );

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    showHome();

    return;
  }

  // ----------------------------------------------------------
  // DISPLAY RESULT
  // ----------------------------------------------------------

  lcd.clear();

  lcd.setCursor(0, 0);

  if (newStatus == 1)
  {
    lcd.print("WELCOME ");
  }
  else
  {
    lcd.print("GOODBYE ");
  }

  lcd.setCursor(0, 1);
  lcd.print(name.substring(0, 9));

  Serial.print("ID     : ");
  Serial.println(uid);

  Serial.print("Status : ");

  if (newStatus == 1)
  {
    Serial.println("IN");
  }
  else
  {
    Serial.println("OUT");
  }

  char dateTime[21];

  getDateTimeString(dateTime);

  Serial.print("Time   : ");
  Serial.println(dateTime);

  Serial.print("Logs   : ");
  Serial.println(logCount);

  Serial.println("==============================");

  delay(2000);

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  showHome();
}

// ============================================================
// VIEW LOGS
// ============================================================

void viewLogs()
{
  if (logCount == 0)
  {
    showMessage(
      "NO LOGS",
      "Available",
      1500
    );

    currentMenu = HOME;
    showHome();

    return;
  }

  for (int i = 0; i < logCount; i++)
  {
    AttendanceRecord record;

    if (!readAttendanceRecord(i, record))
    {
      continue;
    }

    Serial.println();
    Serial.println("------------------------------");

    Serial.print("Log No : ");
    Serial.println(i + 1);

    Serial.print("UID    : ");
    Serial.println(record.uid);

    Serial.print("Name   : ");
    Serial.println(record.name);

    Serial.print("Time   : ");
    Serial.println(record.dateTime);

    Serial.print("Status : ");

    if (record.status == 1)
    {
      Serial.println("IN");
    }
    else
    {
      Serial.println("OUT");
    }

    Serial.println("------------------------------");

    lcd.clear();

    lcd.setCursor(0, 0);

    // Show name, maximum 16 characters.
    lcd.print(record.name);

    lcd.setCursor(0, 1);

    if (record.status == 1)
    {
      lcd.print("IN ");
    }
    else
    {
      lcd.print("OUT ");
    }

    // Display time only on the LCD.
    // The full date/time remains available
    // through the Serial Monitor.
    lcd.print(record.dateTime + 11);

    delay(2000);
  }

  currentMenu = HOME;
  showHome();
}

// ============================================================
// BUTTON DEBOUNCE
// ============================================================

bool buttonPressed(byte pin)
{
  if (digitalRead(pin) == LOW)
  {
    if (millis() - lastButtonTime > BUTTON_DELAY)
    {
      lastButtonTime = millis();

      while (digitalRead(pin) == LOW)
      {
        delay(5);
      }

      return true;
    }
  }

  return false;
}

// ============================================================
// MAIN MENU
// ============================================================

void showMenu()
{
  lcd.clear();

  if (menuOption == 0)
  {
    lcd.setCursor(0, 0);
    lcd.print(">VIEW LOGS");

    lcd.setCursor(0, 1);
    lcd.print(" CLEAR LOGS");
  }
  else
  {
    lcd.setCursor(0, 0);
    lcd.print(" VIEW LOGS");

    lcd.setCursor(0, 1);
    lcd.print(">CLEAR LOGS");
  }
}

// ============================================================
// CLEAR CONFIRMATION
// ============================================================

void showClearConfirmation()
{
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("CLEAR ALL LOGS?");

  lcd.setCursor(0, 1);
  lcd.print("SEL=YES MENU=NO");
}

// ============================================================
// PROCESS MENU BUTTONS
// ============================================================

void processButtons()
{
  // ==========================================================
  // HOME SCREEN
  // ==========================================================

  if (currentMenu == HOME)
  {
    if (buttonPressed(MENU_BUTTON))
    {
      currentMenu = MENU;
      menuOption = 0;

      showMenu();
    }

    return;
  }

  // ==========================================================
  // MAIN MENU
  // ==========================================================

  if (currentMenu == MENU)
  {
    // MENU button switches between options.
    if (buttonPressed(MENU_BUTTON))
    {
      menuOption++;

      if (menuOption > 1)
      {
        menuOption = 0;
      }

      showMenu();

      return;
    }

    // SELECT button chooses the highlighted option.
    if (buttonPressed(SELECT_BUTTON))
    {
      // VIEW LOGS
      if (menuOption == 0)
      {
        viewLogs();
      }

      // CLEAR LOGS
      else
      {
        currentMenu = CLEAR_CONFIRM;
        showClearConfirmation();
      }

      return;
    }
  }

  // ==========================================================
  // CLEAR CONFIRMATION
  // ==========================================================

  if (currentMenu == CLEAR_CONFIRM)
  {
    // MENU = cancel
    if (buttonPressed(MENU_BUTTON))
    {
      currentMenu = MENU;
      showMenu();

      return;
    }

    // SELECT = confirm
    if (buttonPressed(SELECT_BUTTON))
    {
      clearAllLogs();

      showMessage(
        "LOGS CLEARED",
        "Successfully",
        1500
      );

      currentMenu = HOME;
      showHome();

      return;
    }
  }
}

// ============================================================
// RTC INITIALIZATION
// ============================================================

void initializeRTC()
{
  if (!rtc.begin())
  {
    Serial.println("ERROR: RTC NOT FOUND");

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("RTC ERROR");

    lcd.setCursor(0, 1);
    lcd.print("Check Wiring");

    while (1)
    {
      delay(1000);
    }
  }

  /*
    IMPORTANT:

    Uncomment this line ONCE if the RTC needs to be
    initialized with the computer's date and time.

    Upload once, then comment the line again and
    upload the program a second time.

    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  */

  if (!rtc.isrunning())
  {
    Serial.println("WARNING: RTC is not running.");
    Serial.println("Set RTC time using rtc.adjust().");
  }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(9600);

  Serial.println();
  Serial.println("=================================");
  Serial.println(" RFID BASED ATTENDANCE SYSTEM");
  Serial.println("=================================");

  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  Wire.begin();

  // ----------------------------------------------------------
  // LCD
  // ----------------------------------------------------------

  lcd.init();
  lcd.backlight();

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("RFID ATTENDANCE");

  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  delay(1500);

  // ----------------------------------------------------------
  // BUTTONS
  // ----------------------------------------------------------

  pinMode(
    MENU_BUTTON,
    INPUT_PULLUP
  );

  pinMode(
    SELECT_BUTTON,
    INPUT_PULLUP
  );

  // ----------------------------------------------------------
  // SPI + RFID
  // ----------------------------------------------------------

  SPI.begin();

  mfrc522.PCD_Init();

  delay(100);

  // ----------------------------------------------------------
  // RTC
  // ----------------------------------------------------------

  initializeRTC();

  // ----------------------------------------------------------
  // EEPROM
  // ----------------------------------------------------------

  loadLogCount();

  Serial.print("Stored logs: ");
  Serial.println(logCount);

  // ----------------------------------------------------------
  // RFID READER INFORMATION
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("RFID Reader initialized.");
  Serial.println("Scan a card to test.");
  Serial.println();

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("SYSTEM READY");

  lcd.setCursor(0, 1);
  lcd.print("Scan RFID");

  delay(1500);

  showHome();
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  // ----------------------------------------------------------
  // RFID SCANNING
  // ----------------------------------------------------------

  if (millis() - lastScanTime > SCAN_DELAY)
  {
    processRFID();

    lastScanTime = millis();
  }

  // ----------------------------------------------------------
  // BUTTONS
  // ----------------------------------------------------------

  processButtons();
}
