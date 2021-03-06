/*
  Title:    Cryologger - Glacier Velocity Measurement System (GVMS)
  Author:   Adam Garbo
  Date:     August 21th, 2020
  Version:  0.1

  Based extensively on:
  - OpenLog Artemis GNSS Logging
    By: Paul Clark (PaulZC)
    Date: July 18th, 2020
    Version: V1.1
    https://github.com/sparkfun/OpenLog_Artemis_GNSS_Logger

  Components:
  - SparkFun GPS-RTK2 Board ZED-F9P
  - u-blox ANN-MB antenna
  - SparkFun OpenLog Artemis

  Comments:
  - This code is currently under development

  License:
  - This project is released under the MIT License (http://opensource.org/licenses/MIT)

*/

// Define firmware version
#define FIRMWARE_VERSION_MAJOR 1
#define FIRMWARE_VERSION_MINOR 2

// Define OpenLog Artemis unique board identifier
// Sum of:
// Variant * 0x100  (OLA = 1; GNSS_LOGGER = 2; GEOPHONE_LOGGER = 3; CRYOLOGGER = 4)
// Major firmware version * 0x10
// Minor firmware version
#define OLA_IDENTIFIER 0x212

// Load default sensor settings
#include "settings.h"

// Define hardware version
#define HARDWARE_VERSION_MAJOR 1
#define HARDWARE_VERSION_MINOR 0

// Define pin functions
#if(HARDWARE_VERSION_MAJOR == 0 && HARDWARE_VERSION_MINOR == 4)
#define PIN_MICROSD_CHIP_SELECT 10
#define PIN_IMU_POWER           22
#elif(HARDWARE_VERSION_MAJOR == 1 && HARDWARE_VERSION_MINOR == 0)
#define PIN_MICROSD_CHIP_SELECT 23
#define PIN_IMU_POWER           27
#define PIN_PWR_LED             29
#define PIN_VREG_ENABLE         25
#define PIN_VIN_MONITOR         34 // VIN/3 (1M/2M - will require a correction factor)
#endif
#define PIN_POWER_LOSS          3
#define PIN_MICROSD_POWER       15
#define PIN_QWIIC_POWER         18
#define PIN_STAT_LED            19
#define PIN_IMU_INT             37
#define PIN_IMU_CHIP_SELECT     44
#define PIN_STOP_LOGGING        32

enum returnStatus
{
  STATUS_GETBYTE_TIMEOUT = 255,
  STATUS_GETNUMBER_TIMEOUT = -123455555,
  STATUS_PRESSED_X,
};

// Libraries
#include <Wire.h>                           // https://www.arduino.cc/en/Reference/Wire
#include <WDT.h>                            // https://github.com/sparkfun/Arduino_Apollo3
#include <SPI.h>                            // https://www.arduino.cc/en/Reference/SPI
#include <SparkFun_Ublox_Arduino_Library.h> // https://github.com/sparkfun/SparkFun_Ublox_Arduino_Library
#include <SdFat.h>                          // https://github.com/greiman/SdFat
#include <RTC.h>                            // https://github.com/sparkfun/Arduino_Apollo3
#include <EEPROM.h>                         // https://github.com/sparkfun/Arduino_Apollo3

// Definitons
#define MAX_PAYLOAD_SIZE  384 // Override MAX_PAYLOAD_SIZE for getModuleInfo which can return up to 348 bytes
#define QWIIC_PULLUPS     0   // Default to no pull-ups on the Qwiic bus to minimise u-blox bus errors

// Object instantiations
APM3_RTC      rtc;
APM3_WDT      wdt;
SdFat         sd;
SdFile        file;
SFE_UBLOX_GPS gnss;
TwoWire       qwiic(1); // Pads 8/9

// Global variables
volatile bool alarmFlag             = false;  // Flag to indicate if RTC ISR triggered
volatile bool stopLoggingFlag       = false;  // Flag to indicate if data logging should be halted
bool          rtcSyncFlag           = false;  // Flag to indicate if the RTC has synced to GNSS
bool          rtcSyncRequiredFlag   = true;   // Flag to indicate if the RTC needs to be synced (after sleep)
bool          gnssSettingsFlag      = false;  // Flag to indicate if the GNSS settings have been changed
bool          ledState              = LOW;    // Flag to indicate state of LED in blinkLed() function
bool          watchdogFlag          = false;  // Flag to indicate if WDT ISR triggered
char          dirName[9]            = "";     // Log file directory name. Format: YYYYMMDD/HHMMSS.ubx
char          fileName[30]          = "";     // Log file name. Limited to 8.3 characters. Format: YYYYMMDD/HHMMSS.ubx
const int     sdPowerDownDelay      = 100;    // Delay for this many ms before turning off the SD card power
const byte    menuTimeout           = 30;     // Menu timeout in seconds
unsigned long lastReadTime          = 0;      // Used to delay between u-blox readings
unsigned long lastDataLogSyncTime   = 0;      // Used to sync SD every 500 ms
unsigned long previousMillis        = 0;      // millis() timer variable
uint64_t      measurementStartTime  = 0;      // Used to calculate the elapsed time

