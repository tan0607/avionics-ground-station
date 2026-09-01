#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD.h>

// =====================================================
// STORAGE - SD card probe, mount, logging, recovery
//
// A missing or broken card never stops the flight.
// =====================================================

extern SPIClass sdSPI;
extern File     logFile;

extern char          logFileName[32];
extern unsigned long logLineCount;
extern unsigned long sdErrorCount;

// Hardware probe results
#define PROBE_NO_MODULE 0
#define PROBE_MISO_LOW  1
#define PROBE_NO_CARD   2
#define PROBE_CARD_OK   3

extern uint8_t probeResult;

bool initSD(bool verbose);
void startNewLogFile();

void serviceLogging();
void flushSD();

void dumpLogFile();
void listFiles();
void formatCard();
void printSdHelp();
