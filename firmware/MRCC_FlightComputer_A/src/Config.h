#pragma once

// =====================================================
// MRCC FLIGHT COMPUTER - CONFIGURATION
//
// Every pin and tunable lives here. Nothing else in
// the project hardcodes a pin number.
// =====================================================


// -----------------------------------------------------
// PHYSICAL CONSTANTS
//
// Defined first - the flight thresholds below are
// written in g and need this before they are declared.
// -----------------------------------------------------

const float GRAVITY = 9.80665;


// -----------------------------------------------------
// LORA (SX1278) - on the default SPI bus
// -----------------------------------------------------

#define LORA_SCK   12
#define LORA_MISO  13
#define LORA_MOSI  11
#define LORA_SS    10
#define LORA_RST   9
#define LORA_DIO0  8


// -----------------------------------------------------
// VEHICLE  <-- FIXED FOR THIS SKETCH; OPEN THE MATCHING A/B FOLDER
//
// TWO ROCKETS CANNOT SHARE A CHANNEL. LoRa does not
// pair: a receiver decodes every packet whose freq /
// SF / BW / CR / syncword match, no matter which
// airframe sent it. On one channel you get both
// failures at once -
//
//   each ground station decodes the OTHER rocket's
//   frames, so PKT jumps, the loss count becomes
//   noise and the map hops between vehicles; and
//
//   the two transmitters collide on air, so neither
//   link survives.
//
// The second one is the killer here, and the move to a
// 10 Hz binary downlink did not soften it. One telemetry
// cycle is now a single 52-67 byte packet at SF7 / BW250
// / CR4-5: ~48-60 ms of air inside a 100 ms
// SEND_INTERVAL. So ONE rocket radiates ~60% of the time
// - check the `air=` figure [TX] prints against that 60.
//
// That is DOWN from the ~73% the old two-copy ASCII
// packet cost, and it still leaves no room for a second
// airframe: 60% duty means a second transmitter on the
// same channel collides with this one on well over half
// its packets, and neither link survives that. Freeing
// 40% of the air did not create a second channel. Only a
// different frequency does.
//
// Both channels sit inside Malaysia's 433 MHz ISM
// allocation (MCMC: 433.05 - 434.79 MHz) and are
// 800 kHz apart, comfortably wider than the 250 kHz
// occupied bandwidth, so the two links do not overlap
// even at the skirts.
//
// THE GROUND STATION MUST BE CHANGED TO MATCH. Same
// frequency both ends or the link is simply deaf -
// there is no partial-reception failure mode to warn
// you. Flash the rocket and its receiver together.
//
// A wrong VEHICLE here is invisible on the bench: the
// rocket boots, transmits, and looks perfect right up
// until the other airframe powers on. So initRadio()
// prints the channel at boot - read it before you
// close up.
// -----------------------------------------------------

#define VEHICLE_A 1
#define VEHICLE_B 2

#define VEHICLE  VEHICLE_A          // fixed for this vehicle sketch

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
// SD CARD - on its own SPI bus so LoRa is never disturbed
// -----------------------------------------------------

#define SD_SCK   14
#define SD_MISO  16
#define SD_MOSI  15
#define SD_CS    7


// -----------------------------------------------------
// GPS
// -----------------------------------------------------

#define GPS_RX   18
#define GPS_TX   17
#define GPS_BAUD 9600


// -----------------------------------------------------
// I2C (ICM20948)  AD0 -> GND
// -----------------------------------------------------

#define I2C_SDA 4
#define I2C_SCL 5
#define AD0_VAL 0


// -----------------------------------------------------
// SUPPLY VOLTAGE MONITOR (optional hardware)
//
//   5V ---[ R1 10k ]---+---[ R2 10k ]--- GND
//                      |
//                    GPIO1
//
// Set VBAT_ENABLED to 1 once the divider is wired.
// -----------------------------------------------------

