#pragma once

#include <stddef.h>
#include <stdint.h>

class NodePrefs;

// Resolves the MQTT host and checks that its TCP port accepts a connection.
// The lookup runs off the mesh loop: DNS and a connect timeout would stall the radio.
class MqttReach {
public:
  void begin(NodePrefs* prefs);
  void poll(bool mqtt_up);
  void copyStatus(char* ip, size_t ip_cap, char* tcp, size_t tcp_cap) const;

private:
  NodePrefs* _prefs = nullptr;
};
