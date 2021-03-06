/*
    Title:    Cryologger - Glacier Velocity Measurement System (GVMS) v2.0 Prototype
    Date:     March 10, 2021
    Author:   Adam Garbo

    Components:
    - SparkFun Artemis Processor
    - SparkFun MicroMod Data Logging Carrier Board
    - SparkFun GPS-RTK-SMA Breakout - ZED-F9P (Qwiic)
    - SparkFun Buck-Boost Converter

    Comments:
*/

// ----------------------------------------------------------------------------
// Libraries
// ----------------------------------------------------------------------------

#include <RTC.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h> // https://github.com/sparkfun/SparkFun_Ublox_Arduino_Library
#include <SdFat.h>                                // https://github.com/greiman/SdFat
#include <SPI.h>
#include <TimeLib.h>                              // https://github.com/PaulStoffregen/Time
#include <WDT.h>
#include <Wire.h>                                 // https://www.arduino.cc/en/Reference/Wire

// -----------------------------------------------------------------------------
// Debugging macros
// -----------------------------------------------------------------------------
#define DEBUG           true   // Output debug messages to Serial Monitor
#define DEBUG_GNSS      true   // Output GNSS information to Serial Monitor

#if DEBUG
#define DEBUG_PRINT(x)            Serial.print(x)
#define DEBUG_PRINTLN(x)          Serial.println(x)
#define DEBUG_PRINT_HEX(x)        Serial.print(x, HEX)
#define DEBUG_PRINTLN_HEX(x)      Serial.println(x, HEX)
#define DEBUG_PRINT_DEC(x, y)     Serial.print(x, y)
#define DEBUG_PRINTLN_DEC(x, y)   Serial.println(x, y)
#define DEBUG_WRITE(x)            Serial.write(x)

#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINT_HEX(x)
#define DEBUG_PRINTLN_HEX(x)
#define DEBUG_PRINT_DEC(x, y)
#define DEBUG_PRINTLN_DEC(x, y)
#define DEBUG_WRITE(x)
#endif

// ----------------------------------------------------------------------------
// Pin definitions
// ----------------------------------------------------------------------------
#define PIN_VBAT          A0
#define PIN_PWC_POWER     33 // G1
#define PIN_QWIIC_POWER   34 // G2
#define PIN_SD_CS         41 // CS

// ----------------------------------------------------------------------------
// Object instantiations
// ----------------------------------------------------------------------------
APM3_RTC          rtc;
APM3_WDT          wdt;
SdFs              sd;             // File system object
FsFile            file;           // Log file
SFE_UBLOX_GNSS    gnss;           // I2C address: 0x42

// ----------------------------------------------------------------------------
// User defined global variable declarations
// ----------------------------------------------------------------------------
byte          sleepAlarmMinutes     = 60;
byte          sleepAlarmHours       = 0;
byte          loggingAlarmHours     = 0;
byte          loggingAlarmMinutes   = 60;
unsigned int  gnssTimeout           = 300;   // Timeout for GNSS signal acquisition (s)

// ----------------------------------------------------------------------------
// Global variable declarations
// ----------------------------------------------------------------------------

const int     sdWriteSize         = 512;    // Write data to SD in blocks of 512 bytes
const int     fileBufferSize      = 16384;  // Allocate 16 KB RAM for UBX message storage
volatile bool alarmFlag           = false;   // Flag for alarm interrupt service routine
volatile bool watchdogFlag        = false;  // Flag for Watchdog Timer interrupt service routine
volatile int  watchdogCounter     = 0;      // Counter for Watchdog Timer interrupts
volatile bool loggingFlag         = false;  // Flag to
bool          firstTimeFlag       = true;   // Flag to determine if the program is running for the first time
bool          resetFlag           = 0;      // Flag to force system reset using Watchdog Timer
bool          gnssFixFlag         = false;  // Flag to indicate if GNSS valid fix has been acquired
bool          rtcSyncFlag         = false;  // Flag to indicate if the RTC was syned with the GNSS
byte          gnssFixCounter      = 0;      // Counter for valid GNSS fixes
byte          gnssFixCounterMax   = 60;     // Counter limit for threshold of valid GNSS fixes
char          fileName[30]        = "";     // Keep a record of this file name so that it can be re-opened upon wakeup from sleep
unsigned int  sdPowerDelay        = 250;    // Delay before disabling power to microSD (milliseconds)
unsigned int  qwiicPowerDelay     = 2500;   // Delay after enabling power to Qwiic connector (milliseconds)
unsigned long previousMillis      = 0;      // Global millis() timer
unsigned long unixtime            = 0;
unsigned long bytesWritten        = 0;      // Counter how many bytes have been written to SD card
float         voltage             = 0.0;    // Battery voltage
time_t        alarmTime           = 0;