#define VBAT_ENABLED 0
#define VBAT_PIN     1
#define VBAT_DIVIDER 2.0


// -----------------------------------------------------
// TIMING
// -----------------------------------------------------

// SEND_INTERVAL is 100 ms - 10 Hz - and that number is not free. It is the
// air time of ONE binary packet plus margin, and it only became reachable when
// the downlink stopped being ASCII.
//
// The arithmetic, at the SF7 / BW250 / CR4-5 this link has always used:
//
//   ASCII, 185-237 bytes   149-187 ms of air   ONE copy overruns a 100 ms window
//   binary, 52-67 bytes     48- 60 ms of air   60% duty at 10 Hz, single copy
//
// So the old packet could not be sent at 10 Hz at any setting. It was not a
// tuning problem - one copy of it is longer than the whole window, and the TX
// state machine below would simply have run late forever, reporting 10 Hz in
// this constant while putting 5-6 Hz on the air. The packet had to shrink
// first; see the WIRE FORMAT block further down.
//
// TX_COPIES is 1 now and cannot be 2 (see TX_COPIES_DEFAULT). The redundancy
// that the second copy used to buy is bought by the rate instead: a packet lost
// at 10 Hz costs 100 ms of timeline, where a packet lost at 2 Hz cost 500 ms.
const unsigned long SEND_INTERVAL   = 100;   // telemetry packet, 10 Hz
const unsigned long LOG_INTERVAL    = 100;   // SD log, 10 Hz
const unsigned long FLUSH_INTERVAL  = 1000;  // force to card
const unsigned long STATUS_INTERVAL = 5000;  // status line
const unsigned long HEALTH_INTERVAL = 5000;  // recovery attempts

const unsigned long COPY_GAP    = 60;    // between the two copies

// Fallback if DIO0 never fires. It has to sit ABOVE the worst-case air time and
// BELOW the send interval, and at 10 Hz those two bounds are 60 ms apart rather
// than 240. 90 ms is 1.5x the 60 ms worst case and still inside the window, so
// a missed TxDone costs one packet instead of wedging the state machine into
// the next one. It was 300 ms, which at this rate would swallow three windows.
const unsigned long TX_MAX_AIR  = 90;

// The [TX] line, one packet in this many. At 2 Hz a line per packet was two
// lines a second and readable; at 10 Hz it is ten, which buries every other
// message on the console - including the ones that matter. 10 packets is one
// line a second, and the packet is still on the air at full rate either way.
const unsigned long TX_REPORT_EVERY = 10;

const unsigned long IMU_STALE   = 2000;  // no IMU data for this long = down
const unsigned long GPS_STALE   = 2000;  // no NMEA for this long = down

const unsigned long GPS_START_TIMEOUT = 5000;


// -----------------------------------------------------
// RADIO DEFAULTS (adjustable live with + - and C)
// -----------------------------------------------------

// 17 dBm - the flight value, and the one this link is characterised at.
//
// KNOWN, AND WATCH FOR IT: running 17 dBm at this rate put the BMP280 back into
// its documented A1R failure - a steady ~4000 m, which is the part's power-on
// register value read back after it loses its configuration. That fault had
// been fixed on the supply side and had not been seen since; it returned when
// the radio's load changed, and nothing else changed with it.
//
// What changed is not the duty cycle so much as the RECOVERY. The old link was
// two 182 ms bursts per 500 ms: 73% duty, but ~313 ms of quiet between
// keyings. This one is a packet every 100 ms: ~60% duty with ~40 ms of quiet.
// Lower average current, far less time for a supply path with resistance in it
// to come back up. Baro.cpp calls that path a board fault in as many words.
//
// So on every bench run at this setting, check three things before trusting a
// flight: AL not sitting at ~4000, brownoutCount not climbing, and no
// unexplained reset. `-` on the console drops the power live if you need to
// separate an electrical fault from a software one - if the symptom follows the
// power, the fix is decoupling at the module (bulk >=100 uF plus 100 nF at
// VCC), not a lower setting.
#define TX_POWER_DEFAULT  17

