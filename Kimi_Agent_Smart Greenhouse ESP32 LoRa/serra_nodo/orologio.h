/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : OROLOGIO (DS1307)
 * ============================================================================
 *
 *  L'ora corretta e' un requisito di sicurezza, non una comodita': tutta la
 *  logica anti-allagamento (una irrigazione al giorno, intervallo minimo,
 *  contatori giornalieri) si basa sul sapere che ore sono davvero.
 *
 *  ---------------------------------------------------------------------------
 *  DIFFERENZA IMPORTANTE RISPETTO ALLA VERSIONE PRECEDENTE
 *  ---------------------------------------------------------------------------
 *  Prima, se il DS1307 aveva perso l'ora (CR2032 scarica), il codice faceva
 *  rtc.adjust(DATE, TIME) impostando l'ora di COMPILAZIONE. Il risultato era
 *  un orologio che sembrava funzionante ma segnava un momento arbitrario nel
 *  passato: i timestamp in Home Assistant erano falsi e, peggio, la finestra
 *  di irrigazione poteva scattare in un momento qualunque.
 *
 *  Ora, se l'ora non e' attendibile il nodo lo dichiara, NON irriga (meglio
 *  saltare un giorno che allagare) e aspetta la sincronizzazione: il ponte
 *  allega l'ora NTP corrente ("now=") a ogni ACK, quindi bastano pochi secondi
 *  dal primo contatto perche' l'orologio si rimetta a posto da solo, senza
 *  nessun intervento manuale.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <RTClib.h>

// Inizializza il DS1307 sul bus I2C (Wire.begin() gia' fatto dal chiamante).
bool orologioInit();

// Ora corrente. Se non attendibile ritorna comunque un DateTime valido,
// ma orologioAttendibile() vale false e nessuna irrigazione partira'.
DateTime orologioAdesso();

/*
 * true solo se il DS1307 risponde, sta contando e segna una data plausibile
 * (anno >= 2024). Serve a distinguere "ora vera" da "ora inventata".
 */
bool orologioAttendibile();

// Imposta l'orologio a un timestamp Unix (comando TIME, o sync dal ponte).
bool orologioImposta(uint32_t epoch);

/*
 * Confronta l'ora locale con quella dichiarata dal ponte e corregge il DS1307
 * se la deriva supera RTC_DRIFT_MAX_SEC (o se l'ora locale non e' attendibile).
 * Ritorna true se l'orologio e' stato effettivamente aggiornato.
 */
bool orologioSincronizza(uint32_t epochPonte);

// Data odierna nel formato AAAAMMGG, usato per i contatori giornalieri.
uint32_t orologioGiorno(const DateTime& d);

void orologioStampa(const DateTime& d);
