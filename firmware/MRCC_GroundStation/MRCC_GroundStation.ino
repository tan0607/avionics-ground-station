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
// WHAT ARRIVES ON THE AIR IS NO LONGER THAT LINE. The
// vehicle downlinks a 52-67 byte binary packet now, and
// this sketch expands it back into the identical ASCII
// above. The ASCII packet cost 185-237 bytes, which is
// 149-187 ms of air, which does not fit in the 100 ms
// window a 10 Hz link has - so the format had to shrink,
// and this is where it grows back.
//
// Doing the expansion HERE rather than on the laptop is
// the whole reason the rest of the system did not have
// to change: mrcc.py, the loss tracker, telemetry.csv,
// the WebSocket and the dashboard all still consume the
// one shape they already knew. The binary exists only
// between the two radios.
//
// An ASCII packet still decodes. A vehicle running the
// old firmware is passed through untouched (see loop),
// so a box flashed with this sketch talks to either
// build - which matters on a launch day where the two
// airframes may not be flashed from the same commit.
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
#include <math.h>
#include <string.h>


// -----------------------------------------------------
// CHANNELS
//
// These two frequencies MUST match the ones in
// MRCC_FlightComputer_A/src/Config.h. They are the only
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
// link survives. One rocket alone radiates ~60% of the
// time - a single 48-60 ms binary packet inside a 100 ms
// window - so there is no room to share.
//
// That figure has read 73% and then 87%, from two ASCII
// copies plus COPY_GAP in a 500 ms window. The 10 Hz
// binary downlink brought it DOWN to ~60%, and it still
// does not make room: at 60% duty a second transmitter
// on this channel collides with well over half of these
// packets. TX_Doctor's test 3 measures it on the bench;
// re-read it after any change to the packet, because the
// number moves with it.
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
// (MRCC_FlightComputer_A/src/Radio.cpp, initRadio). A
// mismatch in ANY of them is silent - the same failure
// as a wrong channel: zero packets, no error.
// -----------------------------------------------------

#define LORA_SF        7
#define LORA_BW    250E3
#define LORA_CR        5
#define LORA_PREAMBLE  8
#define LORA_SYNCWORD  0x12


volatile bool gotPkt = false;
// BYTES. The payload is binary and 0x00 is a legal value inside it, so nothing
// here may treat this as a C string. gLen is the authority on length.
uint8_t gBuf[256];
int     gLen  = 0;

// Where the decoded ASCII line is built. 320 is comfortably above the ~215 the
// widest possible expansion produces (every field at its format maximum, both
// optional blocks present) and well under the 255-byte limit that applied to
// the packet when the ASCII WAS the packet - that limit was the radio's, and
// this string never goes near a radio.
char    gLine[320];
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

// -----------------------------------------------------
// WIRE FORMAT - the binary telemetry packet
//
// THIS BLOCK IS DUPLICATED FROM
// MRCC_FlightComputer_A/src/Config.h, for the same
// reason the frequencies above are: Arduino builds each
// sketch on its own and there is no header to share.
// CHANGE ONE, CHANGE THE OTHER.
//
// A mismatch here is NOT the silent failure a wrong
// SF or frequency is. The magic byte and the length
// check below reject a packet this sketch cannot read,
// and TLM_VERSION rejects one from a build that changed
// the layout - so the box says nothing rather than
// printing a confidently mis-read line. The full field
// table, and the reasoning for every scale factor, is
// in Config.h.
// -----------------------------------------------------

#define TLM_MAGIC    0xA5
#define TLM_VERSION  1

#define TLM_BASE_LEN 52
#define TLM_ARM_LEN  5
#define TLM_SD_LEN   10

#define TLM_FLAG_ARMED    0x01
#define TLM_FLAG_FIRED    0x02
#define TLM_FLAG_GPS_DATA 0x04
#define TLM_FLAG_GPS_FIX  0x08
#define TLM_FLAG_SD_OK    0x10
#define TLM_FLAG_BARO_OK  0x20
#define TLM_FLAG_IMU_OK   0x40

#define TLM_BLOCK_ARM  0x01
#define TLM_BLOCK_SD   0x02

#define TLM_NAN_I16  ((int16_t) -32768)
#define TLM_NAN_I32  ((int32_t) -2147483647 - 1)
#define TLM_NAN_U16  ((uint16_t) 0xFFFF)


// ---- TLM DECODE BEGIN ----

// Mirrors Flight.cpp's stateName(). firmware/tests/test_downlink_codec.py reads
// that function and fails if the two tables ever disagree - the state word is
// what mrcc.py matches on to decide whether a vehicle is armed, so a name that
// drifts here does not produce a wrong label, it produces a rocket that reports
// PAD with a live pyro bus.
static const char *tlmStateName(uint8_t s) {
  switch (s) {
    case 0: return "PAD";
    case 1: return "ARMED";
    case 2: return "BOOST";
    case 3: return "COAST";
    case 4: return "APOGEE";
    case 5: return "DESCENT";
    case 6: return "LANDED";
    default: return "?";
  }
}

