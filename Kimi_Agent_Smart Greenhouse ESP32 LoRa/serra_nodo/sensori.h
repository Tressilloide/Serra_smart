/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : REGISTRO SENSORI MODULARE
 * ============================================================================
 *
 *  ---------------------------------------------------------------------------
 *  COME AGGIUNGERE UN SENSORE NUOVO  (la parte che rende la serra modulare)
 *  ---------------------------------------------------------------------------
 *
 *  1. Scrivi in sensori.cpp una funzione di lettura:
 *
 *         static float leggiIlMioSensore(void* ctx) {
 *           if (!disponibile) return NAN;   // NAN = "non disponibile"
 *           return valore;
 *         }
 *
 *     (facoltativa: una funzione di init con la stessa forma che ritorna bool)
 *
 *  2. Aggiungi UNA riga alla tabella SENSORI[] in sensori.cpp:
 *
 *         { "uv", BIT_UV, 2, initIlMioSensore, leggiIlMioSensore, nullptr },
 *
 *  3. Aggiungi il bit corrispondente all'enum BitSensore qui sotto.
 *
 *  Fatto. Non devi toccare ne' il pacchetto LoRa, ne' il backlog su SD, ne' il
 *  firmware del ponte: il sensore appare da solo in Home Assistant grazie alla
 *  discovery generica. Per dargli nome ed unita' leggibili, aggiungi poi una
 *  riga alla tabella in camera_ponte/discovery.cpp (facoltativo).
 *
 *  Se il sensore e' analogico non serve nemmeno una funzione nuova: riusa
 *  leggiPercentuale() passando un CtxAnalogico, come fanno i sensori terreno.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include "config.h"
#include "protocollo.h"

// ---------------------------------------------------------------------------
//  Bit di abilitazione (rispecchiati in g_cfg.sensoriAbilitati, comando SENS)
// ---------------------------------------------------------------------------

enum BitSensore : uint8_t {
  BIT_TEMP = 0,
  BIT_HUM,
  BIT_PRES,
  BIT_LUCE,
  BIT_VOLT,
  BIT_SOIL1,
  BIT_SOIL2,
  BIT_SOIL3,
  BIT_SOIL4,
  BIT_ACQUA,
  BIT_ACQUA_TOT,
  // <-- i bit dei sensori futuri vanno qui
  N_BIT_SENSORI
};

// ---------------------------------------------------------------------------
//  Contesto riutilizzabile per qualunque sensore analogico
//  (sensori terreno oggi; domani UV, piranometro, pH, EC...)
// ---------------------------------------------------------------------------

struct CtxAnalogico {
  uint8_t canale;      // GPIO ADC1, oppure canale 0-3 dell'ADS1115 se USA_ADS1115
  uint8_t indiceCal;   // indice nelle tabelle di calibrazione in NVS (0-3)
  bool    invertito;   // true se il valore grezzo CRESCE quando e' bagnato
};

// ---------------------------------------------------------------------------
//  Descrittore di un sensore
// ---------------------------------------------------------------------------

struct DescrittoreSensore {
  const char* chiave;                  // chiave nel pacchetto LoRa (max 9 char)
  uint8_t     bit;                     // posizione in g_cfg.sensoriAbilitati
  uint8_t     decimali;                // cifre decimali nella trasmissione
  bool      (*init)(void* ctx);        // nullptr se non serve inizializzazione
  float     (*leggi)(void* ctx);       // ritorna NAN se non disponibile
  void*       ctx;                     // configurazione specifica del sensore
};

extern DescrittoreSensore SENSORI[];
extern const uint8_t N_SENSORI;

// ---------------------------------------------------------------------------
//  API
// ---------------------------------------------------------------------------

// Inizializza tutti i sensori abilitati. Wire.begin() dev'essere gia' stato
// chiamato dal chiamante. Ritorna il numero di sensori pronti.
uint8_t sensoriInit();

// Umidita' del sensore terreno piu' secco (NAN se nessuno disponibile).
// Va chiamata PRIMA di decidere se irrigare.
float sensoriSoilMin();

// Scarta le letture del terreno memorizzate, forzando una nuova misura.
// Da chiamare dopo un'irrigazione: e' l'unico momento in cui il valore cambia
// davvero, e rileggere serve a verificare che l'acqua sia arrivata.
void sensoriInvalidaCache();

// Legge tutti i sensori abilitati e li accoda al pacchetto come chiave=valore.
// I sensori assenti vengono inviati come -127 (sentinella "non disponibile").
void sensoriLeggiTutti(PacchettoKV& pkt);

// Accende/spegne il rail dei sensori tramite PIN_PWR_SENSORI (relay o MOSFET).
// I sensori terreno resistivi si corrodono se tenuti sempre alimentati:
// vengono accesi solo per il tempo della misura.
// Se PIN_PWR_SENSORI vale -1 la funzione non fa nulla e i sensori restano
// costantemente alimentati: e' la configurazione attuale.
void sensoriAlimenta(bool acceso);

// Cerca un sensore per chiave. Ritorna l'indice o -1.
int8_t sensoreIndiceDaChiave(const char* chiave);

bool sensoreAbilitato(uint8_t bit);
void sensoreImpostaAbilitato(uint8_t bit, bool abilitato);

// ---------------------------------------------------------------------------
//  Flussometro (usato anche da irrigazione.cpp)
// ---------------------------------------------------------------------------

bool     flussoDisponibile();
void     flussoAzzera();          // azzera il contatore e attacca l'interrupt
void     flussoStacca();          // stacca l'interrupt (risparmio in deep sleep)
uint32_t flussoImpulsi();         // impulsi contati dall'ultimo flussoAzzera()

// Diagnostica: conta i cambi di livello sul pin campionandolo a raffica per
// durataMs, SENZA passare dall'interrupt. Distingue "nessun segnale sul filo"
// da "segnale presente ma interrupt che non lo raccoglie".
uint32_t flussoSondaTransizioni(uint16_t durataMs);
float    flussoLitri();           // litri corrispondenti, secondo la calibrazione

// ---------------------------------------------------------------------------
//  Lettura di un canale analogico (nasconde ADC nativo vs ADS1115)
// ---------------------------------------------------------------------------

uint16_t leggiCanaleRaw(uint8_t canale);
float    leggiCanaleMilliVolt(uint8_t canale);
