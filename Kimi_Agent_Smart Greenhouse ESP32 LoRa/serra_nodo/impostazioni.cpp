#include "impostazioni.h"
#include "irrigazione.h"   // per EsitoIrrigazione, che finisce in esitoUltima
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

  g_cfg.giornoCorrente    = 0;
  g_cfg.irrigazioniOggi   = 0;
  g_cfg.litriOggi         = 0.0f;
  g_cfg.ultimaIrrigEpoch  = 0;
  g_cfg.giornoProgrammata = 0;

  g_cfg.litriTotali     = 0.0f;
  g_cfg.litriUltima     = 0.0f;
  g_cfg.backlogScartati = 0;
  g_cfg.esitoUltima     = IRR_NO_ORARIO;
  g_cfg.irrigInCorso    = 0;

  g_cfg.voltDivider    = VOLT_DIVIDER_DEF;
  g_cfg.flussoImpLitro = FLUSSO_IMP_LITRO_DEF;
  for (uint8_t i = 0; i < 4; i++) {
    g_cfg.soilSecco[i]   = SOIL_RAW_SECCO_DEF;
    g_cfg.soilBagnato[i] = SOIL_RAW_BAGNATO_DEF;
  }

  g_cfg.sleepSec         = SLEEP_TIME_SEC;
  g_cfg.sensoriAbilitati = 0xFFFFFFFF;   // tutti attivi salvo diverso ordine

  // Zero = UTC, ed e' il valore giusto per un nodo che non ha ancora sentito
  // il ponte: fino ad allora l'orario programmato non e' interpretabile. Dura
  // un solo risveglio, perche' l'ACK che porta l'ora porta anche il fuso.
  g_cfg.tzOffsetSec      = 0;
  g_cfg.tzNoto           = 0;
}

/*
 * Riporta entro limiti sensati tutto cio' che arriva dalla NVS.
 *
 * I valori in NVS sopravvivono ai cambi di firmware, e non c'e' nessuna
 * garanzia che siano ancora sensati: una versione precedente puo' aver
 * scritto una chiave con un altro significato, la partizione puo' essersi
 * corrotta, oppure un default puo' essere cambiato. Senza questo controllo
 * i guasti sarebbero silenziosi e sconcertanti:
 *
 *   sleepSec a 0        -> risveglio immediato, il nodo si riavvia all'infinito
 *                          e in poche ore scarica la batteria
 *   irrigOra a 47       -> la finestra oraria non coincide MAI e la serra non
 *                          viene piu' irrigata, senza un solo messaggio d'errore
 *   flussoImpLitro a 0  -> litri sempre a zero, quindi falsi allarmi
 *                          "nessun flusso" a ogni irrigazione
 *
 * Ogni correzione viene stampata: se compare, c'e' qualcosa da capire.
 */