// Structure to hold GNSS module info
struct minfoStructure
{
  char  swVersion[30];  // Currently running firmware version
  char  hwVersion[10];  // Hardware version of the u-blox receiver
  int   protVerMajor;   // Supported protocol version
  int   protVerMinor;   // Supported protocol version
  char  mod[8];         // The module type from the "MOD=" extension (7 chars + NULL)
  bool  SPG;            // Standard Precision
  bool  HPG;            // High Precision (ZED-F9P)
  bool  ADR;            // Automotive Dead Reckoning (ZED-F9K)
  bool  UDR;            // Untethered Dead Reckoning (NEO-M8U does not support protocol 27)
  bool  TIM;            // Time sync (ZED-F9T) (Guess!)
  bool  FTS;            // Frequency and Time Sync
  bool  LAP;            // Lane Accurate Positioning (ZED-F9R)
  bool  HDG;            // Heading (ZED-F9H)
} minfo;                // Module info

// Custom UBX Packet for getModuleInfo and powerManagementTask
uint8_t customPayload[MAX_PAYLOAD_SIZE]; // Array to hold payload data bytes

// Create and initialise packet information that wraps around the payload
ubxPacket customCfg = {0, 0, 0, 0, 0, customPayload, 0, 0, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED};

// Setup
void setup()
{
  // If 3.3V rail drops below 3V the system will power down and maintain RTC
  pinMode(PIN_POWER_LOSS, INPUT); // BD49K30G-TL has CMOS output and does not need a pull-up
  pinMode(PIN_STAT_LED, OUTPUT);
  delay(1); // Let PIN_POWER_LOSS stabilize

  if (digitalRead(PIN_POWER_LOSS) == LOW)
  {
    powerDown(); // Check PIN_POWER_LOSS in case the falling edge was missed
    // TO DO: Change to goToSleep()
  }
  attachInterrupt(digitalPinToInterrupt(PIN_POWER_LOSS), powerDown, FALLING);

  analogReadResolution(14); // Default: 10-bit

  Serial.begin(115200); // Start Serial at 115200 baud for initial debug messages
  Serial.flush(); // Wait for outgoing serial data to complete
  Serial.begin(settings.serialTerminalBaudRate); // Start Serial at specified baud rate
  Serial.printf("Cryologger - Glacier Velocity Measurement System v%d.%d\n", FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR);

  if (settings.useGPIO32ForStopLogging)
  {
    Serial.println(F("Data logging stop button enabled. Pull GPIO pin 32 to GND to stop logging."));
    pinMode(PIN_STOP_LOGGING, INPUT_PULLUP);
    delay(1); // Allow pin to stabilize
    attachInterrupt(digitalPinToInterrupt(PIN_STOP_LOGGING), stopLoggingIsr, FALLING); // Enable the interrupt
    stopLoggingFlag = false; // Clear the flag
  }

  powerLedOn();       // Turn on PWR LED
  SPI.begin();        // Required if SD disabled
  beginSd();          // Initialize microSD
  loadSettings();     // Load settings from non-volatile memory (NVM) and/or settings.h file
  beginQwiic();       // Initialize I2C bus
  disableImu();       // Disable IMU
  beginSensors();     // Initialize sensor(s)
  configureRtc();     // Configure RTC alarms
  syncRtc();          // Synchronize RTC date and time with GNSS

  if (!settings.enableTerminalOutput && settings.logData)
  {
    Serial.println(F("Warning: Logging to microSD card with no terminal output"));
  }

  if (!online.microSd || !online.dataLogging)
  {
    // Allow extra time for the u-blox module to start (~1 second)
    delay(750);
  }

  // Use RTC clock if sleeping between readings. millis() powers down during sleep.
  measurementStartTime = rtcMillis();

  // Blink upon completion of setup
  blinkLed(20, 50);

  // If Serial Monitor is open, present configuration menu
  if (Serial.available())
  {
    menuMain();
  }

  powerLedOff();  // Turn on PWR LED

  // Once user configuration is complete, start data logging
  beginDataLogging();
}

