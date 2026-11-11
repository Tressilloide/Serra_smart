// SPI.h finto: ogni byte passa dal modulo simulato.
#pragma once

#include "Arduino.h"

struct SPISettings {
  SPISettings() {}
  SPISettings(uint32_t, int, int) {}
};

class SPIClass {
public:
  void    begin(int = -1, int = -1, int = -1, int = -1) {}
  void    end() {}
  void    beginTransaction(const SPISettings&) {}
  void    endTransaction() {}
  uint8_t transfer(uint8_t dato) { return sim.trasferisci(dato); }
  void    usingInterrupt(int) {}
  void    notUsingInterrupt(int) {}
};

extern SPIClass SPI;