static void validaImpostazioni() {
  struct { const char* nome; bool fuori; } corretti[] = {
    { "irrigOra",       g_cfg.irrigOra > 23 },
    { "irrigMinuto",    g_cfg.irrigMinuto > 59 },
    { "irrigDurataSec", g_cfg.irrigDurataSec == 0 || g_cfg.irrigDurataSec > IRRIG_MAX_SEC },
    { "soilSoglia",     g_cfg.soilSoglia < -1 || g_cfg.soilSoglia > 100 },
    { "sleepSec",       g_cfg.sleepSec < SLEEP_MIN_SEC || g_cfg.sleepSec > 86400UL },
    { "voltDivider",    !(g_cfg.voltDivider > 0.1f && g_cfg.voltDivider < 100.0f) },
    { "flussoImpLitro", !(g_cfg.flussoImpLitro > 1.0f && g_cfg.flussoImpLitro < 10000.0f) },
  };

  for (uint8_t i = 0; i < sizeof(corretti) / sizeof(corretti[0]); i++)
    if (corretti[i].fuori)
      Serial.printf("[CFG] ATTENZIONE: %s fuori range nella NVS, riporto al default.\n",
                    corretti[i].nome);

  if (g_cfg.irrigOra > 23)       g_cfg.irrigOra       = IRRIG_ORA_DEF;
  if (g_cfg.irrigMinuto > 59)    g_cfg.irrigMinuto    = IRRIG_MIN_DEF;
  if (g_cfg.irrigDurataSec == 0 || g_cfg.irrigDurataSec > IRRIG_MAX_SEC)
                                 g_cfg.irrigDurataSec = IRRIG_SEC_DEF;
  if (g_cfg.soilSoglia < -1 || g_cfg.soilSoglia > 100)
                                 g_cfg.soilSoglia     = SOIL_SOGLIA_DEF;
  if (g_cfg.sleepSec < SLEEP_MIN_SEC || g_cfg.sleepSec > 86400UL)
                                 g_cfg.sleepSec       = SLEEP_TIME_SEC;
  if (!(g_cfg.voltDivider > 0.1f && g_cfg.voltDivider < 100.0f))
                                 g_cfg.voltDivider    = VOLT_DIVIDER_DEF;
  if (!(g_cfg.flussoImpLitro > 1.0f && g_cfg.flussoImpLitro < 10000.0f))
                                 g_cfg.flussoImpLitro = FLUSSO_IMP_LITRO_DEF;

  // Contatori: un valore assurdo bloccherebbe l'irrigazione per sempre
  if (g_cfg.irrigazioniOggi > IRRIG_MAX_AL_GIORNO) g_cfg.irrigazioniOggi = IRRIG_MAX_AL_GIORNO;

  // Uno scarto di fuso assurdo sposterebbe l'orario programmato di ore senza
  // dire niente a nessuno. I fusi reali stanno fra UTC-12 e UTC+14.
  if (g_cfg.tzOffsetSec < -43200L || g_cfg.tzOffsetSec > 50400L) {
    Serial.printf("[CFG] ATTENZIONE: tzOffsetSec (%ld s) fuori range, torno a UTC.\n",
                  (long)g_cfg.tzOffsetSec);
    g_cfg.tzOffsetSec = 0;
  }
  if (g_cfg.esitoUltima > IRR_INTERROTTA) g_cfg.esitoUltima  = IRR_NO_ORARIO;
  if (g_cfg.irrigInCorso > 1)             g_cfg.irrigInCorso = 1;
  if (g_cfg.tzNoto > 1)                   g_cfg.tzNoto       = 1;
  if (isnan(g_cfg.litriOggi)   || g_cfg.litriOggi   < 0.0f) g_cfg.litriOggi   = 0.0f;
  if (isnan(g_cfg.litriTotali) || g_cfg.litriTotali < 0.0f) g_cfg.litriTotali = 0.0f;
  if (isnan(g_cfg.litriUltima) || g_cfg.litriUltima < 0.0f) g_cfg.litriUltima = 0.0f;

  for (uint8_t i = 0; i < 4; i++)
    if (g_cfg.soilSecco[i] == g_cfg.soilBagnato[i]) {   // taratura impossibile
      g_cfg.soilSecco[i]   = SOIL_RAW_SECCO_DEF;
      g_cfg.soilBagnato[i] = SOIL_RAW_BAGNATO_DEF;
    }
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

  g_cfg.giornoCorrente    = prefs.getULong("giorno",   g_cfg.giornoCorrente);
  g_cfg.irrigazioniOggi   = prefs.getUChar("irrOggi",  g_cfg.irrigazioniOggi);
  g_cfg.litriOggi         = prefs.getFloat("litOggi",  g_cfg.litriOggi);
  g_cfg.ultimaIrrigEpoch  = prefs.getULong("ultIrr",   g_cfg.ultimaIrrigEpoch);
  g_cfg.giornoProgrammata = prefs.getULong("giornoPr", g_cfg.giornoProgrammata);

  g_cfg.litriTotali     = prefs.getFloat("litTot",   g_cfg.litriTotali);
  g_cfg.litriUltima     = prefs.getFloat("litUlt",   g_cfg.litriUltima);
  g_cfg.backlogScartati = prefs.getULong("blScart",  g_cfg.backlogScartati);
  g_cfg.esitoUltima     = prefs.getUChar("esitoUlt", g_cfg.esitoUltima);
  g_cfg.irrigInCorso    = prefs.getUChar("irrCorso", g_cfg.irrigInCorso);

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
  g_cfg.tzOffsetSec      = (int32_t)prefs.getLong("tzOff", (long)g_cfg.tzOffsetSec);
  g_cfg.tzNoto           = prefs.getUChar("tzNoto", g_cfg.tzNoto);

  prefs.end();
  g_sporco = false;

  validaImpostazioni();
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

  prefs.putULong("giorno",   g_cfg.giornoCorrente);
  prefs.putUChar("irrOggi",  g_cfg.irrigazioniOggi);
  prefs.putFloat("litOggi",  g_cfg.litriOggi);
  prefs.putULong("ultIrr",   g_cfg.ultimaIrrigEpoch);
  prefs.putULong("giornoPr", g_cfg.giornoProgrammata);

  prefs.putFloat("litTot",   g_cfg.litriTotali);
  prefs.putFloat("litUlt",   g_cfg.litriUltima);
  prefs.putULong("blScart",  g_cfg.backlogScartati);
  prefs.putUChar("esitoUlt", g_cfg.esitoUltima);
  prefs.putUChar("irrCorso", g_cfg.irrigInCorso);

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
  prefs.putLong ("tzOff",  (long)g_cfg.tzOffsetSec);
  prefs.putUChar("tzNoto", g_cfg.tzNoto);

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
  // Lo scarto del fuso e il "non lo so ancora" sono due informazioni diverse
  // e vanno stampate diversamente: UTC+0 e' un fuso valido, non un'assenza.
  char fuso[32];
  if (g_cfg.tzNoto) snprintf(fuso, sizeof(fuso), "UTC%+.1f h", g_cfg.tzOffsetSec / 3600.0f);
  else              snprintf(fuso, sizeof(fuso), "SCONOSCIUTO (attendo il ponte)");

  Serial.printf("[CFG] Fuso: %s | ultima irrigazione: %s (%.2f L)%s\n",
                fuso,
                irrigazioneEsitoTesto((EsitoIrrigazione)g_cfg.esitoUltima),
                g_cfg.litriUltima,
                g_cfg.irrigInCorso ? "  <-- VALVOLA RISULTAVA APERTA" : "");
}