// Loop
void loop()
{
  // If Serial Monitor is open, present menu
  if (Serial.available())
  {
    menuMain();
  }

  // Read I2C data and write to SD
  if (online.dataLogging)
  {
    storeData();
  }

  // Check if stop logging button was pressed
  if (settings.useGPIO32ForStopLogging && stopLoggingFlag)
  {
    stopLogging();
  }

  // Check if RTC alarm ISR triggered
  if (alarmFlag)
  {
    openNewLogFile();           // Create new log file
    configureRtc();             // Set RTC alarm
    alarmFlag = false;          // Clear alarm flag
    rtcSyncRequiredFlag = true; // Set flag to indicate RTC sync is required
  }

  uint64_t timeNow = rtcMillis();

  // Check if sleep conditions are met
  if ((settings.usSleepDuration > 0) && (timeNow > (measurementStartTime + (settings.usLoggingDuration / 1000ULL))))
  {
    goToSleep(); // Enter deep sleep

    // Update measurementStartTime to calculate when to return to sleep
    measurementStartTime = measurementStartTime + (settings.usLoggingDuration / 1000ULL) + (settings.usSleepDuration / 1000ULL);

    rtcSyncRequiredFlag = true; // Set flag to sync RTC after sleep
  }
}

// Initialize I2C
void beginQwiic()
{
  pinMode(PIN_QWIIC_POWER, OUTPUT);
  qwiicPowerOn();
  qwiic.begin();
  qwiic.setPullups(QWIIC_PULLUPS); // Set pull-ups here to make it clear which pull-ups are used
  for (int i = 0; i < 250; i++) // Allow extra time for the Qwiic sensors to power up
  {
    delay(1);
  }
}

// Initialize microSD
void beginSd()
{
  pinMode(PIN_MICROSD_POWER, OUTPUT);
  pinMode(PIN_MICROSD_CHIP_SELECT, OUTPUT);
  digitalWrite(PIN_MICROSD_CHIP_SELECT, HIGH); // Ensure SD is deselected

  if (settings.enableSd)
  {
    microSdPowerOn();

    // Western Digital Industrial IX QD334 SDSDQED
    // Maximum current draw (averaged over 1s): 100 mA (read/write), 500 uA (sleep)
    // Maximum power up time: ? ms
    for (int i = 0; i < 10; i++) // Non-blocking delay
    {
      delay(1);
    }

    if (!sd.begin(PIN_MICROSD_CHIP_SELECT, SD_SCK_MHZ(24)))
    {
      Serial.println(F("Warning: SD initialization failed. Retrying..."));
      for (int i = 0; i < 250; i++) // Give SD more time to power up, then try again
      {
        delay(1);
      }
      if (!sd.begin(PIN_MICROSD_CHIP_SELECT, SD_SCK_MHZ(24)))
      {
        Serial.println(F("Warning: SD initialization failed. Check if SD card is present and formatted."));
        digitalWrite(PIN_MICROSD_CHIP_SELECT, HIGH); // Ensure SD is deselected
        online.microSd = false;
        Serial.println(F("Warning: microSD offline"));
        return;
      }
    }

    // Change to root directory for all new file creation
    if (!sd.chdir())
    {
      Serial.println(F("Warning: SD change directory failed"));
      online.microSd = false;
      Serial.println(F("Warning: microSD offline"));
      return;
    }

    online.microSd = true;
    Serial.println(F("microSD online"));
  }
  else
  {
    microSdPowerOff();
    online.microSd = false;
    Serial.println(F("Warning: microSD offline"));
  }
}

// Disable IMU
void disableImu()
{
  pinMode(PIN_IMU_POWER, OUTPUT);
  pinMode(PIN_IMU_CHIP_SELECT, OUTPUT);
  digitalWrite(PIN_IMU_CHIP_SELECT, HIGH); // Ensure IMU is deselected
  imuPowerOff();
}

