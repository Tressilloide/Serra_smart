/*
 * Arduino.h finto per compilare sul PC camera_ponte/radio.cpp e la libreria
 * LoRa contro il modulo simulato di sx1278_sim.h. Solo quello che usano.
 */
#pragma once

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sx1278_sim.h"

typedef uint8_t byte;
typedef bool    boolean;

#define B111  0x07
#define B1000 0x08

#define OUTPUT   1
#define INPUT    0
#define HIGH     1
#define LOW      0
#define RISING   1
#define MSBFIRST 1
#define SPI_MODE0 0
#define HEX      16
#define F(x)     (x)
#define ICACHE_RAM_ATTR
#define bitWrite(v, bit, b) ((b) ? ((v) |= (1UL << (bit))) : ((v) &= ~(1UL << (bit))))
#define digitalPinToInterrupt(p) (p)

inline void     pinMode(int, int) {}
inline void     digitalWrite(int pin, int livello) { sim.piedino(pin, livello); }
inline uint32_t millis() { return simOraMs; }
inline void     delay(uint32_t ms) { simOraMs += ms; }
inline void     yield() {}
inline void     attachInterrupt(int, void (*)(), int) {}
inline void     detachInterrupt(int) {}

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    while (size--) n += write(*buffer++);
    return n;
  }
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
  size_t print(int v, int base = 10) {
    char b[16];
    snprintf(b, sizeof(b), base == HEX ? "%X" : "%d", v);
    return print(b);
  }
  size_t println(const char* s = "") { return print(s) + print("\n"); }
  size_t println(int v, int base = 10) { return print(v, base) + print("\n"); }
};

class Stream : public Print {
public:
  virtual int  available() = 0;
  virtual int  read() = 0;
  virtual int  peek() = 0;
  virtual void flush() {}
  void setTimeout(unsigned long) {}
};

// La seriale: sta zitta, a meno che il test non chieda di vederla.
extern bool simSerialeVisibile;
struct SerialeFinta {
  void printf(const char* fmt, ...) {
    if (!simSerialeVisibile) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
  }
  void println(const char* s = "") { if (simSerialeVisibile) ::printf("%s\n", s); }
  void print(const char* s) { if (simSerialeVisibile) ::printf("%s", s); }
};
extern SerialeFinta Serial;
