#pragma once

#include <Arduino.h>
#include <string.h>
#include <Packet.h>

// Ask a configured node for its clock (ANON_REQ_TYPE_BASIC). Direct path only.
#define CLOCK_SKEW_SECS          30
#define CLOCK_PULL_INTERVAL_MS   (6UL * 60UL * 60UL * 1000UL)
#define CLOCK_PULL_RETRY_MS      (2UL * 60UL * 1000UL)
#define CLOCK_PULL_FIRST_MS      20000UL
#define CLOCK_PULL_TIMEOUT_MS    20000UL
#define CLOCK_NODE_MAX           16

inline bool clockNodeSlotUsed(const uint8_t key[PUB_KEY_SIZE]) {
  for (int i = 0; i < PUB_KEY_SIZE; i++) {
    if (key[i] != 0) return true;
  }
  return false;
}

inline bool clockNodesConfigured(const uint8_t nodes[][PUB_KEY_SIZE]) {
  for (int i = 0; i < CLOCK_NODE_MAX; i++) {
    if (clockNodeSlotUsed(nodes[i])) return true;
  }
  return false;
}

inline void clockNodesClear(uint8_t nodes[][PUB_KEY_SIZE]) {
  memset(nodes, 0, CLOCK_NODE_MAX * PUB_KEY_SIZE);
}

inline bool clockNodeListHas(const uint8_t nodes[][PUB_KEY_SIZE], const uint8_t* pub) {
  for (int i = 0; i < CLOCK_NODE_MAX; i++) {
    if (memcmp(nodes[i], pub, PUB_KEY_SIZE) == 0 && clockNodeSlotUsed(nodes[i])) return true;
  }
  return false;
}

// 1 added, 0 already present, -1 list full
inline int clockNodeAdd(uint8_t nodes[][PUB_KEY_SIZE], const uint8_t key[PUB_KEY_SIZE]) {
  int free_slot = -1;
  for (int i = 0; i < CLOCK_NODE_MAX; i++) {
    if (!clockNodeSlotUsed(nodes[i])) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (memcmp(nodes[i], key, PUB_KEY_SIZE) == 0) return 0;
  }
  if (free_slot < 0) return -1;
  memcpy(nodes[free_slot], key, PUB_KEY_SIZE);
  return 1;
}

// Display order is the used slots, 0 .. count-1. Returns 1 if removed, 0 if the index is unused.
inline int clockNodeRemoveAt(uint8_t nodes[][PUB_KEY_SIZE], int index) {
  if (index < 0) return 0;
  int seen = 0;
  for (int i = 0; i < CLOCK_NODE_MAX; i++) {
    if (!clockNodeSlotUsed(nodes[i])) continue;
    if (seen != index) {
      seen++;
      continue;
    }
    for (int j = i; j < CLOCK_NODE_MAX - 1; j++) {
      memcpy(nodes[j], nodes[j + 1], PUB_KEY_SIZE);
    }
    memset(nodes[CLOCK_NODE_MAX - 1], 0, PUB_KEY_SIZE);
    return 1;
  }
  return 0;
}

// One prefs key per slot. A pubkey is 64 hex chars; all 16 slots in one string
// are 1024 chars and abort prefs parsing (the token limit is 128).
inline void clockNodePrefKey(char* dest, size_t size, int index) {
  snprintf(dest, size, "c%d", index);
}

// Half the round trip, in whole seconds. Unix timestamps are 1-second steps.
inline uint32_t remoteClockAdjusted(uint32_t remote_now, uint32_t rtt_ms) {
  return remote_now + (rtt_ms / 2000);
}

// Invalid local clock takes the remote time as-is. A valid clock only moves forward,
// and only when the gap is larger than the path delay. Stepping backward would
// reuse earlier packet timestamps and break replay protection.
inline bool acceptRemoteClock(uint32_t local_now, bool time_valid, uint32_t remote_now,
                              uint32_t rtt_ms, uint32_t* accepted) {
  uint32_t adj = remoteClockAdjusted(remote_now, rtt_ms);
  if (!time_valid) {
    *accepted = adj;
    return true;
  }
  if (adj > local_now && (adj - local_now) > CLOCK_SKEW_SECS) {
    *accepted = adj;
    return true;
  }
  return false;
}

inline void reversePath(uint8_t* dest, const uint8_t* src, uint8_t path_len) {
  uint8_t n = path_len & 63;
  uint8_t sz = (path_len >> 6) + 1;
  for (uint8_t i = 0; i < n; i++) {
    memcpy(dest + (n - 1 - i) * sz, src + i * sz, sz);
  }
}

// Body after the 4-byte timestamp tag: type, reply path len, reply path.
inline uint8_t buildClockReqBody(uint8_t* dest, uint8_t reply_path_len, const uint8_t* reply_path) {
  dest[0] = 0x03; // ANON_REQ_TYPE_BASIC
  dest[1] = reply_path_len;
  size_t n = mesh::Packet::writePath(&dest[2], reply_path, reply_path_len);
  return (uint8_t)(2 + n);
}
