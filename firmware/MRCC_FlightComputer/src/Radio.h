#pragma once
#include <Arduino.h>

// =====================================================
// RADIO - LoRa init and non-blocking transmit
//
// The loop is never blocked waiting for air time. A
// dead radio is skipped, not fatal.
// =====================================================

extern int  txPower;      // dBm, 2..17
extern int  txCopies;     // 1 or 2 copies per packet
extern bool showPacket;   // print full packet contents

extern unsigned long lastAirTime;
extern unsigned long txBusyCount;
extern unsigned long txTimeoutCount;
extern unsigned long txFallbackCount;

extern char txPacket[250];
extern int  txPacketLen;

bool initRadio(bool verbose);
void serviceTelemetry();
void applyTxPower();
