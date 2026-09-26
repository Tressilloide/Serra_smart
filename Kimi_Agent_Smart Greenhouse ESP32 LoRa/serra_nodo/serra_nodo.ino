/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA
 *  ESP32 + LoRa Ra-01 + BME280 + DS1307 + microSD + relay irrigazione
 *  + sensori umidita' terreno + flussometro YF-S201
 * ============================================================================
 *
 *  Versione in config.h (FW_VERSION) — vedi docs/Guida_Serra_Smart.md e
 *  docs/PROTOCOLLO.md
 *
 *  ---------------------------------------------------------------------------
 *  CICLO DI VITA (il nodo vive solo dentro setup(), poi torna a dormire)
 *  ---------------------------------------------------------------------------
 *   1. Relay FORZATO spento come primissima istruzione (stato sicuro)
 *   2. Watchdog globale armato: copre tutto il ciclo, non solo l'irrigazione
 *   3. Caricamento impostazioni da NVS (modificabili da Home Assistant)
 *   4. Lettura orologio; se non e' attendibile non si irriga e si aspetta la
 *      sincronizzazione automatica dal ponte
 *   5. Lettura umidita' terreno -> decisione irrigazione condizionata
 *   6. Eventuale irrigazione, protetta a piu' livelli
 *   7. Lettura di tutti i sensori abilitati (tabella modulare in sensori.cpp)
 *   8. Trasmissione LoRa con ACK. L'ACK puo' contenere COMANDI da eseguire:
 *      e' cosi' che Home Assistant parla con un nodo che dorme
 *   9. Svuotamento del backlog su microSD se il link funziona
 *  10. Deep sleep allineato all'intervallo configurato
 *
 *  ---------------------------------------------------------------------------
 *  LIBRERIE NECESSARIE (Gestore librerie di Arduino IDE)
 *  ---------------------------------------------------------------------------
 *    - "LoRa" by Sandeep Mistry
 *    - "Adafruit BME280 Library" (+ "Adafruit Unified Sensor")
 *    - "RTClib" by Adafruit
 *    - "Adafruit ADS1X15"  (SOLO se metti USA_ADS1115 a 1 in config.h)
 *
 *  Scheda: "ESP32 Dev Module"
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "config.h"
#include "protocollo.h"
#include "watchdog.h"
#include "traccia.h"
#include "impostazioni.h"
#include "orologio.h"
#include "sensori.h"
#include "irrigazione.h"
#include "backlog.h"
#include "radio.h"
#include "comandi.h"

// ---------------------------------------------------------------------------
//  Stato che sopravvive al deep sleep (RAM del dominio RTC)
// ---------------------------------------------------------------------------

/*
 * RTC_NOINIT_ATTR e non RTC_DATA_ATTR: le variabili con inizializzatore in
 * memoria RTC vengono riazzerate dal bootloader a ogni avvio che non sia il
 * risveglio dal deep sleep, quindi un riavvio da watchdog faceva ripartire la
 * numerazione da capo. Il risultato era che un riavvio il cui pacchetto non
 * arrivava al ponte diventava invisibile: succedeva davvero, e si e' capito
 * solo notando un numero di sequenza troppo basso. Con la numerazione continua
 * un buco nella sequenza e' la prova che un ciclo e' andato perso.
 *
 * Il prezzo e' che all'accensione contengono spazzatura: le azzera setup()
 * quando tracciaMemoriaPersa() dice che la memoria RTC non e' attendibile.
 */
RTC_NOINIT_ATTR uint32_t g_seq;             // contatore pacchetti
RTC_NOINIT_ATTR uint32_t g_risvegli;        // risvegli dall'ultima mancanza di corrente

