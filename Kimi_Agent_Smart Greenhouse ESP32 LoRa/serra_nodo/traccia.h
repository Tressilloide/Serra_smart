/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : SCATOLA NERA
 * ============================================================================
 *
 *  Un byte che dice a che punto del ciclo il nodo si e' fermato l'ultima volta
 *  che si e' riavviato da solo.
 *
 *  Serve perche' un riavvio da watchdog non lascia nessuna traccia di dove sia
 *  successo: si vede solo che il nodo e' ripartito 180 secondi dopo, e da li'
 *  in poi si tira a indovinare fra la trasmissione LoRa, la microSD e il bus
 *  I2C. Con questo, al riavvio successivo il nodo lo dice esplicitamente.
 *
 *  Perche' RTC_NOINIT_ATTR e non RTC_DATA_ATTR: le variabili RTC_DATA_ATTR
 *  hanno un inizializzatore e il bootloader lo riapplica a OGNI avvio che non
 *  sia il risveglio dal deep sleep — proprio i casi che ci interessano. Quelle
 *  in RTC_NOINIT_ATTR invece non vengono toccate: sopravvivono a watchdog,
 *  panic e reset software, e si perdono solo togliendo corrente. E' anche il
 *  motivo per cui serve il numero magico: appena data corrente quella memoria
 *  contiene spazzatura, e senza un modo di riconoscerla il nodo riferirebbe
 *  una tappa inventata.
 *
 *  Costo: due variabili in memoria RTC e una scrittura in RAM per tappa.
 *  Nessuna scrittura in flash, quindi nessun consumo di cicli della NVS.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <esp_attr.h>

// Le tappe del ciclo di veglia, nell'ordine in cui il nodo le attraversa.
// I nomi in chiaro viaggiano nel campo "tp" del pacchetto, quindi vanno tenuti
// corti: il protocollo concede 20 caratteri per valore.
enum Tappa : uint8_t {
  TAPPA_IGNOTA      = 0,   // nessuna informazione (prima accensione)
  TAPPA_AVVIO       = 1,
  TAPPA_IMPOSTAZIONI,      // lettura della NVS
  TAPPA_OROLOGIO,          // I2C + DS1307
  TAPPA_SENSORI,           // I2C + BME280
  TAPPA_SD,                // montaggio della microSD
  TAPPA_RECUPERO,          // recupero di un'irrigazione interrotta (scrive su SD)
  TAPPA_RADIO,             // inizializzazione dell'SX1278
  TAPPA_SOIL,              // lettura umidita' terreno
  TAPPA_IRRIGAZIONE,       // valvola aperta
  TAPPA_LETTURE,           // lettura di tutti i sensori
  TAPPA_TX_STATO,          // trasmissione del pacchetto regolare
  TAPPA_COMANDO,           // esecuzione di un comando ricevuto nell'ACK
  TAPPA_TX_ESITO,          // trasmissione dell'esito del comando
  TAPPA_SD_ACCODA,         // scrittura di un pacchetto non consegnato
  TAPPA_BACKLOG,           // svuotamento della coda arretrata
  TAPPA_MANUTENZIONE,      // finestra WAKE
  TAPPA_SLEEP              // si va a dormire: fine regolare del ciclo
};

/*
 * Legge la tappa lasciata dal ciclo precedente e riparte da TAPPA_AVVIO.
 * Va chiamata una sola volta, il prima possibile dentro setup().
 */
void tracciaInit();

// Segna il punto raggiunto. Costa una scrittura in RAM.
void traccia(Tappa t);

/*
 * La tappa in cui si trovava il ciclo precedente.
 * Vale TAPPA_SLEEP dopo un ciclo finito bene, TAPPA_IGNOTA alla prima
 * accensione, e qualunque altra cosa dopo un riavvio anomalo: quella "altra
 * cosa" e' la risposta che si sta cercando.
 */
Tappa tracciaPrecedente();

// Nome breve della tappa, adatto a viaggiare nel pacchetto.
const char* tracciaTesto(Tappa t);

/*
 * true se la memoria RTC era da buttare, cioe' se e' appena tornata la
 * corrente. Chi tiene contatori in RTC_NOINIT_ATTR deve azzerarli qui: e'
 * l'unico momento in cui si sa che quello che c'e' dentro non significa
 * niente.
 */
bool tracciaMemoriaPersa();