// Initialize RTC
void configureRtc()
{
  rtc.setAlarm(0, 0, 0, 0, 0, 0); // Set RTC alarm to trigger on day/hour/minute rollover (00:00:00)
  // RTC alarm modes:
  // 0: Alarm interrupt disabled
  // 1: Alarm match every year   (hundredths, seconds, minutes, hour, day, month)
  // 2: Alarm match every month  (hundredths, seconds, minutes, hours, day)
  // 3: Alarm match every week   (hundredths, seconds, minutes, hours, weekday)
  // 4: Alarm match every day    (hundredths, seconds, minute, hours)
  // 5: Alarm match every hour   (hundredths, seconds, minutes)
  // 6: Alarm match every minute (hundredths, seconds)
  // 7: Alarm match every second (hundredths)
  rtc.setAlarmMode(5);
  rtc.attachInterrupt(); // Attach RTC alarm interrupt
}

// Synchronize RTC date and time with GNSS
void syncRtc()
{
  // Check if u-blox is available
  if (qwiicOnline.uBlox)
  {
    uint32_t loopStartTime = millis();
    bool dateValid = false,
         timeValid = false;
    rtcSyncFlag = false;

    // Attempt to sync RTC with GNSS for up to 5 minutes
    Serial.println(F("Attempting to sync RTC with GNSS. Type any character to abort."));

    while ((!dateValid || !timeValid) && millis() - loopStartTime < 5UL * 60UL * 1000UL)
    {
      dateValid = gnss.getDateValid();
      timeValid = gnss.getTimeValid();

      blinkLed(1, 1000); // Non-blocking LED blink pattern

      // Check if GNSS date and time are valid and sync RTC
      if (dateValid && timeValid)
      {
        rtc.setTime(gnss.getHour(), gnss.getMinute(),  gnss.getSecond(), gnss.getMillisecond() / 10,
                    gnss.getDay(), gnss.getMonth(), gnss.getYear() - 2000);

        rtcSyncFlag = true; // Set flag
        Serial.println(F("Success: RTC time synced"));
        printDateTime();
      }

      // Check for user input to abort RTC sync
      if (Serial.available())
      {
        break;
      }
    }

    if (!rtcSyncFlag)
    {
      Serial.println(F("Warning: RTC sync failed"));
    }
  }
  else
  {
    Serial.println(F("Warning: No device detected on Qwiic bus"));
  }
}

// Start data logging
void beginDataLogging()
{
  // Check if conditions are met to begin data logging
  if (online.microSd && settings.logData && qwiicOnline.uBlox && rtcSyncFlag)
  {
    createLogFile();
    /*
      if (rtcSyncFlag)
      {

      }
      else
      {
      // If we don't have a file yet, create one. Otherwise, re-open the last used file
      if ((strlen(fileName) == 0) || (settings.openNewLogFile == true))
      {
        strcpy(fileName, findNextAvailableLog(settings.nextDataLogNumber, "dataLog"));
      }

      if (file.open(fileName, O_CREAT | O_APPEND | O_WRITE) == false)
      {
        Serial.println(F("Failed to create sensor data file"));
        online.dataLogging = false;
        return;
      }

      updateDataFileCreate(); // Update the data file creation time stamp
      }
    */
    online.dataLogging = true;
    Serial.println(F("Data logging online"));
  }
  else
  {
    online.dataLogging = false;
    Serial.println(F("Warning: Data logging offline"));
  }
}

// Non-blocking LED blink
void blinkLed(byte flashes, unsigned long interval)
{
  byte i = 0;
  while (i < flashes * 2)
  {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval)
    {
      previousMillis = currentMillis;
      if (ledState == LOW)
      {
        ledState = HIGH;
      }
      else
      {
        ledState = LOW;
      }
      digitalWrite(PIN_STAT_LED, ledState);
      i++;
    }
  }
}

// Interrupt handler for STIMER CMPR6
extern "C" void am_stimer_cmpr6_isr(void)
{
  uint32_t ui32Status = am_hal_stimer_int_status_get(false);
  if (ui32Status & AM_HAL_STIMER_INT_COMPAREG)
  {
    am_hal_stimer_int_clear(AM_HAL_STIMER_INT_COMPAREG);
  }
}

// Interrupt handler for the RTC
extern "C" void am_rtc_isr(void)
{
  am_hal_rtc_int_clear(AM_HAL_RTC_INT_ALM); // Clear the RTC alarm interrupt
  alarmFlag = true; // Set the alarm flag
}

// Interrupt handler for the WDT
extern "C" void am_watchdog_isr(void)
{
  wdt.clear(); // Clear the watchdog interrupt
  wdt.restart(); // "Pet" the dog
  watchdogFlag = true; // Set the watchdog flag
}

// Interrupt handler for the data logging stop button
void stopLoggingIsr(void)
{
  stopLoggingFlag = true; // Set the stop logging flag
}
