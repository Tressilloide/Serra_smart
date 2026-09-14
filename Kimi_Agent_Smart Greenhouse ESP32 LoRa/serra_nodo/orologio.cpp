#include "orologio.h"
#include "config.h"
#include "impostazioni.h"
#include <Wire.h>

static RTC_DS1307 rtc;
static bool g_rtcPresente   = false;
static bool g_rtcAttendibile = false;

// Anno minimo perche' una data sia considerata plausibile. Un DS1307 che ha
// perso l'alimentazione tampone riparte tipicamente dal 2000.
#define ANNO_MINIMO_PLAUSIBILE 2024

// ---------------------------------------------------------------------------

bool orologioInit() {
  g_rtcPresente = rtc.begin(&Wire);
  if (!g_rtcPresente) {
    Serial.println(F("[RTC] ERRORE: DS1307 non trovato sul bus I2C!"));
    g_rtcAttendibile = false;
    return false;
  }

  if (!rtc.isrunning()) {
    Serial.println(F("[RTC] L'orologio e' fermo: batteria CR2032 scarica o assente."));
    Serial.println(F("[RTC] Attendo la sincronizzazione dall'ora del ponte."));
    g_rtcAttendibile = false;
    return false;
  }

  DateTime d = rtc.now();
  g_rtcAttendibile = (d.year() >= ANNO_MINIMO_PLAUSIBILE);

  if (!g_rtcAttendibile) {
    Serial.printf("[RTC] Data implausibile (%04d): la considero non attendibile.\n", d.year());
    Serial.println(F("[RTC] Attendo la sincronizzazione dall'ora del ponte."));
  }
  return g_rtcAttendibile;
}

DateTime orologioAdesso() {
  if (g_rtcPresente) return rtc.now();
  // Nessun RTC: si restituisce una data segnaposto. Chi chiama deve comunque
  // controllare orologioAttendibile() prima di prendere decisioni sull'acqua.
  return DateTime((uint32_t)0);
}

bool orologioAttendibile() { return g_rtcAttendibile; }

// ---------------------------------------------------------------------------
//  Fuso orario
// ---------------------------------------------------------------------------

int32_t orologioFuso()     { return g_cfg.tzOffsetSec; }
bool    orologioFusoNoto() { return g_cfg.tzNoto != 0; }

DateTime orologioLocale() {
  // Il cast a int64 non e' pedanteria: unixtime() e' unsigned, e sommargli
  // uno scarto negativo senza allargare il tipo darebbe un numero enorme
  // invece di un'ora indietro. Con UTC-5 la serra si ritroverebbe nel 2106.
  int64_t locale = (int64_t)orologioAdesso().unixtime() + (int64_t)g_cfg.tzOffsetSec;
  if (locale < 0) locale = 0;
  return DateTime((uint32_t)locale);
}

void orologioImpostaFuso(int32_t offsetSec) {
  // I fusi reali stanno fra UTC-12 e UTC+14: fuori da li' e' un campo
  // corrotto, e applicarlo sposterebbe l'irrigazione di ore.
  if (offsetSec < -43200L || offsetSec > 50400L) {
    Serial.printf("[RTC] Scarto di fuso implausibile dal ponte (%ld s): ignorato.\n",
                  (long)offsetSec);
    return;
  }
  if (g_cfg.tzOffsetSec == offsetSec && g_cfg.tzNoto) return;

  Serial.printf("[RTC] Fuso dal ponte: UTC%+.1f h (era %s).\n",
                offsetSec / 3600.0f,
                g_cfg.tzNoto ? "un altro valore" : "sconosciuto");
  g_cfg.tzOffsetSec = offsetSec;
  g_cfg.tzNoto      = 1;
  impostazioniModificate();
}

bool orologioImposta(uint32_t epoch) {
  if (!g_rtcPresente) {
    Serial.println(F("[RTC] Impossibile impostare l'ora: DS1307 assente."));
    return false;
  }
  if (epoch < 1700000000UL) {          // ~novembre 2023: sotto e' certamente errata
    Serial.printf("[RTC] Timestamp %lu rifiutato: non plausibile.\n", (unsigned long)epoch);
    return false;
  }

  rtc.adjust(DateTime(epoch));
  g_rtcAttendibile = true;

  DateTime d = rtc.now();
  Serial.printf("[RTC] Orologio impostato a %04d-%02d-%02d %02d:%02d:%02d\n",
                d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second());
  return true;
}

bool orologioSincronizza(uint32_t epochPonte) {
  if (epochPonte < 1700000000UL) return false;   // il ponte non aveva ancora NTP
  if (!g_rtcPresente) return false;

  if (!g_rtcAttendibile) {
    Serial.println(F("[RTC] Ora non attendibile: la prendo dal ponte."));
    return orologioImposta(epochPonte);
  }

  uint32_t locale = rtc.now().unixtime();
  int32_t  deriva = (int32_t)epochPonte - (int32_t)locale;

  if (abs(deriva) <= RTC_DRIFT_MAX_SEC) return false;

  Serial.printf("[RTC] Deriva di %ld s rispetto al ponte: risincronizzo.\n", (long)deriva);
  return orologioImposta(epochPonte);
}

uint32_t orologioGiorno(const DateTime& d) {
  return (uint32_t)d.year() * 10000UL + (uint32_t)d.month() * 100UL + (uint32_t)d.day();
}

void orologioStampa(const DateTime& d) {
  Serial.printf("[RTC] %04d-%02d-%02d %02d:%02d:%02d UTC  (%s)\n",
                d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(),
                g_rtcAttendibile ? "attendibile" : "NON ATTENDIBILE");

  // L'ora civile stampata accanto a quella UTC: e' quella che va confrontata
  // con l'orario impostato in Home Assistant, ed e' l'unico modo di accorgersi
  // a colpo d'occhio che il fuso non e' ancora arrivato dal ponte.
  DateTime l = orologioLocale();
  Serial.printf("[RTC] %04d-%02d-%02d %02d:%02d:%02d locale (UTC%+.1f h)\n",
                l.year(), l.month(), l.day(), l.hour(), l.minute(), l.second(),
                g_cfg.tzOffsetSec / 3600.0f);
}