// ONE COPY, AND IT CANNOT BE TWO. A second copy needs COPY_GAP + another full
// air time - 60 + 60 ms on top of the first 60 - which does not fit in a 100 ms
// SEND_INTERVAL. Setting this to 2 does not break the state machine (it starts
// a cycle only when idle, so it self-limits) but it silently halves the rate
// to ~6 Hz. The redundancy is bought by the rate now, not by repetition.
#define TX_COPIES_DEFAULT 1

// LoRa's hard payload limit. The binary packet is nowhere near it - 67 bytes
// worst case against 255 - but every optional block is still measured against
// this before it is appended, because the failure mode has not changed: a block
// written past the end of the buffer is not rejected by anything downstream,
// it just corrupts whatever the encoder writes next.
const int TX_PAYLOAD_MAX = 255;


// -----------------------------------------------------
// WIRE FORMAT - the binary telemetry packet
//
// THIS BLOCK IS DUPLICATED IN MRCC_GroundStation.ino.
// Arduino builds each sketch on its own, so there is no
// header to share; the frequencies above have the same
// problem and the same rule. CHANGE ONE, CHANGE THE
// OTHER, AND BUMP TLM_VERSION.
//
// Why binary at all: the ASCII key=value packet this
// replaced cost 185-237 bytes, which is 149-187 ms of
// air, which does not fit in a 100 ms window at any
// power or coding rate. `LAT=%.5f` spent 13 bytes on a
// number that is exact in 4. The fields below are the
// same fields - nothing was dropped to make 10 Hz fit,
// only re-encoded.
//
// THE ASCII FORMAT IS NOT GONE. The ground station
// decodes this and prints the identical
// `MRCC,PKT=...` line it always printed, so
// shared/protocol/mrcc.py, the loss tracker, the CSV
// and the dashboard are untouched by this change. The
// binary exists only between the two radios.
//
// All multi-byte fields are LITTLE-ENDIAN. Both ends
// are ESP32s, but the encoder writes byte-by-byte
// rather than memcpy-ing a struct, so the format does
// not silently depend on that.
//
//   off  type  field        note
//     0  u8    magic        TLM_MAGIC
//     1  u8    version      TLM_VERSION
//     2  u32   pkt          packetNumber
//     6  u32   t_ms         millis()
//    10  u8    state        FS_* index
//    11  u8    flags        see TLM_FLAG_*
//    12  u8    sat
//    13  u8    blocks       which optional blocks follow
//    14  f32   altFiltered  m AGL
//    18  f32   maxAlt       m AGL
//    22  i16   vertVel      x10, m/s
//    24  i32   latitude     x1e5, deg
//    28  i32   longitude    x1e5, deg
//    32  i16   gpsAltitude  x10, m
//    34  i16   gpsSpeed     x10, m/s
//    36  u16   gpsCourse    whole deg
//    38  i16   ax           x100, m/s2
//    40  i16   ay           x100
//    42  i16   az           x100
//    44  i16   gx           whole deg/s
//    46  i16   gy
//    48  i16   gz
//    50  u16   heading      whole deg
//    52  = TLM_BASE_LEN
//
//   then, if TLM_BLOCK_ARM:   u8 wait, u16 delay_s, u16 still_s
//   then, if TLM_BLOCK_SD:    i16 fileIndex, u32 lines, u32 errors
//
// ALTITUDE IS f32, NOT A SCALED INTEGER, and that is
// deliberate. Every other field here has a provable
// range - 33 g of accel, 2000 deg/s of gyro, 360
// degrees of heading - so a scaled i16 cannot overflow
// on a flight this airframe can have. Altitude has no
// such bound: an i16 in decimetres tops out at 3276.7 m,
// which is a ceiling written into the wire format where
// nobody would look for it, and the failure is a
// wrapped sign - an apogee that reports as a hole in
// the ground. The two fields it costs 4 extra bytes
// each are the two the whole vehicle exists to measure.
//
// NaN travels: the f32 fields carry it natively, and
// the scaled integers reserve their most negative value
// (TLM_NAN_I16 / TLM_NAN_I32) for it. The old ASCII
// packet printed `nan` and the ground station still
// does, so a dead sensor reads as dead rather than as
// a plausible zero.
// -----------------------------------------------------

