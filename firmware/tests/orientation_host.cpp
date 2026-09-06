// Run the real filter on sensor-frame samples; no serial device or GPIO access.
#include "Arduino.h"
#include "State.h"
#include "Filters.h"
#include <iomanip>
#include <iostream>

unsigned long hostNowMs = 1500;
HostSerial Serial;

int main() {
  filterInit();
  int samples;
  while (std::cin >> samples >> ax >> ay >> az >> gx >> gy >> gz >> mx >> my >> mz) {
    for (int i = 0; i < samples; ++i) {
      hostNowMs += 10;
      filterUpdate();
    }
    std::cout << std::setprecision(9)
      << rollAcc << ' ' << pitchAcc << ' ' << rollLpf << ' ' << pitchLpf << ' '
      << rollKal << ' ' << pitchKal << ' ' << rollComp << ' ' << pitchComp << ' '
      << heading << ' ' << headingFilt << ' ' << accelNormRaw << ' ' << accelNormFilt << ' '
      << ax << ' ' << ay << ' ' << az << ' ' << fax << ' ' << fay << ' ' << faz << '\n';
  }
}