/*
 * Eventi da ripetere finche' non arrivano davvero a Home Assistant.
 *
 * Un pacchetto che non passa finisce nel backlog, e dal firmware 2.5.0 il
 * ponte pubblica i record arretrati su serra/nodo/storico e non piu' sullo
 * stato (vedi marcaArretrato()). E' giusto per le letture, che invecchiano,
 * ma NON per gli eventi: se il pacchetto perso era quello dell'irrigazione,
 * Home Assistant non vedrebbe mai un "nessun_flusso" e l'allarme non
 * partirebbe; se era il primo dopo un riavvio anomalo, il riavvio
 * resterebbe invisibile. Fin qui li vedeva solo per sbaglio, perche' il
 * record arretrato sovrascriveva lo stato.
 *
 * Quindi questi fatti restano "pendenti" e si ripetono nel pacchetto
 * principale dei risvegli successivi, finche' uno di quei pacchetti non
 * viene confermato. In RTC_NOINIT_ATTR, come g_seq, per sopravvivere anche
 * a un riavvio: si azzerano solo quando torna la corrente.
 */
RTC_NOINIT_ATTR uint8_t  g_esitoIrrigPendente;   // l'esito dell'ultima irrigazione non e' ancora arrivato
RTC_NOINIT_ATTR uint8_t  g_resetPendente;        // la diagnostica dell'ultimo reset non e' ancora arrivata
RTC_NOINIT_ATTR uint8_t  g_resetMotivo;          // ...con il suo motivo (esp_reset_reason_t)
RTC_NOINIT_ATTR uint8_t  g_resetTappa;           // ...e la tappa in cui il ciclo si era fermato

// Tentativi che ha richiesto il pacchetto principale del ciclo precedente
// (campo "txp"). Dice quanto e' fragile il collegamento, e in particolare se
// la trasmissione subito dopo l'irrigazione fatica ancora: il 22/09 e'
// passata al terzo e ultimo tentativo, e lo si e' capito solo dai tempi.
RTC_NOINIT_ATTR uint8_t  g_txpPrec;

// ---------------------------------------------------------------------------
//  Stato del ciclo corrente
// ---------------------------------------------------------------------------

static EsitoIrrigazione g_esitoIrrig = IRR_NO_ORARIO;
static bool             g_linkOk     = false;
static uint32_t         g_wakeExtraSec = 0;

/*
 * true quando al risveglio si e' trovata in NVS un'irrigazione interrotta da
 * un reset: c'e' un fatto da riferire a Home Assistant anche se in questo
 * ciclo la valvola non si e' mai aperta.
 */
static bool             g_irrigazioneDaRiferire = false;

// ---------------------------------------------------------------------------

/*
 * Tutto cio' che un ACK porta oltre alla conferma: l'ora del ponte e lo scarto
 * del fuso orario.
 *
 * Va applicato a OGNI ack e non solo a quello del pacchetto principale,
 * altrimenti un nodo che in un risveglio parla soltanto per svuotare il
 * backlog resterebbe indietro su entrambi.
 *
 * L'ordine conta: prima il fuso, poi il giorno, perche' il giorno dei
 * contatori si calcola sull'ora locale.
 */
static void assimilaAck(const RispostaAck& ack) {
  if (!ack.ricevuto) return;

  // Il fuso arriva solo da un ponte aggiornato: con uno vecchio il campo manca
  // e si tiene quello gia' in NVS, invece di ricadere silenziosamente su UTC.
  if (ack.tzValido) orologioImpostaFuso(ack.tzOffsetSec);

  // L'ora del ponte viaggia su ogni ACK: l'orologio si corregge da solo,
  // senza bisogno di comandi manuali ne' di ricompilare lo sketch.
  if (ack.epochPonte > 0) orologioSincronizza(ack.epochPonte);

  if (orologioAttendibile())
    impostazioniNuovoGiorno(orologioGiorno(orologioLocale()));
}

