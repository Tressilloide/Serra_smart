/*
 * ============================================================================
 *  SERRA SMART — PONTE : MQTT DISCOVERY PER HOME ASSISTANT
 * ============================================================================
 *
 *  ---------------------------------------------------------------------------
 *  DUE LIVELLI, PERCHE' LA SERRA SIA DAVVERO MODULARE
 *  ---------------------------------------------------------------------------
 *
 *  1. TABELLA DESCRITTIVA (ENTITA[] in discovery.cpp)
 *     Per le chiavi conosciute: nome leggibile, unita' di misura, device_class,
 *     state_class, icona. E' quello che rende la dashboard presentabile.
 *
 *  2. FALLBACK GENERICO
 *     Se dal nodo arriva una chiave che NON e' in tabella, il ponte pubblica
 *     comunque una entita' (nome = chiave, nessuna unita', measurement).
 *
 *  Il secondo livello e' il punto chiave: puoi aggiungere un sensore alla serra
 *  modificando SOLO il firmware del nodo, e lo vedi comparire in Home Assistant
 *  senza toccare ne' il ponte ne' file YAML. Quando poi vuoi l'etichetta bella
 *  e l'unita' giusta, aggiungi una riga alla tabella e riflashi il ponte —
 *  ma nel frattempo il dato non si perde ed e' gia' storicizzato.
 *
 *  Le entita' di comando (bottoni, numeri, interruttori) sono pubblicate una
 *  volta a ogni riconnessione: sono quelle che permettono a Home Assistant di
 *  MANDARE comandi alla serra invece di limitarsi a leggerla.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

void discoveryInit(PubSubClient* client);

// Azzera la cache delle chiavi gia' annunciate: da chiamare a ogni
// riconnessione MQTT, cosi' la configurazione viene ripubblicata anche se il
// broker ha perso i messaggi retained (per esempio dopo un suo riavvio).
void discoveryReset();

// Pubblica le entita' di comando (button / number / switch / time).
void discoveryPubblicaComandi();

// Pubblica le entita' diagnostiche del PONTE (uptime, RSSI WiFi, pacchetti,
// heap). Sono in categoria "diagnostic", quindi Home Assistant le raggruppa
// da sola in fondo alla scheda del dispositivo senza intasare la dashboard.
void discoveryPubblicaPonte();

/*
 * Assicura che esista una entita' Home Assistant per questa chiave.
 * Se la chiave e' in tabella usa il descrittore, altrimenti ne genera uno
 * generico. Non fa nulla se la chiave e' gia' stata annunciata.
 */
void discoveryAssicuraSensore(const char* chiave);

// true per le chiavi di trasporto/controllo che non devono diventare entita'.
bool discoveryDaIgnorare(const char* chiave);
