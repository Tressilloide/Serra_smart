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

/*
 * Ora corrente del DS1307, che conta in UTC perche' UTC e' quello che arriva
 * dal ponte. E' l'ora giusta per i timestamp dei pacchetti e per qualunque
 * conto fra istanti; NON e' l'ora da confrontare con un orario impostato da
 * una persona. Per quello c'e' orologioLocale().
 *
 * Se l'ora non e' attendibile ritorna comunque un DateTime valido, ma
 * orologioAttendibile() vale false e nessuna irrigazione partira'.
 */
DateTime orologioAdesso();

/*
 * Ora CIVILE: quella dell'orologio in cucina, ora legale compresa.
 *
 * E' orologioAdesso() piu' lo scarto del fuso che il ponte comunica a ogni
 * ACK. Va usata ovunque si confronti l'ora con qualcosa che ha scelto una
 * persona: l'orario di irrigazione impostato da Home Assistant e il cambio
 * di giorno dei contatori giornalieri.
 *
 * Il bug che questa separazione elimina: il nodo prendeva l'epoch UTC dal
 * ponte, lo scriveva nel DS1307 e poi confrontava rtc.now().hour() con
 * l'orario di Home Assistant. "Irriga alle 6:00" voleva dire le 6:00 UTC,
 * cioe' le 8:00 italiane d'estate: l'acqua arrivava ogni giorno due ore
 * dopo, e nei log l'unica traccia era un "fuori_orario" alle 6 del mattino.
 */
DateTime orologioLocale();

/*
 * Registra lo scarto fra ora civile e UTC comunicato dal ponte (secondi).
 * Viene salvato in NVS: il nodo deve poterne disporre anche al primo
 * risveglio dopo un blackout, prima di aver parlato con qualcuno.
 * Gli scarti implausibili vengono ignorati.
 */
void orologioImpostaFuso(int32_t offsetSec);

// Scarto attualmente in uso, in secondi.
int32_t orologioFuso();

/*
 * false finche' il ponte non ha mai dichiarato il fuso.
 *
 * Non e' deducibile da orologioFuso(): zero secondi e' uno scarto legittimo,
 * quindi dal solo numero non si distingue "siamo a UTC" da "non lo so".
 * Finche' vale false l'orario programmato non e' interpretabile.
 */
bool orologioFusoNoto();

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

// Data nel formato AAAAMMGG, usata per i contatori giornalieri.
// Va passata l'ora LOCALE, altrimenti i contatori si azzerano a mezzanotte UTC.
uint32_t orologioGiorno(const DateTime& d);

void orologioStampa(const DateTime& d);
