#include "irrigazione.h"
#include "impostazioni.h"
#include "orologio.h"
#include "sensori.h"
#include "watchdog.h"
#include "backlog.h"

#include <driver/gpio.h>

static uint32_t g_durataUltima = 0;
static bool     g_eseguitaOra  = false;

// I litri dell'ultima irrigazione vivono in NVS (g_cfg), non qui: vedi il
// commento sul campo in impostazioni.h. Il deep sleep azzererebbe una static.
float    irrigazioneLitriUltima()  { return g_cfg.litriUltima; }
uint32_t irrigazioneDurataUltima() { return g_durataUltima; }
bool     irrigazioneEseguitaOra()  { return g_eseguitaOra; }

// Anche l'esito vive in NVS, per lo stesso motivo dei litri e per uno in piu':
// deve sopravvivere pure a un reset avvenuto a valvola aperta.
EsitoIrrigazione irrigazioneEsitoUltima() { return (EsitoIrrigazione)g_cfg.esitoUltima; }

bool irrigazioneRecuperaInterrotta() {
  if (!g_cfg.irrigInCorso) return false;

  /*
   * In NVS la valvola risulta aperta, quindi l'ultima irrigazione non e' mai
   * arrivata alla riga che ne scrive l'esito: il nodo si e' resettato mentre
   * l'acqua scorreva (watchdog, panic, calo di tensione all'avvio della pompa).
   *
   * esitoUltima contiene gia' IRR_INTERROTTA, scritto prima di aprire, e
   * litriUltima e' gia' a zero: qui non c'e' niente da indovinare, basta
   * spegnere il marcatore e far sapere al chiamante che c'e' qualcosa da
   * riferire. Senza questo un'irrigazione interrotta spariva del tutto, e in
   * Home Assistant restava esposto l'esito di quella PRECEDENTE come se fosse
   * appena successo.
   */
  Serial.println(F("[IRRIG] ATTENZIONE: in NVS la valvola risultava ancora aperta."));
  Serial.println(F("[IRRIG] L'ultima irrigazione e' stata interrotta da un reset."));
  backlogLog("IRRIGAZIONE INTERROTTA da un reset");

  g_cfg.irrigInCorso = 0;
  g_cfg.esitoUltima  = IRR_INTERROTTA;
  impostazioniModificate();
  impostazioniSalva();
  return true;
}

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
    case IRR_NO_FUSO:       return "fuso_sconosciuto";
    case IRR_INTERROTTA:    return "interrotta";
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

  /*
   * Senza il fuso non si apre nulla, per lo stesso motivo per cui non si apre
   * senza un'ora attendibile: l'orario programmato e' ora civile, e finche'
   * non si sa quanto dista da UTC quel "6:00" puo' voler dire le 8:00 come le
   * 4:00. Capita solo dopo un flash o un RESETCFG, e dura un risveglio: il
   * primo ACK porta il fuso insieme all'ora.
   *
   * Se invece dura, vuol dire che il ponte non e' stato aggiornato insieme al
   * nodo. In Home Assistant si legge "fuso_sconosciuto" ed e' esattamente
   * quello che sta succedendo.
   */
  if (!orologioFusoNoto()) return IRR_NO_FUSO;

  if (!g_cfg.irrigAuto) return IRR_NO_AUTO;

  /*
   * L'appuntamento di oggi si onora UNA volta sola.
   *
   * Non basta irrigazioniOggi, che conta anche le manuali: un "Irriga ora"
   * premuto alle 5 non deve far saltare l'irrigazione programmata delle 6.
   * Ed e' questo marcatore a rendere sicura la finestra larga qui sotto: per
   * quanto a lungo resti aperta, l'acqua programmata scorre una volta al
   * giorno e basta.
   */
  if (g_cfg.giornoCorrente != 0 && g_cfg.giornoProgrammata == g_cfg.giornoCorrente)
    return IRR_NO_GIA_FATTA;

  // Finestra oraria in SECONDI e non in minuti: la tolleranza sul risveglio
  // anticipato si misura in secondi, e arrotondare ai minuti se la mangerebbe.
  uint32_t secOra   = (uint32_t)adesso.hour()   * 3600UL
                    + (uint32_t)adesso.minute() * 60UL
                    + (uint32_t)adesso.second();
  uint32_t secSched = ((uint32_t)g_cfg.irrigOra    * 3600UL
                    +  (uint32_t)g_cfg.irrigMinuto * 60UL) % 86400UL;

  /*
   * Distanza in avanti dall'orario programmato, calcolata sul giro delle 24 h.
   *
   * Il confronto diretto "secOra >= secSched" sembra ovvio ma si rompe a
   * mezzanotte: con la pianificazione alle 23:50 la finestra andrebbe oltre la
   * fine del giorno, ma i secondi tornano a zero. Il risveglio delle 23:45 e'
   * troppo presto, quello delle 00:00 ricomincia da capo, e la serra non
   * verrebbe irrigata MAI senza dire perche'.
   */
  uint32_t daSched = (secOra + 86400UL - secSched) % 86400UL;

  /*
   * Risveglio ANTICIPATO (vedi IRRIG_ANTICIPO_SEC in config.h).
   *
   * Il timer del deep sleep si sveglia qualche secondo prima del dovuto, e su
   * questo calcolo circolare "pochi secondi prima delle 6:00" non vale 0 ma
   * 86396: quasi un giorno di RITARDO. Il risveglio allineato all'orario
   * programmato mancava quindi la finestra ogni singola volta, e l'irrigazione
   * restava appesa a quello successivo con pochissimo margine.
   */
  if (daSched > 86400UL - (uint32_t)IRRIG_ANTICIPO_SEC) daSched = 0;

  /*
   * Ampiezza: un intervallo di sleep (il tempo che serve perche' un risveglio
   * veda l'appuntamento), piu' un minuto di durata del ciclo, piu' la finestra
   * di recupero che assorbe risvegli slittati e riavvii. Prima bastava un
   * risveglio in ritardo di due minuti per lasciare la serra a secco fino al
   * giorno dopo, e nei log si leggeva soltanto "fuori_orario".
   */
  uint32_t finestraSec = g_cfg.sleepSec + 60UL + (uint32_t)IRRIG_RECUPERO_SEC;
  if (finestraSec > 86400UL) finestraSec = 86400UL;

  if (daSched >= finestraSec) return IRR_NO_ORARIO;

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