#define TLM_MAGIC    0xA5
#define TLM_VERSION  1

const int TLM_BASE_LEN = 52;
const int TLM_ARM_LEN  = 5;
const int TLM_SD_LEN   = 10;

// flags byte
#define TLM_FLAG_ARMED    0x01   // AR
#define TLM_FLAG_FIRED    0x02   // FI
#define TLM_FLAG_GPS_DATA 0x04   // GD
#define TLM_FLAG_GPS_FIX  0x08   // GF
#define TLM_FLAG_SD_OK    0x10   // SD
#define TLM_FLAG_BARO_OK  0x20   // BA
#define TLM_FLAG_IMU_OK   0x40   // IM
// 0x80 is spare. txFiltered is the obvious candidate - the console's R key
// swaps the six IMU fields between raw and filtered and nothing on the ground
// says which arrived - but that needs a key on the emitted line to be worth
// anything, so it is a decision, not a leftover bit. Left unset.

// blocks byte
#define TLM_BLOCK_ARM  0x01
#define TLM_BLOCK_SD   0x02

// Reserved sentinels for a non-finite value.
#define TLM_NAN_I16  ((int16_t) -32768)
#define TLM_NAN_I32  ((int32_t) -2147483647 - 1)
#define TLM_NAN_U16  ((uint16_t) 0xFFFF)

// ---- recorder block cadence ----
//
// The vehicle prints its recording state to USB every STATUS_INTERVAL; SDF/SDL/
// SDE put the same state on the air, for the operator who is 19 km away with no
// cable. This is the cadence in PACKETS, and it exists to hold that block to
// the 5 s STATUS_INTERVAL the console prints on - so it has to move with
// SEND_INTERVAL, which is the trap the number below is guarding against.
//
// It was 10 packets, which WAS 5 s at the old 2 Hz. At 10 Hz the same 10 would
// be one second, and the constant would still have read like a deliberate
// choice while quietly sending the block five times more often than the thing
// it mirrors. 50 packets at 100 ms is the same 5 s it always meant.
//
// The old reason for the cadence - byte budget - is largely gone: the block is
// 10 bytes against 255, where in ASCII it was ~25 bytes against a packet
// already at 237. What remains is that it is still 10 bytes of air on a link
// running at 60% duty, and that reporting a line count faster than the console
// that produces it buys nothing.
//
// A block that does not fit is dropped, never truncated (see
// buildTelemetryPacket), so this cadence is a floor on freshness, not a
// guarantee: a packet that arrives late or not at all just delays the next
// report by 5 s.
const unsigned long SD_BLOCK_EVERY = 50;   // packets = 5 s at SEND_INTERVAL


// -----------------------------------------------------
// BAROMETER (BMP280) - shares the IMU I2C bus
//
// ICM20948 sits at 0x68 (AD0 -> GND), BMP280 at 0x76
// or 0x77. No conflict. Both hang off SDA 4 / SCL 5.
//
// The ebay MUST have static ports (3-4 x 1.5 mm holes,
// evenly spaced) and MUST be sealed from ejection gas,
// or the altitude reading is worthless.
// -----------------------------------------------------

#define BARO_ADDR_PRIMARY   0x76
#define BARO_ADDR_FALLBACK  0x77

#define SEA_LEVEL_HPA 1013.25

const unsigned long BARO_INTERVAL = 50;    // 20 Hz sampling
const unsigned long BARO_STALE    = 1000;  // no reading this long = down

