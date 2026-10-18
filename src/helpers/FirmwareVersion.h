#pragma once

#include <stddef.h>
#include <string.h>

// Own fork version line (no upstream MeshCore version prefix).
// Release tags must match it exactly; build.sh appends "-<commit hash>".
// A candidate tag may add a suffix, for example v0.2.0-rc11.
// Device-facing builds can override FIRMWARE_VERSION (e.g. termin36-v0.2.0-rc11).
#define FORK_VERSION  "v0.2.0"

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION  FORK_VERSION
#endif

// Copies the version without the trailing "-<commit hash>" added by build.sh:
// termin36-v0.2.0-rc11-abc1234 -> termin36-v0.2.0-rc11
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

// Splash screens already show "by Termin36" — keep only the version number:
// termin36-v0.2.0-rc11-abc1234 -> v0.2.0-rc11
inline void firmwareVersionForSplash(char* dest, size_t size, const char* ver) {
  firmwareVersionNoHash(dest, size, ver);
  static const char kPrefix[] = "termin36-";
  const size_t plen = sizeof(kPrefix) - 1;
  if (strncmp(dest, kPrefix, plen) == 0) {
    size_t n = strlen(dest + plen);
    memmove(dest, dest + plen, n + 1);
  }
}
