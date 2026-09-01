// =====================================================
// FILTER REPLAY - runs the FLIGHT filter code on a log
//
//   ./replay < raw.csv > filtered.csv
//
// Input  : T,AX,AY,AZ,GX,GY,GZ,MX,MY,MZ   (raw, any rate)
// Output : the same rows plus every filtered channel,
//          using the same column names the flight
//          computer writes to the SD card.
//
// This links src/Filters.cpp directly. If a cutoff or a
// Kalman gain is changed in Config.h, the graphs in the
// report change with it. There is no second copy of the
// maths to drift out of step.
// =====================================================

#include "Arduino.h"
#include "State.h"
#include "Filters.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>

unsigned long g_replayMicros = 0;
HostSerial    Serial;


// ---- the State.cpp globals Filters.cpp writes into ----

float ax = 0.0, ay = 0.0, az = 0.0;
float gx = 0.0, gy = 0.0, gz = 0.0;
float mx = 0.0, my = 0.0, mz = 0.0;
float heading = 0.0;

float fax = 0.0, fay = 0.0, faz = 0.0;
float fgx = 0.0, fgy = 0.0, fgz = 0.0;
float fmx = 0.0, fmy = 0.0, fmz = 0.0;

float accelNormRaw  = 0.0;
float accelNormFilt = 0.0;

float rollAcc  = 0.0, pitchAcc  = 0.0;
float rollLpf  = 0.0, pitchLpf  = 0.0;
float rollComp = 0.0, pitchComp = 0.0;
float rollKal  = 0.0, pitchKal  = 0.0;

float headingFilt = 0.0;

bool  accelTrusted = false;
bool  gyroCalDone  = false;
float gyroBiasX = 0.0, gyroBiasY = 0.0, gyroBiasZ = 0.0;

float imuHz    = 0.0;
bool  txFiltered = true;


static std::vector<std::string> splitCsv(const std::string &line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string cell;

  while (std::getline(ss, cell, ',')) out.push_back(cell);
  return out;
}


static int findCol(const std::vector<std::string> &hdr, const char *name) {
  for (size_t i = 0; i < hdr.size(); i++) {
    if (hdr[i] == name) return (int)i;
  }
  return -1;
}


int main() {
  std::string line;

  if (!std::getline(std::cin, line)) {
    fprintf(stderr, "replay: empty input\n");
    return 1;
  }

  std::vector<std::string> hdr = splitCsv(line);

  const char *want[] = {"T","AX","AY","AZ","GX","GY","GZ","MX","MY","MZ"};
  int col[10];

  for (int i = 0; i < 10; i++) {
    col[i] = findCol(hdr, want[i]);

    if (col[i] < 0) {
      fprintf(stderr, "replay: input is missing column %s\n", want[i]);
      return 1;
    }
  }

  // millis() must already be moving before filterInit, or
  // the calibration timeout starts from an odd place.
  g_replayMicros = 1000;
  filterInit();

  printf("T,AX,AY,AZ,GX,GY,GZ,MX,MY,MZ,"
         "FAX,FAY,FAZ,FGX,FGY,FGZ,FMX,FMY,FMZ,"
         "ANRM,FANRM,RA,PA,RL,PL,RC,PC,RK,PK,HDG,FHDG,ATR,IHZ\n");

  while (std::getline(std::cin, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::vector<std::string> f = splitCsv(line);
    if ((int)f.size() <= col[9]) continue;

    double t = atof(f[col[0]].c_str());

    ax = atof(f[col[1]].c_str());
    ay = atof(f[col[2]].c_str());
    az = atof(f[col[3]].c_str());
    gx = atof(f[col[4]].c_str());
    gy = atof(f[col[5]].c_str());
    gz = atof(f[col[6]].c_str());
    mx = atof(f[col[7]].c_str());
    my = atof(f[col[8]].c_str());
    mz = atof(f[col[9]].c_str());

    // Drive the clock from the log, so dt inside the
    // filters is the dt the flight actually had.
    g_replayMicros = (unsigned long)(t * 1.0e6) + 1000;

    // Sensors.cpp computes the raw heading before calling
    // the filter chain. Same order here.
    heading = atan2(my, mx) * RAD_TO_DEG;
    if (heading < 0)      heading += 360.0;
    if (heading >= 360.0) heading -= 360.0;

    filterUpdate();

    printf("%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
           "%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
           "%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
           "%.2f,%.2f,%d,%.1f\n",
           t, ax, ay, az, gx, gy, gz, mx, my, mz,
           fax, fay, faz, fgx, fgy, fgz, fmx, fmy, fmz,
           accelNormRaw, accelNormFilt,
           rollAcc, pitchAcc, rollLpf, pitchLpf,
           rollComp, pitchComp, rollKal, pitchKal,
           heading, headingFilt, accelTrusted ? 1 : 0, imuHz);
  }

  return 0;
}