/*
 * Cosa scrivere nel campo "irr", quello che alimenta "Esito irrigazione".
 *
 * Se in questo ciclo l'acqua e' davvero scorsa — o se al risveglio si e'
 * scoperta un'irrigazione interrotta da un reset — e' QUELLO il fatto da
 * riferire, e vale anche per il bottone "Irriga ora" di Home Assistant.
 * Prima l'esito di un'irrigazione manuale viaggiava solo dentro il campo
 * "det" del pacchetto di esito comando: in "Esito irrigazione" non compariva
 * mai, e se quel pacchetto si perdeva non ne restava traccia da nessuna parte.
 *
 * Lo stesso vale nei risvegli successivi finche' quel fatto non e' arrivato
 * davvero (g_esitoIrrigPendente): se il pacchetto che lo portava e' finito
 * nel backlog, dallo storico Home Assistant non lo leggerebbe.
 *
 * Negli altri risvegli il campo torna a dire perche' l'automatica non e'
 * partita ("fuori_orario", "ora_non_attendibile", ...), che e' la diagnostica
 * per cui era nato.
 */
static bool esitoIrrigDaRiferire() {
  return g_irrigazioneDaRiferire || irrigazioneEseguitaOra() || g_esitoIrrigPendente;
}

static const char* esitoDaSegnalare() {
  if (esitoIrrigDaRiferire())
    return irrigazioneEsitoTesto(irrigazioneEsitoUltima());
  return irrigazioneEsitoTesto(g_esitoIrrig);
}

// ===========================================================================
//  Costruzione dei pacchetti
// ===========================================================================

// Intestazione comune a ogni pacchetto in uscita
static void intestazione(PacchettoKV& pkt, const DateTime& adesso) {
  pkt.reset();
  pkt.aggiungiU("v", PROTO_VERSIONE);
  pkt.aggiungiU("s", ++g_seq);
  pkt.aggiungiU("t", orologioAttendibile() ? adesso.unixtime() : 0);
}

/*
 * Blocco di stato: serve a Home Assistant per mostrare la configurazione REALE
 * del nodo invece di quella che crede di aver impostato. Se un comando si e'
 * perso, la dashboard se ne accorge da sola al risveglio successivo.
 */
static void aggiungiStato(PacchettoKV& pkt) {
  // ORDINE VOLUTO: prima i campi di configurazione, poi quelli informativi.
  //
  // Se il pacchetto supera il tetto, la serializzazione omette i campi in
  // fondo. I campi "s*" alimentano le entita' di COMANDO di Home Assistant
  // (durata, orario, soglia, interruttore): se sparissero, quei controlli
  // mostrerebbero un valore sbagliato. "irr" e "bl" invece sono normali
  // sensori: se mancano, Home Assistant conserva l'ultimo valore buono e
  // al risveglio successivo si riallinea da solo.
  pkt.aggiungiU("sAuto", g_cfg.irrigAuto ? 1 : 0);
  pkt.aggiungiU("sOra",  g_cfg.irrigOra);
  pkt.aggiungiU("sMin",  g_cfg.irrigMinuto);
  pkt.aggiungiU("sDur",  g_cfg.irrigDurataSec);
  pkt.aggiungiI("sSoil", g_cfg.soilSoglia);
  pkt.aggiungiU("slp",   g_cfg.sleepSec);

  pkt.aggiungi ("irr",   esitoDaSegnalare());

  // -1 = microSD assente o guasta. Prima si mandava 0 anche in quel caso, e
  // "coda vuota" e "nessuna coda" erano indistinguibili: con la scheda morta
  // ogni pacchetto non consegnato andava perso mentre Home Assistant
  // mostrava un rassicurante zero.
  pkt.aggiungiI("bl", backlogDisponibile() ? (int32_t)backlogConta() : -1);
}

// Invia un pacchetto e restituisce l'eventuale ACK.
static bool inviaPacchetto(const PacchettoKV& pkt, RispostaAck& ack) {
  char buf[PROTO_MAX_PAYLOAD + 8];
  size_t n = pkt.serializza(NODE_ID, buf, sizeof(buf));
  if (n == 0) return false;

  Serial.printf("[TX] %s\n", buf);
  return radioInviaConAck(buf, ack);
}

