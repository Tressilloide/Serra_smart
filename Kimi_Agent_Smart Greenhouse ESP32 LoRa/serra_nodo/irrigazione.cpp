#include "irrigazione.h"
#include "impostazioni.h"
#include "sensori.h"
#include "watchdog.h"
#include "backlog.h"

#include <driver/gpio.h>

static float    g_litriUltima  = 0.0f;
static uint32_t g_durataUltima = 0;
static bool     g_eseguitaOra  = false;

float    irrigazioneLitriUltima()  { return g_litriUltima; }
uint32_t irrigazioneDurataUltima() { return g_durataUltima; }
bool     irrigazioneEseguitaOra()  { return g_eseguitaOra; }

// ---------------------------------------------------------------------------

void relayOffImmediato() {
  // Ordine importante: si sblocca l'hold del deep sleep, si scrive il livello
  // sicuro nel latch di uscita e solo dopo si configura il pin come output.
  // Cosi' il pin non passa mai per uno stato pilotato ma indefinito.
  gpio_hold_dis((gpio_num_t)PIN_RELAY);
  gpio_deep_sleep_hold_dis();

  digitalWrite(PIN_RELAY, RELAY_OFF);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF);
}

const char* irrigazioneEsitoTesto(EsitoIrrigazione e) {
  switch (e) {
    case IRR_OK:            return "ok";
    case IRR_NO_ORARIO:     return "fuori_orario";
    case IRR_NO_AUTO:       return "auto_disattivata";
    case IRR_NO_GIA_FATTA:  return "gia_fatta";
    case IRR_NO_INTERVALLO: return "troppo_presto";
    case IRR_NO_TERRENO:    return "terreno_umido";
    case IRR_NO_BUDGET:     return "budget_esaurito";
    case IRR_NO_RTC:        return "ora_non_attendibile";
    case IRR_ERR_FLUSSO:    return "nessun_flusso";
    default:                return "sconosciuto";
  }
}

// ---------------------------------------------------------------------------
//  Decisione
// ---------------------------------------------------------------------------

EsitoIrrigazione irrigazioneValuta(const DateTime& adesso, bool rtcAttendibile, float soilMin) {
  // Senza un'ora attendibile non si apre nulla: rischieremmo di irrigare a
  // ripetizione a ogni risveglio credendo che sia sempre l'ora programmata.
  if (!rtcAttendibile) return IRR_NO_RTC;

  if (!g_cfg.irrigAuto) return IRR_NO_AUTO;

  // Finestra oraria: il risveglio non cade mai esattamente al minuto giusto,
  // quindi si accetta tutta la finestra che va dall'orario programmato fino
  // al risveglio successivo.
  uint32_t minutiOra   = (uint32_t)adesso.hour() * 60UL + adesso.minute();
  uint32_t minutiSched = (uint32_t)g_cfg.irrigOra * 60UL + g_cfg.irrigMinuto;
  uint32_t finestraMin = (g_cfg.sleepSec / 60UL) + 1UL;

  if (minutiOra < minutiSched || minutiOra >= minutiSched + finestraMin)
    return IRR_NO_ORARIO;

  if (g_cfg.irrigazioniOggi >= IRRIG_MAX_AL_GIORNO) return IRR_NO_GIA_FATTA;

  if (g_cfg.ultimaIrrigEpoch > 0) {
    uint32_t adessoEpoch = adesso.unixtime();
    if (adessoEpoch > g_cfg.ultimaIrrigEpoch &&
        (adessoEpoch - g_cfg.ultimaIrrigEpoch) < (uint32_t)IRRIG_MIN_INTERVALLO_M * 60UL)
      return IRR_NO_INTERVALLO;
  }

  if (BUDGET_LITRI_GIORNO > 0.0f && flussoDisponibile() &&
      g_cfg.litriOggi >= BUDGET_LITRI_GIORNO)
    return IRR_NO_BUDGET;

  // Irrigazione condizionata: se la soglia e' attiva e c'e' almeno una lettura
  // valida del terreno, si irriga solo se il piu' secco e' sotto soglia.
  if (g_cfg.soilSoglia >= 0 && !isnan(soilMin) && soilMin >= (float)g_cfg.soilSoglia)
    return IRR_NO_TERRENO;

  return IRR_OK;
}

