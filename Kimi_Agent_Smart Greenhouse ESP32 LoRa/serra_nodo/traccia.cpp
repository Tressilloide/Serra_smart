#include "traccia.h"

/*
 * Numero magico scelto a caso ma riconoscibile in un dump esadecimale.
 * Se cambia il significato delle tappe conviene cambiarlo anche qui: cosi' il
 * primo avvio dopo un aggiornamento scarta il valore vecchio invece di
 * tradurlo con la tabella nuova e riferire una tappa sbagliata.
 */
static const uint32_t TRACCIA_MAGIA = 0x5E44A001UL;

static RTC_NOINIT_ATTR uint32_t g_magia;
static RTC_NOINIT_ATTR uint8_t  g_tappa;

static Tappa g_precedente = TAPPA_IGNOTA;
static bool  g_persa      = false;

void tracciaInit() {
  if (g_magia != TRACCIA_MAGIA) {
    // Memoria RTC non attendibile: e' la prima accensione, oppure e' mancata
    // la corrente. Qualunque valore ci sia dentro e' spazzatura.
    g_magia      = TRACCIA_MAGIA;
    g_precedente = TAPPA_IGNOTA;
    g_persa      = true;
  } else {
    g_precedente = (g_tappa <= (uint8_t)TAPPA_SLEEP) ? (Tappa)g_tappa : TAPPA_IGNOTA;
  }
  g_tappa = (uint8_t)TAPPA_AVVIO;
}

void traccia(Tappa t) {
  g_tappa = (uint8_t)t;
}

Tappa tracciaPrecedente() {
  return g_precedente;
}

bool tracciaMemoriaPersa() {
  return g_persa;
}

const char* tracciaTesto(Tappa t) {
  switch (t) {
    case TAPPA_AVVIO:        return "avvio";
    case TAPPA_IMPOSTAZIONI: return "impostazioni";
    case TAPPA_OROLOGIO:     return "orologio";
    case TAPPA_SENSORI:      return "sensori";
    case TAPPA_SD:           return "sd";
    case TAPPA_RECUPERO:     return "recupero";
    case TAPPA_RADIO:        return "radio";
    case TAPPA_SOIL:         return "soil";
    case TAPPA_IRRIGAZIONE:  return "irrigazione";
    case TAPPA_LETTURE:      return "letture";
    case TAPPA_TX_STATO:     return "tx_stato";
    case TAPPA_COMANDO:      return "comando";
    case TAPPA_TX_ESITO:     return "tx_esito";
    case TAPPA_SD_ACCODA:    return "sd_accoda";
    case TAPPA_BACKLOG:      return "backlog";
    case TAPPA_MANUTENZIONE: return "manutenzione";
    case TAPPA_SLEEP:        return "sleep";
    default:                 return "ignota";
  }
}
