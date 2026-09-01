/*
 * ============================================================================
 *  SERRA SMART — PONTE : CODA DEI COMANDI VERSO LA SERRA
 * ============================================================================
 *
 *  ---------------------------------------------------------------------------
 *  IL PROBLEMA
 *  ---------------------------------------------------------------------------
 *  Il nodo in serra dorme quasi sempre. Quando premi un bottone in Home
 *  Assistant, la radio della serra e' spenta: il comando non puo' partire
 *  subito, va conservato finche' il nodo non si fa vivo.
 *
 *  ---------------------------------------------------------------------------
 *  LA SOLUZIONE: il broker MQTT E' la coda
 *  ---------------------------------------------------------------------------
 *  Home Assistant pubblica su serra/nodo/cmd/<OPCODE> con retain = true.
 *  Il broker conserva il messaggio ritenuto finche' qualcuno non lo cancella:
 *  la coda persiste da sola, sopravvive a un riavvio del ponte, e non serve
 *  scriverla su nessun file.
 *
 *  Il ponte si iscrive a serra/nodo/cmd/+, tiene una copia in RAM per l'accesso
 *  rapido e, quando consegna un comando al nodo, cancella il retained
 *  pubblicando un payload vuoto sullo stesso topic.
 *
 *  Un topic per opcode significa "l'ultimo vince" per tipo di comando: se
 *  cambi tre volte la durata dell'irrigazione prima che il nodo si svegli,
 *  arrivera' solo l'ultimo valore. E' esattamente il comportamento giusto.
 *
 *  ---------------------------------------------------------------------------
 *  SEMANTICA DI CONSEGNA
 *  ---------------------------------------------------------------------------
 *  Il comando viene rimosso dalla coda al momento della CONSEGNA, non alla
 *  ricezione dell'esito: al massimo una volta ("at-most-once"). Se l'esito si
 *  perde per strada, l'irrigazione NON viene ripetuta. E' la direzione sicura,
 *  coerente con il principio del progetto: meglio un'irrigazione mancata che
 *  una serra allagata.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include "config.h"

struct ComandoInCoda {
  uint32_t id;
  char     opcode[12];
  char     args[48];
};

void codaInit(PubSubClient* client);

// Chiamata dal callback MQTT per ogni messaggio su serra/nodo/cmd/+
void codaMessaggioMqtt(const char* topic, const uint8_t* payload, unsigned int len);

// true se c'e' almeno un comando da consegnare.
bool codaVuota();
uint8_t codaConta();

/*
 * Estrae il prossimo comando, gli assegna un id progressivo, lo rimuove dalla
 * coda e cancella il messaggio ritenuto sul broker.
 * Ritorna false se la coda e' vuota.
 */
bool codaEstrai(ComandoInCoda& out);

// Pubblica l'esito ricevuto dal nodo su serra/nodo/cmd/res.
void codaPubblicaEsito(uint32_t id, uint32_t rc, const char* dettaglio);

// Pubblica lo stato della coda su serra/nodo/cmd/pending (retained),
// cosi' la dashboard puo' mostrare "comando in attesa del prossimo risveglio".
void codaPubblicaPending();