// ---- spike gate ----
//
// readBaro() used to pass anything that was not zero or
// NaN. A1R has been seen reading 4000 m, which is about
// 616 hPa against a real 1010 - not drift and not noise,
// but a corrupted transfer arriving as a perfectly valid
// float. Two vehicles run this firmware and only one does
// it, so the cause is in the wiring, not here; this is the
// net under it either way.
//
// UPDATE - the paragraph above guessed at the mechanism and
// guessed wrong, though its conclusion held. Measured on the
// bench with TX_Doctor test 8: A1R's part accepts ctrl_meas
// 0x33, holds normal mode for ~150 ms, then loses the
// configuration outright - ctrl_meas reads back 0x00 and the
// data registers sit at 0x80000, their power-on reset value.
// It is not a corrupted transfer. It is the part RESETTING
// under the sustained current of continuous conversion, and
// the same part runs the identical x8 conversion perfectly
// when asked one sample at a time.
//
// That matters to this gate, because a reset part does not
// produce a spike. It produces a STEADY wrong value, and a
// rate gate cannot see something with no rate. The absolute
// BARO_MIN_HPA/BARO_MAX_HPA net below is what actually
// caught it. Baro.cpp now runs the sensor in forced mode,
// which removes the cause; keep both nets anyway.
//
// It has to be a net, because that reading is not a
// cosmetic problem. Fed to the alpha-beta filter a 4000 m
// step becomes thousands of m/s, and the sample that comes
// back becomes thousands negative - which is APOGEE_VEL
// satisfied many times over. In COAST past MIN_ALT_GAIN
// that is the charge.
//
// The gate is a RATE, not a distance, because the sample
// interval is not fixed. readBaro() is rate-LIMITED to
// BARO_INTERVAL, never rate-guaranteed: it runs when the
// loop reaches it, and the loop stalls - an SD write can
// block for a hundred ms or more. This file already
// concedes that twice, at Flight.cpp's `dt > 0.5` stall
// clamp and at serviceLogging()'s catch-up.
//
// A fixed distance would therefore tighten exactly when it
// must not. 40 m per sample is 4x margin over a 200 m/s
// burnout at the nominal 50 ms - and none at all after a
// 200 ms stall, where the same 200 m/s moves the airframe
// those same 40 m and the gate starts eating real flight.
//
// Measured against elapsed time instead, 800 m/s holds the
// nominal behaviour (40 m at 50 ms) and widens with the gap
// the way the airframe does, so the margin survives a stall
// that a fixed threshold would not.
const float BARO_MAX_RATE = 800.0;   // m/s implied between samples

// Cap on the elapsed term, so a long gap cannot open the
// gate wide enough to let a real spike through. 0.5 s is
// this project's own idea of a stall (Flight.cpp), and
// caps the allowance at 400 m - still an order of magnitude
// under the 4000 m this exists for.
const float BARO_GATE_DT_MAX = 0.5;  // s

// A sensor that keeps saying the same new thing is telling
// the truth, or is broken in a way rejection cannot fix.
// Either way, stop arguing and re-seed - 10 samples is
// 500 ms, comfortably inside BARO_STALE, so the gate can
// never be what marks the barometer down.
//
// It also matters that a consistently offset reading is
// still USEFUL: apogee is called on velocity, which is a
// difference, so an altitude that is wrong by a constant
// still finds the top.
const uint8_t BARO_REJECT_RUN = 10;

// Coarse absolute net, for the first sample only - there
// is nothing to compare it against, and a garbage seed
// makes the jump gate reject every good reading after it
// until the run expires. 300 hPa is ~9000 m, far above
// anything this airframe will see, so real flight never
// touches this.
const float   BARO_MIN_HPA    = 300.0;
const float   BARO_MAX_HPA    = 1100.0;


