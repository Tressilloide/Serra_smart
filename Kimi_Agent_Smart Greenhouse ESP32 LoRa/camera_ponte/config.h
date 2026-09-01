/*
 * ============================================================================
 *  SERRA SMART — PONTE IN CAMERA : CONFIGURAZIONE
 * ============================================================================
 *  Le credenziali WiFi e MQTT NON stanno qui: sono in secrets.h, che e'
 *  escluso dal versionamento tramite .gitignore. Copia secrets.h.example in
 *  secrets.h e compilalo con i tuoi dati.
 * ============================================================================
 */

#pragma once

#define FW_VERSION_PONTE "2.0.0"

// ======================= RADIO LoRa =========================================
// DEVONO coincidere esattamente con serra_nodo/config.h, altrimenti i due
// moduli non si sentono nemmeno stando uno accanto all'altro.

#define LORA_BAND   433E6
#define LORA_SF     7
#define LORA_BW     125E3
#define LORA_CR     5
#define LORA_TX_POWER 17

// ======================= MQTT ===============================================

#define MQTT_HOST   "192.168.1.36"     // IP statico del server in garage
#define MQTT_PORT   1883
#define MQTT_CLIENT_ID "serra-ponte"

// --- Topic ---
// Dati freschi. Retained: cosi' dopo un riavvio di Home Assistant i sensori
// hanno subito un valore invece di restare vuoti fino al risveglio successivo
// del nodo (fino a 15 minuti di buco).
#define TOPIC_STATO    "serra/nodo/stato"

// Record recuperati dal backlog della microSD. Vanno su un topic SEPARATO:
// se finissero su TOPIC_STATO, Home Assistant mostrerebbe come "valore
// attuale" una lettura di ore prima, perche' il nodo trasmette il pacchetto
// fresco per primo e la coda storica subito dopo.
#define TOPIC_STORICO  "serra/nodo/storico"

#define TOPIC_PONTE    "serra/ponte/stato"    // online/offline (Last Will)
#define TOPIC_DIAG     "serra/ponte/diag"     // diagnostica del ponte
#define TOPIC_LOG      "serra/ponte/log"      // eventi testuali

// Comandi: Home Assistant pubblica RETAINED su serra/nodo/cmd/<OPCODE>.
// Il broker fa da coda persistente; il ponte cancella il retained quando
// consegna il comando al nodo.
#define TOPIC_CMD_BASE "serra/nodo/cmd"
#define TOPIC_CMD_SUB  "serra/nodo/cmd/+"
#define TOPIC_CMD_RES  "serra/nodo/cmd/res"
#define TOPIC_CMD_PEND "serra/nodo/cmd/pending"

// Prefisso della MQTT Discovery di Home Assistant (cambialo solo se hai
// modificato discovery_prefix nella configurazione dell'integrazione MQTT).
#define HA_DISCOVERY_PREFIX "homeassistant"

// ======================= TIMING =============================================

#define WIFI_RETRY_MS        10000UL    // Intervallo tra i tentativi WiFi
#define MQTT_RETRY_MS        5000UL     // Intervallo tra i tentativi MQTT
#define DIAG_INTERVALLO_MS   60000UL    // Pubblicazione diagnostica
#define WDT_LOOP_SEC         30         // Watchdog del loop principale

/*
 * Il ponte NON si riavvia piu' a intervalli fissi.
 * La versione precedente faceva ESP.restart() ogni ora: durante i ~8 secondi
 * di riavvio la radio non ascolta, e con il nodo che trasmette ogni 15 minuti
 * la probabilita' di perdere proprio quel pacchetto non era trascurabile.
 * Al suo posto: watchdog sul loop e riavvio SOLO se il WiFi resta giu' a lungo,
 * cioe' quando il riavvio serve davvero a qualcosa.
 */
#define REBOOT_WIFI_DOWN_MS  900000UL   // 15 minuti senza WiFi -> riavvio

// Un record e' "storico" se il suo timestamp e' piu' vecchio di questo valore.
#define SOGLIA_STORICO_SEC   2400UL     // 40 minuti

// Anti-duplicati: quante coppie (seq,timestamp) ricordare. Se un ACK si perde
// il nodo ritrasmette lo stesso pacchetto: senza questo controllo il dato
// finirebbe due volte nello storico e nel conteggio dell'acqua.
#define DEDUP_MEMORIA        16

// Quanti comandi tenere in coda in RAM.
#define CODA_CMD_MAX         8

// ======================= NTP ================================================
// L'ora corretta viaggia su ogni ACK: e' cosi' che il DS1307 del nodo si
// risincronizza da solo quando la batteria tampone si scarica.

#define NTP_SERVER1  "pool.ntp.org"
#define NTP_SERVER2  "time.google.com"
#define NTP_TZ       "CET-1CEST,M3.5.0,M10.5.0/3"   // Italia, con ora legale

// ======================= PINOUT LoRa ========================================
// Sul ponte il LoRa ha il bus SPI tutto per se' (VSPI standard).

#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SCK  18
#define LORA_RST  14
#define LORA_DIO0 26
#define LORA_NSS  5

// ======================= OTA ================================================
// Aggiornamento del firmware via WiFi: comodo perche' il ponte spesso e'
// dietro un mobile. La password sta in secrets.h.

#define ABILITA_OTA  1
#define OTA_HOSTNAME "serra-ponte"
