#include <helpers/AdvertDataHelpers.h>
#include <helpers/UTF8Helpers.h>

  void AdvertDataBuilder::setAntenna(uint8_t kind, uint16_t height_dm, uint16_t bearing_deg) {
    bool has_height = height_dm != ADV_ANT_UNSET && height_dm <= ADV_ANT_HEIGHT_MASK;
    uint16_t height = has_height ? (uint16_t)(height_dm & ADV_ANT_HEIGHT_MASK) : 0;
    uint8_t kind_low = (kind <= ADV_ANT_KIND_MASK) ? (uint8_t)(kind & ADV_ANT_KIND_MASK) : 0;
    uint16_t feat1 = (uint16_t)((kind_low << ADV_ANT_KIND_SHIFT) | height);
    if (has_height) feat1 |= ADV_ANT_HEIGHT_FLAG;
    if (feat1) _extra1 = feat1;
    uint16_t feat2 = 0;
    if (bearing_deg != ADV_ANT_UNSET && bearing_deg <= 359) {
      feat2 = (uint16_t)(ADV_ANT_BEARING_FLAG | (bearing_deg & ADV_ANT_BEARING_MASK));
    }
    if (kind > ADV_ANT_KIND_MASK && kind <= ADV_ANT_KIND_EXT_MASK) {
      feat2 |= (uint16_t)((kind & ADV_ANT_KIND_EXT_MASK) << ADV_ANT_KIND_EXT_SHIFT);
    }
    if (feat2) _extra2 = feat2;
  }

  uint8_t AdvertDataBuilder::encodeTo(uint8_t app_data[]) {
    app_data[0] = _type;
    int i = 1;
    if (_has_loc) {
      app_data[0] |= ADV_LATLON_MASK;
      memcpy(&app_data[i], &_lat, 4); i += 4;
      memcpy(&app_data[i], &_lon, 4); i += 4;
    }
    if (_extra1) {
      app_data[0] |= ADV_FEAT1_MASK;
      memcpy(&app_data[i], &_extra1, 2); i += 2;
    }
    if (_extra2) {
      app_data[0] |= ADV_FEAT2_MASK;
      memcpy(&app_data[i], &_extra2, 2); i += 2;
    }
    if (_name && *_name != 0) {
      size_t name_len = mesh::validUtf8PrefixLength(_name, MAX_ADVERT_DATA_SIZE - i);
      if (name_len > 0) {
        app_data[0] |= ADV_NAME_MASK;
        memcpy(&app_data[i], _name, name_len);
        i += name_len;
      }
    }
    return i;
  }

  AdvertDataParser::AdvertDataParser(const uint8_t app_data[], uint8_t app_data_len) {
    _name[0] = 0;
    _lat = _lon = 0;
    _flags = app_data[0];
    _valid = false;
    _extra1 = _extra2 = 0;
  
    int i = 1;
    if (_flags & ADV_LATLON_MASK) {
      memcpy(&_lat, &app_data[i], 4); i += 4;
      memcpy(&_lon, &app_data[i], 4); i += 4;
    }
    if (_flags & ADV_FEAT1_MASK) {
      memcpy(&_extra1, &app_data[i], 2); i += 2;
    }
    if (_flags & ADV_FEAT2_MASK) {
      memcpy(&_extra2, &app_data[i], 2); i += 2;
    }

    if (app_data_len >= i) {
      int nlen = 0;
      if (_flags & ADV_NAME_MASK) {
        nlen = app_data_len - i;  // remainder of app_data
      }
      if (nlen > 0) {
        memcpy(_name, &app_data[i], nlen);
        _name[nlen] = 0;  // set null terminator
      }
      _valid = true;
    }
  }

#include <Arduino.h>

void AdvertTimeHelper::formatRelativeTimeDiff(char dest[], int32_t seconds_from_now, bool short_fmt) {
  const char *suffix;
  if (seconds_from_now < 0) {
    suffix = short_fmt ? "" : " ago";
    seconds_from_now = -seconds_from_now;
  } else {
    suffix = short_fmt ? "" : " from now";
  }

  if (seconds_from_now < 60) {
    sprintf(dest, "%d secs %s", seconds_from_now, suffix);
  } else {
    int32_t mins = seconds_from_now / 60;
    if (mins < 60) {
      sprintf(dest, "%d mins %s", mins, suffix);
    } else {
      int32_t hours = mins / 60;
      if (hours < 24) {
        sprintf(dest, "%d hours %s", hours, suffix);
      } else {
        sprintf(dest, "%d days %s", hours / 24, suffix);
      }
    }
  }
}