EsitoIrrigazione irrigazioneEsegui(uint32_t durataSec, float litriTarget, uint32_t epoch,
                                   bool programmata) {
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

  // L'appuntamento di oggi risulta onorato da adesso, non da quando l'acqua
  // avra' finito di scorrere: se il nodo muore a valvola aperta non deve
  // riprovarci al riavvio.
  if (programmata) g_cfg.giornoProgrammata = g_cfg.giornoCorrente;

  /*
   * Verita' pessimistica scritta PRIMA di aprire: "sto irrigando, e finche'
   * non dico il contrario e' finita male".
   *
   * Il nodo si resetta davvero, ed e' successo a valvola aperta: esito e litri
   * venivano scritti solo alla fine, quindi quell'irrigazione spariva senza
   * lasciare traccia e in Home Assistant restava l'esito della PRECEDENTE,
   * come se non fosse successo niente. Con questi tre campi il riavvio
   * successivo trova scritto cosa stava succedendo. Azzerare litriUltima fa
   * parte della stessa onesta': i litri di prima non sono i litri di adesso,
   * e dichiararli sarebbe peggio che ammettere di non saperli.
   */
  g_cfg.irrigInCorso = 1;
  g_cfg.esitoUltima  = IRR_INTERROTTA;
  g_cfg.litriUltima  = 0.0f;

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

  uint32_t ultimoLog = 0;

  while (true) {
    wdtNutri();

    uint32_t trascorso = millis() - t0;   // aritmetica unsigned: rollover sicuro
    if (trascorso >= durMs) break;

    if (flussoDisponibile()) {
      litri = flussoLitri();

      /*
       * Riga di avanzamento ogni 2 secondi. Serve a diagnosticare i problemi
       * del flussometro guardando il monitor seriale mentre l'acqua scorre.
       *
       * "pin" e' il livello logico letto sul filo del segnale, ed e' il dato
       * piu' rivelatore: se resta fisso a 1 mentre l'acqua passa, gli impulsi
       * non arrivano proprio all'ESP32 (massa non in comune, filo staccato,
       * sensore non alimentato). Se invece cambia ma gli impulsi restano a
       * zero, il problema e' nell'interrupt.
       */
      if (trascorso - ultimoLog >= 2000UL) {
        ultimoLog = trascorso;
        uint32_t imp = flussoImpulsi();

        if (imp > 0) {
          // Tutto regolare: riga breve, nessun campionamento.
          Serial.printf("[IRRIG] %2lus/%lus  impulsi=%lu  %.3f L\n",
                        (unsigned long)(trascorso / 1000UL),
                        (unsigned long)durataSec, (unsigned long)imp, litri);
        } else {
          /*
           * Nessun impulso: solo qui vale la pena spendere 200 ms a campionare
           * il pin, perche' e' l'unico caso in cui c'e' qualcosa da capire.
           * Farlo sempre significherebbe passare il 10% dell'irrigazione in
           * un ciclo di attesa attiva, durante il quale non si controllano
           * ne' il target volumetrico ne' il budget litri.
           */
          uint32_t transizioni = flussoSondaTransizioni(200);
          Serial.printf("[IRRIG] %2lus/%lus  impulsi=0  pin=%d  transizioni=%lu  %s\n",
                        (unsigned long)(trascorso / 1000UL),
                        (unsigned long)durataSec,
                        digitalRead(PIN_FLUSSO),
                        (unsigned long)transizioni,
                        transizioni == 0 ? "(nessun segnale sul filo)"
                                         : "(SEGNALE PRESENTE, interrupt non conta!)");
        }
      }

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
  const EsitoIrrigazione esito = anomaliaFlusso ? IRR_ERR_FLUSSO : IRR_OK;

  g_cfg.litriUltima  = litri;
  g_cfg.esitoUltima  = (uint8_t)esito;   // sostituisce il "interrotta" provvisorio
  g_cfg.irrigInCorso = 0;                // valvola chiusa: marcatore spento
  g_eseguitaOra      = true;
  impostazioniModificate();   // va salvato anche se sono zero litri

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

  return esito;
}
