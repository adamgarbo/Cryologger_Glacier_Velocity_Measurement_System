// storeData is the workhorse. It reads I2C data and writes it to SD in 512 byte blocks.
// The I2C code comes directly from checkUbloxI2C in the u-blox library but has been tweaked!

// Call storeData as often as possible, including during menu functions.
// We don't want the data streaming to stall.

// storeData will request new I2C data from the module every usBetweenReadings / 3

// Data is read from I2C and added to UBXbuffer until we know what type of frame it is.
// If it is not an ACK/NACK and is valid, it is then moved from UBXbuffer into GNSSbuffer.
// ACK/NACKs received here are ignored and not written to SD.
// Data is read from GNSSbuffer into SDpacket and written to SD 512 bytes at a time.

// Each time storeData is called, it will write one packet to SD (if there is enough
// data available). This is to avoid it stalling while all available data is written,
// allowing GNSSbuffer to do its job in terms of buffering the data while it is written to SD.

// TO DO: maybe implement some kind of acknowledgement mechanism?
// We will still use the normal u-blox library functions when the module is not streaming
// frames for logging and we can intercept the ACK/NACK as normal.
// We only discard ACK/NACKs when streaming messages to SD.

// Storage for the incoming I2C data
// ** Test carefully when changing the size of GNSSbuffer **
// ** The storeData: Max bufAvail: debug messages will help **
// ** The buffer needs to be big enough to buffer RAWX data from all constellations **
// ** with a flaky SD card! **
RingBufferN<24576> GNSSbuffer;

// Storage for a single UBX frame (so we can discard ACK/NACKs)
// Needs to be large enough to hold the largest RAWX frame (8 + 16 + (32 * numMeas))
// RAWX frames can be in excess of 2KB
const size_t UBXbufferSize = 4096;
char UBXbuffer[UBXbufferSize];
size_t UBXpointer = 0;

// Storage for the SD packet
// Define packet size, buffer and buffer pointer for SD card writes
const size_t SDpacket = 512;
uint8_t SDbuffer[SDpacket];
size_t SDpointer = 0;

int maxGNSSbufferAvailable = 0; // Record how full the GNSSbuffer gets

unsigned long lastRAWXmicros1 = 0; // The last times (micros) that a RAWX frame was received
unsigned long lastRAWXmicros2 = 0;
unsigned long lastRAWXmicros3 = 0;
unsigned long lastRAWXmicros4 = 0;
unsigned long lastRAWXmicros5 = 0;
unsigned long lastRAWXdebug = 0; // The last time (millis) that a RAWX frame debug message was printed

unsigned long lastPVTmicros1 = 0; // The last times (micros) that a PVT frame was received
unsigned long lastPVTmicros2 = 0;
unsigned long lastPVTmicros3 = 0;
unsigned long lastPVTmicros4 = 0;
unsigned long lastPVTmicros5 = 0;
unsigned long lastPVTdebug = 0; // The last time (millis) that a PVT frame debug message was printed

unsigned long lastHPPOSLLHmicros1 = 0; // The last times (micros) that a HPPOSLLH frame was received
unsigned long lastHPPOSLLHmicros2 = 0;
unsigned long lastHPPOSLLHmicros3 = 0;
unsigned long lastHPPOSLLHmicros4 = 0;
unsigned long lastHPPOSLLHmicros5 = 0;
unsigned long lastHPPOSLLHdebug = 0; // The last time (millis) that a HPPOSLLH frame debug message was printed

unsigned long lastRELPOSNEDmicros1 = 0; // The last times (micros) that a RELPOSNED frame was received
unsigned long lastRELPOSNEDmicros2 = 0;
unsigned long lastRELPOSNEDmicros3 = 0;
unsigned long lastRELPOSNEDmicros4 = 0;
unsigned long lastRELPOSNEDmicros5 = 0;
unsigned long lastRELPOSNEDdebug = 0; // The last time (millis) that a RELPOSNED frame debug message was printed

