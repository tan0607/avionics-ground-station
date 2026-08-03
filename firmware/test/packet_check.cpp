// packet_check.cpp -- host-side proof that the C mirror matches the Python codec.
//
// Builds the SAME sample that shared/protocol/packet.py encodes in its self-test and
// asserts the firmware's telem_build_frame() reproduces the exact 32 wire bytes. If
// this passes, firmware and ground station agree byte-for-byte with no hardware.
//
// Reference (generated from packet.py):
//   frame = aa5500d2040188130000df048c0508f3ee01881e9f3c14050b03044f0d3fc889
//   crc16(body) = 0x89c8   (byte 29 = health 0x3F = all peripherals nominal)
//
// Build & run on the laptop (no ESP32 toolchain needed):
//   cc -std=c++11 -I firmware/lib/TelemPacket firmware/test/packet_check.cpp -o firmware/test/packet_check
//   ./firmware/test/packet_check
#include <cstdio>
#include <cstring>
#include "TelemPacket.h"

// The 32-byte frame packet.py produces for the reference Telemetry sample.
static const uint8_t REF_FRAME[TELEM_PACKET_SIZE] = {
    0xaa,0x55,0x00,0xd2,0x04,0x01,0x88,0x13,0x00,0x00,0xdf,0x04,0x8c,0x05,0x08,0xf3,
    0xee,0x01,0x88,0x1e,0x9f,0x3c,0x14,0x05,0x0b,0x03,0x04,0x4f,0x0d,0x3f,0xc8,0x89,
};
static const uint16_t REF_CRC = 0x89c8;

int main() {
  telem_body_t body;
  memset(&body, 0, sizeof(body));
  body.msg_type     = TELEM_MSG_TELEMETRY;
  body.seq          = 1234;
  body.flight_state = FS_BOOST;
  body.onboard_ms   = 5000;
  body.baro_alt_m   = 1247;
  body.vspeed_dms   = 1420;
  body.gps_lat      = 32437000;      // 3.2437 deg * 1e7
  body.gps_lon      = 1017061000;    // 101.7061 deg * 1e7
  body.gps_alt_m    = 1300;
  body.gps_sats     = 11;
  body.gps_fix      = GPS_FIX_3D;
  body.tilt_deg     = 4;
  body.vbat_dv      = 79;            // 7.9 V
  body.flags        = FLAG_CONTINUITY | FLAG_ARMED | FLAG_SD_OK;  // 0x0D
  body.health       = HEALTH_ALL_OK;                              // 0x3F

  uint8_t frame[TELEM_PACKET_SIZE];
  size_t n = telem_build_frame(&body, frame);

  int ok = 1;
  if (n != TELEM_PACKET_SIZE) ok = 0;
  if (sizeof(telem_body_t) != TELEM_BODY_SIZE) ok = 0;

  uint16_t crc = telem_crc16_ccitt((const uint8_t*)&body, TELEM_BODY_SIZE);
  if (crc != REF_CRC) ok = 0;

  if (memcmp(frame, REF_FRAME, TELEM_PACKET_SIZE) != 0) ok = 0;

  printf("sizeof(telem_body_t) = %zu (want %d)\n", sizeof(telem_body_t), TELEM_BODY_SIZE);
  printf("crc16(body)          = 0x%04X (want 0x%04X)\n", crc, REF_CRC);
  printf("frame                = ");
  for (size_t i = 0; i < TELEM_PACKET_SIZE; i++) printf("%02x", frame[i]);
  printf("\n");

  if (ok) {
    printf("PASS: firmware frame is byte-for-byte identical to packet.py\n");
    return 0;
  }
  printf("FAIL: firmware frame does NOT match packet.py -- fix TelemPacket.h\n");
  return 1;
}