// -----------------------------------------------------
// PYRO - ejection channel
//
//   GPIO -> module (PWM)+     module GND -> board GND
//   e-match across OUT+ / OUT-
//
// GPIO2 is NOT a strapping pin on the ESP32-S3 (those
// are 0, 3, 45, 46). Check it is not your board's RGB
// LED before wiring.
//
// FIT A 10k PULLDOWN FROM THE GATE PIN TO GND.
// The pin floats during reset and boot. The pulldown,
// not the firmware, is what stops a reset from firing.
// -----------------------------------------------------

#define PYRO_GATE_PIN   2
#define PYRO_CONT_PIN   6     // ADC1_CH5, continuity divider
#define ARM_SWITCH_PIN  21    // input pullup, closed = LOW

// The arm switch must ALSO physically break the pyro
// battery line. This GPIO only reports its position -
// it is never the only thing standing between the
// battery and the match.
#define ARM_SWITCH_ENABLED  0   // set 1 once the switch is wired

#define PYRO_CONT_ENABLED   0   // set 1 once the divider is wired
#define PYRO_CONT_DIVIDER   2.0
#define PYRO_CONT_MIN_VOLTS 0.4 // below this = open circuit / no match

// An e-match bridgewire burns in single-digit ms.
// 400 ms is 20x margin and keeps the rail sag short,
// which matters because pyro shares the main battery.
const unsigned long FIRE_DURATION = 400;


// -----------------------------------------------------
// FLIGHT STATE MACHINE
//
// PAD -> ARMED -> BOOST -> COAST -> APOGEE(fire)
//     -> DESCENT -> LANDED
//
// Transitions latch. The machine never runs backwards
// in flight.
// -----------------------------------------------------

const unsigned long FLIGHT_INTERVAL = 50;   // 20 Hz, same as baro

// ---- arming ----
const float         PAD_ACCEL_TOL  = 0.5;    // m/s2 away from 9.81
const float         PAD_GYRO_TOL   = 5.0;    // deg/s
const unsigned long PAD_STILL_TIME = 10000;  // must be still this long
const unsigned long AUTO_ARM_DELAY = 180000; // initial session wait; valid warm resets retain progress
// PAD/calibration only: a missing 250 ms run breaks observed stillness.
// Flight launch freshness and thresholds remain unchanged.
const unsigned long PAD_IMU_MAX_GAP = 250;

// ---- auto arm ----
//
// Normal prelaunch arming requires the session waiting period AND a full observed
// stationary IMU window AND completed gyro calibration. No IMU means no
// prelaunch arm; barometer fallback remains available AFTER arming.
// A software delay is only an operator buffer, not physical isolation.
// Actual MOSFET wiring / power isolation must be verified on the hardware.
// PAD has no launch detector: confirm received ARMED before launching.
#define AUTO_ARM_ENABLED 1

// Retry interval after a refusal. Slow on purpose - the
// refusals that survive the settled test (already fired,
// no continuity) would otherwise repeat 20 times a
// second for the whole pad wait.
const unsigned long AUTO_ARM_RETRY = 5000;

// A board that never settles arms nothing and, without
// this, says nothing either - the silent no-arm is the
// exact failure auto arm exists to remove, so it must
// not come back in through the settle test. After this
// long in PAD the board explains what is blocking it,
// and repeats at the same interval.
const unsigned long AUTO_ARM_STUCK_AFTER = 30000;

// ---- launch ----
// Acceleration is compared as a VECTOR MAGNITUDE.
// Nothing in this project tracks orientation, so a
// single axis cannot be trusted.
const float    LAUNCH_ACCEL   = 3.0 * GRAVITY;  // m/s2
const uint8_t  LAUNCH_CONFIRM = 5;           // consecutive samples

// Fallback if the IMU is down at launch. The baro
// alone can still tell us we left the pad.
const float    LAUNCH_ALT     = 15.0;        // m AGL

// ---- burnout ----
const float         BURNOUT_ACCEL     = 1.5 * GRAVITY;
const unsigned long BURNOUT_CONFIRM   = 200;
const unsigned long MOTOR_BURN_MAX    = 4000;  // force COAST after this

