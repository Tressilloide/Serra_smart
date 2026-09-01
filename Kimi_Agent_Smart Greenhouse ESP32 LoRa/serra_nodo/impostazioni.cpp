#include "impostazioni.h"
#include <Preferences.h>

Impostazioni g_cfg;

static Preferences prefs;
static bool g_sporco = false;    // true = ci sono modifiche non ancora salvate

#define NVS_NAMESPACE "serra"

void impostazioniModificate() { g_sporco = true; }

// ---------------------------------------------------------------------------

static void applicaDefault() {
  g_cfg.irrigOra        = IRRIG_ORA_DEF;
  g_cfg.irrigMinuto     = IRRIG_MIN_DEF;
  g_cfg.irrigDurataSec  = IRRIG_SEC_DEF;
  g_cfg.irrigAuto       = IRRIG_AUTO_DEF;
  g_cfg.soilSoglia      = SOIL_SOGLIA_DEF;

  g_cfg.giornoCorrente   = 0;
  g_cfg.irrigazioniOggi  = 0;
  g_cfg.litriOggi        = 0.0f;
  g_cfg.ultimaIrrigEpoch = 0;

  g_cfg.litriTotali     = 0.0f;
  g_cfg.backlogScartati = 0;

  g_cfg.voltDivider    = VOLT_DIVIDER_DEF;
  g_cfg.flussoImpLitro = FLUSSO_IMP_LITRO_DEF;
  for (uint8_t i = 0; i < 4; i++) {
    g_cfg.soilSecco[i]   = SOIL_RAW_SECCO_DEF;
    g_cfg.soilBagnato[i] = SOIL_RAW_BAGNATO_DEF;
  }

  g_cfg.sleepSec         = SLEEP_TIME_SEC;
  g_cfg.sensoriAbilitati = 0xFFFFFFFF;   // tutti attivi salvo diverso ordine
}

void impostazioniCarica() {
  applicaDefault();

  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    // Primo avvio in assoluto: il namespace non esiste ancora. Lo creiamo
    // scrivendo i default, cosi' i risvegli successivi trovano tutto pronto.
    Serial.println(F("[CFG] NVS vuota: scrivo i valori di default."));
    g_sporco = true;
    impostazioniSalva();
    return;
  }

  g_cfg.irrigOra       = prefs.getUChar ("irrOra",   g_cfg.irrigOra);
  g_cfg.irrigMinuto    = prefs.getUChar ("irrMin",   g_cfg.irrigMinuto);
  g_cfg.irrigDurataSec = prefs.getUShort("irrDur",   g_cfg.irrigDurataSec);
  g_cfg.irrigAuto      = prefs.getBool  ("irrAuto",  g_cfg.irrigAuto);
  g_cfg.soilSoglia     = prefs.getShort ("soilSg",   g_cfg.soilSoglia);

  g_cfg.giornoCorrente   = prefs.getULong("giorno",  g_cfg.giornoCorrente);
  g_cfg.irrigazioniOggi  = prefs.getUChar("irrOggi", g_cfg.irrigazioniOggi);
  g_cfg.litriOggi        = prefs.getFloat("litOggi", g_cfg.litriOggi);
  g_cfg.ultimaIrrigEpoch = prefs.getULong("ultIrr",  g_cfg.ultimaIrrigEpoch);

  g_cfg.litriTotali     = prefs.getFloat("litTot",   g_cfg.litriTotali);
  g_cfg.backlogScartati = prefs.getULong("blScart",  g_cfg.backlogScartati);

  g_cfg.voltDivider    = prefs.getFloat("voltDiv",   g_cfg.voltDivider);
  g_cfg.flussoImpLitro = prefs.getFloat("flImpL",    g_cfg.flussoImpLitro);

  char chiave[12];
  for (uint8_t i = 0; i < 4; i++) {
    snprintf(chiave, sizeof(chiave), "soilS%u", i);
    g_cfg.soilSecco[i]   = prefs.getUShort(chiave, g_cfg.soilSecco[i]);
    snprintf(chiave, sizeof(chiave), "soilB%u", i);
    g_cfg.soilBagnato[i] = prefs.getUShort(chiave, g_cfg.soilBagnato[i]);
  }

  g_cfg.sleepSec         = prefs.getULong("sleepS",  g_cfg.sleepSec);
  g_cfg.sensoriAbilitati = prefs.getULong("sensEn",  g_cfg.sensoriAbilitati);

  prefs.end();
  g_sporco = false;
}

