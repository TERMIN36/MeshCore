#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Packet.h"
#include "helpers/bridges/MqttBridgePolicy.h"
#include "helpers/bridges/MqttEnvelope.h"

static MqttIdentity ident(const char* name, bool timed = false, uint32_t unix_time = 0) {
  MqttIdentity id{};
  for (int i = 0; i < 32; i++) id.id[i] = (uint8_t)(0x10 + i);
  std::strncpy(id.name, name, MQTT_NODE_NAME_MAX);
  id.has_time = timed;
  id.unix_time = unix_time;
  return id;
}

static MqttRadioDesc radioDesc() {
  MqttRadioDesc radio{};
  radio.freq_hz = 869618000;
  radio.bw_hz = 62500;
  radio.sf = 8;
  radio.cr = 5;
  radio.tx_dbm = 22;
  radio.has_ant = true;
  radio.ant_cm = 1250;
  radio.has_coords = true;
  radio.lat_e7 = 557558000;
  radio.lon_e7 = 376176000;
  radio.forwarding = true;
  radio.noise_floor = -108;
  radio.tx_air_secs = 12;
  radio.rx_air_secs = 34;
  radio.uptime_secs = 3600;
  radio.tx_queue = 2;
  radio.batt_mv = 4120;
  radio.temp_c_x10 = 365;
  std::strncpy(radio.firmware, "v1.17.1-0.1.3", MQTT_FIRMWARE_MAX);
  return radio;
}

static mesh::Packet makePacket(uint8_t seed, uint8_t path_hash) {
  mesh::Packet packet;
  packet.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT);
  packet.payload[0] = seed;
  packet.payload[1] = 0x5A;
  packet.payload_len = 2;
  packet.path[0] = path_hash;
  packet.setPathHashSizeAndCount(1, path_hash ? 1 : 0);
  return packet;
}

static bool containsText(const uint8_t* data, size_t len, const char* text) {
  size_t n = std::strlen(text);
  if (n == 0 || n > len) return false;
  for (size_t i = 0; i + n <= len; i++) {
    if (std::memcmp(data + i, text, n) == 0) return true;
  }
  return false;
}

TEST(MqttEnvelope, RejectsUnknownVersion) {
  uint8_t raw[8] = {2, MQTT_TYPE_PACKET, 0, 0, 0, 0, 0, 0};
  MqttEnvelopeView view;
  EXPECT_EQ(mqttDecodeEnvelope(raw, sizeof(raw), &view), MqttDecode::BadVersion);
}

TEST(MqttEnvelope, OmitsTunnelNameClockAndOptionalRadio) {
  uint8_t id[32];
  std::memset(id, 0xAB, sizeof(id));
  uint8_t body[4] = {1, 2, 3, 4};
  uint8_t raw[MQTT_ENVELOPE_MAX];
  size_t n = mqttEncodeEnvelope(raw, sizeof(raw), MQTT_TYPE_PACKET, id, "North", 7, false, 0, body, 4);
  ASSERT_GT(n, 0u);
  EXPECT_FALSE(containsText(raw, n, "tunnel/alpha"));
  MqttEnvelopeView view;
  ASSERT_EQ(mqttDecodeEnvelope(raw, n, &view), MqttDecode::Ok);
  EXPECT_EQ(view.seq, 7u);
  EXPECT_FALSE(view.has_time);
  EXPECT_STREQ(view.name, "North");
  EXPECT_EQ(view.body_len, 4);
  EXPECT_EQ(std::memcmp(view.id, id, 32), 0);
}