// ---- apogee ----
const unsigned long MIN_COAST_TIME   = 1500;   // after launch, no fire before this
const float         MIN_ALT_GAIN     = 30.0;   // m AGL, no fire below this
const float         APOGEE_VEL       = -2.0;   // m/s, descending
const uint8_t       APOGEE_CONFIRM   = 4;      // consecutive samples

// BACKUP. Fires on a timer if the baro never calls it.
// TUNE THIS FROM YOUR OWN SIM before you fly.
//
// It MUST sit LATER than the real apogee, with margin.
// The COAST block races this against the baro and takes
// whichever lands first, so a timeout set below apogee
// does not wait for a sensor failure - it pre-empts the
// baro on EVERY flight and deploys under thrust-side
// velocity every time.
//
// It was 12000 against an OpenRocket apogee of 14.1 s,
// which is exactly that failure: the charge would have
// gone at T+12 s, 2.1 s early, with the airframe still
// climbing at 20 m/s or better. Nothing in the log would
// have looked wrong either - timerBackupUsed would just
// be set, on a board whose baro was working perfectly.
//
// User-selected 16 s backup (2026-09-09), with reported simulated
// apogee around 14 s. Timing margin still requires flight-specific validation.
//
// Re-tune this whenever the motor or the mass changes.
const unsigned long APOGEE_TIMEOUT   = 16000;  // ms after launch

// ---- landing ----
const float         LAND_ALT_BAND    = 2.0;    // m
const unsigned long LAND_CONFIRM     = 10000;  // ms

// ---- altitude filter (alpha-beta, runs at FLIGHT_INTERVAL) ----
const float FILTER_ALPHA = 0.30;
const float FILTER_BETA  = 0.05;


// -----------------------------------------------------
// FILTERS (ICM20948)
//
// Set FILTER_ENABLED to 0 to compile the whole chain out.
// The filtered log columns then mirror the raw ones,
// which is the cleanest possible A/B for the report.
// -----------------------------------------------------

#define FILTER_ENABLED 1

// Low pass cutoffs, Hz. Below the vibration, above the
// motion you care about.
//
// The IMU is sampled at ~100 Hz (see initIMU), so Nyquist
// is 50 Hz and anything above that is the ICM's own DLPF
// problem, not ours. Boost and burnout are 1-2 Hz events,
// so there is a wide gap to put a cutoff in.
//   accel  - airframe and motor vibration, cut hard
//   gyro   - quieter to begin with, so a looser cutoff
//   mag    - the field cannot change fast at all
const float LPF_FC_ACCEL = 12.0;
const float LPF_FC_GYRO  = 15.0;
const float LPF_FC_MAG   = 3.0;

// Kalman tuning
//   Q_ANGLE   trust in the gyro integration
//   Q_BIAS    how fast the bias state is allowed to move
//   R_MEASURE trust in the accelerometer angle
// Raise R_MEASURE for a smoother, lazier output.
const float KF_Q_ANGLE   = 0.001;
const float KF_Q_BIAS    = 0.003;
const float KF_R_MEASURE = 0.03;

// Complementary filter time constant, seconds.
// Baseline only - it is not what flies the rocket.
const float COMP_TAU = 0.50;

// The accelerometer is only believed while the total
// measured acceleration is within this band of 1 g.
// Under thrust it is outside it and the Kalman coasts
// on the gyro alone.
const float ACC_TRUST_BAND = 2.0;   // m/s2

// Gyro zero rate calibration, on the pad
const int           GYRO_CAL_SAMPLES  = 300;
const float         GYRO_CAL_MAX_RATE = 15.0;   // deg/s, above this = moving
const unsigned long GYRO_CAL_TIMEOUT  = 20000;  // give up rather than be wrong

// dt sanity limits, seconds
const float DT_MAX     = 0.20;   // longer than this = a stall, not a sample
const float DT_DEFAULT = 0.01;
