#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include "CheckedLogFile.h"

// =====================================================
// STORAGE - SD card probe, mount, logging, recovery
//
// A missing or broken card never stops the flight.
// =====================================================

extern SPIClass sdSPI;
extern CheckedLogFile logFile;

extern char          logFileName[32];
extern unsigned long logLineCount; // complete rows verified by checkpoint readback
extern unsigned long sdErrorCount;
extern unsigned long sdCheckpointLastUs;
extern unsigned long sdCheckpointMaxUs;

// Which /FLIGHT%03d.CSV the name above resolved to, 1..999. Kept beside the
// name because the name cannot go on the air: the downlink parses numeric
// K=V pairs only, so the ground station can be told the FILE but not its
// filename. 0 means no file was ever opened - the loop that picks one starts
// at 1, so it is a value a mounted card can never report.
extern int           logFileIndex;

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
