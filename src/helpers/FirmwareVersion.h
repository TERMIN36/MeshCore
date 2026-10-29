#pragma once

#include <stddef.h>
#include <string.h>

// Own fork version line (no upstream MeshCore version prefix).
// Release tags must match it exactly; build.sh appends "-<commit hash>".
// A candidate tag may add a suffix, for example v0.2.0-rc12.
// Device-facing release builds override FIRMWARE_VERSION:
//   stable tag v0.2.0        -> termin36-v0.2.0
//   candidate v0.2.0-rc12    -> t36-v0.2.0-rc12
#define FORK_DEVICE_VERSION_PREFIX  "t36-"
#define FORK_STABLE_VERSION_PREFIX  "termin36-"
#define FORK_VERSION  "v0.2.0"

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION  FORK_VERSION
#endif

// Copies the version without the trailing "-<commit hash>" added by build.sh:
// v0.2.0-rc12-abc1234 -> v0.2.0-rc12
inline void firmwareVersionNoHash(char* dest, size_t size, const char* ver) {
  if (size == 0) return;
  size_t len = strlen(ver);
  const char* dash = strrchr(ver, '-');
  if (dash) {
    const char* p = dash + 1;
    size_t n = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')) { p++; n++; }
    if (*p == 0 && n >= 7) len = (size_t)(dash - ver);
  }
  if (len >= size) len = size - 1;
  memcpy(dest, ver, len);
  dest[len] = 0;
}

// Device-info Version field (PACKET_DEVICE_INFO bytes 60-79): fixed width, no hash.
// A shorter string is null-padded. A string of exactly `field` characters fills the
// field and has no null inside it — do not write past `field`.
inline void firmwareVersionForDeviceInfo(char* dest, size_t field, const char* ver) {
  if (field == 0) return;
  char tmp[48];
  firmwareVersionNoHash(tmp, sizeof(tmp), ver);
  size_t n = strlen(tmp);
  if (n > field) n = field;
  memcpy(dest, tmp, n);
  if (n < field) memset(dest + n, 0, field - n);
}

// Splash uses the same string as device info (no commit hash suffix).
inline void firmwareVersionForSplash(char* dest, size_t size, const char* ver) {
  firmwareVersionNoHash(dest, size, ver);
}
