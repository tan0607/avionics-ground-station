// TelemPacket.h -- C mirror of the telemetry wire format.
//
// SOURCE OF TRUTH is shared/protocol/PROTOCOL.md + shared/protocol/packet.py. This
// header exists because firmware can't `import packet`; it must reproduce the exact
// bytes. Field order, types, packing, and CRC here MUST stay identical to packet.py.
// The host test (test/packet_check.cpp) proves that by rebuilding a Python-generated
// reference frame and comparing byte-for-byte.
//
// Deliberately free of <Arduino.h> so it compiles on the laptop for that test.
//
// Wire frame (32 bytes, little-endian):
//   [0:2]   SYNC  = 0xAA 0x55
//   [2:30]  BODY  = telem_body_t (28 bytes, packed)
//   [30:32] CRC16 = CRC-16/CCITT-FALSE over BODY, little-endian
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define TELEM_SYNC0      0xAA
#define TELEM_SYNC1      0x55
#define TELEM_BODY_SIZE   28
#define TELEM_PACKET_SIZE 32

#define TELEM_MSG_TELEMETRY 0  // msg_type; 1/2 reserved for phase-2 command/ack

// flight_state enum (matches packet.FlightState)
enum {
  FS_PAD    = 0,
  FS_BOOST  = 1,
  FS_COAST  = 2,
  FS_APOGEE = 3,
  FS_DROGUE = 4,
  FS_MAIN   = 5,
  FS_LANDED = 6,
};

// gps_fix enum (matches packet.GpsFix)
enum { GPS_FIX_NONE = 0, GPS_FIX_2D = 2, GPS_FIX_3D = 3 };

// flags bitfield (matches packet.py)
#define FLAG_CONTINUITY (1u << 0)  // e-match continuity OK
#define FLAG_PYRO_FIRED (1u << 1)  // a pyro channel has fired
#define FLAG_SD_OK      (1u << 2)  // onboard SD logging healthy
#define FLAG_ARMED      (1u << 3)  // flight computer armed

// BODY layout. Order/types match Python struct "<BHBIhhiihBBBBBB" exactly.
// ESP32 (and the host) are little-endian, so this packed struct maps straight
// onto the wire BODY with no byte-swapping.
#pragma pack(push, 1)
typedef struct {
  uint8_t  msg_type;      // 0 = telemetry
  uint16_t seq;           // +1 per packet, wraps at 65535 -> drives loss rate
  uint8_t  flight_state;
  uint32_t onboard_ms;    // ms since flight-computer boot
  int16_t  baro_alt_m;    // m AGL
  int16_t  vspeed_dms;    // dm/s  (m/s * 10)
  int32_t  gps_lat;       // deg * 1e7
  int32_t  gps_lon;       // deg * 1e7
  int16_t  gps_alt_m;     // m MSL
  uint8_t  gps_sats;
  uint8_t  gps_fix;       // 0 / 2 / 3
  uint8_t  tilt_deg;      // 0..180
  uint8_t  vbat_dv;       // V * 10
  uint8_t  flags;
  uint8_t  reserved;      // 0
} telem_body_t;
#pragma pack(pop)

// Compile-time proof the packing produced exactly 28 bytes (C++11 / C11).
#if defined(__cplusplus)
static_assert(sizeof(telem_body_t) == TELEM_BODY_SIZE, "telem_body_t must be 28 bytes");
#else
_Static_assert(sizeof(telem_body_t) == TELEM_BODY_SIZE, "telem_body_t must be 28 bytes");
#endif

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
// Byte-for-byte identical to packet.crc16_ccitt (PROTOCOL.md C reference).
static inline uint16_t telem_crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
  }
  return crc;
}

// Serialize body -> 32-byte wire frame in `out`. Returns TELEM_PACKET_SIZE.
static inline size_t telem_build_frame(const telem_body_t* body, uint8_t* out) {
  out[0] = TELEM_SYNC0;
  out[1] = TELEM_SYNC1;
  memcpy(out + 2, body, TELEM_BODY_SIZE);
  uint16_t crc = telem_crc16_ccitt((const uint8_t*)body, TELEM_BODY_SIZE);
  out[30] = (uint8_t)(crc & 0xFF);         // CRC little-endian: low byte first
  out[31] = (uint8_t)((crc >> 8) & 0xFF);
  return TELEM_PACKET_SIZE;
}
