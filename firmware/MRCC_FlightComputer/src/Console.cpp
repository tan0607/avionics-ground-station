#include "Console.h"
#include "Config.h"
#include "State.h"
#include "Storage.h"
#include "Radio.h"
#include "Health.h"
#include "Filters.h"
#include "Flight.h"
#include "Pyro.h"

// A bench test fire needs two keystrokes, five seconds
// apart at most. One stray character on the serial line
// must never be able to light a match.
static bool          testPending     = false;
static unsigned long testPendingTime = 0;

const unsigned long TEST_CONFIRM_WINDOW = 5000;


void printMenu() {
  Serial.println();
  Serial.println("--------------- COMMANDS ---------------");
  Serial.println(" A = ARM   (pad, still, arm switch closed)");
  Serial.println(" X = DISARM");
  Serial.println(" T = bench test fire (needs Y to confirm)");
  Serial.println(" D = dump log file to serial");
  Serial.println(" L = list files on card");
  Serial.println(" S = status");
  Serial.println(" N = start a new log file");
  Serial.println(" I = re-run the SD hardware test");
  Serial.println(" F = format the card (erases everything)");
  Serial.println(" P = toggle packet contents in the TX line");
  Serial.println(" + / - = LoRa TX power up / down");
  Serial.println(" C = toggle 1 or 2 copies per packet");
  Serial.println(" K = re-calibrate the gyro (HOLD STILL)");
  Serial.println(" R = telemetry sends raw / filtered IMU");
  Serial.println(" M = this menu");
  Serial.println("----------------------------------------");
  Serial.print  (" STATE=");
  Serial.print(stateName(flightState));
  Serial.print("  PYRO=");
  Serial.print(pyroArmed ? "ARMED" : "safe");
  Serial.print(pyroFired ? " FIRED" : "");
  Serial.println();
  Serial.println("----------------------------------------");
}


void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (testPending && millis() - testPendingTime > TEST_CONFIRM_WINDOW) {
      testPending = false;
      Serial.println("[PYRO] Test confirmation timed out - cancelled");
    }

    if (c == 'a' || c == 'A') {
      armFlight();
    }
    else if (c == 'x' || c == 'X') {
      disarmFlight();
    }
    else if (c == 't' || c == 'T') {
      if (pyroArmed) {
        Serial.println("[PYRO] TEST REFUSED - disarm first (X)");
      }
      else {
        testPending     = true;
        testPendingTime = millis();

        Serial.println();
        Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        Serial.println("! BENCH TEST FIRE                      !");
        Serial.println("! Is the e-match DISCONNECTED?         !");
        Serial.println("! Press Y within 5 s to fire the gate. !");
        Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        Serial.println();
      }
    }
    else if (c == 'y' || c == 'Y') {
      if (testPending) {
        testPending = false;
        testFirePyro();
      }
      else {
        Serial.println("[PYRO] Nothing to confirm");
      }
    }
    else if (c == 'd' || c == 'D') {
      dumpLogFile();
    }
    else if (c == 'l' || c == 'L') {
      listFiles();
    }
    else if (c == 's' || c == 'S') {
      printStatus();
    }
    else if (c == 'n' || c == 'N') {
      startNewLogFile();
    }
    else if (c == 'i' || c == 'I') {
      sdOK = initSD(true);
    }
    else if (c == 'f' || c == 'F') {
      formatCard();
    }
    else if (c == 'p' || c == 'P') {
      showPacket = !showPacket;
      Serial.print("[TX] Packet contents ");
      Serial.println(showPacket ? "SHOWN" : "HIDDEN");
    }
    else if (c == '+' || c == '=') {
      if (txPower < 17) txPower++;
      applyTxPower();
      Serial.print("[TX] Power = ");
      Serial.print(txPower);
      Serial.println(" dBm");
    }
    else if (c == '-' || c == '_') {
      if (txPower > 2) txPower--;
      applyTxPower();
      Serial.print("[TX] Power = ");
      Serial.print(txPower);
      Serial.println(" dBm");
    }
    else if (c == 'c' || c == 'C') {
      txCopies = (txCopies == 2) ? 1 : 2;
      Serial.print("[TX] Copies per packet = ");
      Serial.println(txCopies);
    }
    else if (c == 'k' || c == 'K') {
      startGyroCal();
    }
    else if (c == 'r' || c == 'R') {
      // NOT on Y - that key confirms a pyro test fire.
      //
      // The CARD always gets both raw and filtered. This
      // only changes which one goes over the radio, so a
      // live before/after can be seen on the ground.
      txFiltered = !txFiltered;
      Serial.print("[FILT] Telemetry IMU fields = ");
      Serial.println(txFiltered ? "FILTERED" : "RAW");
    }
    else if (c == 'm' || c == 'M') {
      printMenu();
    }
  }
}