// ===========================================================================
//  Accodamento dei pacchetti non consegnati
// ===========================================================================

static const char   MARCA_ARRETRATO[] = ";bk=1";
static const size_t LUNG_MARCA        = sizeof(MARCA_ARRETRATO) - 1;

/*
 * Inserisce ";bk=1" subito dopo il prefisso: "GH1;v=2;..." -> "GH1;bk=1;v=2;...".
 *
 * E' cosi' che il ponte riconosce un record uscito dal backlog e lo pubblica
 * su serra/nodo/storico invece che sullo stato attuale. Prima lo capiva solo
 * dall'eta', oltre 40 minuti: un record del ciclo precedente, vecchio di 15,
 * passava per fresco e sovrascriveva in Home Assistant il pacchetto vero
 * arrivato un attimo prima. Nel recorder e' successo nove volte fra il 12 e
 * il 14/09, e il comando eventualmente allegato al suo ACK andava perso.
 *
 * In testa e non in coda perche' la serializzazione, se il pacchetto e'
 * troppo lungo, omette i campi in fondo: il marcatore non deve essere fra
 * quelli.
 */
static void marcaArretrato(char* riga) {
  char* sep = strchr(riga, PROTO_SEP);
  if (!sep) return;
  size_t n = strlen(riga);
  memmove(sep + LUNG_MARCA, sep, n - (size_t)(sep - riga) + 1);   // terminatore compreso
  memcpy(sep, MARCA_ARRETRATO, LUNG_MARCA);
}

/*
 * Accoda su microSD un pacchetto che il ponte non ha confermato.
 * Ritorna false se non ci e' riuscito, cioe' se il pacchetto e' perso.
 */
static bool accodaNonConsegnato(const PacchettoKV& p) {
  traccia(TAPPA_SD_ACCODA);

  // Si serializza lasciando libero lo spazio del marcatore: la riga finale
  // non supera mai PROTO_MAX_PAYLOAD, nemmeno con un pacchetto al limite.
  char buf[PROTO_MAX_PAYLOAD + 8];
  size_t n = p.serializza(NODE_ID, buf, PROTO_MAX_PAYLOAD + 1 - LUNG_MARCA);
  if (n == 0) return false;
  marcaArretrato(buf);

  if (backlogAccoda(buf)) {
    Serial.println(F("[BACKLOG] Pacchetto accodato su microSD."));
    return true;
  }

  // Prima questo caso stampava lo stesso "accodato": backlogAccoda() fallisce
  // in silenzio se la scheda non c'e', e il valore di ritorno era ignorato.
  Serial.println(F("[BACKLOG] ERRORE: accodamento fallito (microSD assente o guasta): "
                   "questo pacchetto e' PERSO."));
  return false;
}

// ===========================================================================
//  Gestione dei comandi ricevuti negli ACK
// ===========================================================================

/*
 * Esegue in catena i comandi che il ponte accoda agli ACK.
 * Ogni esito e' esso stesso un pacchetto, il cui ACK puo' portare il comando
 * successivo: cosi' una coda di piu' comandi si svuota in un solo risveglio,
 * senza bisogno di finestre di ascolto aggiuntive.
 */
