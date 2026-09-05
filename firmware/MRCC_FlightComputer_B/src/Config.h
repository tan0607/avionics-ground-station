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

#define LORA_SCK   47
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
// The second one is the killer here. One telemetry
// cycle is two copies of a ~231 byte packet at SF7 /
// BW250 / CR4-5: ~182 ms of air each, plus the 60 ms
// COPY_GAP, inside a 500 ms SEND_INTERVAL. So ONE
// rocket already radiates ~73% of the time and its TX
// sequence occupies ~85% of every window - check the
// `air=` figure [TX] prints against that 182. There is
// no room for a second airframe on this channel, and
// no setting short of a different frequency makes room.
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

#define VEHICLE  VEHICLE_B          // fixed for this vehicle sketch

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
#define SD_CS    6


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

const unsigned long SEND_INTERVAL   = 500;   // telemetry packet
const unsigned long LOG_INTERVAL    = 100;   // SD log, 10 Hz
const unsigned long FLUSH_INTERVAL  = 1000;  // force to card
const unsigned long STATUS_INTERVAL = 5000;  // status line
const unsigned long HEALTH_INTERVAL = 5000;  // recovery attempts

const unsigned long COPY_GAP    = 60;    // between the two copies
const unsigned long TX_MAX_AIR  = 300;   // fallback if DIO0 never fires
const unsigned long IMU_STALE   = 2000;  // no IMU data for this long = down
const unsigned long GPS_STALE   = 2000;  // no NMEA for this long = down

const unsigned long GPS_START_TIMEOUT = 5000;


// -----------------------------------------------------
// RADIO DEFAULTS (adjustable live with + - and C)
// -----------------------------------------------------

#define TX_POWER_DEFAULT  17
#define TX_COPIES_DEFAULT 2

// LoRa's hard payload limit. txPacket is one byte larger, for snprintf's NUL.
// Nothing enforces this on the way out - an over-long packet is truncated at
// the buffer and the transmitter reports the truncated length - so it is the
// number every optional field has to be measured against before it is added.
const int TX_PAYLOAD_MAX = 255;

// ---- recorder block cadence ----
//
// The vehicle prints its recording state to USB every STATUS_INTERVAL; SDF/SDL/
// SDE put the same state on the air, for the operator who is 19 km away with no
// cable. They ride ONE PACKET IN TEN rather than every packet, which at
// SEND_INTERVAL is that same 5 s.
//
// Not every packet, for two reasons that are both hard limits rather than
// preferences. Bytes: the flight fields alone measure 189-204 on the logs in
// flights/, against 255. Air: two copies plus COPY_GAP already fill ~87% of the
// 500 ms window, and the ~25 bytes this block costs is ~40 ms across both
// copies - affordable once per ten windows, not ten times out of ten.
//
// A block that does not fit is dropped, never truncated (see
// buildTelemetryPacket), so this cadence is a floor on freshness, not a
// guarantee: a packet that arrives late or not at all just delays the next
// report by 5 s.
const unsigned long SD_BLOCK_EVERY = 10;   // packets


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

// ---- auto arm ----
//
// The board arms itself once it has been sitting still
// for PAD_STILL_TIME, instead of waiting for the serial
// A key.
//
// Why: the A key means a laptop at the pad, and on this
// vehicle the USB port is also the supply - unplugging
// it resets the board, and a reset lands back in PAD,
// DISARMED, without saying so. An operator who armed and
// walked away would fly an unarmed rocket, and an
// unarmed rocket is not a late deployment, it is no
// deployment at all: FS_PAD has no launch detector, so
// the machine sleeps through the whole flight.
//
// This does not delete a safety layer, it moves it into
// hardware. The switch in the pyro battery line is what
// actually stands between the battery and the match, and
// it is the last thing thrown before walking away. Auto
// arm only makes the firmware ready before that switch
// is closed; with it open the charge cannot fire
// whatever state the machine is in.
//
// Nothing is bypassed. It calls the same armFlight() the
// A key does, so the stillness window, the gyro-zero
// gate, the sensor-health gate and the fired latch all
// still apply. Set to 0 to go back to A only.
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
// 19000 = 14.1 s x ~1.35. The margin covers what the sim
// does not: motor lot variation, weathercocking, and a
// headwind, any of which pushes apogee later. Below about
// 17 s that margin is gone.
//
// Re-tune this whenever the motor or the mass changes.
const unsigned long APOGEE_TIMEOUT   = 19000;  // ms after launch

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
