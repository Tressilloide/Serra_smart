/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : WATCHDOG
 * ============================================================================
 *
 *  Wrapper sul Task Watchdog Timer (TWDT) che funziona sia con il core ESP32
 *  2.x (IDF 4.x) sia con il 3.x (IDF 5.x), le cui API sono incompatibili.
 *
 *  Nota sul bug che questo file corregge: nel core 3.x il TWDT e' GIA' avviato
 *  dal framework Arduino, quindi esp_task_wdt_init() ritorna ESP_ERR_INVALID_STATE
 *  e non fa nulla. Il codice precedente ignorava il valore di ritorno, quindi
 *  durante l'irrigazione il timeout restava quello di default (5 s) invece dei
 *  minuti richiesti. Qui, se l'init fallisce perche' e' gia' attivo, si passa a
 *  esp_task_wdt_reconfigure(), che e' l'API corretta in quel caso.
 *
 *  Il watchdog copre TUTTO il setup(), non solo l'irrigazione: un blocco su
 *  SD.begin(), LoRa.begin() o sul bus I2C lascerebbe altrimenti il nodo appeso
 *  per sempre, a batteria che si scarica e senza mai irrigare.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>

#ifndef ESP_ARDUINO_VERSION_VAL
  #define ESP_ARDUINO_VERSION_VAL(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#endif
#ifndef ESP_ARDUINO_VERSION
  #define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(2, 0, 0)
#endif

// Imposta (o riconfigura) il timeout del watchdog e iscrive il task corrente.
inline void wdtImposta(uint32_t secondi) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = secondi * 1000UL;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;

  // Si prova PRIMA a riconfigurare e solo dopo a inizializzare.
  // Nel core 3.x il TWDT e' gia' avviato dal framework, quindi chiamare
  // init() per primo fallisce sempre e stampa un "E (...) task_wdt: TWDT
  // already initialized" rosso sul seriale a ogni irrigazione: sembra un
  // guasto grave e invece e' rumore. Invertendo l'ordine il caso normale
  // e' silenzioso.
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);
  if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&cfg);
  if (e != ESP_OK)
    Serial.printf("[WDT] Configurazione fallita (err=%d)\n", (int)e);
#else
  esp_task_wdt_init(secondi, true);
#endif

  // Stesso motivo: iscrivere un task gia' iscritto stampa un errore rosso.
  if (esp_task_wdt_status(NULL) != ESP_OK) esp_task_wdt_add(NULL);
}

// Da chiamare periodicamente nei cicli lunghi ("nutrire il cane da guardia").
inline void wdtNutri() {
  esp_task_wdt_reset();
}

// Disiscrive il task corrente SENZA smontare il TWDT globale.
// (Il codice precedente chiamava esp_task_wdt_deinit() a fine irrigazione,
//  disattivando il watchdog per tutto il resto del ciclo.)
inline void wdtRimuoviTask() {
  esp_task_wdt_delete(NULL);
}

// Testo leggibile del motivo dell'ultimo reset, utile in Home Assistant per
// accorgersi che il nodo si sta riavviando da solo.
inline const char* wdtMotivoReset(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "accensione";
    case ESP_RST_EXT:      return "reset esterno";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic/eccezione";
    case ESP_RST_INT_WDT:  return "watchdog interrupt";
    case ESP_RST_TASK_WDT: return "watchdog task";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (alimentazione insufficiente)";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "sconosciuto";
  }
}