static void gestisciComandi(RispostaAck& ack) {
  uint8_t eseguiti = 0;

  while (ack.ricevuto && eseguiti < MAX_CMD_PER_RISVEGLIO) {
    ComandoRicevuto cmd;
    comandoDaAck(ack, cmd);
    if (!cmd.presente) break;

    PacchettoKV extra;
    extra.reset();

    traccia(TAPPA_COMANDO);
    DateTime adesso = orologioAdesso();
    EsitoComando es = comandoEsegui(cmd, adesso, extra);
    eseguiti++;

    Serial.printf("[CMD] Esito id=%lu rc=%u (%s)\n",
                  (unsigned long)cmd.id, es.rc, es.dettaglio);

    if (es.restaSveglioSec > 0) g_wakeExtraSec = es.restaSveglioSec;

    // --- Pacchetto di esito, che vale anche come nuova richiesta di comandi ---
    PacchettoKV res;
    intestazione(res, orologioAdesso());
    res.aggiungiU("res", cmd.id);
    res.aggiungiU("rc",  es.rc);
    res.aggiungi ("det", es.dettaglio);
    for (uint8_t i = 0; i < extra.n(); i++)
      res.aggiungi(extra.campo(i).chiave, extra.campo(i).valore);

    /*
     * Qui NON si rileggono tutti i sensori: l'esito piu' il blocco di stato
     * riempirebbero il pacchetto oltre il tetto di 230 byte, e i campi in
     * eccesso verrebbero omessi — proprio quelli di stato, che sono in fondo.
     *
     * Il blocco di stato invece e' indispensabile: le entita' di comando di
     * Home Assistant (durata, orario, soglia, interruttore automatico) leggono
     * il loro valore da questo stesso topic, e senza quei campi tornerebbero
     * a zero per un istante prima di riprendersi.
     *
     * Le letture dei sensori arrivano comunque col pacchetto regolare
     * successivo: nel frattempo Home Assistant conserva l'ultimo valore buono,
     * perche' il template rende stringa vuota per le chiavi assenti e gli
     * aggiornamenti vuoti vengono scartati.
     */
    if (flussoDisponibile()) {
      res.aggiungiF("acqua",    irrigazioneLitriUltima(), 3);
      res.aggiungiF("acquaTot", g_cfg.litriTotali, 2);
    }
    aggiungiStato(res);

    traccia(TAPPA_TX_ESITO);
    RispostaAck ackRes;
    bool consegnato = inviaPacchetto(res, ackRes);

    if (!consegnato) {
      // L'esito non e' arrivato: lo si accoda come qualunque altro dato, e il
      // ponte lo pubblichera' su serra/nodo/cmd/res al prossimo aggancio.
      // Se il comando ha fatto scorrere l'acqua, il suo esito in "irr" va
      // anche ripetuto nei prossimi pacchetti: dallo storico HA non lo legge.
      if (irrigazioneEseguitaOra()) g_esitoIrrigPendente = 1;
      accodaNonConsegnato(res);
      break;
    }

    if (es.riavvia) {
      Serial.println(F("[CMD] Riavvio richiesto da Home Assistant."));
      impostazioniSalva();
      delay(200);
      ESP.restart();
    }

    ack = ackRes;   // l'ACK dell'esito puo' portare il comando successivo
  }

  if (eseguiti > 0) Serial.printf("[CMD] %u comandi eseguiti in questo risveglio.\n", eseguiti);
}

/*
 * Finestra di manutenzione richiesta dal comando WAKE: il nodo resta sveglio e
 * continua a chiedere comandi, cosi' si possono fare piu' regolazioni di
 * seguito senza aspettare un risveglio per ognuna.
 */
static void finestraManutenzione(uint32_t secondi) {
  Serial.printf("\n[WAKE] Finestra di manutenzione: resto sveglio %lu s.\n",
                (unsigned long)secondi);

  uint32_t fine = millis() + secondi * 1000UL;
  while ((int32_t)(fine - millis()) > 0) {
    wdtNutri();

    PacchettoKV ping;
    intestazione(ping, orologioAdesso());
    ping.aggiungiU("ping", 1);
    aggiungiStato(ping);

    RispostaAck ack;
    if (inviaPacchetto(ping, ack)) {
      assimilaAck(ack);
      g_wakeExtraSec = 0;              // azzerato: un nuovo WAKE puo' prorogarlo
      gestisciComandi(ack);
      if (g_wakeExtraSec > 0) fine = millis() + g_wakeExtraSec * 1000UL;
    }
    delay(5000);
  }
  Serial.println(F("[WAKE] Finestra di manutenzione terminata."));
}

