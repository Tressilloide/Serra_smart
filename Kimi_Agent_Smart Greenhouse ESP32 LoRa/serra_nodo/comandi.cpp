#include "comandi.h"
#include "config.h"
#include "impostazioni.h"
#include "irrigazione.h"
#include "sensori.h"
#include "orologio.h"
#include "backlog.h"

#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
//  Utility: estrae l'n-esimo argomento separato da virgola
// ---------------------------------------------------------------------------

static bool argomento(const char* args, uint8_t n, char* out, size_t maxOut) {
  out[0] = '\0';
  if (!args) return false;

  const char* p = args;
  for (uint8_t i = 0; i < n; i++) {
    p = strchr(p, ',');
    if (!p) return false;
    p++;
  }
  const char* fine = strchr(p, ',');
  size_t lung = fine ? (size_t)(fine - p) : strlen(p);
  if (lung == 0) return false;
  if (lung > maxOut - 1) lung = maxOut - 1;
  memcpy(out, p, lung);
  out[lung] = '\0';
  return true;
}

static long argL(const char* args, uint8_t n, long def) {
  char buf[24];
  return argomento(args, n, buf, sizeof(buf)) ? atol(buf) : def;
}

static float argF(const char* args, uint8_t n, float def) {
  char buf[24];
  return argomento(args, n, buf, sizeof(buf)) ? (float)atof(buf) : def;
}

static void dettaglio(EsitoComando& es, const char* testo) {
  strncpy(es.dettaglio, testo, sizeof(es.dettaglio) - 1);
  es.dettaglio[sizeof(es.dettaglio) - 1] = '\0';
}

// "soil1" -> 0, "soil2" -> 1, ... ; -1 se non e' un sensore terreno
static int8_t indiceSoil(const char* chiave) {
  if (strncmp(chiave, "soil", 4) != 0) return -1;
  char c = chiave[4];
  if (c < '1' || c > '4') return -1;
  return (int8_t)(c - '1');
}

// ---------------------------------------------------------------------------

void comandoDaAck(const RispostaAck& ack, ComandoRicevuto& out) {
  memset(&out, 0, sizeof(out));
  if (!ack.ricevuto || !ack.haComando) return;

  out.presente = true;
  out.id       = ack.cmdId;
  strncpy(out.opcode, ack.opcode, sizeof(out.opcode) - 1);
  strncpy(out.args,   ack.args,   sizeof(out.args) - 1);
}

// ---------------------------------------------------------------------------
//  Esecuzione
// ---------------------------------------------------------------------------

