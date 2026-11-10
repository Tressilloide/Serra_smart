#include "riserva.h"
#include "riserva_coda.h"
#include "watchdog.h"

#include <esp_attr.h>

/*
 * RTC_NOINIT_ATTR come g_seq: sopravvive anche a watchdog e panic, che sono
 * proprio i momenti in cui un pacchetto rischia di piu'. All'accensione ci
 * trova spazzatura, ed e' per questo che riservaInit() controlla la somma.
 */
static RTC_NOINIT_ATTR RiservaCoda g_riserva;

void riservaInit(bool memoriaPersa) {
  if (memoriaPersa || !rcValida(g_riserva)) {
    if (!memoriaPersa && g_riserva.magia == RISERVA_MAGIA)
      Serial.println(F("[RISERVA] Memoria RTC incoerente (reset a meta' scrittura?): azzerata."));
    rcAzzera(g_riserva);
    return;
  }
  if (g_riserva.record > 0)
    Serial.printf("[RISERVA] %u pacchetti in attesa in memoria RTC (%u/%u byte).\n",
                  g_riserva.record, g_riserva.usati, (unsigned)RISERVA_BYTE);
}

uint16_t riservaConta()     { return g_riserva.record; }
uint32_t riservaScartati()  { return g_riserva.scartati; }

bool riservaAccoda(const char* riga) {
  const uint32_t scartatiPrima = g_riserva.scartati;
  if (!rcAccoda(g_riserva, riga)) return false;

  if (g_riserva.scartati != scartatiPrima)
    Serial.printf("[RISERVA] Piena: sacrificati %lu pacchetti vecchi (%lu dall'accensione).\n",
                  (unsigned long)(g_riserva.scartati - scartatiPrima),
                  (unsigned long)g_riserva.scartati);
  return true;
}

uint16_t riservaDrena(FnInvioRecord invia, uint16_t maxInvii) {
  if (!invia) return 0;

  uint16_t consegnati = 0;
  const char* riga;
  while (consegnati < maxInvii && (riga = rcPrima(g_riserva)) != nullptr) {
    wdtNutri();
    if (!invia(riga)) break;
    rcTogliPrima(g_riserva);
    consegnati++;
    delay(150);                 // lo stesso respiro del backlog su SD
  }

  if (consegnati > 0)
    Serial.printf("[RISERVA] %u pacchetti consegnati, %u ancora in attesa.\n",
                  consegnati, g_riserva.record);
  return consegnati;
}

void riservaSvuota() {
  rcAzzera(g_riserva);
  Serial.println(F("[RISERVA] Svuotata su richiesta."));
}