TEST(MqttEnvelope, HelloCarriesRadioAndSessionWithoutSecrets) {
  MqttHelloBody hello{};
  hello.freq_hz = 869618000;
  hello.bw_hz = 62500;
  hello.sf = 8;
  hello.cr = 5;
  hello.tx_dbm = 22;
  hello.has_ant = true;
  hello.ant_cm = 1000;
  hello.has_coords = true;
  hello.lat_e7 = 1;
  hello.lon_e7 = -2;
  hello.forwarding = true;
  hello.session_id = 3;
  hello.noise_floor = -108;
  hello.tx_air_secs = 12;
  hello.rx_air_secs = 34;
  hello.uptime_secs = 3600;
  hello.tx_queue = 2;
  hello.batt_mv = 4120;
  hello.temp_c_x10 = 365;
  std::strncpy(hello.firmware, "v1.17.1-0.1.3", MQTT_FIRMWARE_MAX);
  uint8_t body[128];
  size_t body_len = mqttEncodeHelloBody(body, sizeof(body), hello);
  uint8_t id[32];
  std::memset(id, 1, sizeof(id));
  uint8_t raw[MQTT_ENVELOPE_MAX];
  size_t n = mqttEncodeEnvelope(raw, sizeof(raw), MQTT_TYPE_HELLO, id, "R1", 1, true, 1700000000, body, (uint16_t)body_len);
  ASSERT_GT(n, 0u);
  EXPECT_FALSE(containsText(raw, n, "secret-pass"));
  EXPECT_FALSE(containsText(raw, n, "mesh/tunnel"));
  MqttEnvelopeView view;
  ASSERT_EQ(mqttDecodeEnvelope(raw, n, &view), MqttDecode::Ok);
  EXPECT_TRUE(view.has_time);
  EXPECT_EQ(view.unix_time, 1700000000u);
  MqttHelloBody back;
  ASSERT_TRUE(mqttDecodeHelloBody(view.body, view.body_len, &back));
  EXPECT_EQ(back.freq_hz, 869618000u);
  EXPECT_EQ(back.bw_hz, 62500u);
  EXPECT_EQ(back.ant_cm, 1000);
  EXPECT_EQ(back.lat_e7, 1);
  EXPECT_EQ(back.lon_e7, -2);
  EXPECT_TRUE(back.forwarding);
  EXPECT_EQ(back.session_id, 3u);
  EXPECT_EQ(back.pkt_pub, 0u);
  EXPECT_EQ(back.noise_floor, -108);
  EXPECT_EQ(back.tx_air_secs, 12u);
  EXPECT_EQ(back.rx_air_secs, 34u);
  EXPECT_EQ(back.uptime_secs, 3600u);
  EXPECT_EQ(back.tx_queue, 2u);
  EXPECT_EQ(back.batt_mv, 4120u);
  EXPECT_EQ(back.temp_c_x10, 365);
  EXPECT_STREQ(back.firmware, "v1.17.1-0.1.3");
}

TEST(MqttEnvelope, CaPemRequiresBeginAndEnd) {
  const char* ok = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
  EXPECT_TRUE(mqttCaPemOk(ok, std::strlen(ok)));
  EXPECT_FALSE(mqttCaPemOk(ok, 10));
  EXPECT_FALSE(mqttCaPemOk("not a cert", 10));
  const char* backwards = "-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\n";
  EXPECT_FALSE(mqttCaPemOk(backwards, std::strlen(backwards)));
}

TEST(MqttEnvelope, HelloOmitsUnsetAntennaAndCoordinates) {
  MqttHelloBody hello{};
  hello.freq_hz = 1;
  hello.bw_hz = 2;
  hello.session_id = 1;
  uint8_t body[96];
  size_t n = mqttEncodeHelloBody(body, sizeof(body), hello);
  MqttHelloBody back;
  ASSERT_TRUE(mqttDecodeHelloBody(body, (uint16_t)n, &back));
  EXPECT_FALSE(back.has_ant);
  EXPECT_FALSE(back.has_coords);
  EXPECT_EQ(back.noise_floor, 0);
  EXPECT_EQ(back.tx_air_secs, 0u);
  EXPECT_EQ(back.rx_air_secs, 0u);
  EXPECT_EQ(back.uptime_secs, 0u);
  EXPECT_EQ(back.tx_queue, 0u);
  EXPECT_EQ(back.batt_mv, 0u);
  EXPECT_EQ(back.temp_c_x10, 0);
  EXPECT_STREQ(back.firmware, "");
}

