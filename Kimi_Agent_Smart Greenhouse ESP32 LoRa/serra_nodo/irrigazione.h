/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : IRRIGAZIONE
 * ============================================================================
 *
 *  Principio guida, ereditato dal progetto originale e mantenuto:
 *  MEGLIO UN GIORNO SENZA IRRIGAZIONE CHE UNA SERRA ALLAGATA.
 *  In ogni situazione ambigua il codice sceglie di NON aprire la valvola.
 *
 *  ---------------------------------------------------------------------------
 *  PROTEZIONI, dalla piu' esterna alla piu' interna
 *  ---------------------------------------------------------------------------
 *  1. relayOffImmediato() e' la primissima istruzione del setup(): dopo
 *     qualunque reset (watchdog, brownout, panic) la valvola si chiude entro
 *     pochi millisecondi dall'avvio.
 *  2. gpio_hold_en() sul pin del relay durante il deep sleep: GPIO25 e' nel
 *     dominio RTC e mantiene attivamente il livello OFF mentre l'ESP32 dorme.
 *  3. Il contatore in NVS viene aggiornato PRIMA di aprire la valvola: se il
 *     nodo muore a valvola aperta, al riavvio sa di aver gia' irrigato e non
 *     ci riprova.
 *  4. Watchdog hardware armato per durata + margine: se il codice si impalla
 *     l'ESP32 si resetta e si ricade nella protezione 1.
 *  5. Timeout software con millis() nel ciclo di attesa.
 *  6. Tetti compilati in config.h che nessun comando remoto puo' superare:
 *     durata massima, intervallo minimo, numero massimo giornaliero, budget litri.
 *  7. Con il flussometro: chiusura immediata al raggiungimento del budget
 *     giornaliero di litri, anche a meta' irrigazione.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <RTClib.h>
#include "config.h"

enum EsitoIrrigazione : uint8_t {
  IRR_OK = 0,               // Eseguita
  IRR_NO_ORARIO,            // Non e' l'ora programmata
  IRR_NO_AUTO,              // Irrigazione automatica disattivata
  IRR_NO_GIA_FATTA,         // Raggiunto il numero massimo giornaliero
  IRR_NO_INTERVALLO,        // Troppo presto rispetto all'ultima irrigazione
  IRR_NO_TERRENO,           // Terreno gia' abbastanza umido
  IRR_NO_BUDGET,            // Budget litri giornaliero esaurito
  IRR_NO_RTC,               // Ora non attendibile: non si rischia
  IRR_ERR_FLUSSO            // Eseguita ma senza flusso d'acqua rilevato
};

// Primissima istruzione del setup(): porta il relay in stato sicuro.
void relayOffImmediato();

/*
 * Decide se far partire l'irrigazione automatica in questo risveglio.
 * soilMin = umidita' minima tra i sensori terreno (NAN se non disponibile).
 * Non apre nulla: ritorna solo la decisione e il motivo.
 */
EsitoIrrigazione irrigazioneValuta(const DateTime& adesso, bool rtcAttendibile, float soilMin);

/*
 * Esegue l'irrigazione applicando TUTTI i tetti di sicurezza.
 *   durataSec   : durata richiesta, viene clampata a IRRIG_MAX_SEC
 *   litriTarget : se > 0 chiude appena raggiunti i litri (irrigazione
 *                 volumetrica); durataSec resta comunque il tetto massimo
 *   epoch       : ora corrente, per registrare l'ultima irrigazione
 * Ritorna IRR_OK, IRR_ERR_FLUSSO, oppure il motivo del rifiuto.
 */
EsitoIrrigazione irrigazioneEsegui(uint32_t durataSec, float litriTarget, uint32_t epoch);

// Litri erogati dall'ultima irrigazione eseguita in questo ciclo di veglia.
float irrigazioneLitriUltima();

// Secondi effettivamente durati dall'ultima irrigazione di questo ciclo.
uint32_t irrigazioneDurataUltima();

// true se in questo ciclo di veglia e' stata eseguita un'irrigazione.
bool irrigazioneEseguitaOra();

// Descrizione breve dell'esito, da inviare a Home Assistant.
const char* irrigazioneEsitoTesto(EsitoIrrigazione e);
