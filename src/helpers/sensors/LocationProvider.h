#pragma once

#include "Mesh.h"


class LocationProvider {
protected:
    bool _time_sync_needed = true;
    bool _clock_set = false;

    void noteClockSet() { _clock_set = true; }

public:
    virtual void syncTime() { _time_sync_needed = true; }
    bool consumeClockSet() {
      bool v = _clock_set;
      _clock_set = false;
      return v;
    }
    virtual bool waitingTimeSync() { return _time_sync_needed; }
    virtual long getLatitude() = 0;
    virtual long getLongitude() = 0;
    virtual long getAltitude() = 0;
    virtual long satellitesCount() = 0;
    virtual bool isValid() = 0;
    virtual long getTimestamp() = 0;
    virtual void sendSentence(const char * sentence);
    virtual void reset() = 0;
    virtual void begin() = 0;
    virtual void stop() = 0;
    virtual void loop() = 0;
    virtual bool isEnabled() = 0;
};