static inline uint8_t tlmGetU8(const uint8_t *b, int &i) {
  return b[i++];
}
static inline uint16_t tlmGetU16(const uint8_t *b, int &i) {
  uint16_t v = (uint16_t) b[i] | ((uint16_t) b[i + 1] << 8);
  i += 2;
  return v;
}
static inline uint32_t tlmGetU32(const uint8_t *b, int &i) {
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) v |= ((uint32_t) b[i + k]) << (8 * k);
  i += 4;
  return v;
}
static inline int16_t tlmGetI16(const uint8_t *b, int &i) {
  return (int16_t) tlmGetU16(b, i);
}
static inline int32_t tlmGetI32(const uint8_t *b, int &i) {
  return (int32_t) tlmGetU32(b, i);
}
static inline float tlmGetF32(const uint8_t *b, int &i) {
  uint32_t bits = tlmGetU32(b, i);
  float v;
  memcpy(&v, &bits, 4);
  return v;
}

// The sentinels come back as NAN so that %f prints `nan`, which is what the
// ASCII packet printed for a dead sensor and what mrcc.py already handles. A
// sentinel decoded as its literal value would be -3276.8 - a number, in range,
// and wrong.
static inline float tlmDeq16(int16_t v, float scale) {
  return v == TLM_NAN_I16 ? NAN : (float) v / scale;
}
static inline double tlmDeq32(int32_t v, double scale) {
  return v == TLM_NAN_I32 ? NAN : (double) v / scale;
}
static inline float tlmDeqDeg(uint16_t v) {
  return v == TLM_NAN_U16 ? NAN : (float) v;
}


// Expand one binary packet into the ASCII line the rest of the system reads.
// Returns the length written, or -1 if the packet is not one of ours.
//
// REJECTION IS THE POINT of the checks at the top. This box hears anything on
// the channel whose SF/BW/CR/syncword match, and the old failure mode for a
// stray frame was a half-parsed line on the laptop. A frame that is not
// TLM_MAGIC, not TLM_VERSION, or not long enough for the fields it claims is
// dropped here and counted, rather than expanded into plausible-looking
// telemetry from whatever bytes happened to arrive.
static int tlmDecode(const uint8_t *b, int len, char *out, size_t outSize) {
  if (len < TLM_BASE_LEN)        return -1;
  if (b[0] != TLM_MAGIC)         return -1;
  if (b[1] != TLM_VERSION)       return -1;

  int i = 2;

  const uint32_t pkt   = tlmGetU32(b, i);
  const uint32_t tMs   = tlmGetU32(b, i);
  const uint8_t  state = tlmGetU8(b, i);
  const uint8_t  flags = tlmGetU8(b, i);
  const uint8_t  sat   = tlmGetU8(b, i);
  const uint8_t  blocks = tlmGetU8(b, i);

  const float  alt    = tlmGetF32(b, i);
  const float  maxAlt = tlmGetF32(b, i);
  const float  vz     = tlmDeq16(tlmGetI16(b, i), 10.0f);
  const double lat    = tlmDeq32(tlmGetI32(b, i), 100000.0);
  const double lon    = tlmDeq32(tlmGetI32(b, i), 100000.0);
  const float  ga     = tlmDeq16(tlmGetI16(b, i), 10.0f);
  const float  gs     = tlmDeq16(tlmGetI16(b, i), 10.0f);
  const float  crs    = tlmDeqDeg(tlmGetU16(b, i));
  const float  ax     = tlmDeq16(tlmGetI16(b, i), 100.0f);
  const float  ay     = tlmDeq16(tlmGetI16(b, i), 100.0f);
  const float  az     = tlmDeq16(tlmGetI16(b, i), 100.0f);
  const float  gx     = tlmDeq16(tlmGetI16(b, i), 1.0f);
  const float  gy     = tlmDeq16(tlmGetI16(b, i), 1.0f);
  const float  gz     = tlmDeq16(tlmGetI16(b, i), 1.0f);
  const float  hdg    = tlmDeqDeg(tlmGetU16(b, i));

  // A block the sender flagged but did not fit is a torn packet, not a short
  // one. Refuse the whole frame rather than emit the base fields and silently
  // drop a countdown the operator is watching.
  int need = TLM_BASE_LEN;
  if (blocks & TLM_BLOCK_ARM) need += TLM_ARM_LEN;
  if (blocks & TLM_BLOCK_SD)  need += TLM_SD_LEN;
  if (len < need) return -1;

  int n = snprintf(
    out, outSize,
    "MRCC,PKT=%lu,T=%.1f,ST=%s,AL=%.1f,VZ=%.1f,MX=%.1f,AR=%d,FI=%d,"
    "GD=%d,GF=%d,SAT=%d,"
    "LAT=%.5f,LON=%.5f,GA=%.1f,GS=%.1f,CRS=%.0f,"
    "AX=%.2f,AY=%.2f,AZ=%.2f,GX=%.0f,GY=%.0f,GZ=%.0f,"
    "HDG=%.0f,SD=%d,BA=%d,IM=%d",
    (unsigned long) pkt, tMs / 1000.0,
    tlmStateName(state), alt, vz, maxAlt,
    (flags & TLM_FLAG_ARMED)    ? 1 : 0,
    (flags & TLM_FLAG_FIRED)    ? 1 : 0,
    (flags & TLM_FLAG_GPS_DATA) ? 1 : 0,
    (flags & TLM_FLAG_GPS_FIX)  ? 1 : 0,
    (int) sat,
    lat, lon, ga, gs, crs,
    ax, ay, az, gx, gy, gz, hdg,
    (flags & TLM_FLAG_SD_OK)   ? 1 : 0,
    (flags & TLM_FLAG_BARO_OK) ? 1 : 0,
    (flags & TLM_FLAG_IMU_OK)  ? 1 : 0
  );
  if (n < 0 || (size_t) n >= outSize) return -1;

  // Optional blocks, in the order the ASCII packet always carried them: the
  // PAD countdown first, the recorder details second.
  if (blocks & TLM_BLOCK_ARM) {
    const uint8_t  aw = tlmGetU8(b, i);
    const uint16_t ad = tlmGetU16(b, i);
    const uint16_t as = tlmGetU16(b, i);
    int m = snprintf(out + n, outSize - n, ",AW=%u,AD=%lu,AS=%lu",
                     (unsigned int) aw, (unsigned long) ad, (unsigned long) as);
    if (m < 0 || (size_t)(n + m) >= outSize) return -1;
    n += m;
  }

  if (blocks & TLM_BLOCK_SD) {
    const int16_t  sdf = tlmGetI16(b, i);
    const uint32_t sdl = tlmGetU32(b, i);
    const uint32_t sde = tlmGetU32(b, i);
    int m = snprintf(out + n, outSize - n, ",SDF=%d,SDL=%lu,SDE=%lu",
                     (int) sdf, (unsigned long) sdl, (unsigned long) sde);
    if (m < 0 || (size_t)(n + m) >= outSize) return -1;
    n += m;
  }

  // The over-air byte count, which `len=` can no longer carry. mrcc.py checks
  // len= against the length of THIS string as a splice guard, so it has to
  // describe the ASCII - and that leaves nothing saying how many bytes the
  // frame actually cost on the air, which is the number the duty-cycle
  // argument in Config.h is made of and the one TX_Doctor's test 3 is checked
  // against. It rides here instead. mrcc.py files unknown keys in `extra`, so
  // nothing downstream needed teaching about it.
  int m = snprintf(out + n, outSize - n, ",AIR=%d", len);
  if (m < 0 || (size_t)(n + m) >= outSize) return -1;
  n += m;

  return n;
}

