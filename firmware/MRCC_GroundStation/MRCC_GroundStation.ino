// =====================================================
// MRCC GROUND STATION - LoRa receiver
//
// Listens on one vehicle's channel and prints every
// frame to USB as:
//
//   len=<n> RSSI=<dBm> SNR=<dB> | MRCC,PKT=...
//
// That exact line shape is what the laptop backend
// parses (shared/protocol/mrcc.py in the ground-station
// repo), so the len=/RSSI=/SNR= prefix is part of the
// contract - do not reorder or rename it.
//
// Board: classic ESP32 DevKit (NOT the S3 - the pin map
// below is the classic VSPI bus). The flight computer
// is the S3; these are two different boards and two
// different sketches.
// =====================================================

#include <SPI.h>
#include <LoRa.h>


// -----------------------------------------------------
// VEHICLE  <-- SET THIS BEFORE EVERY FLASH
//
// THIS MUST MATCH THE ROCKET THIS BOX IS LISTENING TO.
// Same block, same values, as MRCC_FlightComputer/src/
// Config.h - change the two together or this receiver
// is simply deaf.
//
// There is NO partial-reception failure mode to warn
// you. Wrong channel does not mean weak signal or
// garbled text; it means zero packets, forever, on a
// receiver that boots fine and says "RX ready". The
// boot banner below is the only place the mistake is
// visible, so read it before you walk to the pad.
//
// Rocket A -> 433.3 MHz      Rocket B -> 434.1 MHz
//
// Why they cannot share: LoRa does not pair. A receiver
// decodes every packet whose freq/SF/BW/CR/syncword
// match, whoever sent it. On one channel this box would
// decode BOTH rockets - PKT jumps, the loss count turns
// to noise, the map hops between airframes - while the
// two transmitters collide on air and neither link
// survives. One rocket alone already radiates ~73% of
// the time (two ~182 ms copies per 500 ms window), so
// there is no room to share and no setting short of a
// different frequency makes room.
// -----------------------------------------------------

#define VEHICLE_A 1
#define VEHICLE_B 2

#define VEHICLE  VEHICLE_A          // <<<< CHANGE ME PER ROCKET

#if   VEHICLE == VEHICLE_A
  #define LORA_FREQ     433300000   // 433.3 MHz
  #define VEHICLE_NAME  "A"
#elif VEHICLE == VEHICLE_B
  #define LORA_FREQ     434100000   // 434.1 MHz
  #define VEHICLE_NAME  "B"
#else
  // Deliberately fatal. A typo in VEHICLE must not fall
  // through to "whatever was here last" - that is the
  // exact mistake this block exists to catch.
  #error "VEHICLE must be VEHICLE_A or VEHICLE_B"
#endif


// -----------------------------------------------------
// PINS - classic ESP32 VSPI
// -----------------------------------------------------

#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  22
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26


// -----------------------------------------------------
// RADIO PARAMETERS
//
// Every one of these must equal the transmitter's
// (MRCC_FlightComputer/src/Radio.cpp, initRadio). A
// mismatch in ANY of them is silent - same failure as a
// wrong channel: zero packets, no error.
// -----------------------------------------------------

#define LORA_SF        7
#define LORA_BW    250E3
#define LORA_CR        5
#define LORA_PREAMBLE  8
#define LORA_SYNCWORD  0x12


volatile bool gotPkt = false;
char    gBuf[256];
int     gLen  = 0;
int     gRssi = 0;
float   gSnr  = 0;

void onRx(int n) {
  if (n <= 0 || n > 250 || gotPkt) return;   // 上一包还没处理完就跳过
  int i = 0;
  while (LoRa.available() && i < 250) gBuf[i++] = (char)LoRa.read();
  gBuf[i] = '\0';
  gLen  = i;
  gRssi = LoRa.packetRssi();
  gSnr  = LoRa.packetSnr();
  gotPkt = true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== RX BOOT ===");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);   // ← 必须在前面

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) { Serial.println("begin FAILED"); while(1); }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  // Set explicitly, even though 0x12 is also the SX1278's
  // reset default. It matched the transmitter by luck
  // before; luck is not a link parameter, and the next
  // person to read this file should see all five in one
  // place rather than four here and one in a datasheet.
  LoRa.setSyncWord(LORA_SYNCWORD);
  LoRa.enableCrc();

  LoRa.onReceive(onRx);
  LoRa.receive();                            // 进 continuous RX mode

  // Which vehicle, not just "ready". This is the one
  // line that catches a box flashed for the wrong rocket.
  Serial.print("RX ready - vehicle ");
  Serial.print(VEHICLE_NAME);
  Serial.print(" @ ");
  Serial.print(LORA_FREQ / 1e6, 3);
  Serial.println(" MHz  (rocket must match)");
}

void loop() {
  if (gotPkt) {
    Serial.printf("len=%d RSSI=%d SNR=%.1f | %s\n", gLen, gRssi, gSnr, gBuf);
    gotPkt = false;
  }
}