bool storeData(void)
{
  bool ret_val = true; // Return value

  if (!qwiicOnline.uBlox || !qwiicAvailable.uBlox)
    goto SD_WRITE; // u-blox is offline so let's not try to talk to it. Still try to save any remaining data to SD.

  // Check for new I2C data three times faster than usBetweenReadings to avoid pounding the I2C bus
  if ((micros() - lastReadTime) > (settings.usBetweenReadings / 3))
  {
    // **** Start of code taken from checkUbloxI2C ****

    // Get the number of bytes available from the module
    uint16_t bytesAvailable = 0;
    qwiic.beginTransmission(settings.sensor_uBlox.ubloxI2Caddress);
    qwiic.write(0xFD); // 0xFD (MSB) and 0xFE (LSB) are the registers that contain number of bytes available
    if (qwiic.endTransmission(false) != 0) // Send a restart command. Do not release bus.
    {
      ret_val = false;
      goto SD_WRITE; // Sensor did not ACK so bail
    }
    qwiic.requestFrom(settings.sensor_uBlox.ubloxI2Caddress, (uint8_t)2);
    if (qwiic.available())
    {
      uint8_t msb = qwiic.read();
      uint8_t lsb = qwiic.read();
      if (lsb == 0xFF)
      {
        // u-blox bug. Device should never present an 0xFF.
        if (settings.printMajorDebugMessages)
        {
          Serial.println(F("storeData: Ublox bug, length lsb is 0xFF"));
        }
        lastReadTime = micros(); // Put off checking to avoid I2C bus traffic
        ret_val = false;
        goto SD_WRITE; // Bail
      }
      bytesAvailable = (uint16_t)msb << 8 | lsb;
    }

    if (bytesAvailable == 0)
    {
      lastReadTime = micros(); // Put off checking to avoid I2C bus traffic
      goto SD_WRITE; // No data to read (we don't flag this as an error - there's just no data available)
    }

    // Check for undocumented bit error. We found this doing logic scans.
    // This error is rare but if we incorrectly interpret the first bit of the two 'data available' bytes as 1
    // then we have far too many bytes to check.
    // May be related to I2C setup time violations: https://github.com/sparkfun/SparkFun_Ublox_Arduino_Library/issues/40
    // Or pull-ups? (PaulZC)
    if (bytesAvailable & ((uint16_t)1 << 15))
    {
      // Clear the MSbit
      bytesAvailable &= ~((uint16_t)1 << 15);

      if (settings.printMajorDebugMessages)
      {
        Serial.print(F("storeData: Bytes available error:"));
        Serial.println(bytesAvailable);
      }
    }

    while (bytesAvailable)
    {
      qwiic.beginTransmission(settings.sensor_uBlox.ubloxI2Caddress);
      qwiic.write(0xFF);                     // 0xFF is the register to read data from
      if (qwiic.endTransmission(false) != 0) // Send a restart command. Do not release bus.
      {
        ret_val = false;
        goto SD_WRITE; // Sensor did not ACK so bail
      }

      // Limit to 32 bytes or whatever the buffer limit is for given platform
      uint16_t bytesToRead = bytesAvailable;
      if (bytesToRead > I2C_BUFFER_LENGTH)
        bytesToRead = I2C_BUFFER_LENGTH;

TRY_AGAIN:

      qwiic.requestFrom(settings.sensor_uBlox.ubloxI2Caddress, (uint8_t)bytesToRead);
      if (qwiic.available())
      {
        for (uint16_t x = 0; x < bytesToRead; x++)
        {
          uint8_t incoming = qwiic.read(); // Grab the actual character

          processUbx(incoming); // Process the incoming byte. This will update parseUbxState

          UBXbuffer[UBXpointer] = incoming; // Store incoming in the UBX buffer
          UBXpointer++; // Increment the pointer
          if (UBXpointer == UBXbufferSize) // This should never happen!
          {
            Serial.print(F("storeData: UBXbuffer overflow! You need to increase the size of UBXbufferSize in storeData. Freezing..."));
            while (1)
              ;
          }

          if (parseUbxState == SYNC_LOST) // If the UBX frame was invalid or we lost sync
          {
            // TO DO: close the current file and open a new one when parseUbxState == SYNC_LOST ?
            UBXpointer = 0; // Discard the frame by resetting the UBXpointer
            parseUbxState = PARSE_UBX_SYNC_CHAR_1; // Start looking for the start of a new frame
          }
          else if (parseUbxState == FRAME_VALID) // If the UBX frame is valid
          {
            if (UBXbuffer[2] != UBX_CLASS_ACK) // If the frame is not an ACK/NACK
            {
              // Check for a RAWX frame so we can calculate the frame rate
              if (UBXbuffer[2] == UBX_CLASS_RXM) // Class 0x02
              {
                if (UBXbuffer[3] == UBX_RXM_RAWX) // ID 0x15
                {
                  lastRAWXmicros5 = lastRAWXmicros4; // Update lastRAWXmicros
                  lastRAWXmicros4 = lastRAWXmicros3;
                  lastRAWXmicros3 = lastRAWXmicros2;
                  lastRAWXmicros2 = lastRAWXmicros1;
                  lastRAWXmicros1 = micros();
                  if ((millis() - lastRAWXdebug) > 5000UL) // Print a debug message every 5 secs if enabled
                  {
                    if (settings.printMinorDebugMessages)
                    {
                      Serial.print(F("storeData: RAWX interval (secs, approximate): "));
                      float interval1 = ((float)(lastRAWXmicros1 - lastRAWXmicros2)) / 1000000;
                      float interval2 = ((float)(lastRAWXmicros2 - lastRAWXmicros3)) / 1000000;
                      float interval3 = ((float)(lastRAWXmicros3 - lastRAWXmicros4)) / 1000000;
                      float interval4 = ((float)(lastRAWXmicros4 - lastRAWXmicros5)) / 1000000;
                      float interval = (interval1 + interval2 + interval3 + interval4) / 4;
                      Serial.println(interval, 3);
                    }
                    lastRAWXdebug = millis(); // Update lastRAWXdebug
                  }
                }
              }

              // Check for a PVT frame so we can calculate the frame rate
              if (UBXbuffer[2] == UBX_CLASS_NAV) // Class 0x01
              {
                if (UBXbuffer[3] == UBX_NAV_PVT) // ID 0x07
                {
                  lastPVTmicros5 = lastPVTmicros4; // Update lastPVTmicros
                  lastPVTmicros4 = lastPVTmicros3;
                  lastPVTmicros3 = lastPVTmicros2;
                  lastPVTmicros2 = lastPVTmicros1;
                  lastPVTmicros1 = micros();
                  if ((millis() - lastPVTdebug) > 5000UL) // Print a debug message every 5 secs if enabled
                  {
                    if (settings.printMinorDebugMessages)
                    {
                      Serial.print(F("storeData: PVT interval (secs, approximate): "));
                      float interval1 = ((float)(lastPVTmicros1 - lastPVTmicros2)) / 1000000;
                      float interval2 = ((float)(lastPVTmicros2 - lastPVTmicros3)) / 1000000;
                      float interval3 = ((float)(lastPVTmicros3 - lastPVTmicros4)) / 1000000;
                      float interval4 = ((float)(lastPVTmicros4 - lastPVTmicros5)) / 1000000;
                      float interval = (interval1 + interval2 + interval3 + interval4) / 4;
                      Serial.println(interval, 3);
                    }
                    lastPVTdebug = millis(); // Update lastPVTdebug
                  }
                }
              }

              // Check for a HPPOSLLH frame so we can calculate the frame rate
              if (UBXbuffer[2] == UBX_CLASS_NAV) // Class 0x01
              {
                if (UBXbuffer[3] == UBX_NAV_HPPOSLLH) // ID 0x14
                {
                  lastHPPOSLLHmicros5 = lastHPPOSLLHmicros4; // Update lastHPPOSLLHmicros
                  lastHPPOSLLHmicros4 = lastHPPOSLLHmicros3;
                  lastHPPOSLLHmicros3 = lastHPPOSLLHmicros2;
                  lastHPPOSLLHmicros2 = lastHPPOSLLHmicros1;
                  lastHPPOSLLHmicros1 = micros();
                  if ((millis() - lastHPPOSLLHdebug) > 5000UL) // Print a debug message every 5 secs if enabled
                  {
                    if (settings.printMinorDebugMessages)
                    {
                      Serial.print(F("storeData: HPPOSLLH interval (secs, approximate): "));
                      float interval1 = ((float)(lastHPPOSLLHmicros1 - lastHPPOSLLHmicros2)) / 1000000;
                      float interval2 = ((float)(lastHPPOSLLHmicros2 - lastHPPOSLLHmicros3)) / 1000000;
                      float interval3 = ((float)(lastHPPOSLLHmicros3 - lastHPPOSLLHmicros4)) / 1000000;
                      float interval4 = ((float)(lastHPPOSLLHmicros4 - lastHPPOSLLHmicros5)) / 1000000;
                      float interval = (interval1 + interval2 + interval3 + interval4) / 4;
                      Serial.println(interval, 3);
                    }
                    lastHPPOSLLHdebug = millis(); // Update lastHPPOSLLHdebug
                  }
                }
              }

              // Check for a RELPOSNED frame so we can calculate the frame rate
              if (UBXbuffer[2] == UBX_CLASS_NAV) // Class 0x01
              {
                if (UBXbuffer[3] == UBX_NAV_RELPOSNED) // ID 0x3C
                {
                  lastRELPOSNEDmicros5 = lastRELPOSNEDmicros4; // Update lastRELPOSNEDmicros
                  lastRELPOSNEDmicros4 = lastRELPOSNEDmicros3;
                  lastRELPOSNEDmicros3 = lastRELPOSNEDmicros2;
                  lastRELPOSNEDmicros2 = lastRELPOSNEDmicros1;
                  lastRELPOSNEDmicros1 = micros();
                  if ((millis() - lastRELPOSNEDdebug) > 5000UL) // Print a debug message every 5 secs if enabled
                  {
                    if (settings.printMinorDebugMessages)
                    {
                      Serial.print(F("storeData: RELPOSNED interval (secs, approximate): "));
                      float interval1 = ((float)(lastRELPOSNEDmicros1 - lastRELPOSNEDmicros2)) / 1000000;
                      float interval2 = ((float)(lastRELPOSNEDmicros2 - lastRELPOSNEDmicros3)) / 1000000;
                      float interval3 = ((float)(lastRELPOSNEDmicros3 - lastRELPOSNEDmicros4)) / 1000000;
                      float interval4 = ((float)(lastRELPOSNEDmicros4 - lastRELPOSNEDmicros5)) / 1000000;
                      float interval = (interval1 + interval2 + interval3 + interval4) / 4;
                      Serial.println(interval, 3);
                    }
                    lastRELPOSNEDdebug = millis(); // Update lastRELPOSNEDdebug
                  }
                }
              }

              // Check for a UBX-NAV-TIMEUTC frame so we can set the RTC
              if (UBXbuffer[2] == UBX_CLASS_NAV) // Class 0x01
              {
                if (UBXbuffer[3] == UBX_NAV_TIMEUTC) // ID 0x21
                {
                  if (rtcSyncRequiredFlag) // Do we need to sync the RTC
                  {
                    if (((UBXbuffer[25] & 0x04) == 0x04) && (rtcSyncRequiredFlag)) // If the validUTC flag bit is set and a sync is needed
                    {
                      uint16_t rtcYear = ((uint16_t)UBXbuffer[18]) | (((uint16_t)UBXbuffer[19]) << 8); //RTC year
                      union {
                        int32_t signed32;
                        uint32_t unsigned32;
                      } nanos; // Union for accurate conversion to int32_t without needing a cast
                      // Assemble the nanos
                      nanos.unsigned32 = ((uint32_t)UBXbuffer[14]) | (((uint32_t)UBXbuffer[15]) << 8) | (((uint32_t)UBXbuffer[16]) << 16) | (((uint32_t)UBXbuffer[17]) << 24);
                      // If nanos is negative, set it to zero
                      // The ZED-F9P Integration Manual says: "the nano value can range from -5000000 (i.e. -5 ms) to +994999999 (i.e. nearly 995 ms)."
                      // "if a resolution of one hundredth of a second is adequate, negative nano values can simply be rounded up to 0 and effectively ignored."
                      if (nanos.signed32 < 0)
                        nanos.signed32 = 0;
                      uint8_t centis = (uint8_t)(nanos.unsigned32 / 10000000); // Convert nanos to hundredths (centiseconds)
                      rtc.setTime(UBXbuffer[22], UBXbuffer[23], UBXbuffer[24], centis, UBXbuffer[21], UBXbuffer[20], (rtcYear - 2000)); // Set the RTC
                      rtcSyncFlag = true; // Set rtcSyncFlag to show RTC has been synced
                      rtcSyncRequiredFlag = false; // Clear rtcSyncRequiredFlag so we don't set the RTC multiple times
                      if (settings.printMinorDebugMessages)
                      {
                        Serial.printf("storeData: RTC synced to %04d-%02d-%02d %02d:%02d:%02d.%02d\n",
                                      rtcYear, UBXbuffer[20], UBXbuffer[21], UBXbuffer[22], UBXbuffer[23], UBXbuffer[24], centis);
                        Serial.print(F("storeData: RTC time is ")); printDateTime();
                      }
                    }
                  }
                }
              }

              // Copy this frame into the GNSS buffer
              for (size_t i = 0; i < UBXpointer; i++) //For each char in the frame
              {
                //TO DO: speed this up by doing some form of memory copy
                GNSSbuffer.store_char(UBXbuffer[i]); //Copy it into the GNSS buffer
              }
            }
            else // Frame is an ACK/NACK
            {
              if (settings.printMajorDebugMessages)
              {
                if (UBXbuffer[3] == UBX_ACK_NACK) // If this is a NACK
                {
                  Serial.print(F("UBX NACK Class:0x"));
                }
                else if (UBXbuffer[3] == UBX_ACK_ACK) // If this is a ACK
                {
                  Serial.print(F("UBX ACK Class:0x"));
                }
                Serial.print(UBXbuffer[6], HEX);
                Serial.print(F(" ID:0x"));
                Serial.println(UBXbuffer[7], HEX);
              }
            }
            UBXpointer = 0; // Reset the UBXpointer
            parseUbxState = PARSE_UBX_SYNC_CHAR_1; // Start looking for the start of a new frame
          }
        }
      }
      else
      {
        ret_val = false; // Sensor provided no data so bail
        goto SD_WRITE;
      }

      bytesAvailable -= bytesToRead;
    }

    // **** End of code taken from checkUbloxI2C ****
  }

SD_WRITE:

  // Record one packet to SD
  // Only do this if logging is enabled and the SD card is ready
  if (settings.logData  && online.microSd && online.dataLogging)
  {
    int bufAvail = GNSSbuffer.available(); // Check how many bytes are in the GNSS buffer

    if (bufAvail > maxGNSSbufferAvailable) // Check if we have reached a new Max bufAvail
    {
      maxGNSSbufferAvailable = bufAvail; // We have - so record it
      if (settings.printMajorDebugMessages) // If debug messages are enabled
      {
        Serial.print(F("storeData: Max bufAvail: ")); // Print the new Max bufAvail so we can watch how full the buffer gets
        Serial.println(maxGNSSbufferAvailable);
      }
    }

    bool keep_going = true; // Use this to control how many chars are processed
    if (bufAvail == 0) // Check if we have data to write
      keep_going = false;

    while (keep_going)
    {
      uint8_t c = GNSSbuffer.read_char(); // Read a char from the buffer
      SDbuffer[SDpointer] = c;            // Store it in the SDbuffer
      SDpointer++;                        // Increment the SDpointer
      if (SDpointer == SDpacket)          // Have we reached SDpacket (512) bytes?
      {
        SDpointer = 0;                    // Reset the SDpointer
        digitalWrite(PIN_STAT_LED, HIGH); // Flash the LED while writing
        file.write(SDbuffer, SDpacket);   // Record the buffer to the card
        digitalWrite(PIN_STAT_LED, LOW);
        keep_going = false;               // Stop now that we have written one packet
      }

      bufAvail--; // Decrement bufAvail
      if (bufAvail == 0) // Stop if we have run out of data
      {
        keep_going = false;
      }
    }

    // Force sync every 1000 ms
    if (millis() - lastDataLogSyncTime > 500)
    {
      lastDataLogSyncTime = millis();
      digitalWrite(PIN_STAT_LED, HIGH);   // Flash the LED while writing
      if (SDpointer > 0)                  // Check if we have any 'extra' bytes in SDbuffer
      {
        file.write(SDbuffer, SDpointer);  // Write the 'extra' bytes
        SDpointer = 0;                    // Reset the SDpointer
      }
      file.sync();                        // Sync the file system
      //updateDataFileAccess();             // Update the file access timestamp
      digitalWrite(PIN_STAT_LED, LOW);
    }

  }
  else // Logging is disabled so discard the data in order to not overflow the buffer
  {
    if (settings.printMinorDebugMessages)
    {
      Serial.println(F("storeData: we are not logging. Discarding data!"));
    }
    while (GNSSbuffer.available())
    {
      GNSSbuffer.read_char();
    }
  }
  return (ret_val);
}