EsitoComando comandoEsegui(const ComandoRicevuto& cmd, const DateTime& adesso,
                           PacchettoKV& extra) {
  EsitoComando es = {};
  es.rc = RC_OK;
  dettaglio(es, "ok");

  const char* op = cmd.opcode;
  Serial.printf("\n[CMD] Eseguo id=%lu opcode=%s args=[%s]\n",
                (unsigned long)cmd.id, op, cmd.args);

  // --- IRR,<secondi> : irrigazione manuale a tempo -------------------------
  // 0 (o argomento assente) significa "usa la durata configurata": e' quello
  // che invia il bottone "Irriga ora" di Home Assistant, che non ha modo di
  // conoscere la durata impostata sul nodo.
  if (strcmp(op, "IRR") == 0) {
    long sec = argL(cmd.args, 0, 0);
    if (sec <= 0) sec = g_cfg.irrigDurataSec;

    EsitoIrrigazione r = irrigazioneEsegui((uint32_t)sec, 0.0f, adesso.unixtime());
    dettaglio(es, irrigazioneEsitoTesto(r));
    if (r != IRR_OK && r != IRR_ERR_FLUSSO) es.rc = RC_NEGATO_SICUREZZA;
    extra.aggiungiF("cmdL", irrigazioneLitriUltima(), 3);
    extra.aggiungiU("cmdS", irrigazioneDurataUltima());
    return es;
  }

  // --- IRRVOL,<litri>[,<max_sec>] : irrigazione volumetrica ----------------
  if (strcmp(op, "IRRVOL") == 0) {
    if (!flussoDisponibile()) {
      es.rc = RC_HW_ASSENTE;
      dettaglio(es, "flussometro_assente");
      return es;
    }
    float litri  = argF(cmd.args, 0, 0.0f);
    long  maxSec = argL(cmd.args, 1, IRRIG_MAX_SEC);
    if (litri <= 0.0f) { es.rc = RC_ARGOMENTI; dettaglio(es, "litri_non_validi"); return es; }

    EsitoIrrigazione r = irrigazioneEsegui((uint32_t)maxSec, litri, adesso.unixtime());
    dettaglio(es, irrigazioneEsitoTesto(r));
    if (r != IRR_OK && r != IRR_ERR_FLUSSO) es.rc = RC_NEGATO_SICUREZZA;
    extra.aggiungiF("cmdL", irrigazioneLitriUltima(), 3);
    extra.aggiungiU("cmdS", irrigazioneDurataUltima());
    return es;
  }

  // --- STOP : niente piu' irrigazioni automatiche per oggi -----------------
  // Non "chiude la valvola": quando il comando arriva l'irrigazione e' gia'
  // terminata da un pezzo. Esaurisce il contatore giornaliero.
  if (strcmp(op, "STOP") == 0) {
    g_cfg.irrigazioniOggi = IRRIG_MAX_AL_GIORNO;
    impostazioniModificate();
    impostazioniSalva();
    dettaglio(es, "bloccata_per_oggi");
    return es;
  }

  // --- AUTO,<0|1> ----------------------------------------------------------
  if (strcmp(op, "AUTO") == 0) {
    g_cfg.irrigAuto = (argL(cmd.args, 0, 1) != 0);
    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "auto=%d", g_cfg.irrigAuto ? 1 : 0);
    return es;
  }

  // --- SCHED,<ora>,<minuto>,<durata_sec> -----------------------------------
  if (strcmp(op, "SCHED") == 0) {
    long ora = argL(cmd.args, 0, -1);
    long min = argL(cmd.args, 1, 0);
    long dur = argL(cmd.args, 2, g_cfg.irrigDurataSec);

    if (ora < 0 || ora > 23 || min < 0 || min > 59 || dur <= 0) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "orario_non_valido");
      return es;
    }
    if (dur > IRRIG_MAX_SEC) dur = IRRIG_MAX_SEC;   // il tetto vale anche qui

    g_cfg.irrigOra       = (uint8_t)ora;
    g_cfg.irrigMinuto    = (uint8_t)min;
    g_cfg.irrigDurataSec = (uint16_t)dur;
    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "%02ld:%02ld/%lds", ora, min, dur);
    return es;
  }

  // --- DUR,<secondi> : solo la durata ---------------------------------------
  // Corrisponde all'entita' number "Durata irrigazione" di Home Assistant,
  // che puo' inviare un solo valore per volta (a differenza di SCHED).
  if (strcmp(op, "DUR") == 0) {
    long dur = argL(cmd.args, 0, 0);
    if (dur <= 0) { es.rc = RC_ARGOMENTI; dettaglio(es, "durata_non_valida"); return es; }
    if (dur > IRRIG_MAX_SEC) dur = IRRIG_MAX_SEC;
    g_cfg.irrigDurataSec = (uint16_t)dur;
    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "durata=%lds", dur);
    return es;
  }

  // --- ORA,<HH:MM[:SS]> : solo l'orario -------------------------------------
  // Corrisponde all'entita' time "Orario irrigazione", che pubblica nel
  // formato "17:30:00". I secondi vengono ignorati: il nodo si sveglia a
  // intervalli, quindi una precisione al secondo non avrebbe significato.
  if (strcmp(op, "ORA") == 0) {
    int ora = -1, minuto = -1;
    if (sscanf(cmd.args, "%d:%d", &ora, &minuto) != 2 ||
        ora < 0 || ora > 23 || minuto < 0 || minuto > 59) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "orario_non_valido");
      return es;
    }
    g_cfg.irrigOra    = (uint8_t)ora;
    g_cfg.irrigMinuto = (uint8_t)minuto;
    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "orario=%02d:%02d", ora, minuto);
    return es;
  }

  // --- SOIL,<soglia%> : -1 disattiva l'irrigazione condizionata ------------
  if (strcmp(op, "SOIL") == 0) {
    long s = argL(cmd.args, 0, -1);
    if (s < -1 || s > 100) { es.rc = RC_ARGOMENTI; dettaglio(es, "soglia_non_valida"); return es; }
    g_cfg.soilSoglia = (int16_t)s;
    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "soglia=%ld", s);
    return es;
  }

  // --- SLEEP,<secondi> -----------------------------------------------------
  if (strcmp(op, "SLEEP") == 0) {
    long s = argL(cmd.args, 0, 0);
    if (s < SLEEP_MIN_SEC || s > 86400L) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "intervallo_non_valido");
      return es;
    }
    g_cfg.sleepSec = (uint32_t)s;
    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "sleep=%lds", s);
    return es;
  }

  // --- TIME,<epoch> : forza la sincronizzazione dell'orologio --------------
  if (strcmp(op, "TIME") == 0) {
    uint32_t epoch = (uint32_t)argL(cmd.args, 0, 0);
    if (!orologioImposta(epoch)) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "epoch_non_valido");
      return es;
    }
    dettaglio(es, "orologio_aggiornato");
    return es;
  }

  // --- CAL,<chiave>,<p1>[,<p2>] : calibrazione -----------------------------
  //     soilN -> p1 = grezzo in aria (secco), p2 = grezzo in acqua (bagnato)
  //     volt  -> p1 = rapporto del partitore
  //     acqua -> p1 = impulsi per litro
  if (strcmp(op, "CAL") == 0) {
    char chiave[12];
    if (!argomento(cmd.args, 0, chiave, sizeof(chiave))) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "chiave_mancante");
      return es;
    }

    int8_t idx = indiceSoil(chiave);
    if (idx >= 0) {
      long secco   = argL(cmd.args, 1, -1);
      long bagnato = argL(cmd.args, 2, -1);
      if (secco < 0 || bagnato < 0 || secco == bagnato) {
        es.rc = RC_ARGOMENTI;
        dettaglio(es, "valori_non_validi");
        return es;
      }
      g_cfg.soilSecco[idx]   = (uint16_t)secco;
      g_cfg.soilBagnato[idx] = (uint16_t)bagnato;

    } else if (strcmp(chiave, "volt") == 0) {
      float d = argF(cmd.args, 1, 0.0f);
      if (d <= 0.1f || d > 100.0f) { es.rc = RC_ARGOMENTI; dettaglio(es, "divisore_non_valido"); return es; }
      g_cfg.voltDivider = d;

    } else if (strcmp(chiave, "acqua") == 0) {
      float i = argF(cmd.args, 1, 0.0f);
      if (i <= 1.0f || i > 10000.0f) { es.rc = RC_ARGOMENTI; dettaglio(es, "impulsi_non_validi"); return es; }
      g_cfg.flussoImpLitro = i;

    } else {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "chiave_sconosciuta");
      return es;
    }

    impostazioniModificate();
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "cal_%s_ok", chiave);
    return es;
  }

  // --- SENS,<chiave>,<0|1> : accende/spegne un sensore da remoto -----------
  if (strcmp(op, "SENS") == 0) {
    char chiave[12];
    if (!argomento(cmd.args, 0, chiave, sizeof(chiave))) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "chiave_mancante");
      return es;
    }
    int8_t i = sensoreIndiceDaChiave(chiave);
    if (i < 0) {
      es.rc = RC_ARGOMENTI;
      dettaglio(es, "sensore_sconosciuto");
      return es;
    }
    bool on = (argL(cmd.args, 1, 1) != 0);
    sensoreImpostaAbilitato(SENSORI[i].bit, on);
    impostazioniSalva();
    snprintf(es.dettaglio, sizeof(es.dettaglio), "%s=%d", chiave, on ? 1 : 0);
    return es;
  }

  // --- WAKE,<secondi> : finestra di manutenzione ---------------------------
  // Il nodo resta sveglio e continua a chiedere comandi: comodo per fare
  // piu' regolazioni di seguito senza aspettare un risveglio ogni volta.
  if (strcmp(op, "WAKE") == 0) {
    long s = argL(cmd.args, 0, 60);
    if (s < 10) s = 10;
    if (s > WAKE_MAX_SEC) s = WAKE_MAX_SEC;
    es.restaSveglioSec = (uint32_t)s;
    snprintf(es.dettaglio, sizeof(es.dettaglio), "sveglio_%lds", s);
    return es;
  }

  // --- CLRBL : svuota il backlog -------------------------------------------
  if (strcmp(op, "CLRBL") == 0) {
    backlogSvuota();
    dettaglio(es, "backlog_svuotato");
    return es;
  }

  // --- RESETCFG : ripristina le impostazioni di fabbrica -------------------
  if (strcmp(op, "RESETCFG") == 0) {
    impostazioniReset();
    dettaglio(es, "config_ripristinata");
    return es;
  }

  // --- RESET : riavvia il nodo ---------------------------------------------
  if (strcmp(op, "RESET") == 0) {
    es.riavvia = true;
    dettaglio(es, "riavvio");
    return es;
  }

  Serial.printf("[CMD] Opcode sconosciuto: %s\n", op);
  es.rc = RC_OPCODE_IGNOTO;
  dettaglio(es, "opcode_ignoto");
  return es;
}
