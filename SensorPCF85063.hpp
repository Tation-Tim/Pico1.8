#ifndef SENSOR_PCF85063_HPP
#define SENSOR_PCF85063_HPP

#include <Wire.h>

#define PCF85063_SLAVE_ADDRESS 0x51

// Simple date/time container
class RTC_DateTime {
public:
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

// Minimal PCF85063 driver
class SensorPCF85063 {
public:
  bool begin(TwoWire &wire, uint8_t addr = PCF85063_SLAVE_ADDRESS, int sda = -1, int scl = -1) {
    _wire = &wire;
    _addr = addr;
    _wire->begin();
    return probe();
  }

  bool probe() {
    _wire->beginTransmission(_addr);
    return (_wire->endTransmission() == 0);
  }

  RTC_DateTime getDateTime() {
    RTC_DateTime t;
    _wire->beginTransmission(_addr);
    _wire->write(0x04); // seconds register
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)7);
    uint8_t sec = _wire->read() & 0x7F;
    uint8_t min = _wire->read() & 0x7F;
    uint8_t hour = _wire->read() & 0x3F;
    uint8_t day = _wire->read() & 0x3F;
    uint8_t weekday = _wire->read() & 0x07;
    uint8_t month = _wire->read() & 0x1F;
    uint8_t year = _wire->read();

    t.second = bcd2dec(sec);
    t.minute = bcd2dec(min);
    t.hour   = bcd2dec(hour);
    t.day    = bcd2dec(day);
    t.month  = bcd2dec(month);
    t.year   = 2000 + bcd2dec(year);
    return t;
  }

  void setDateTime(uint16_t y, uint8_t m, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    _wire->beginTransmission(_addr);
    _wire->write(0x04); // start at seconds register
    _wire->write(dec2bcd(s));
    _wire->write(dec2bcd(mi));
    _wire->write(dec2bcd(h));
    _wire->write(dec2bcd(d));
    _wire->write(0x00); // weekday not set
    _wire->write(dec2bcd(m));
    _wire->write(dec2bcd(y - 2000));
    _wire->endTransmission();
  }

private:
  TwoWire *_wire;
  uint8_t _addr;

  uint8_t bcd2dec(uint8_t val) { return ((val / 16 * 10) + (val % 16)); }
  uint8_t dec2bcd(uint8_t val) { return ((val / 10 * 16) + (val % 10)); }
};

#endif
