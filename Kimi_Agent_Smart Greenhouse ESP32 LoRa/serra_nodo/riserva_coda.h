/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : LA CODA DI RISERVA, SENZA ARDUINO
 * ============================================================================
 *
 *  La logica pura della riserva in memoria RTC (vedi riserva.h): solo byte e
 *  contatori, nessuna dipendenza dall'ESP32. Sta in un file a parte perche'
 *  tools/test_riserva.cpp la possa compilare e mettere alla prova sul PC con
 *  g++, compresi i casi che sul nodo non si riescono a provocare a comando:
 *  coda piena, memoria sporca dopo un reset a meta' scrittura, riga piu'
 *  lunga dell'intera riserva.
 *
 *  Il formato e' il piu' semplice possibile: le righe una dopo l'altra, ognuna
 *  chiusa dal suo terminatore, dalla piu' vecchia alla piu' recente.
 *
 *    dati: "GH1;bk=1;v=2;s=10;...\0GH1;bk=1;v=2;s=11;...\0"
 *           ^ la prossima da consegnare        usati ^
 *
 *  Consegnare la prima riga vuol dire farla scorrere via con una memmove di
 *  qualche kilobyte: sul nodo costa microsecondi, ed evita tutti gli
 *  indici circolari che una coda ad anello richiederebbe.
 * ============================================================================
 */

#pragma once

#include <stdint.h>
#include <string.h>

#ifndef RISERVA_BYTE
  #define RISERVA_BYTE 6144
#endif

#define RISERVA_MAGIA 0x52534B31UL   // "RSK1"

struct RiservaCoda {
  uint32_t magia;
  uint16_t usati;      // byte occupati in dati[], terminatori compresi
  uint16_t record;     // righe in coda
  uint32_t scartati;   // righe buttate per far posto, dall'ultimo azzeramento
  uint32_t somma;      // controllo d'integrita' di tutto il resto
  char     dati[RISERVA_BYTE];
};

// FNV-1a a 32 bit: non serve resistere a nessuno, solo accorgersi di una
// memoria lasciata a meta' da un reset o riempita di spazzatura all'accensione.
inline uint32_t rcFnv(uint32_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619UL; }
  return h;
}

inline uint32_t rcSomma(const RiservaCoda& c) {
  uint32_t h = 2166136261UL;
  h = rcFnv(h, &c.magia,    sizeof(c.magia));
  h = rcFnv(h, &c.usati,    sizeof(c.usati));
  h = rcFnv(h, &c.record,   sizeof(c.record));
  h = rcFnv(h, &c.scartati, sizeof(c.scartati));
  if (c.usati <= RISERVA_BYTE) h = rcFnv(h, c.dati, c.usati);
  return h;
}

inline void rcSigilla(RiservaCoda& c) { c.somma = rcSomma(c); }

inline void rcAzzera(RiservaCoda& c) {
  c.magia    = RISERVA_MAGIA;
  c.usati    = 0;
  c.record   = 0;
  c.scartati = 0;
  rcSigilla(c);
}

/*
 * La memoria e' coerente? Oltre alla somma si ricontano i terminatori: una
 * somma giusta per caso su dati sbagliati e' improbabile, ma il controllo
 * costa poco e protegge rcPrima() dal leggere oltre la fine.
 */
inline bool rcValida(const RiservaCoda& c) {
  if (c.magia != RISERVA_MAGIA || c.usati > RISERVA_BYTE) return false;
  if (c.somma != rcSomma(c)) return false;
  if (c.usati > 0 && c.dati[c.usati - 1] != '\0') return false;
  uint16_t terminatori = 0;
  for (uint16_t i = 0; i < c.usati; i++) if (c.dati[i] == '\0') terminatori++;
  return terminatori == c.record;
}

// La riga piu' vecchia, oppure nullptr se la coda e' vuota.
inline const char* rcPrima(const RiservaCoda& c) {
  return c.record > 0 ? c.dati : nullptr;
}

// Toglie la riga piu' vecchia (consegnata, o sacrificata per far posto).
inline void rcTogliPrima(RiservaCoda& c) {
  if (c.record == 0) return;
  const size_t lung = strlen(c.dati) + 1;
  memmove(c.dati, c.dati + lung, c.usati - lung);
  c.usati  = (uint16_t)(c.usati - lung);
  c.record = (uint16_t)(c.record - 1);
  rcSigilla(c);
}

/*
 * Accoda una riga. Se non c'e' posto si sacrificano le piu' VECCHIE, come fa
 * il backlog su microSD: in un'interruzione lunga contano di piu' le ultime
 * ore, che dicono com'e' la serra adesso.
 * false solo per una riga vuota o piu' lunga dell'intera riserva.
 */
inline bool rcAccoda(RiservaCoda& c, const char* riga) {
  if (!riga || !*riga) return false;
  const size_t lung = strlen(riga) + 1;
  if (lung > RISERVA_BYTE) return false;

  while (c.usati + lung > RISERVA_BYTE && c.record > 0) {
    rcTogliPrima(c);
    c.scartati++;
  }
  memcpy(c.dati + c.usati, riga, lung);
  c.usati  = (uint16_t)(c.usati + lung);
  c.record = (uint16_t)(c.record + 1);
  rcSigilla(c);
  return true;
}
