#pragma once

#include <stddef.h>
#include <stdint.h>
#include <Mesh.h>

#define ADV_TYPE_NONE         0
#define ADV_TYPE_CHAT         1
#define ADV_TYPE_REPEATER     2
#define ADV_TYPE_ROOM         3
#define ADV_TYPE_SENSOR       4
//FUTURE: 5..15

#define ADV_LATLON_MASK       0x10
#define ADV_FEAT1_MASK        0x20
#define ADV_FEAT2_MASK        0x40
#define ADV_NAME_MASK         0x80

// Antenna installation, carried in the two feature words of an advert.
// feat1: bit 15 set when height is present, bits 12–14 kind (0–7), bits 0–11 height in decimetres.
//        The word is omitted when kind is 0 and height is absent.
// feat2: bits 0–8 bearing in degrees (0 = north, clockwise), bit 15 set when a bearing is present.
//        bits 9–12 are the kind when it is 8…15. Kinds 0–7 leave these bits clear and use feat1.
#define ADV_ANT_UNSET         0xFFFF
#define ADV_ANT_HEIGHT_MASK   0x0FFF
#define ADV_ANT_KIND_SHIFT    12
#define ADV_ANT_KIND_MASK     0x07
#define ADV_ANT_KIND_EXT_SHIFT 9
#define ADV_ANT_KIND_EXT_MASK 0x0F
#define ADV_ANT_HEIGHT_FLAG   0x8000
#define ADV_ANT_BEARING_MASK  0x01FF
#define ADV_ANT_BEARING_FLAG  0x8000

#define ADV_ANT_NONE          0
#define ADV_ANT_OMNI          1
#define ADV_ANT_COLLINEAR     2
#define ADV_ANT_WHIP          3
#define ADV_ANT_DIPOLE        4
#define ADV_ANT_YAGI          5
#define ADV_ANT_PANEL         6
#define ADV_ANT_MAGNET        7
#define ADV_ANT_MAXON         8

class AdvertDataBuilder {
  uint8_t _type;
  bool _has_loc;
  const char* _name;
  int32_t _lat, _lon;
  uint16_t _extra1 = 0;
  uint16_t _extra2 = 0;
public:
  AdvertDataBuilder(uint8_t adv_type) : _type(adv_type), _name(NULL), _has_loc(false) { }
  AdvertDataBuilder(uint8_t adv_type, const char* name) : _type(adv_type), _name(name), _has_loc(false) { }
  AdvertDataBuilder(uint8_t adv_type, const char* name, double lat, double lon) : 
      _type(adv_type), _name(name), _has_loc(true), _lat(lat * 1E6), _lon(lon * 1E6)  { }

  void setFeat1(uint16_t extra) { _extra1 = extra; }
  void setFeat2(uint16_t extra) { _extra2 = extra; }
  // kind is ADV_ANT_*. Pass ADV_ANT_UNSET when height or bearing is not set.
  void setAntenna(uint8_t kind, uint16_t height_dm, uint16_t bearing_deg);

  /**
   * \brief  encode the given advertisement data.
   * \param app_data  dest array, must be MAX_ADVERT_DATA_SIZE
   * \returns  the encoded length in bytes
   */
  uint8_t encodeTo(uint8_t app_data[]);
};

class AdvertDataParser {
  uint8_t _flags;
  bool _valid;
  char _name[MAX_ADVERT_DATA_SIZE];
  int32_t _lat, _lon;
  uint16_t _extra1;
  uint16_t _extra2;
public:
  AdvertDataParser(const uint8_t app_data[], uint8_t app_data_len);

  bool isValid() const { return _valid; }
  uint8_t getType() const { return _flags & 0x0F; }
  uint16_t getFeat1() const { return _extra1; }
  uint16_t getFeat2() const { return _extra2; }

  bool hasName() const { return _name[0] != 0; }
  const char* getName() const { return _name; }

  bool hasLatLon() const { return (_flags & ADV_LATLON_MASK) != 0; }
  int32_t getIntLat() const { return _lat; }
  int32_t getIntLon() const { return _lon; }
  double getLat() const { return ((double)_lat) / 1000000.0; }
  double getLon() const { return ((double)_lon) / 1000000.0; }
};

class AdvertTimeHelper {
public:
  static void formatRelativeTimeDiff(char dest[], int32_t seconds_from_now, bool short_fmt);
};