// ----------------------------------------------------------------------------
// Unions/structures
// ----------------------------------------------------------------------------

// Union to store device online/offline states
struct struct_online
{
  bool gnss     = false;
  bool microSd  = false;
  bool sensor   = false;
} online;

// Union to store loop timers
struct struct_timer
{
  unsigned long voltage;
  unsigned long rtc;
  unsigned long microSd;
  unsigned long sensors;
  unsigned long gnss;
  unsigned long logGnss;

} timer;

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup()
{
  // Pin assignments
  pinMode(PIN_QWIIC_POWER, OUTPUT);
  pinMode(PIN_PWC_POWER, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // Set analog resolution to 14-bits
  analogReadResolution(14);

  Wire.begin(); // Initialize I2C
  Wire.setClock(100000); // Set I2C clock speed to 400 kHz
  Wire.setPullups(0);   // Disable Artemis internal I2C pull-ups to reduce bus errors
  SPI.begin(); // Initialize SPI

#if DEBUG
  Serial.begin(115200);
  //while (!Serial); // Wait for user to open Serial Monitor
  blinkLed(2, 1000); // Non-blocking delay to allow user to open Serial Monitor
#endif

  qwiicPowerOn();       // Enable power to Qwiic connector
  peripheralPowerOn();  // Enable power to peripherials

  DEBUG_PRINTLN();
  printLine();
  DEBUG_PRINTLN("Cryologger - Glacier Velocity Measurement System v2.0");
  printLine();
  printDateTime();

  // Configure devices
  configureGnss();    // Configure GNSS receiver
  readGnss();         // Acquire GNSS fix and synchronize RTC with GNSS
  configureWdt();     // Configure and start Watchdog Timer (WDT)
  configureSd();      // Configure microSD
  createDebugLogFile();
  configureSensors(); // Configure attached sensors
  //createLogFile();    // Create initial log file
  configureRtc();     // Configure initial real-time clock (RTC) alarm

  DEBUG_PRINT("Datetime: "); printDateTime();
  DEBUG_PRINT("Initial alarm: "); printAlarm();

  // Blink LED to indicate completion of setup
  blinkLed(10, 100);
}

// ----------------------------------------------------------------------------
// Loop
// ----------------------------------------------------------------------------
void loop()
{
  // Check if alarm flag is set or if program is running for the first time
  if (alarmFlag)
  {
    // Clear alarm flag
    alarmFlag = false;

    DEBUG_PRINT("Alarm trigger: "); printDateTime();

    // Toggle logging flag
    loggingFlag = !loggingFlag;
    DEBUG_PRINT("loggingFlag: "); DEBUG_PRINTLN(loggingFlag);

    // Perform measurements
    if (loggingFlag)
    {
      setLoggingAlarm();

      peripheralPowerOn();  // Enable power to peripherals
      configureSd();        // Configure microSD
      createLogFile();

      qwiicPowerOn();       // Enable power to Qwiic connector
      configureGnss();      // Configure u-blox receiver
      logGnss();

      printTimers();  // Print function execution timers
    }

    // Log system diagnostic information
    logDebugData();

    // Clear logging alarm flag
    alarmFlag = false;

    // Set the next RTC alarm
    setSleepAlarm();
  }

  // Check for watchdog interrupt
  if (watchdogFlag)
  {
    petDog();
  }

  // Blink LED
  blinkLed(1, 25);

  // Enter deep sleep and await RTC alarm interrupt
  goToSleep();
}

// ----------------------------------------------------------------------------
// Interupt Service Routines (ISR)
// ----------------------------------------------------------------------------

// Interrupt handler for the RTC
extern "C" void am_rtc_isr(void)
{
  // Clear the RTC alarm interrupt
  //rtc.clearInterrupt();
  am_hal_rtc_int_clear(AM_HAL_RTC_INT_ALM);

  // Set alarm flag
  alarmFlag = true;
}

// Interrupt handler for the watchdog timer
extern "C" void am_watchdog_isr(void)
{
  // Clear the watchdog interrupt
  wdt.clear();

  // Perform system reset after 10 watchdog interrupts (should not occur)
  if (watchdogCounter < 10 )
  {
    //DEBUG_PRINTLN("CAP'N! WE'RE GOING DOWN! AHHH!!!!!!!");

    wdt.restart(); // "Pet" the dog
  }
  else
  {
    wdt.stop();
    digitalWrite(LED_BUILTIN, HIGH);
    while (1);
  }
  watchdogFlag = true; // Set the watchdog flag
  watchdogCounter++; // Increment watchdog interrupt counter
}