TEST(MqttEnvelope, HeartbeatOmitsUnsetCoordinates) {
  MqttHeartbeatBody beat{};
  beat.session_id = 4;
  beat.noise_floor = -120;
  beat.tx_air_secs = 1;
  beat.rx_air_secs = 2;
  beat.uptime_secs = 9;
  beat.tx_queue = 1;
  beat.batt_mv = 3700;
  beat.temp_c_x10 = MQTT_TEMP_UNKNOWN;
  std::strncpy(beat.firmware, "v1.17.1-0.1.3", MQTT_FIRMWARE_MAX);
  uint8_t body[96];
  size_t n = mqttEncodeHeartbeatBody(body, sizeof(body), beat);
  MqttHeartbeatBody back;
  ASSERT_TRUE(mqttDecodeHeartbeatBody(body, (uint16_t)n, &back));
  EXPECT_FALSE(back.has_coords);
  EXPECT_EQ(back.noise_floor, -120);
  EXPECT_EQ(back.tx_air_secs, 1u);
  EXPECT_EQ(back.rx_air_secs, 2u);
  EXPECT_EQ(back.session_id, 4u);
  EXPECT_EQ(back.uptime_secs, 9u);
  EXPECT_EQ(back.tx_queue, 1u);
  EXPECT_EQ(back.batt_mv, 3700u);
  EXPECT_EQ(back.temp_c_x10, MQTT_TEMP_UNKNOWN);
  EXPECT_STREQ(back.firmware, "v1.17.1-0.1.3");
}

TEST(MqttPolicy, PublishRequiresSubscriptionAndBridgeCache) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  mesh::Packet packet = makePacket(1, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  EXPECT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Skipped);

  policy.onSubscribed();
  EXPECT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 1000);
  EXPECT_EQ(policy.session().pkt_pub, 0u);
  EXPECT_EQ(policy.lifetime().connects, 1u);

  EXPECT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  mesh::Packet later = packet;
  later.path[0] = 9;
  later.setPathHashSizeAndCount(1, 1);
  uint8_t raw2[32];
  uint8_t n2 = later.writeTo(raw2);
  EXPECT_EQ(policy.enqueuePacket(&later, raw2, n2, id), MqttEnqueue::Skipped);

  MqttOutbound published;
  ASSERT_TRUE(policy.popOutbound(published));
  EXPECT_EQ(published.type, MQTT_TYPE_PACKET);
  policy.onPublishOk(published, 1100);
  EXPECT_EQ(policy.session().pkt_pub, 1u);
  EXPECT_EQ(policy.lifetime().totals.pkt_pub, 1u);
}

TEST(MqttPolicy, PacketHashIgnoresPath) {
  mesh::Packet a = makePacket(4, 1);
  mesh::Packet b = makePacket(4, 2);
  b.setPathHashSizeAndCount(1, 1);
  uint8_t ha[MAX_HASH_SIZE];
  uint8_t hb[MAX_HASH_SIZE];
  a.calculatePacketHash(ha);
  b.calculatePacketHash(hb);
  EXPECT_EQ(std::memcmp(ha, hb, MAX_HASH_SIZE), 0);
}

TEST(MqttPolicy, InboundDuplicateDoesNotEnterAndHelloIsIgnored) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  EXPECT_FALSE(containsText(hello.bytes, hello.len, "tunnel-name"));
  policy.onPublishOk(hello, 50);

  mesh::Packet packet = makePacket(8, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  MqttOutbound out;
  ASSERT_TRUE(policy.popOutbound(out));
  policy.onPublishOk(out, 60);

  uint8_t echoed[MAX_TRANS_UNIT];
  uint16_t echoed_len = 0;
  EXPECT_EQ(policy.receive(out.bytes, out.len, echoed, &echoed_len), MqttRx::Duplicate);
  EXPECT_EQ(policy.session().mqtt_dup, 1u);
  EXPECT_EQ(policy.receive(hello.bytes, hello.len, echoed, &echoed_len), MqttRx::Ignore);
  EXPECT_EQ(policy.session().mqtt_dup, 1u);

  mesh::Packet other = makePacket(9, 0);
  uint8_t raw_other[32];
  uint8_t other_len = other.writeTo(raw_other);
  uint8_t env[MQTT_ENVELOPE_MAX];
  size_t env_len = mqttEncodeEnvelope(env, sizeof(env), MQTT_TYPE_PACKET, id.id, id.name, 9, false, 0, raw_other, other_len);
  EXPECT_EQ(policy.receive(env, env_len, echoed, &echoed_len), MqttRx::Packet);
  EXPECT_EQ(echoed_len, other_len);
  uint8_t hash[MAX_HASH_SIZE];
  other.calculatePacketHash(hash);
  policy.commitReceived(hash);
  EXPECT_EQ(policy.session().pkt_in, 1u);
  EXPECT_EQ(policy.receive(env, env_len, echoed, &echoed_len), MqttRx::Duplicate);
}

