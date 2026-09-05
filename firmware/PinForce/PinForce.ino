// =====================================================
// PinForce - the "unplug the wire" test, in software
//
// Everything is soldered, so the wire cannot be lifted.
// Instead, ask the S3 to DRIVE each line and read back
// what the pad actually settled at.
//
// A passive pull test (which TX_Doctor already does)
// uses the ~45 kOhm internal pull. Anything can win
// against 45 kOhm. Driving the pin puts ~40 mA behind
// it: if the pad STILL reads the wrong way, something
// is holding it hard, and that is either a dead S3 pad
// or a real short - not a module that merely fails to
// answer.
//
// Run this on BOTH boards and diff the two tables. The
// working board is the control; any line that differs
// is the fault. That comparison is the whole point -
// a single board's numbers mean much less on their own.
//
// RST and NSS are included. TX_Doctor's line-state
// report covers MISO/SCK/MOSI/DIO0 only, and a RST held
// low parks the SX1278 in reset forever: powered, wired,
// and silent, with REG_VERSION reading 0x00.
// =====================================================

// The pyro gate. This sketch has no business firing
// anything, but it runs on a board that CAN - and a
// gate left floating is the same hazard whether the
// firmware meant to arm it or not. The flight build
// drives this low before Serial even starts, and
// TX_Doctor prints that it did. This does both.
#define PYRO_GATE_PIN 2

struct Line { const char *name; int pin; };

static const Line LINES[] = {
  { "MISO", 13 },
  { "SCK ", 12 },
  { "MOSI", 11 },
  { "NSS ", 10 },
  { "RST ",  9 },
  { "DIO0",  8 },
};
static const int N = sizeof(LINES) / sizeof(LINES[0]);

// Passive: what does the line do against the internal pull?
static const char *pullState(int pin) {
  pinMode(pin, INPUT_PULLUP);   delayMicroseconds(500);
  int up = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN); delayMicroseconds(500);
  int down = digitalRead(pin);
  pinMode(pin, INPUT);

  if ( up && !down) return "floating";
  if (!up && !down) return "HELD LOW";
  if ( up &&  down) return "HELD HIGH";
  return "inverted?";
}

// Active: drive the pad and read what it actually reached.
// Kept to a few hundred microseconds - if the far end is
// driving the other way, this is a fight, and a short one
// is survivable where a continuous one is not.
static bool driveTo(int pin, int level) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level);
  delayMicroseconds(200);
  int got = digitalRead(pin);     // reads the pad, not the latch
  pinMode(pin, INPUT);            // release immediately
  delayMicroseconds(200);
  return got == level;
}

void setup() {
  // ---------------------------------------------------
  // NOTHING GOES ABOVE THIS LINE - same rule as
  // MRCC_FlightComputer_A.ino. Serial takes 1.5 s to come
  // up and the gate floats the whole time otherwise.
  // ---------------------------------------------------
  pinMode(PYRO_GATE_PIN, OUTPUT);
  digitalWrite(PYRO_GATE_PIN, LOW);

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=========================================");
  Serial.println(" PinForce - drive test on the LoRa lines");
  Serial.printf ( " Chip: %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.println("=========================================");
  Serial.println(" PYRO GATE HELD LOW. No fire path here.");
  Serial.println("=========================================");
  Serial.println();
  Serial.println("  line  gpio  passive      drive HIGH  drive LOW");
  Serial.println("  ----  ----  -----------  ----------  ---------");

  for (int i = 0; i < N; i++) {
    const Line &L = LINES[i];
    const char *pull = pullState(L.pin);
    bool hi = driveTo(L.pin, HIGH);
    bool lo = driveTo(L.pin, LOW);

    Serial.printf("  %s  %4d  %-11s  %-10s  %s\n",
                  L.name, L.pin, pull,
                  hi ? "reached 1" : "STUCK 0 <<",
                  lo ? "reached 0" : "STUCK 1 <<");
  }

  Serial.println();
  Serial.println("  Reading it:");
  Serial.println("   drive HIGH -> STUCK 0   the pad cannot be pulled up even");
  Serial.println("                           with ~40 mA behind it. A dead S3");
  Serial.println("                           pad, or a hard short to GND.");
  Serial.println("   drive HIGH -> reached 1 the S3 pad is alive and nothing is");
  Serial.println("                           hard-clamping. A passive HELD LOW");
  Serial.println("                           alongside this is a SOFT clamp -");
  Serial.println("                           the signature of an unpowered or");
  Serial.println("                           failed chip at the far end.");
  Serial.println();
  Serial.println("  Now run this on the WORKING board and diff the tables.");
  Serial.println("  Identical tables mean the fault is not on these six lines.");

  printHoldMenu();
}

// =====================================================
// HOLD MODE
//
// The table says a pad is STUCK 0. It cannot say WHY,
// because 200 us is not long enough to get a meter on
// it. So hold one line high indefinitely and let the
// multimeter answer the question the logic level hides:
//
//   ~0.6-0.7 V  a diode drop. Something at the far end
//               is clamping through its ESD protection,
//               which is what a chip with a broken
//               supply or ground does. MODULE side.
//
//   ~0.0 V      nothing survives. Solder bridge to GND,
//               or a dead pad on the S3. BOARD side.
//
// That one number splits the last fork in this hunt,
// and no amount of firmware can read it - the S3's own
// input stage reports both cases as "0".
// =====================================================

static int held = -1;

static void holdHigh(int idx) {
  // Release whatever was held first, so two lines are
  // never driven against each other by accident.
  if (held >= 0) {
    pinMode(LINES[held].pin, INPUT);
    held = -1;
  }

  if (idx < 0 || idx >= N) {
    Serial.println("  all lines released.");
    return;
  }

  held = idx;
  pinMode(LINES[idx].pin, OUTPUT);
  digitalWrite(LINES[idx].pin, HIGH);

  Serial.printf("  HOLDING %s (GPIO%d) HIGH.\n", LINES[idx].name, LINES[idx].pin);
  Serial.println("  Put the meter on DC volts: black on GND, red on that pad.");
  Serial.println("    ~3.3 V     the line is fine");
  Serial.println("    ~0.6-0.7 V diode clamp  -> MODULE side");
  Serial.println("    ~0.0 V     hard short   -> BOARD side");
}

static void printHoldMenu() {
  Serial.println();
  Serial.println("  --- HOLD MODE ---");
  for (int i = 0; i < N; i++) {
    Serial.printf("   %d = hold %s (GPIO%d) high\n", i + 1, LINES[i].name, LINES[i].pin);
  }
  Serial.println("   0 = release    M = this menu");
  Serial.println();
  Serial.println("  Measure the three STUCK lines first: MISO, SCK, DIO0.");
}

void loop() {
  // Re-asserted every pass, as in the flight loop. Hold
  // mode drives a pin for minutes at a time while someone
  // leans in with a meter probe; that is exactly when a
  // slipped probe must not find a live gate.
  digitalWrite(PYRO_GATE_PIN, LOW);

  if (!Serial.available()) return;

  char c = Serial.read();
  if (c == '\n' || c == '\r') return;

  if (c == 'M' || c == 'm')      printHoldMenu();
  else if (c == '0')             holdHigh(-1);
  else if (c >= '1' && c <= '6') holdHigh(c - '1');
  else                           Serial.println("  ? press M for the menu");
}
