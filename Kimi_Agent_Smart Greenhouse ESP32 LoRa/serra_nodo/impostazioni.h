/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : IMPOSTAZIONI PERSISTENTI (NVS)
 * ============================================================================
 *
 *  Tutto cio' che Home Assistant puo' cambiare da remoto vive qui, nella
 *  memoria non volatile dell'ESP32 (NVS, partizione dedicata in flash).
 *
 *  Perche' NVS e non la microSD:
 *    - sopravvive al deep sleep, ai reset del watchdog E ai blackout
 *    - non dipende dalla scheda SD: prima il marker "oggi ho gia' irrigato"
 *      stava solo su SD, quindi senza SD l'irrigazione non partiva MAI
 *    - le scritture sono wear-levelled dal driver
 *
 *  Perche' non RTC_DATA_ATTR: la RAM RTC si azzera a ogni interruzione di
 *  alimentazione, e proprio dopo un blackout e' il momento in cui e' piu'
 *  importante ricordarsi se la valvola era gia' stata aperta oggi.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include "config.h"

struct Impostazioni {
  // --- Pianificazione irrigazione (comando SCHED / AUTO) ---
  uint8_t  irrigOra;
  uint8_t  irrigMinuto;
  uint16_t irrigDurataSec;
  bool     irrigAuto;

  // --- Irrigazione condizionata (comando SOIL) ---
  int16_t  soilSoglia;        // % sotto la quale irrigare. -1 = disattivata

  // --- Stato giornaliero (azzerato al cambio di data) ---
  uint32_t giornoCorrente;    // AAAAMMGG (ora locale) dell'ultimo aggiornamento
  uint8_t  irrigazioniOggi;
  float    litriOggi;
  uint32_t ultimaIrrigEpoch;  // per far rispettare IRRIG_MIN_INTERVALLO_M

  /*
   * Giorno in cui l'irrigazione PROGRAMMATA e' gia' partita.
   *
   * Non basta irrigazioniOggi, che conta anche le manuali: senza questa
   * distinzione la finestra di recupero non saprebbe dire se l'appuntamento
   * di oggi e' stato onorato o se l'unica irrigazione della giornata e' stata
   * un "Irriga ora" premuto a mano. E' questo campo a rendere sicura una
   * finestra larga: l'appuntamento vale una volta al giorno, punto.
   */
  uint32_t giornoProgrammata;

  // --- Contatori cumulativi (mai azzerati) ---
  float    litriTotali;       // alimenta la statistica "total_increasing" in HA
  /*
   * Litri dell'ULTIMA irrigazione. Sta in NVS e non fra le variabili normali
   * perche' ogni risveglio da deep sleep e' un avvio da zero: una variabile
   * static tornerebbe a 0, e il primo pacchetto regolare dopo l'irrigazione
   * sovrascriverebbe in Home Assistant il valore buono con uno zero.
   */
  float    litriUltima;
  uint32_t backlogScartati;   // record persi per superamento di BACKLOG_MAX_BYTE

  /*
   * Com'e' finita l'ULTIMA irrigazione davvero eseguita (un EsitoIrrigazione),
   * e se in questo momento la valvola risulta aperta.
   *
   * Vengono scritti PRIMA di aprire, con il valore pessimistico "interrotta":
   * se il nodo si resetta a valvola aperta — e succede — al riavvio trova
   * scritto cosa stava facendo, lo dice a Home Assistant e non fa sparire
   * l'irrigazione nel nulla. Sono anche l'unico modo perche' un "Irriga ora"
   * comparisca nello storico: l'esito del comando viaggia in un pacchetto
   * solo, e se quel pacchetto si perde non resta traccia di nulla.
   */
  uint8_t  esitoUltima;
  uint8_t  irrigInCorso;

  // --- Calibrazioni (comando CAL) ---
  float    voltDivider;
  uint16_t soilSecco[4];
  uint16_t soilBagnato[4];
  float    flussoImpLitro;

  // --- Timing (comando SLEEP) ---
  uint32_t sleepSec;

  /*
   * Secondi da sommare all'ora del DS1307 per ottenere l'ora civile.
   *
   * Il DS1307 conta in UTC, perche' UTC e' quello che arriva dal ponte, ed e'
   * giusto cosi': i timestamp dei pacchetti devono restare UTC. Questo scarto
   * serve solo a interpretare l'orario programmato da Home Assistant come ora
   * italiana. Sta in NVS e non fra le variabili normali perche' ogni risveglio
   * da deep sleep e' un avvio da zero, e il primo risveglio dopo un blackout
   * potrebbe dover decidere se irrigare prima ancora di sentire il ponte.
   */
  int32_t  tzOffsetSec;

  /*
   * 0 finche' il ponte non ha mai dichiarato il fuso.
   *
   * Non e' ridondante rispetto a tzOffsetSec: uno scarto di zero secondi e' un
   * valore legittimo (Londra d'inverno), quindi dal solo numero non si
   * distingue "siamo a UTC" da "non lo so ancora". La differenza conta,
   * perche' nel secondo caso l'orario programmato non e' interpretabile e
   * l'irrigazione automatica deve aspettare invece di tirare a indovinare.
   */
  uint8_t  tzNoto;

  // --- Abilitazione sensori (comando SENS): bitmask, 1 bit per sensore ---
  uint32_t sensoriAbilitati;
};

extern Impostazioni g_cfg;

// Carica da NVS applicando i default di config.h alle chiavi mai scritte.
void impostazioniCarica();

// Scrive su NVS solo se qualcosa e' cambiato davvero (risparmia cicli di flash).
void impostazioniSalva();

// Marca le impostazioni come "da salvare" al prossimo impostazioniSalva().
void impostazioniModificate();

// Riporta tutto ai default di config.h (comando RESETCFG).
void impostazioniReset();

/*
 * Azzera i contatori giornalieri se e' cambiato il giorno.
 * Va chiamata dopo aver letto l'RTC e prima di decidere se irrigare.
 * Ritorna true se il giorno e' effettivamente cambiato.
 */
bool impostazioniNuovoGiorno(uint32_t aaaammgg);

void impostazioniStampa();