TEST(MqttPolicy, HeartbeatYieldsToPacketAndDoesNotAccumulate) {
  MqttBridgePolicy policy;
  auto id = ident("A", true, 5000);
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 0);

  policy.pollHeartbeat(MqttBridgePolicy::HEARTBEAT_MS, id, radioDesc());
  MqttOutbound beat;
  ASSERT_TRUE(policy.popOutbound(beat));
  EXPECT_EQ(beat.type, MQTT_TYPE_HEARTBEAT);
  MqttEnvelopeView beat_view;
  ASSERT_EQ(mqttDecodeEnvelope(beat.bytes, beat.len, &beat_view), MqttDecode::Ok);
  MqttHeartbeatBody beat_body;
  ASSERT_TRUE(mqttDecodeHeartbeatBody(beat_view.body, beat_view.body_len, &beat_body));
  EXPECT_EQ(beat_body.noise_floor, -108);
  EXPECT_EQ(beat_body.tx_air_secs, 12u);
  EXPECT_EQ(beat_body.rx_air_secs, 34u);
  EXPECT_EQ(beat_body.uptime_secs, 3600u);
  EXPECT_EQ(beat_body.tx_queue, 2u);
  EXPECT_EQ(beat_body.batt_mv, 4120u);
  EXPECT_EQ(beat_body.temp_c_x10, 365);
  EXPECT_STREQ(beat_body.firmware, "v1.17.1-0.1.3");
  EXPECT_TRUE(beat_body.has_coords);
  EXPECT_EQ(beat_body.lat_e7, 557558000);
  EXPECT_EQ(beat_body.lon_e7, 376176000);
  policy.onPublishOk(beat, MqttBridgePolicy::HEARTBEAT_MS);
  EXPECT_EQ(policy.lifetime().last_heartbeat_unix, 5000u);

  policy.pollHeartbeat(MqttBridgePolicy::HEARTBEAT_MS + 10, id, radioDesc());
  EXPECT_FALSE(policy.popOutbound(beat));

  for (int i = 0; i < MqttBridgePolicy::QUEUE_CAP - 1; i++) {
    mesh::Packet packet = makePacket((uint8_t)(20 + i), 0);
    uint8_t raw[32];
    uint8_t n = packet.writeTo(raw);
    ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  }
  policy.pollHeartbeat(2 * MqttBridgePolicy::HEARTBEAT_MS, id, radioDesc());
  mesh::Packet urgent = makePacket(99, 0);
  uint8_t raw[32];
  uint8_t n = urgent.writeTo(raw);
  EXPECT_EQ(policy.enqueuePacket(&urgent, raw, n, id), MqttEnqueue::Queued);
  bool saw_beat = false;
  bool saw_urgent = false;
  MqttOutbound item;
  while (policy.popOutbound(item)) {
    if (item.type == MQTT_TYPE_HEARTBEAT) saw_beat = true;
    if (item.type == MQTT_TYPE_PACKET && item.has_hash) {
      uint8_t hash[MAX_HASH_SIZE];
      urgent.calculatePacketHash(hash);
      if (std::memcmp(item.hash, hash, MAX_HASH_SIZE) == 0) saw_urgent = true;
    }
    policy.onPublishOk(item, 1);
  }
  EXPECT_FALSE(saw_beat);
  EXPECT_TRUE(saw_urgent);
  EXPECT_GE(policy.session().pub_err, 1u);
}