// ===========================================================================
//  Backlog
// ===========================================================================

// Callback usata da backlogDrena(): consegna un singolo record storico.
static bool consegnaRecordStorico(const char* riga) {
  RispostaAck ack;
  bool ok = radioInviaConAck(riga, ack);
  if (ok) assimilaAck(ack);
  return ok;
}

// ===========================================================================
//  Deep sleep
// ===========================================================================

static void vaiInDeepSleep() {
  // Ultima tappa: al risveglio successivo dira' che il ciclo era finito bene.
  traccia(TAPPA_SLEEP);

  uint32_t sleepSec = g_cfg.sleepSec;

  // Allineamento all'intervallo: con l'ora esatta i risvegli cadono sempre
  // negli stessi istanti (:00 :15 :30 :45 con 900 s), il che rende i grafici
  // regolari e la finestra di irrigazione prevedibile.
  // Funziona quando sleepSec e' un divisore di 3600.
  if (orologioAttendibile() && sleepSec > 0 && (3600UL % sleepSec) == 0) {
    DateTime ora = orologioAdesso();
    uint32_t secNellOra = (uint32_t)ora.minute() * 60UL + ora.second();
    sleepSec = g_cfg.sleepSec - (secNellOra % g_cfg.sleepSec);
    if (sleepSec < SLEEP_MIN_SEC) sleepSec += g_cfg.sleepSec;
  }

  impostazioniSalva();
  flussoStacca();
  sensoriAlimenta(false);
  radioSpegni();

  Serial.printf("[SLEEP] Deep sleep per %lu secondi. Buonanotte.\n",
                (unsigned long)sleepSec);
  Serial.flush();

  // GPIO25 e' nel dominio RTC: mantiene ATTIVAMENTE il livello OFF del relay
  // per tutta la durata del sonno, invece di lasciarlo flottante.
  gpio_hold_en((gpio_num_t)PIN_RELAY);

  // Stesso trattamento per l'interruttore dei sensori: se e' un canale di
  // relay, lasciarlo flottante per 15 minuti significa rischiare che si
  // ecciti da solo e tenga i sensori sotto tensione per tutta la notte,
  // cioe' esattamente quello che l'alimentazione commutata deve evitare.
  // Funziona perche' anche GPIO 15 appartiene al dominio RTC.
#if PIN_PWR_SENSORI >= 0
  gpio_hold_en((gpio_num_t)PIN_PWR_SENSORI);
#endif

  gpio_deep_sleep_hold_en();

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

// ===========================================================================
//  SETUP — l'intero ciclo di vita del nodo
// ===========================================================================

void setup() {
  // (1) Stato sicuro PRIMA di qualunque altra cosa. Se l'ESP32 si e' appena
  //     riavviato per un watchdog durante l'irrigazione, la valvola si chiude
  //     entro pochi millisecondi dall'avvio.
  relayOffImmediato();

  Serial.begin(115200);
  delay(100);

  esp_reset_reason_t motivo = esp_reset_reason();

  // Scatola nera: va letta prima di qualunque altra cosa, perche' da qui
  // in avanti ogni tappa la sovrascrive.
  tracciaInit();
  const Tappa tappaPrec = tracciaPrecedente();
  if (tracciaMemoriaPersa()) {
    g_seq = 0;
    g_risvegli = 0;
    g_esitoIrrigPendente = 0;
    g_resetPendente = 0;
    g_resetMotivo = 0;
    g_resetTappa = 0;             // TAPPA_IGNOTA
    g_txpPrec = 0;                // 0 = non si sa: primo ciclo dopo la corrente
  }
  g_risvegli++;

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.printf ("  NODO SERRA %s - firmware %s\n", NODE_ID, FW_VERSION);
  Serial.printf ("  Reset: %s | risveglio #%lu | seq %lu\n",
                 wdtMotivoReset(motivo), (unsigned long)g_risvegli, (unsigned long)g_seq);
  if (motivo != ESP_RST_DEEPSLEEP && tappaPrec != TAPPA_IGNOTA)
    Serial.printf ("  Il ciclo precedente si era fermato a: %s\n", tracciaTesto(tappaPrec));
  Serial.println(F("============================================================"));

  // (2) Watchdog globale: senza, un blocco in SD.begin(), LoRa.begin() o sul
  //     bus I2C lascerebbe il nodo appeso a batteria fino a scaricarla.
  wdtImposta(WDT_SETUP_SEC);

  // Identita' della scheda. Stampata sempre, non solo in caso di problemi:
  // le schede a 38 pin montano di solito un modulo WROVER, la cui PSRAM
  // occupa i GPIO 16 e 17 rendendoli inservibili come I/O. Sapere subito su
  // che modulo si sta girando evita di inseguire un sensore che non legge.
  Serial.printf("  Chip: %s rev %d, %d core | PSRAM: %s\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                psramFound() ? "SI (WROVER: GPIO 16 e 17 NON usabili)" : "no");
  Serial.println(F("============================================================"));

  // (3) Impostazioni persistenti
  traccia(TAPPA_IMPOSTAZIONI);
  impostazioniCarica();
  impostazioniStampa();

  // (4) Orologio
  traccia(TAPPA_OROLOGIO);
  Wire.begin(I2C_SDA, I2C_SCL);
  orologioInit();
  DateTime adesso = orologioAdesso();   // UTC: e' l'ora dei timestamp e dei
  orologioStampa(adesso);               // conti fra istanti, non quella civile

  // Il giorno dei contatori e' quello LOCALE: sull'ora UTC "le irrigazioni di
  // oggi" si azzererebbero alle 2 del mattino, in mezzo alla notte italiana.
  if (orologioAttendibile())
    impostazioniNuovoGiorno(orologioGiorno(orologioLocale()));

  // Periferiche
  traccia(TAPPA_SENSORI);
  sensoriInit();

  traccia(TAPPA_SD);
  backlogInit();

  /*
   * Il backlog serve gia' qui: se l'irrigazione precedente e' stata interrotta
   * da un reset, la cosa va anche scritta su SD e non solo riferita a Home
   * Assistant, perche' e' il genere di evento che si capisce solo rileggendo
   * la sequenza di quello che e' successo intorno.
   */
  traccia(TAPPA_RECUPERO);
  g_irrigazioneDaRiferire = irrigazioneRecuperaInterrotta();

  traccia(TAPPA_RADIO);
  bool radioOk = radioInit();

  // (5) Umidita' del terreno PRIMA di decidere: e' la lettura che determina
  //     se serve irrigare. (Nel pacchetto viaggera' poi la lettura successiva
  //     all'irrigazione, utile per verificare che l'acqua sia arrivata.)
  traccia(TAPPA_SOIL);
  float soilMin = sensoriSoilMin();

  // (6) Irrigazione automatica
  //     L'orario programmato lo sceglie una persona guardando l'orologio di
  //     casa, quindi la decisione si prende sull'ora LOCALE. L'epoch che viene
  //     registrato resta UTC, come ogni altro istante del sistema.
  g_esitoIrrig = irrigazioneValuta(orologioLocale(), orologioAttendibile(), soilMin);

  if (g_esitoIrrig == IRR_OK) {
    traccia(TAPPA_IRRIGAZIONE);
    g_esitoIrrig = irrigazioneEsegui(g_cfg.irrigDurataSec, 0.0f, adesso.unixtime(),
                                     /*programmata=*/true);
  } else {
    Serial.printf("[IRRIG] Non irrigo: %s\n", irrigazioneEsitoTesto(g_esitoIrrig));
  }

  // (7) Lettura di tutti i sensori abilitati
  traccia(TAPPA_LETTURE);
  PacchettoKV pkt;
  intestazione(pkt, orologioAdesso());
  sensoriLeggiTutti(pkt);
  aggiungiStato(pkt);

  // Diagnostica: solo dopo un reset che non sia il risveglio dal deep sleep,
  // per non sprecare byte ogni volta. Resta pendente, e si ripete, finche' un
  // pacchetto che la contiene non viene confermato: il primo pacchetto dopo
  // un blocco e' proprio quello che rischia di piu' di non passare.
  if (motivo != ESP_RST_DEEPSLEEP) {
    g_resetPendente = 1;
    g_resetMotivo   = (uint8_t)motivo;
    g_resetTappa    = (uint8_t)tappaPrec;
  }

  if (g_resetPendente) {
    pkt.aggiungi ("fw",  FW_VERSION);
    pkt.aggiungiU("rst", g_resetMotivo);

    /*
     * In che punto del ciclo si era fermato il nodo prima di riavviarsi.
     * Senza questo campo un riavvio da watchdog dice solo CHE si e'
     * bloccato, mai DOVE: la differenza fra sapere e tirare a indovinare
     * fra trasmissione LoRa, microSD e bus I2C.
     */
    if ((Tappa)g_resetTappa != TAPPA_IGNOTA)
      pkt.aggiungi("tp", tracciaTesto((Tappa)g_resetTappa));
  }

  // In fondo perche' e' il campo meno importante: se il pacchetto fosse
  // troppo lungo, la serializzazione ometterebbe per primi gli ultimi.
  pkt.aggiungiU("txp", g_txpPrec);

  // (8) Trasmissione + comandi
  traccia(TAPPA_TX_STATO);
  RispostaAck ack;
  if (radioOk) g_linkOk = inviaPacchetto(pkt, ack);
  g_txpPrec = g_linkOk ? radioTentativiUltimoInvio() : (uint8_t)(TX_RETRIES + 1);

  if (g_linkOk) {
    // Gli eventi pendenti viaggiavano in questo pacchetto e sono arrivati.
    // Si azzerano PRIMA dei comandi, che possono generarne di nuovi.
    g_resetPendente      = 0;
    g_esitoIrrigPendente = 0;

    assimilaAck(ack);
    gestisciComandi(ack);
  } else {
    // Pacchetto non consegnato: finisce nel backlog e verra' ritrasmesso,
    // come record storico. L'esito di un'irrigazione che conteneva va
    // ripetuto nei prossimi pacchetti, o Home Assistant non lo vedrebbe.
    if (esitoIrrigDaRiferire()) g_esitoIrrigPendente = 1;
    accodaNonConsegnato(pkt);
  }

  // (9) Svuotamento del backlog: solo se il link e' vivo, altrimenti si
  //     sprecherebbe batteria per tentativi destinati a fallire.
  if (g_linkOk) {
    traccia(TAPPA_BACKLOG);
    uint32_t inCoda = backlogConta();
    if (inCoda > 0) {
      Serial.printf("[BACKLOG] %lu record da consegnare.\n", (unsigned long)inCoda);
      backlogDrena(consegnaRecordStorico);
    }
  }

  // Finestra di manutenzione, se richiesta con il comando WAKE
  if (g_wakeExtraSec > 0) {
    traccia(TAPPA_MANUTENZIONE);
    finestraManutenzione(g_wakeExtraSec);
  }

  // (10) Buonanotte
  vaiInDeepSleep();
}

void loop() {
  // Mai raggiunto: il nodo vive solo in setup() e poi torna in deep sleep.
  // Se per qualche motivo ci arrivasse, torna a dormire invece di restare
  // sveglio a consumare batteria.
  vaiInDeepSleep();
}
