// =====================================================
// Pyro_Doctor - does the MOSFET module actually switch?
//
// The flight build drives the gate exactly once, for
// 400 ms, on the day it matters. That is the worst
// possible moment to find out the module is dead. So
// this sketch does the same thing on demand, from the
// serial monitor, with a meter on the output instead of
// a match.
//
// It answers three separate questions, and keeping them
// separate is the whole point:
//
//   1. Does GPIO2 actually swing?         (S3 side)
//   2. Does the gate reach the FET?       (wiring)
//   3. Does the FET pass current?         (module side)
//
// A single "no voltage at OUT" cannot tell those apart.
// Run the stages below in order and each one falls out
// on its own.
//
//   GPIO2 -> module (PWM)+     module GND -> board GND
//   pyro battery + -> module VIN+ / OUT+
//   LOAD across OUT+ / OUT-
//
// -----------------------------------------------------
// NO E-MATCH. NO SQUIB. NO IGNITER.
//
// This sketch will hold the gate HIGH for as long as you
// leave it there, which is the one thing the flight code
// is written to never do. A match on those terminals
// fires the instant you press 1. Use a resistor.
// -----------------------------------------------------
// =====================================================

// Must match Config.h. If you move the gate there, move
// it here in the same commit - a doctor that tests the
// wrong pin is worse than no doctor.
#define PYRO_GATE_PIN 2

// Same 400 ms the flight build uses, so the P command
// reproduces the real pulse and not an approximation of
// it. A rail that sags enough to reset the board will do
// it here too.
const unsigned long FIRE_DURATION = 400;

// A held gate is a live output. Say so, repeatedly, for
// as long as it is true - someone leaning in with a
// probe should not have to remember what they pressed.
const unsigned long LIVE_NAG      = 2000;

const unsigned long BLINK_HALF    = 500;   // 1 Hz square
const int           BLINK_CYCLES  = 10;

enum Mode { OFF, HOLD, PULSE, BLINK };

static Mode          mode      = OFF;
static unsigned long modeStart = 0;
static unsigned long lastNag   = 0;
static unsigned long lastEdge  = 0;
static int           edges     = 0;


// =====================================================
// GATE
//
// One place where the pin is written, so there is one
// place to look when the pin is wrong.
// =====================================================

static void gate(int level) {
  digitalWrite(PYRO_GATE_PIN, level);
}


static void allOff(const char *why) {
  gate(LOW);
  mode = OFF;
  Serial.printf("  [gate LOW] %s\n", why);
}


static void printWiring() {
  Serial.println();
  Serial.println("  WIRING");
  Serial.println("    GPIO2 -------> module (PWM)+");
  Serial.println("    board GND ---> module GND        <- both grounds, always");
  Serial.println("    pyro batt + -> module VIN+");
  Serial.println("    pyro batt - -> module VIN-");
  Serial.println("    LOAD --------> across OUT+ / OUT-");
  Serial.println();
  Serial.println("  The load is not optional. With nothing across OUT the");
  Serial.println("  meter's own 10 Mohm is the only path, and it will read");
  Serial.println("  whatever leakage feels like that day - often full battery");
  Serial.println("  volts with the FET hard off. That reading has fooled more");
  Serial.println("  people than a dead module ever has. A 100 ohm - 1k");
  Serial.println("  resistor, or a LED with its resistor, settles it.");
}


static void printProcedure() {
  Serial.println();
  Serial.println("  STAGE 1 - S3 side.  PYRO BATTERY DISCONNECTED.");
  Serial.println("    Meter on DC volts: black on board GND, red on GPIO2.");
  Serial.println("      press 1  ->  3.3 V     the S3 drives the gate");
  Serial.println("      press 0  ->  0.0 V");
  Serial.println("    Stuck at 0 with 1 pressed: the pad is dead or shorted,");
  Serial.println("    or GPIO2 is your board's RGB LED. The module is innocent.");
  Serial.println("    Around 1.5 V: something is loading the gate - a missing");
  Serial.println("    series resistor, or a module input that is not 3.3 V safe.");
  Serial.println();
  Serial.println("  STAGE 2 - module side.  BATTERY IN, LOAD ON, NO MATCH.");
  Serial.println("    Meter across the LOAD (OUT+ to OUT-):");
  Serial.println("      press 1  ->  ~Vbatt    the FET is conducting");
  Serial.println("      press 0  ->  ~0 V");
  Serial.println("    Meter GND to OUT- reads the OTHER way round:");
  Serial.println("      press 1  ->  0.0-0.3 V   Rds(on) drop, this is good");
  Serial.println("      press 0  ->  ~Vbatt      the load is holding OUT- up");
  Serial.println("    Both are correct. Pick one and write down which, because");
  Serial.println("    half the confusion on this bench is a meter clipped to a");
  Serial.println("    different terminal than the last time.");
  Serial.println();
  Serial.println("    Gate swings in stage 1 but OUT never moves in stage 2:");
  Serial.println("    the FET or its ground is the fault. That is the answer");
  Serial.println("    this sketch exists to produce.");
}


static void printMenu() {
  Serial.println();
  Serial.println("  --- COMMANDS ---");
  Serial.println("   1 = gate HIGH and HOLD   (meter time - output stays live)");
  Serial.println("   0 = gate LOW             (safe)");
  Serial.printf ("   P = timed pulse, %lu ms  (exactly what the flight code does)\n", FIRE_DURATION);
  Serial.println("   B = blink 1 Hz for 10 s  (watch the meter or LED chase it)");
  Serial.println("   S = status               W = wiring   H = how to measure");
  Serial.println("   M = this menu");
  Serial.println();
  Serial.println("   Any other key releases the gate. So does the reset button.");
}


