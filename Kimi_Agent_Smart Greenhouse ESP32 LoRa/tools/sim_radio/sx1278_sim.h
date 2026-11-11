/*
 * Un SX1278 finto, quanto basta a far girare sul PC camera_ponte/radio.cpp e
 * la libreria LoRa vera: registri, FIFO, modi, piedino di reset, e i guasti
 * che il ponte ha incontrato davvero. Usato da tools/test_radio_ponte.cpp.
 *
 * Non e' un modello del chip: e' un modello di quello che il firmware ne
 * vede dal bus SPI. In particolare il comportamento dei guasti (un modulo
 * che non esce da sleep, un oscillatore lento) e' quello osservato o
 * ipotizzato, non quello di un datasheet.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <map>

extern uint32_t simOraMs;   // l'orologio finto: lo fanno avanzare delay() e il test

struct Sx1278Sim {
  enum Guasto {
    SANO,
    FERMO_IN_SLEEP,      // tiene la configurazione ma il modo resta sleep (11/11/2026)
    OSCILLATORE_LENTO,   // esce da sleep solo dopo oscillatoreMs, e intanto perde le richieste
    MUTO,                // non risponde: ogni lettura da' 0x00 (modulo senza corrente)
  };

  Guasto   guasto         = SANO;
  bool     resetCollegato = true;
  uint32_t oscillatoreMs  = 20;
  int      pinNss         = -1;
  int      pinRst         = -1;

  // Registri che leggono sempre lo stesso valore, qualunque cosa ci si scriva
  std::map<uint8_t, uint8_t> testardi;

  uint8_t  reg[0x80];
  uint8_t  fifo[256];
  unsigned scritture = 0;

  Sx1278Sim() { accendi(); }

  // Valori di fabbrica dopo l'accensione o il reset: FSK, standby.
  void accendi() {
    memset(reg, 0, sizeof(reg));
    memset(fifo, 0, sizeof(fifo));
    reg[0x01] = 0x09;
    reg[0x06] = 0x6C; reg[0x07] = 0x80; reg[0x08] = 0x00;
    reg[0x09] = 0x4F; reg[0x0A] = 0x09; reg[0x0B] = 0x2B; reg[0x0C] = 0x20;
    reg[0x0E] = 0x80; reg[0x0F] = 0x00;
    reg[0x1D] = 0x72; reg[0x1E] = 0x70; reg[0x1F] = 0x64;
    reg[0x20] = 0x00; reg[0x21] = 0x08; reg[0x22] = 0x01; reg[0x23] = 0xFF;
    reg[0x26] = 0x00;
    reg[0x31] = 0xC3; reg[0x33] = 0x27; reg[0x37] = 0x0A; reg[0x39] = 0x12; reg[0x3B] = 0x1D;
    reg[0x42] = 0x12; reg[0x4D] = 0x84;
    oscillatoreAcceso_ = true;      // in standby l'oscillatore gira
    oscillatorePronto_ = 0;
    inAttesa_ = false;
    inTx_ = false;
  }

  uint8_t modo() const { return reg[0x01]; }

  // Il chip cade in sleep da solo, come per un disturbo: la configurazione resta.
  void cadeInSleep() { reg[0x01] = (reg[0x01] & 0xF8); oscillatoreAcceso_ = false; inAttesa_ = false; }

  // Un pacchetto in arrivo: entra solo se la radio sta davvero ascoltando.
  bool inietta(const char* testo, int snrDb = 7, int rssiDbm = -100) {
    aggiorna();
    if (guasto == MUTO || (reg[0x01] & 0x87) != 0x85) return false;
    const size_t n = strlen(testo);
    memcpy(fifo, testo, n);
    reg[0x10] = 0x00;
    reg[0x13] = (uint8_t)n;
    reg[0x19] = (uint8_t)(snrDb * 4);
    reg[0x1A] = (uint8_t)(rssiDbm + 164);
    reg[0x12] |= 0x50;                // RxDone + ValidHeader
    return true;
  }

  // ---- piedini ------------------------------------------------------------
  void piedino(int pin, int livello) {
    if (pin == pinNss) { nss_ = livello != 0; nByte_ = 0; }
    if (pin == pinRst && livello == 0 && resetCollegato) accendi();
  }

  // ---- bus SPI ------------------------------------------------------------
  uint8_t trasferisci(uint8_t dato) {
    if (nss_) return 0x00;                       // non selezionato: MISO a riposo
    if (nByte_++ == 0) { indirizzo_ = dato; return 0x00; }
    const uint8_t r = indirizzo_ & 0x7F;
    if (indirizzo_ & 0x80) { scrivi(r, dato); return 0x00; }
    return leggi(r);
  }

private:
  bool     nss_ = true;
  int      nByte_ = 0;
  uint8_t  indirizzo_ = 0;
  bool     oscillatoreAcceso_ = true;
  uint32_t oscillatorePronto_ = 0;
  bool     inAttesa_ = false;       // uno standby chiesto mentre l'oscillatore partiva
  bool     inTx_ = false;
  uint32_t txFineMs_ = 0;

  void aggiorna() {
    if (inAttesa_ && simOraMs >= oscillatorePronto_) {
      reg[0x01] = (reg[0x01] & 0xF8) | 0x01;
      inAttesa_ = false;
    }
    if (inTx_ && simOraMs >= txFineMs_) {
      inTx_ = false;
      reg[0x12] |= 0x08;                          // TxDone
      reg[0x01] = (reg[0x01] & 0xF8) | 0x01;      // e torna in standby da sola
    }
  }

  uint8_t leggi(uint8_t r) {
    if (guasto == MUTO) return 0x00;
    aggiorna();
    auto t = testardi.find(r);
    if (t != testardi.end()) return t->second;
    if (r == 0x00) return fifo[reg[0x0D]++];
    return reg[r];
  }

  void scrivi(uint8_t r, uint8_t v) {
    if (guasto == MUTO) return;
    aggiorna();
    scritture++;
    switch (r) {
      case 0x00: fifo[reg[0x0D]++] = v; return;
      case 0x01: scriviModo(v); return;
      case 0x12: reg[0x12] &= (uint8_t)~v; return;   // i flag si azzerano scrivendoci 1
      case 0x42: return;                              // sola lettura
      default:   reg[r] = v; return;
    }
  }

  void scriviModo(uint8_t v) {
    const uint8_t attuale = reg[0x01];
    uint8_t alti = attuale & 0xF8;
    // LongRangeMode (bit 7) si puo' cambiare solo da sleep
    if ((attuale & 0x07) == 0x00) alti = v & 0xF8;
    else                          alti = (attuale & 0x80) | (v & 0x78);
    uint8_t voluto = v & 0x07;

    if (guasto == FERMO_IN_SLEEP) {
      voluto = 0x00;
    } else if (guasto == OSCILLATORE_LENTO) {
      if (voluto == 0x00) {
        oscillatoreAcceso_ = false;
        inAttesa_ = false;
      } else if (!oscillatoreAcceso_) {
        // Da sleep: l'oscillatore parte adesso. Lo standby arrivera' quando
        // e' pronto, qualunque altra richiesta va persa.
        oscillatoreAcceso_ = true;
        oscillatorePronto_ = simOraMs + oscillatoreMs;
        inAttesa_ = (voluto == 0x01);
        voluto = 0x00;
      } else if (simOraMs < oscillatorePronto_) {
        voluto = attuale & 0x07;                  // non ancora pronto: richiesta persa
      }
    } else {
      oscillatoreAcceso_ = (voluto != 0x00);
    }

    reg[0x01] = alti | voluto;
    if (voluto == 0x03) { inTx_ = true; txFineMs_ = simOraMs + 40; }
  }
};

extern Sx1278Sim sim;
