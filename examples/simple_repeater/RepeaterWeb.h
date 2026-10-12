#pragma once

#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
  REPEATER_TIMER_OFF = 0xFFFFFFFFu,
  REPEATER_TIMER_UNSET = 0xFFFFFFFEu
};

enum : int16_t { REPEATER_TEMP_UNKNOWN = -32768 };

struct RepeaterNeighbour {
  char name[32];
  char id[12];
  int8_t snr_q4;
  uint32_t secs_ago;
};

struct RepeaterClockSource {
  char name[32];
  char id[12];
};

#ifndef REPEATER_PAGE_NEIGHBOURS
#define REPEATER_PAGE_NEIGHBOURS 50
#endif
#ifndef REPEATER_PAGE_CLOCKS
#define REPEATER_PAGE_CLOCKS 16
#endif
#ifndef REPEATER_PAGE_REGIONS
#define REPEATER_PAGE_REGIONS 32
#endif

struct RepeaterRegionEntry {
  char name[31];
  char parent[31];
  uint8_t depth;
  uint8_t flood_ok;
  uint8_t is_default;
  uint8_t is_home;
  uint32_t rx;
  uint32_t fwd;
};

struct RepeaterPageInfo {
  char name[32];
  char firmware[40];
  char build[24];
  char board[48];
  char id[12];
  char ssid[33];
  char ip[16];
  uint8_t wifi_ps;
  uint8_t wifi_ps_active;
  uint8_t wifi_up;
  int8_t wifi_rssi;
  char ntp_server[64];
  char mqtt[24];
  char tel[24];
  char tel_iata[8];
  char tel_host[64];
  char tel_user[32];
  char tel_pass[40];
  char lat[16];
  char lon[16];
  uint8_t ant_kind;
  uint16_t ant_height_dm;
  uint16_t ant_bearing;
  char clock_text[32];
  char mqtt_host[64];
  char mqtt_ip[32];
  char mqtt_tcp[24];
  char mqtt_user[32];
  char mqtt_tunnel[65];
  char owner[120];
  float freq;
  float bw;
  float airtime_factor;
  float rx_delay;
  float tx_delay;
  float direct_tx_delay;
  float mqtt_ant_m;
  uint8_t sf;
  uint8_t cr;
  uint8_t radio_profile;
  int8_t tx_dbm;
  uint8_t forwarding;
  uint8_t rx_gain;
  uint8_t fem_rx;
  uint8_t fem_tx;
  uint8_t fem_rx_ok;
  uint8_t fem_tx_ok;
  uint8_t time_valid;
  uint8_t ntp_enabled;
  uint8_t ntp_state;
  uint8_t cad;
  uint8_t interference;
  uint8_t path_hash_bytes;
  uint8_t loop_detect;
  uint8_t flood_max;
  uint8_t flood_max_unscoped;
  uint8_t flood_max_advert;
  uint8_t multi_acks;
  uint8_t mqtt_enabled;
  uint8_t tel_enabled;
  uint8_t tel_tx;
  uint16_t tel_port;
  uint8_t mqtt_tls;
  uint8_t mqtt_cert;
  uint8_t clock_count;
  uint8_t neighbour_count;
  uint16_t batt_mv;
  int16_t temp_c_x10;
  uint16_t agc_secs;
  uint16_t advert_local_mins;
  uint16_t mqtt_port;
  uint8_t advert_flood_hours;
  uint32_t uptime_secs;
  uint32_t next_local_secs;
  uint32_t next_flood_secs;
  uint32_t next_clock_secs;
  uint16_t err_flags;
  uint32_t queue_len;
  int noise_floor;
  int last_rssi;
  float last_snr;
  uint32_t tx_air_secs;
  uint32_t rx_air_secs;
  uint32_t recv;
  uint32_t sent;
  uint32_t flood_tx;
  uint32_t direct_tx;
  uint32_t flood_rx;
  uint32_t direct_rx;
  uint32_t recv_errors;
  uint8_t region_count;
  uint8_t wildcard_flood;
  char region_default[31];
  char region_home[31];
  uint32_t region_unscoped_rx;
  uint32_t region_unscoped_fwd;
  uint32_t region_drop_unknown;
  uint32_t region_drop_denied;
  uint32_t region_drop_unscoped;
  uint32_t region_drop_hop;
  uint32_t region_drop_loop;
  uint32_t region_drop_nofwd;
  RepeaterClockSource clocks[REPEATER_PAGE_CLOCKS];
  RepeaterNeighbour neighbours[REPEATER_PAGE_NEIGHBOURS];
  RepeaterRegionEntry regions[REPEATER_PAGE_REGIONS];
};

// Posted from the status page and applied on the main loop.
struct RepeaterRadioForm {
  float freq;
  float bw;
  float airtime;
  float rx_delay;
  float tx_delay;
  float direct_tx_delay;
  int8_t tx_dbm;
  uint8_t sf;
  uint8_t cr;
  uint8_t profile;
  uint8_t rx_gain;
  uint8_t fem_rx;
  uint8_t fem_tx;
  uint8_t fem_rx_set;
  uint8_t fem_tx_set;
  uint8_t forwarding;
  uint8_t cad;
  uint8_t interference;
  uint8_t flood_max;
  uint8_t flood_max_unscoped;
  uint8_t flood_max_advert;
  uint8_t path_hash_mode;
  uint8_t loop_detect;
  uint8_t multi_acks;
  uint16_t agc_secs;
};

struct RepeaterNodeForm {
  char name[32];
  char owner[120];
  char lat[16];
  char lon[16];
  uint8_t ant_kind;
  uint16_t ant_height_dm;
  uint16_t ant_bearing;
  uint16_t advert_mins;
  uint8_t flood_hours;
};

enum : uint8_t {
  REGION_OP_FLOOD = 1,
  REGION_OP_ADD = 2,
  REGION_OP_REMOVE = 3,
  REGION_OP_DEFAULT = 4,
  REGION_OP_HOME = 5
};

struct RepeaterRegionForm {
  uint8_t op;
  uint8_t flood_ok;
  char name[31];
  char parent[31];
};

// HTTP status page. Starts once the station has an address, stops when WiFi is off, and binds again if the address changes.
class RepeaterWeb {
public:
  void poll(bool enabled);
  void stop();
  bool commandQueued() const;
  bool viewerConnected() const;
  // A queued command or a firmware upload. The CPU should not light-sleep.
  bool webBusy() const;
};