static void printStatus() {
  // digitalRead on an OUTPUT reads the pad, not the
  // latch. If those two disagree, something outside the
  // S3 is holding the gate and no amount of firmware
  // will win that fight.
  int pad = digitalRead(PYRO_GATE_PIN);

  Serial.println();
  Serial.printf("  mode      : %s\n",
                mode == OFF   ? "OFF (gate low)" :
                mode == HOLD  ? "HOLD - OUTPUT IS LIVE" :
                mode == PULSE ? "pulse in progress" : "blinking");
  Serial.printf("  gate pin  : GPIO%d reads %s\n", PYRO_GATE_PIN, pad ? "HIGH" : "LOW");

  if (mode == HOLD)
    Serial.printf("  held for  : %.1f s\n", (millis() - modeStart) / 1000.0);

  if (mode == OFF && pad == HIGH) {
    Serial.println("  *** pad reads HIGH with the gate driven LOW - something");
    Serial.println("  *** external is holding it. Do not connect a match.");
  }
}


void setup() {
  // ---------------------------------------------------
  // NOTHING GOES ABOVE THIS LINE - same rule as
  // MRCC_FlightComputer_A.ino. Serial takes 1.5 s to come
  // up and the gate floats the whole time otherwise.
  //
  // The 10k pulldown at the gate is what actually covers
  // the window between reset and this line. This is the
  // second layer, not the first.
  // ---------------------------------------------------
  pinMode(PYRO_GATE_PIN, OUTPUT);
  digitalWrite(PYRO_GATE_PIN, LOW);

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=========================================");
  Serial.println(" Pyro_Doctor - MOSFET module bench test");
  Serial.printf ( " Chip: %s rev%d   gate: GPIO%d\n",
                  ESP.getChipModel(), ESP.getChipRevision(), PYRO_GATE_PIN);
  Serial.println("=========================================");
  Serial.println(" Gate is LOW. It stays low until you ask.");
  Serial.println();
  Serial.println(" *** NO E-MATCH ON THE OUTPUT. ***");
  Serial.println(" This sketch will hold the FET on for as");
  Serial.println(" long as you leave it. Use a resistor or");
  Serial.println(" a LED as the load. Nothing that burns.");
  Serial.println("=========================================");

  printWiring();
  printProcedure();
  printMenu();
}


void loop() {
  unsigned long now = millis();

  // ---- run whatever mode is active ----
  switch (mode) {

    case PULSE:
      if (now - modeStart >= FIRE_DURATION) {
        gate(LOW);
        mode = OFF;
        Serial.printf("  pulse ended after %lu ms - gate LOW\n", now - modeStart);
        Serial.println("  A meter cannot follow 400 ms. Use it to prove the");
        Serial.println("  pulse is SHORT, not to read the voltage: a LED load");
        Serial.println("  should blip, and the battery should not brown the");
        Serial.println("  board out. If the S3 reboots here, the flight build");
        Serial.println("  will reboot at apogee.");
      }
      break;

    case BLINK:
      if (now - lastEdge >= BLINK_HALF) {
        lastEdge = now;
        edges++;
        gate(edges & 1 ? HIGH : LOW);

        if (edges >= BLINK_CYCLES * 2) {
          gate(LOW);
          mode = OFF;
          Serial.println("  blink done - gate LOW");
          Serial.println("  A meter that swung the full way every second is a");
          Serial.println("  FET that switches. One that only twitched, or sat");
          Serial.println("  somewhere in the middle, is a gate not being driven");
          Serial.println("  hard enough - check the (PWM)+ wire and the grounds.");
        }
      }
      break;

    case HOLD:
      if (now - lastNag >= LIVE_NAG) {
        lastNag = now;
        Serial.printf("  [OUTPUT LIVE] gate HIGH for %.0f s - press 0 to stop\n",
                      (now - modeStart) / 1000.0);
      }
      break;

    case OFF:
      // Re-asserted every pass, as in the flight loop.
      // This is the state someone leaves the bench in.
      gate(LOW);
      break;
  }

  // ---- serial ----
  if (!Serial.available()) return;

  char c = Serial.read();
  if (c == '\n' || c == '\r') return;

  switch (c) {

    case '1':
      gate(HIGH);
      mode      = HOLD;
      modeStart = now;
      lastNag   = now;
      Serial.println();
      Serial.println("  *** GATE HIGH - OUTPUT IS LIVE AND STAYS LIVE ***");
      Serial.println("  Meter it now. Press 0 when you are done.");
      break;

    case '0': case 'l': case 'L':
      allOff("released");
      break;

    case 'p': case 'P':
      gate(HIGH);
      mode      = PULSE;
      modeStart = now;
      Serial.printf("\n  PULSE %lu ms - gate HIGH\n", FIRE_DURATION);
      break;

    case 'b': case 'B':
      mode     = BLINK;
      edges    = 0;
      lastEdge = now;
      gate(LOW);
      Serial.printf("\n  BLINK 1 Hz for %d s - watch the load\n", BLINK_CYCLES);
      break;

    case 's': case 'S': printStatus();    break;
    case 'w': case 'W': printWiring();    break;
    case 'h': case 'H': printProcedure(); break;
    case 'm': case 'M': printMenu();      break;

    default:
      // An unknown key is usually a fumble, and a fumble
      // while the output is live should end with the
      // output not live.
      if (mode != OFF) allOff("unknown key - releasing to be safe");
      else             Serial.println("  ? press M for the menu");
      break;
  }
}
