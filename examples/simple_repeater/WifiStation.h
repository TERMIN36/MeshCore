#pragma once

#include <stddef.h>

class NodePrefs;

// Joins the access point stored in NodePrefs. MQTT rides this link.
class WifiStation {
public:
  void begin(NodePrefs* prefs);
  void apply();
  bool copyAddress(char* dest, size_t cap) const;

private:
  NodePrefs* _prefs = nullptr;
};