void impostazioniSalva() {
  if (!g_sporco) return;

  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    Serial.println(F("[CFG] ERRORE: impossibile aprire la NVS in scrittura!"));
    return;
  }

  prefs.putUChar ("irrOra",  g_cfg.irrigOra);
  prefs.putUChar ("irrMin",  g_cfg.irrigMinuto);
  prefs.putUShort("irrDur",  g_cfg.irrigDurataSec);
  prefs.putBool  ("irrAuto", g_cfg.irrigAuto);
  prefs.putShort ("soilSg",  g_cfg.soilSoglia);

  prefs.putULong("giorno",  g_cfg.giornoCorrente);
  prefs.putUChar("irrOggi", g_cfg.irrigazioniOggi);
  prefs.putFloat("litOggi", g_cfg.litriOggi);
  prefs.putULong("ultIrr",  g_cfg.ultimaIrrigEpoch);

  prefs.putFloat("litTot",  g_cfg.litriTotali);
  prefs.putULong("blScart", g_cfg.backlogScartati);

  prefs.putFloat("voltDiv", g_cfg.voltDivider);
  prefs.putFloat("flImpL",  g_cfg.flussoImpLitro);

  char chiave[12];
  for (uint8_t i = 0; i < 4; i++) {
    snprintf(chiave, sizeof(chiave), "soilS%u", i);
    prefs.putUShort(chiave, g_cfg.soilSecco[i]);
    snprintf(chiave, sizeof(chiave), "soilB%u", i);
    prefs.putUShort(chiave, g_cfg.soilBagnato[i]);
  }

  prefs.putULong("sleepS", g_cfg.sleepSec);
  prefs.putULong("sensEn", g_cfg.sensoriAbilitati);

  prefs.end();
  g_sporco = false;
}

void impostazioniReset() {
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
  applicaDefault();
  g_sporco = true;
  impostazioniSalva();
  Serial.println(F("[CFG] Impostazioni riportate ai valori di default."));
}

bool impostazioniNuovoGiorno(uint32_t aaaammgg) {
  if (aaaammgg == 0 || aaaammgg == g_cfg.giornoCorrente) return false;

  Serial.printf("[CFG] Nuovo giorno %lu (era %lu): azzero i contatori giornalieri.\n",
                (unsigned long)aaaammgg, (unsigned long)g_cfg.giornoCorrente);

  g_cfg.giornoCorrente  = aaaammgg;
  g_cfg.irrigazioniOggi = 0;
  g_cfg.litriOggi       = 0.0f;
  // ultimaIrrigEpoch NON si azzera: l'intervallo minimo tra due irrigazioni
  // deve valere anche a cavallo della mezzanotte.
  g_sporco = true;
  return true;
}

void impostazioniStampa() {
  Serial.println(F("[CFG] --- Impostazioni correnti ---"));
  Serial.printf("[CFG] Irrigazione: %02u:%02u per %u s, auto=%s, sogliaTerreno=%d%%\n",
                g_cfg.irrigOra, g_cfg.irrigMinuto, g_cfg.irrigDurataSec,
                g_cfg.irrigAuto ? "si" : "no", g_cfg.soilSoglia);
  Serial.printf("[CFG] Oggi (%lu): %u irrigazioni, %.2f L. Totale storico: %.1f L\n",
                (unsigned long)g_cfg.giornoCorrente, g_cfg.irrigazioniOggi,
                g_cfg.litriOggi, g_cfg.litriTotali);
  Serial.printf("[CFG] Sleep: %lu s | voltDivider=%.3f | impulsi/L=%.1f\n",
                (unsigned long)g_cfg.sleepSec, g_cfg.voltDivider, g_cfg.flussoImpLitro);
}