// ---- TLM DECODE END ----


// Frames that were not ours, or were torn. Reported by '?' rather than printed
// per-packet: on a busy channel this could otherwise be the loudest thing on
// the console, and the number matters more than the individual events.
static unsigned long gBadPkts = 0;


void onRx(int n) {
  if (n <= 0 || n > 255 || gotPkt) return;   // 上一包还没处理完就跳过
  int i = 0;
  // 255 is LoRa's payload limit and gBuf holds 255 + NUL, so nothing the
  // radio can legally deliver is dropped here. This used to stop at 250,
  // which truncated a long packet AND reported the truncated length - so
  // the len= integrity check on the laptop passed and the frame decoded
  // as a clean short one, missing its tail.
  while (LoRa.available() && i < 255) gBuf[i++] = (uint8_t)LoRa.read();
  gBuf[i] = 0;          // only so the ASCII passthrough can print it as a string
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
  gBadPkts  = 0;
}


static void printStatus() {
  Serial.printf("### GS STATUS channel=%s freq=%.3fMHz pkts=%lu bad=%lu "
                "last_rssi=%d last_snr=%.1f ###\n",
                CHANNELS[gChannel].name, CHANNELS[gChannel].hz / 1e6,
                gPktCount, gBadPkts, gRssi, gSnr);
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
    // Which format arrived. A legacy ASCII packet opens with "MRCC" and is
    // passed through byte for byte; a binary one opens with TLM_MAGIC and is
    // expanded. Nothing else is printed at all - see tlmDecode.
    //
    // `len=` is the length of the ASCII, NOT of the frame, and that is a
    // contract rather than a convenience: mrcc.py compares it against the
    // characters it received and rejects the line if they disagree, which is
    // how a spliced or half-written line is caught. The frame's real size
    // travels in the AIR= field tlmDecode appends.
    if (gLen >= 4 && memcmp(gBuf, "MRCC", 4) == 0) {
      gPktCount++;
      Serial.printf("len=%d RSSI=%d SNR=%.1f | %s\n",
                    gLen, gRssi, gSnr, (const char *) gBuf);
    }
    else {
      int n = tlmDecode(gBuf, gLen, gLine, sizeof(gLine));
      if (n > 0) {
        gPktCount++;
        Serial.printf("len=%d RSSI=%d SNR=%.1f | %s\n", n, gRssi, gSnr, gLine);
      } else {
        gBadPkts++;
      }
    }
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
