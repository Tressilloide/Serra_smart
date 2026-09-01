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
  uint32_t giornoCorrente;    // AAAAMMGG dell'ultimo aggiornamento
  uint8_t  irrigazioniOggi;
  float    litriOggi;
  uint32_t ultimaIrrigEpoch;  // per far rispettare IRRIG_MIN_INTERVALLO_M

  // --- Contatori cumulativi (mai azzerati) ---
  float    litriTotali;       // alimenta la statistica "total_increasing" in HA
  uint32_t backlogScartati;   // record persi per superamento di BACKLOG_MAX_BYTE

  // --- Calibrazioni (comando CAL) ---
  float    voltDivider;
  uint16_t soilSecco[4];
  uint16_t soilBagnato[4];
  float    flussoImpLitro;

  // --- Timing (comando SLEEP) ---
  uint32_t sleepSec;

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
