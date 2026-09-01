/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : COMANDI DA HOME ASSISTANT
 * ============================================================================
 *
 *  ---------------------------------------------------------------------------
 *  COME ARRIVA UN COMANDO A UN NODO CHE DORME
 *  ---------------------------------------------------------------------------
 *  Il nodo e' in deep sleep per il 99% del tempo: non puo' restare in ascolto.
 *  L'unico momento in cui la radio e' accesa e in ricezione e' l'attesa
 *  dell'ACK dopo aver trasmesso i dati. E' li' che infiliamo il comando.
 *
 *      nodo  --- dati ------------------------->  ponte
 *      nodo  <-- ACK;s=42;now=...;c=7;o=IRR;a=120  ponte
 *      nodo  [esegue]
 *      nodo  --- dati;res=7;rc=0 -------------->  ponte  --> MQTT
 *
 *  Costo aggiuntivo: zero. Nessuna finestra di ascolto in piu', nessun consumo
 *  extra di batteria, nessun pacchetto aggiuntivo nel caso normale.
 *
 *  Latenza: un comando viene eseguito al primo risveglio utile, quindi entro
 *  un intervallo di deep sleep (15 minuti con la configurazione di default).
 *  Home Assistant lo mostra come "in coda" finche' non arriva l'esito, cosi'
 *  l'attesa e' visibile e non sembra un malfunzionamento.
 *
 *  Poiche' anche il pacchetto di esito riceve un ACK, e anche quell'ACK puo'
 *  portare un comando, piu' comandi si svuotano a catena nello stesso
 *  risveglio (fino a MAX_CMD_PER_RISVEGLIO).
 *
 *  ---------------------------------------------------------------------------
 *  SICUREZZA
 *  ---------------------------------------------------------------------------
 *  Il collegamento LoRa e' in chiaro e non autenticato: e' una scelta esplicita.
 *  Di conseguenza NESSUN comando puo' superare i tetti compilati in config.h.
 *  Un IRR,99999 viene clampato a IRRIG_MAX_SEC; le irrigazioni giornaliere e i
 *  litri restano contingentati; l'intervallo minimo tra due irrigazioni vale
 *  anche per i comandi manuali. Il firmware e' l'ultima barriera e non si fida
 *  di cio' che arriva via radio.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <RTClib.h>
#include "protocollo.h"
#include "radio.h"

// Comando estratto da un ACK
struct ComandoRicevuto {
  bool     presente;
  uint32_t id;
  char     opcode[12];
  char     args[48];
};

// Effetti collaterali che il comando chiede al ciclo principale
struct EsitoComando {
  uint8_t  rc;              // RC_OK o uno dei codici in protocollo.h
  bool     riavvia;         // il nodo deve riavviarsi dopo aver inviato l'esito
  uint32_t restaSveglioSec; // > 0 = finestra di manutenzione richiesta
  char     dettaglio[24];   // testo breve per Home Assistant
};

// Estrae un eventuale comando dalla risposta del ponte.
void comandoDaAck(const RispostaAck& ack, ComandoRicevuto& out);

/*
 * Esegue il comando applicando i tetti di sicurezza.
 * I campi aggiuntivi da riportare a Home Assistant (litri erogati, valore
 * impostato, ...) vengono accodati a 'extra'.
 */
EsitoComando comandoEsegui(const ComandoRicevuto& cmd, const DateTime& adesso,
                           PacchettoKV& extra);