// ---------------------------------------------------------------------------
//  Esecuzione
// ---------------------------------------------------------------------------

EsitoIrrigazione irrigazioneEsegui(uint32_t durataSec, float litriTarget, uint32_t epoch) {
  // --- Tetti di sicurezza, applicati sempre e comunque -----------------------

  if (durataSec == 0) durataSec = g_cfg.irrigDurataSec;

  if (durataSec > IRRIG_MAX_SEC) {
    Serial.printf("[IRRIG] Durata richiesta %lu s clampata al tetto di %d s.\n",
                  (unsigned long)durataSec, IRRIG_MAX_SEC);
    durataSec = IRRIG_MAX_SEC;
  }

  if (g_cfg.irrigazioniOggi >= IRRIG_MAX_AL_GIORNO) {
    Serial.println(F("[IRRIG] NEGATA: raggiunto il massimo di irrigazioni giornaliere."));
    return IRR_NO_GIA_FATTA;
  }

  if (g_cfg.ultimaIrrigEpoch > 0 && epoch > g_cfg.ultimaIrrigEpoch &&
      (epoch - g_cfg.ultimaIrrigEpoch) < (uint32_t)IRRIG_MIN_INTERVALLO_M * 60UL) {
    Serial.printf("[IRRIG] NEGATA: sono passati meno di %d minuti dall'ultima.\n",
                  IRRIG_MIN_INTERVALLO_M);
    return IRR_NO_INTERVALLO;
  }

  float budgetResiduo = -1.0f;
  if (BUDGET_LITRI_GIORNO > 0.0f && flussoDisponibile()) {
    budgetResiduo = BUDGET_LITRI_GIORNO - g_cfg.litriOggi;
    if (budgetResiduo <= 0.0f) {
      Serial.println(F("[IRRIG] NEGATA: budget litri giornaliero esaurito."));
      return IRR_NO_BUDGET;
    }
    // Il budget residuo e' anche un tetto sul target volumetrico richiesto.
    if (litriTarget > budgetResiduo) litriTarget = budgetResiduo;
  }

  // --- (3) Contatori aggiornati PRIMA di aprire la valvola --------------------
  // Se qui va via la corrente o il nodo si blocca, al riavvio risultera' che
  // l'irrigazione e' gia' stata fatta e non verra' ripetuta.

  g_cfg.irrigazioniOggi++;
  g_cfg.ultimaIrrigEpoch = epoch;
  impostazioniModificate();
  impostazioniSalva();

  backlogLog(String("IRRIGAZIONE INIZIO durata=") + durataSec + "s target=" + litriTarget + "L");
  Serial.printf("[IRRIG] Inizio: durata max %lu s, target %.2f L (0 = solo tempo).\n",
                (unsigned long)durataSec, litriTarget);

  // --- (4) Watchdog armato per durata + margine ------------------------------
  wdtImposta(durataSec + 60UL);

  // --- Flussometro -----------------------------------------------------------
  if (flussoDisponibile()) flussoAzzera();

  // --- (5) Apertura valvola e ciclo di attesa a timeout software -------------
  digitalWrite(PIN_RELAY, RELAY_ON);

  const uint32_t t0     = millis();
  const uint32_t durMs  = durataSec * 1000UL;
  bool  anomaliaFlusso  = false;
  float litri           = 0.0f;

  // Finestra di grazia: l'acqua non arriva istantaneamente alla turbina, deve
  // prima percorrere il tubo e la pompa deve adescarsi. Fino a qui gli impulsi
  // vengono contati ma il flusso non viene giudicato, altrimenti ogni singola
  // irrigazione partirebbe con un falso allarme "nessun flusso".
  const uint32_t graziaMs  = (uint32_t)FLUSSO_GRAZIA_SEC  * 1000UL;
  const uint32_t verdetoMs = graziaMs + (uint32_t)FLUSSO_TIMEOUT_SEC * 1000UL;

  while (true) {
    wdtNutri();

    uint32_t trascorso = millis() - t0;   // aritmetica unsigned: rollover sicuro
    if (trascorso >= durMs) break;

    if (flussoDisponibile()) {
      litri = flussoLitri();

      // Obiettivo volumetrico raggiunto
      if (litriTarget > 0.0f && litri >= litriTarget) {
        Serial.printf("[IRRIG] Target volumetrico raggiunto: %.3f L.\n", litri);
        break;
      }

      // (7) Budget giornaliero: chiusura immediata anche a meta' irrigazione
      if (budgetResiduo > 0.0f && litri >= budgetResiduo) {
        Serial.printf("[IRRIG] Budget giornaliero raggiunto a %.3f L: chiudo.\n", litri);
        break;
      }

      // Anomalia idraulica: valvola aperta da abbastanza tempo e ancora
      // nessun impulso. Pompa guasta, serbatoio vuoto, filtro otturato,
      // tubo staccato o flussometro scollegato.
      if (!anomaliaFlusso && trascorso > verdetoMs && flussoImpulsi() == 0) {
        anomaliaFlusso = true;
        Serial.println(F("[IRRIG] ANOMALIA: valvola aperta ma nessun flusso rilevato!"));
        backlogLog("IRRIGAZIONE ANOMALIA nessun flusso");

        // Su un'irrigazione volumetrica si interrompe: senza flusso il target
        // non arrivera' mai e si terrebbe la valvola aperta per nulla.
        // Su un'irrigazione a tempo si prosegue e si segnala soltanto: un
        // flussometro guasto non deve impedire di annaffiare la serra.
        if (litriTarget > 0.0f) break;
      }
    }

    delay(200);
  }

  // --- Chiusura --------------------------------------------------------------
  digitalWrite(PIN_RELAY, RELAY_OFF);

  const uint32_t trascorsoTot = millis() - t0;
  g_durataUltima = trascorsoTot / 1000UL;

  if (flussoDisponibile()) {
    litri = flussoLitri();

    /*
     * Verdetto finale, per le irrigazioni troppo brevi perche' il controllo
     * dentro il ciclo faccia in tempo a scattare.
     *
     * Senza questo, un'irrigazione piu' corta di GRAZIA + TIMEOUT non avrebbe
     * MAI il rilevamento dell'acqua mancante: la valvola si aprirebbe e
     * chiuderebbe a vuoto senza che nessuno se ne accorga, e il caso tipico
     * e' proprio l'irrigazione manuale breve fatta per provare l'impianto.
     * Superata la sola finestra di grazia, zero impulsi vuol dire zero acqua.
     */
    if (!anomaliaFlusso && trascorsoTot > graziaMs && flussoImpulsi() == 0) {
      anomaliaFlusso = true;
      Serial.println(F("[IRRIG] ANOMALIA: irrigazione conclusa senza un solo impulso!"));
      backlogLog("IRRIGAZIONE ANOMALIA nessun flusso (verdetto finale)");
    }

    flussoStacca();
  }
  g_litriUltima = litri;
  g_eseguitaOra = true;

  // Il terreno e' appena cambiato: la lettura memorizzata non vale piu' e la
  // composizione del pacchetto ne fara' una nuova. E' l'unico caso in cui la
  // seconda accensione del rail dei sensori serve davvero.
  sensoriInvalidaCache();

  wdtRimuoviTask();
  wdtImposta(WDT_SETUP_SEC);   // si torna al watchdog "normale" del ciclo

  // --- Contabilizzazione -----------------------------------------------------
  if (litri > 0.0f) {
    g_cfg.litriOggi   += litri;
    g_cfg.litriTotali += litri;
    impostazioniModificate();
  }
  impostazioniSalva();

  Serial.printf("[IRRIG] Fine: %lu s, %.3f L (oggi %.2f L, totale %.1f L). Relay OFF.\n",
                (unsigned long)g_durataUltima, litri, g_cfg.litriOggi, g_cfg.litriTotali);
  backlogLog(String("IRRIGAZIONE FINE durata=") + g_durataUltima + "s litri=" + litri);

  return anomaliaFlusso ? IRR_ERR_FLUSSO : IRR_OK;
}
