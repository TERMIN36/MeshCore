#pragma once

#include <stddef.h>
#include <string.h>

// Firmware version is "<upstream MeshCore release>-<fork version>", e.g. v1.17.1-0.1.1.
// Release tags must match it exactly; build.sh appends "-<commit hash>".
#define MESHCORE_BASE_VERSION  "v1.17.1"
#define FORK_VERSION           "0.1.1"

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION  MESHCORE_BASE_VERSION "-" FORK_VERSION
#endif

// Copies the version without the trailing "-<commit hash>" added by build.sh:
// v1.17.1-0.1.1-abc1234 -> v1.17.1-0.1.1
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
