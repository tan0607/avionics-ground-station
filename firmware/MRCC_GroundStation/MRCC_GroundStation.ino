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
// ONE BOX, TWO ROCKETS: the channel is switchable at
// runtime from the serial monitor (press A or B), so a
// single ground station can cover both airframes across
// a launch day - listen to A, fly it, switch, fly B. No
// reflash, no reboot, and the backend's serial
// connection survives the switch.
//
// Board: classic ESP32 DevKit (NOT the S3 - the pin map
// below is the classic VSPI bus). The flight computer
// is the S3; these are two different boards and two
// different sketches.
// =====================================================

#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>


// -----------------------------------------------------
// CHANNELS
//
// These two frequencies MUST match the ones in
// MRCC_FlightComputer/src/Config.h. They are the only
// numbers duplicated between the two sketches; Arduino
// builds each sketch on its own, so there is no header
// to share. Change one, change the other.
//
// Rocket A -> 433.3 MHz      Rocket B -> 434.1 MHz
//
// Why they cannot share one channel: LoRa does not pair.
// A receiver decodes every packet whose freq/SF/BW/CR/
// syncword match, whoever sent it. On one channel this
// box would decode BOTH rockets - PKT jumps, the loss
// count turns to noise, the map hops between airframes -
// while the two transmitters collide on air and neither
// link survives. One rocket alone already radiates 87% of
// the time - two 187 ms copies plus the 60 ms COPY_GAP,
// inside a 500 ms window, leaving 66 ms of margin - so
// there is no room to share.
//
// That figure used to read ~73%, from two ~182 ms copies
// and no gap. The gap was always there, and the packet
// has since grown to 237 bytes. TX_Doctor's test 3
// measures it on the bench; re-read it after any change
// to the packet, because the number moves with it.
//
// Both sit inside Malaysia's 433 MHz ISM allocation
// (MCMC: 433.05 - 434.79 MHz) and are 800 kHz apart,
// comfortably wider than the 250 kHz occupied bandwidth.
// -----------------------------------------------------

#define CHANNEL_A_HZ  433300000L
#define CHANNEL_B_HZ  434100000L

#define VEHICLE_A 0
#define VEHICLE_B 1

// Which channel this box powers on listening to. Only
// the DEFAULT - A and B on the serial monitor override
// it at any time, and the override is not remembered
// across a reset. Set it to whichever rocket flies first
// so an unattended boot lands on the right one.
#define VEHICLE  VEHICLE_A

#if (VEHICLE != VEHICLE_A) && (VEHICLE != VEHICLE_B)
  // Catches an out-of-range NUMBER (#define VEHICLE 5).
  // A misspelled NAME (VEHICLE_C) slips past this test --
  // an undefined identifier is 0 in #if, and VEHICLE_A is
  // 0 -- but it is still fatal one line later, where the
  // compiler rejects `gChannel = VEHICLE` as undeclared.
  // Both ways fail the build, which is the whole point:
  // a bad VEHICLE must never fall through to "whatever
  // index that happened to be".
  #error "VEHICLE must be VEHICLE_A or VEHICLE_B"
#endif

struct Channel {
  const char *name;
  long        hz;
};

static const Channel CHANNELS[] = {
  { "A", CHANNEL_A_HZ },   // index must equal VEHICLE_A
  { "B", CHANNEL_B_HZ },   // index must equal VEHICLE_B
};
static const int N_CHANNELS = sizeof(CHANNELS) / sizeof(CHANNELS[0]);

static int gChannel = VEHICLE;    // index into CHANNELS


// -----------------------------------------------------
// PINS - classic ESP32 VSPI
// -----------------------------------------------------

#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  22
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26

// Link-alive LED. Pulses on every packet received, which is the one thing you
// can read from across a field with the laptop shut: lit and flickering means
// frames are arriving, dark means they are not. It says nothing about WHICH
// channel -- the console and the boot banner are the authorities on that.
//
// GPIO2 on most classic DevKits. If yours has no LED there, nothing breaks: the
// pin just toggles with nothing attached.
#define PIN_LED     2

#define LED_PULSE_MS  40        // packet-RX blink


// -----------------------------------------------------
// RADIO PARAMETERS
//
// Every one of these must equal the transmitter's
// (MRCC_FlightComputer/src/Radio.cpp, initRadio). A
// mismatch in ANY of them is silent - the same failure
// as a wrong channel: zero packets, no error.
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

// Packets heard since boot or since the last channel
// switch. Printed on switch and by '?', because "did
// this channel actually have a rocket on it" is the
// question you are asking when you press either.
static unsigned long gPktCount = 0;

// LED pulse deadline for packet activity. Non-blocking: a receiver that slept
// 40 ms per packet would drop the second copy of every frame.
static unsigned long gLedOffAt = 0;

// The chosen channel outlives a reboot, and that is not a nicety. Attaching the
// backend RESETS this board -- sources.py::_reset_board() pulses EN on connect,
// deliberately, to get a known state and a boot banner. Without persistence,
// every backend reconnect would silently drag the box back to the compile-time
// default: you switch to rocket B, the laptop reconnects, and you are listening
// to A again with nothing on screen saying so. That is the exact silent-failure
// shape this whole channel scheme exists to avoid.
//
// So VEHICLE is the FACTORY default -- first boot, or after a flash erase. What
// the operator last chose wins on every boot after that. NVS is only written
// when the channel actually changes, which is a few times a launch day.
static Preferences gPrefs;
static const char *NVS_NAMESPACE = "mrccgs";
static const char *NVS_KEY_CH    = "ch";

