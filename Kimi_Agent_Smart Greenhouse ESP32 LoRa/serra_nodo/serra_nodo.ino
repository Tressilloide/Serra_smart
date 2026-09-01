/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA
 *  ESP32 + LoRa Ra-01 + BME280 + DS1307 + microSD + relay irrigazione
 *  + sensori umidita' terreno + flussometro YF-S201
 * ============================================================================
 *
 *  Firmware 2.0 — vedi docs/Guida_Serra_Smart.md e docs/PROTOCOLLO.md
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

RTC_DATA_ATTR uint32_t g_seq         = 0;   // contatore pacchetti
RTC_DATA_ATTR uint32_t g_risvegli    = 0;   // risvegli dall'ultima accensione

// ---------------------------------------------------------------------------
//  Stato del ciclo corrente
// ---------------------------------------------------------------------------

static EsitoIrrigazione g_esitoIrrig = IRR_NO_ORARIO;
static bool             g_linkOk     = false;
static uint32_t         g_wakeExtraSec = 0;

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

  pkt.aggiungi ("irr",   irrigazioneEsitoTesto(g_esitoIrrig));
  pkt.aggiungiU("bl",    backlogConta());
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

    RispostaAck ackRes;
    bool consegnato = inviaPacchetto(res, ackRes);

    if (!consegnato) {
      // L'esito non e' arrivato: lo si accoda come qualunque altro dato.
      // Home Assistant lo vedra' al prossimo aggancio del link.
      char buf[PROTO_MAX_PAYLOAD + 8];
      res.serializza(NODE_ID, buf, sizeof(buf));
      backlogAccoda(buf);
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
  if (ok && ack.epochPonte > 0) orologioSincronizza(ack.epochPonte);
  return ok;
}

// ===========================================================================
//  Deep sleep
// ===========================================================================

static void vaiInDeepSleep() {
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
  g_risvegli++;

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.printf ("  NODO SERRA %s — firmware %s\n", NODE_ID, FW_VERSION);
  Serial.printf ("  Reset: %s | risveglio #%lu | seq %lu\n",
                 wdtMotivoReset(motivo), (unsigned long)g_risvegli, (unsigned long)g_seq);
  Serial.println(F("============================================================"));

  // (2) Watchdog globale: senza, un blocco in SD.begin(), LoRa.begin() o sul
  //     bus I2C lascerebbe il nodo appeso a batteria fino a scaricarla.
  wdtImposta(WDT_SETUP_SEC);

  // Avviso sui moduli con PSRAM, dove GPIO16/17 non sono liberi.
#if (PIN_FLUSSO == 16 || PIN_FLUSSO == 17 || PIN_PWR_SENSORI == 16 || PIN_PWR_SENSORI == 17)
  if (psramFound()) {
    Serial.println(F("[HW] ATTENZIONE: questa scheda ha PSRAM (modulo WROVER)."));
    Serial.println(F("[HW] I GPIO 16 e 17 sono usati dalla PSRAM e NON sono disponibili."));
    Serial.println(F("[HW] In config.h usa PIN_FLUSSO 15 e PIN_PWR_SENSORI 2."));
  }
#endif

  // (3) Impostazioni persistenti
  impostazioniCarica();
  impostazioniStampa();

  // (4) Orologio
  Wire.begin(I2C_SDA, I2C_SCL);
  orologioInit();
  DateTime adesso = orologioAdesso();
  orologioStampa(adesso);

  if (orologioAttendibile())
    impostazioniNuovoGiorno(orologioGiorno(adesso));

  // Periferiche
  sensoriInit();
  backlogInit();
  bool radioOk = radioInit();

  // (5) Umidita' del terreno PRIMA di decidere: e' la lettura che determina
  //     se serve irrigare. (Nel pacchetto viaggera' poi la lettura successiva
  //     all'irrigazione, utile per verificare che l'acqua sia arrivata.)
  float soilMin = sensoriSoilMin();

  // (6) Irrigazione automatica
  g_esitoIrrig = irrigazioneValuta(adesso, orologioAttendibile(), soilMin);

  if (g_esitoIrrig == IRR_OK) {
    g_esitoIrrig = irrigazioneEsegui(g_cfg.irrigDurataSec, 0.0f, adesso.unixtime());
  } else {
    Serial.printf("[IRRIG] Non irrigo: %s\n", irrigazioneEsitoTesto(g_esitoIrrig));
  }

  // (7) Lettura di tutti i sensori abilitati
  PacchettoKV pkt;
  intestazione(pkt, orologioAdesso());
  sensoriLeggiTutti(pkt);
  aggiungiStato(pkt);

  // Diagnostica: solo dopo un reset anomalo, per non sprecare byte ogni volta
  if (motivo != ESP_RST_DEEPSLEEP) {
    pkt.aggiungi ("fw",  FW_VERSION);
    pkt.aggiungiU("rst", (uint32_t)motivo);
  }

  // (8) Trasmissione + comandi
  RispostaAck ack;
  if (radioOk) {
    g_linkOk = inviaPacchetto(pkt, ack);

    if (g_linkOk) {
      // L'ora del ponte viaggia su ogni ACK: l'orologio si corregge da solo,
      // senza bisogno di comandi manuali ne' di ricompilare lo sketch.
      if (ack.epochPonte > 0 && orologioSincronizza(ack.epochPonte)) {
        DateTime nuova = orologioAdesso();
        impostazioniNuovoGiorno(orologioGiorno(nuova));
      }

      gestisciComandi(ack);
    }
  }

  // Pacchetto non consegnato: finisce nel backlog e verra' ritrasmesso.
  if (!g_linkOk) {
    char buf[PROTO_MAX_PAYLOAD + 8];
    pkt.serializza(NODE_ID, buf, sizeof(buf));
    backlogAccoda(buf);
    Serial.println(F("[BACKLOG] Pacchetto accodato su microSD."));
  }

  // (9) Svuotamento del backlog: solo se il link e' vivo, altrimenti si
  //     sprecherebbe batteria per tentativi destinati a fallire.
  if (g_linkOk) {
    uint32_t inCoda = backlogConta();
    if (inCoda > 0) {
      Serial.printf("[BACKLOG] %lu record da consegnare.\n", (unsigned long)inCoda);
      backlogDrena(consegnaRecordStorico);
    }
  }

  // Finestra di manutenzione, se richiesta con il comando WAKE
  if (g_wakeExtraSec > 0) finestraManutenzione(g_wakeExtraSec);

  // (10) Buonanotte
  vaiInDeepSleep();
}

void loop() {
  // Mai raggiunto: il nodo vive solo in setup() e poi torna in deep sleep.
  // Se per qualche motivo ci arrivasse, torna a dormire invece di restare
  // sveglio a consumare batteria.
  vaiInDeepSleep();
}