TEST(MqttPolicy, PublishFailReleasesHash) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 1);

  mesh::Packet packet = makePacket(11, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  MqttOutbound out;
  ASSERT_TRUE(policy.popOutbound(out));
  policy.onPublishFail(out);
  EXPECT_EQ(policy.session().pub_err, 1u);
  EXPECT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
}

TEST(MqttPolicy, DisconnectDropsHeartbeatAndReleasesQueuedPacket) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 0);
  policy.pollHeartbeat(MqttBridgePolicy::HEARTBEAT_MS, id, radioDesc());

  mesh::Packet packet = makePacket(12, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  uint32_t errors = policy.session().pub_err;
  policy.onDisconnected();
  EXPECT_EQ(policy.session().pub_err, errors + 1u);
  EXPECT_FALSE(policy.popOutbound(hello));
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 10);
  EXPECT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
}

TEST(MqttPolicy, StaleHelloDoesNotCloseNewSession) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound stale;
  ASSERT_TRUE(policy.popOutbound(stale));
  policy.onDisconnected();
  policy.onSubscribed();
  policy.onPublishOk(stale, 100);
  EXPECT_TRUE(policy.needsHello());
  EXPECT_EQ(policy.session().pkt_pub, 0u);
  EXPECT_EQ(policy.lifetime().connects, 2u);
}

TEST(MqttPolicy, ReconnectResetsSessionAndKeepsLifetime) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 10);
  mesh::Packet packet = makePacket(3, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  MqttOutbound out;
  ASSERT_TRUE(policy.popOutbound(out));
  policy.onPublishOk(out, 20);
  EXPECT_EQ(policy.session().pkt_pub, 1u);

  policy.onDisconnected();
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  ASSERT_TRUE(policy.popOutbound(hello));
  MqttEnvelopeView view;
  ASSERT_EQ(mqttDecodeEnvelope(hello.bytes, hello.len, &view), MqttDecode::Ok);
  MqttHelloBody body;
  ASSERT_TRUE(mqttDecodeHelloBody(view.body, view.body_len, &body));
  EXPECT_EQ(body.session_id, 2u);
  EXPECT_EQ(body.pkt_pub, 0u);
  EXPECT_EQ(policy.session().pkt_pub, 0u);
  EXPECT_EQ(policy.lifetime().connects, 2u);
  EXPECT_EQ(policy.lifetime().totals.pkt_pub, 1u);
  policy.onPublishOk(hello, 30);
  EXPECT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Skipped);
}

TEST(MqttPolicy, DeferRetriesSamePacketWithoutError) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 10);

  mesh::Packet packet = makePacket(3, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  MqttOutbound out;
  ASSERT_TRUE(policy.popOutbound(out));
  policy.defer(out);
  EXPECT_EQ(policy.session().pub_err, 0u);
  MqttOutbound again;
  ASSERT_TRUE(policy.popOutbound(again));
  EXPECT_EQ(again.type, MQTT_TYPE_PACKET);
  EXPECT_EQ(again.len, out.len);
  EXPECT_EQ(std::memcmp(again.bytes, out.bytes, out.len), 0);
  policy.onPublishOk(again, 20);
  EXPECT_EQ(policy.session().pkt_pub, 1u);
}

TEST(MqttPolicy, DeferAfterDisconnectDropsPacket) {
  MqttBridgePolicy policy;
  auto id = ident("A");
  policy.onSubscribed();
  ASSERT_EQ(policy.enqueueHello(id, radioDesc()), MqttEnqueue::Queued);
  MqttOutbound hello;
  ASSERT_TRUE(policy.popOutbound(hello));
  policy.onPublishOk(hello, 10);

  mesh::Packet packet = makePacket(5, 0);
  uint8_t raw[32];
  uint8_t n = packet.writeTo(raw);
  ASSERT_EQ(policy.enqueuePacket(&packet, raw, n, id), MqttEnqueue::Queued);
  MqttOutbound out;
  ASSERT_TRUE(policy.popOutbound(out));
  policy.onDisconnected();
  policy.defer(out);
  EXPECT_EQ(policy.lifetime().totals.pub_err, 1u);
  MqttOutbound again;
  EXPECT_FALSE(policy.popOutbound(again));
}