void onRx(int n) {
  if (n <= 0 || n > 255 || gotPkt) return;   // 上一包还没处理完就跳过
  int i = 0;
  // 255 is LoRa's payload limit and gBuf holds 255 + NUL, so nothing the
  // radio can legally deliver is dropped here. This used to stop at 250,
  // which truncated a long packet AND reported the truncated length - so
  // the len= integrity check on the laptop passed and the frame decoded
  // as a clean short one, missing its tail.
  while (LoRa.available() && i < 255) gBuf[i++] = (char)LoRa.read();
  gBuf[i] = '\0';
  gLen  = i;
  gRssi = LoRa.packetRssi();
  gSnr  = LoRa.packetSnr();
  gotPkt = true;
}


// =====================================================
// CHANNEL SWITCH
//
// Retune without a reflash or a reboot.
//
// The detach is NOT optional. handleDio0Rise() runs in
// ISR context and does its own SPI reads (IRQ flags,
// payload length, FIFO pointer) BEFORE it ever calls
// onRx. So retuning from loop() while the interrupt is
// live races the ISR for the SPI bus. onReceive(NULL)
// detaches DIO0 cleanly; re-arming is onReceive(onRx).
//
// idle() likewise is not decoration: setFrequency() only
// writes the FRF registers, and the SX1278 latches them
// on the next mode transition. Without the standby ->
// RX hop the radio can keep listening on the old
// channel while every register says otherwise.
// =====================================================

static void applyChannel(int idx, bool announce) {
  gChannel = idx;

  LoRa.onReceive(NULL);                 // detach DIO0 - no ISR SPI past here
  LoRa.idle();                          // modem to standby
  LoRa.setFrequency(CHANNELS[idx].hz);  // retune
  LoRa.onReceive(onRx);                 // re-arm
  LoRa.receive();                       // back to continuous RX

  // Any half-received frame from the old channel died
  // with the mode change; do not print its remains.
  gotPkt = false;

  if (announce) {
    // Marker into the stream, so raw.log records WHEN the
    // operator switched. Deliberately carries no "MRCC"
    // substring: mrcc.py keys on that word, and a line
    // without it is ignored rather than half-parsed.
    //
    // NOTE the PKT discontinuity this creates. The two
    // rockets count independently, so the ground station's
    // loss tracker sees a jump. backend/loss.py treats a
    // forward jump over RESET_GAP (1000) as a restart and
    // does not invent loss -- but a jump UNDER it is
    // counted as lost packets. Cut a new session or a new
    // flight after switching and the numbers stay honest.
    Serial.printf("### GS CHANNEL=%s FREQ=%.3fMHz PREV_PKTS=%lu ###\n",
                  CHANNELS[idx].name, CHANNELS[idx].hz / 1e6, gPktCount);

    // Only on an operator switch, and only on a real change: NVS is flash, and
    // rewriting it on every boot would wear it for nothing.
    if (gPrefs.getInt(NVS_KEY_CH, -1) != idx) gPrefs.putInt(NVS_KEY_CH, idx);
  }
  gPktCount = 0;
}


static void printStatus() {
  Serial.printf("### GS STATUS channel=%s freq=%.3fMHz pkts=%lu "
                "last_rssi=%d last_snr=%.1f ###\n",
                CHANNELS[gChannel].name, CHANNELS[gChannel].hz / 1e6,
                gPktCount, gRssi, gSnr);
}


static void printHelp() {
  Serial.println("### GS keys:  A = rocket A   B = rocket B   ? = status ###");
}


// One key per press, no line buffering - the serial
// monitor's "no line ending" mode is what an operator
// reaches for with gloves on.
static void handleSerial() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    switch (c) {
      case 'a': case 'A': applyChannel(VEHICLE_A, true); break;
      case 'b': case 'B': applyChannel(VEHICLE_B, true); break;
      case '?': case 'h': case 'H': printStatus(); printHelp(); break;
      default: break;    // ignore stray newlines and everything else
    }
  }
}


void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== RX BOOT ===");

  // Restore the operator's last choice before the radio comes up, so begin()
  // opens on the right channel instead of tuning twice.
  gPrefs.begin(NVS_NAMESPACE, false);
  int saved = gPrefs.getInt(NVS_KEY_CH, -1);
  bool restored = (saved >= 0 && saved < N_CHANNELS);
  if (restored) gChannel = saved;

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);   // ← 必须在前面

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(CHANNELS[gChannel].hz)) { Serial.println("begin FAILED"); while(1); }

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
  // line that catches a box left on the wrong rocket.
  Serial.print("RX ready - vehicle ");
  Serial.print(CHANNELS[gChannel].name);
  Serial.print(" @ ");
  Serial.print(CHANNELS[gChannel].hz / 1e6, 3);
  Serial.print(" MHz  ");
  // Say WHERE the channel came from. "Restored" vs "compile default" is the
  // difference between "this box remembers yesterday" and "this box just forgot
  // what you told it", and only one of those needs acting on.
  Serial.println(restored ? "(restored from last switch; rocket must match)"
                          : "(compile-time default; rocket must match)");
  printHelp();
}

void loop() {
  handleSerial();

  if (gotPkt) {
    gPktCount++;
    Serial.printf("len=%d RSSI=%d SNR=%.1f | %s\n", gLen, gRssi, gSnr, gBuf);
    gotPkt = false;

    digitalWrite(PIN_LED, HIGH);            // link is alive, at a glance
    gLedOffAt = millis() + LED_PULSE_MS;
  }

  // Unsigned-safe deadline compare: millis() wraps at ~49 days and a plain
  // `millis() >= gLedOffAt` would stick the LED on across the wrap.
  if (gLedOffAt && (long)(millis() - gLedOffAt) >= 0) {
    digitalWrite(PIN_LED, LOW);
    gLedOffAt = 0;
  }
}
