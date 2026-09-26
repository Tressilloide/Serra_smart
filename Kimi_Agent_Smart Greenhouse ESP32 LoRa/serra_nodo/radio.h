/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : RADIO LoRa
 * ============================================================================
 *
 *  Il modulo LoRa sta su un bus SPI DEDICATO (HSPI: SCK 27, MISO 12, MOSI 13),
 *  separato da quello della microSD (VSPI: 18/19/23). Averli su bus distinti
 *  evita di dover alternare i chip select durante le trasmissioni ed elimina
 *  un'intera classe di problemi di temporizzazione.
 *
 *  ATTENZIONE HARDWARE (GPIO12): e' lo strapping pin MTDI. Se al reset si
 *  trova a livello alto, l'ESP32 configura VDD_SDIO a 1,8 V e la scheda puo'
 *  non avviarsi affatto. All'accensione il pin NSS del LoRa e' flottante,
 *  quindi l'SX1278 potrebbe pilotare la linea MISO proprio durante il boot.
 *  Rimedio: pulldown da 10k su GPIO12 e pull-up da 10k su GPIO5 (NSS).
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include "protocollo.h"

/*
 * Risposta del ponte a una trasmissione.
 *
 * Contiene i campi estratti, non il PacchettoKV completo: un PacchettoKV pesa
 * ~840 byte e il task che esegue setup() ha solo 8 KB di stack. Con tre
 * RispostaAck annidate (invio dati -> comando -> esito) si arrivava pericolo-
 * samente vicini all'overflow. L'ACK trasporta comunque solo questi campi.
 */
struct RispostaAck {
  bool     ricevuto;
  uint32_t seq;         // sequenza confermata
  uint32_t epochPonte;  // ora NTP del ponte (0 se non disponibile)

  /*
   * Scarto fra ora civile e UTC dichiarato dal ponte (campo "tz" dell'ACK).
   *
   * tzValido serve a distinguere "il ponte dice che siamo a UTC+0" da "il
   * ponte non l'ha detto": con un firmware del ponte piu' vecchio il campo
   * manca, e senza questo flag il nodo leggerebbe zero e tornerebbe a
   * ragionare in UTC senza che nessuno se ne accorga.
   */
  bool     tzValido;
  int32_t  tzOffsetSec;

  bool     haComando;
  uint32_t cmdId;
  char     opcode[12];
  char     args[48];

  int      rssi;
  float    snr;
};

bool radioInit();

/*
 * Invia un pacchetto e attende l'ACK corrispondente, con TX_RETRIES tentativi
 * e backoff crescente. Ritorna true solo se il ponte ha confermato, il che
 * significa che il dato e' effettivamente arrivato al broker MQTT: se manca
 * l'ACK il pacchetto va nel backlog e nulla viene perso.
 */
bool radioInviaConAck(const char* pacchetto, RispostaAck& out);

/*
 * Quanti tentativi ha richiesto l'ultima radioInviaConAck(): da 1 a
 * TX_RETRIES se il ponte ha confermato, TX_RETRIES + 1 se non ha confermato
 * nessuno. Alimenta il campo "txp" del pacchetto successivo.
 */
uint8_t radioTentativiUltimoInvio();

// Mette la radio in sleep prima del deep sleep dell'ESP32.
void radioSpegni();
