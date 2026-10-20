/*
 * ============================================================================
 *  SERRA SMART — PONTE : LA RADIO LoRa, CHE SI ACCORGE DA SOLA DI ESSERE GUASTA
 * ============================================================================
 *
 *  ---------------------------------------------------------------------------
 *  IL PROBLEMA (9-20 ottobre 2026)
 *  ---------------------------------------------------------------------------
 *  Ogni tanto il ponte smetteva di ricevere, e tornava a funzionare solo
 *  riflashandolo. Le statistiche di Home Assistant mostrano tre episodi, piu'
 *  un quarto di tipo diverso (orari UTC):
 *
 *    09/10 07:30 -> 10/10 18:31   sordo, dopo 16 giorni di funzionamento
 *    11/10 14:45 -> 13/10 19:52   sordo, 20 ore dopo il riavvio precedente
 *    13/10 20:08 -> 20/10 17:37   riceveva tutto, ma il nodo non ha sentito
 *                                 UN SOLO ACK: 3 tentativi a ciclo, txp=4
 *                                 sempre, backlog salito fino a 344 record
 *    20/10 06:26 -> 20/10 17:37   di nuovo sordo
 *
 *  In tutti i casi il resto del ponte stava benissimo: uptime che cresceva,
 *  MQTT e WiFi su, diagnostica pubblicata ogni minuto, memoria ferma.
 *  E da sorda la radio non era morta: in 53 ore ha consegnato due pacchetti
 *  di rumore, in 35 ore uno. Un modulo spento o tornato in modalita' FSK non
 *  ne consegnerebbe nessuno. Resta una radio ACCESA ma CONFIGURATA MALE, e il
 *  firmware non controllava mai la configurazione: la scriveva una volta
 *  sola all'avvio. Il riavvio rifaceva LoRa.begin(), e per questo bastava.
 *
 *  Che cosa la scombinasse i dati di Home Assistant non lo possono dire. Il
 *  sospettato principale e' il firmware stesso, fino alla 2.4.0. Per
 *  ascoltare chiamava LoRa.parsePacket() a ogni giro del loop, cioe' qualche
 *  centinaio di volte al secondo, e ogni chiamata sono cinque o sei
 *  transazioni SPI con dentro una lettura-modifica-scrittura del registro
 *  della banda e del coding rate (explicitHeaderMode()). Basta UNA lettura
 *  disturbata sul bus perche' un valore sbagliato finisca riscritto nel
 *  registro, e da li' in poi le letture successive lo conservano: la radio
 *  resta sulla banda sbagliata per sempre. Con piu' di cento milioni di
 *  transazioni al giorno anche un disturbo rarissimo finisce per capitare
 *  nel giro di qualche giorno, che e' la cadenza osservata. Le altre
 *  possibilita' (alimentazione, il modulo stesso) si curano allo stesso
 *  modo, e i contatori qui sotto diranno quale era.
 *
 *  ---------------------------------------------------------------------------
 *  COSA FA ADESSO
 *  ---------------------------------------------------------------------------
 *   1. Ascolto in RICEZIONE CONTINUA, controllato con una sola LETTURA del
 *      registro dei flag: niente scritture finche' non arriva un pacchetto.
 *      La radio continua ad ascoltare anche mentre il loop e' fermo su MQTT,
 *      e il pacchetto aspetta nel FIFO.
 *   2. SPI a 1 MHz invece degli 8 della libreria: i dati sono pochi, e su
 *      fili volanti un clock lento e' molto piu' difficile da disturbare.
 *   3. Configurazione COMPLETA a ogni avvio, anche dei registri che la
 *      libreria non tocca mai: se il filo di reset non facesse il suo lavoro,
 *      quei registri conserverebbero valori di chissa' quando.
 *   4. Ogni RADIO_VERIFICA_MS, e subito dopo ogni ACK, rilettura dei
 *      registri. Un valore sbagliato confermato da altre due letture vuol dire
 *      registro alterato: reset hardware e riconfigurazione, come un riavvio
 *      ma senza toccare WiFi e MQTT. Se invece una rilettura lo smentisce era
 *      il bus a mentire, e lo dice il contatore spi_inc.
 *   5. Riparazione anche per sintomi, quando i registri sembrano a posto:
 *      un ACK che non finisce di partire, il nodo muto da due cicli
 *      (camera_ponte.ino, sorvegliaSilenzio()), o il nodo che ritrasmette per
 *      tre cicli di fila pacchetti gia' confermati (controllaAckPerso()).
 *
 *  Ogni riparazione finisce in Home Assistant ("Riparazioni radio") e su
 *  serra/ponte/log con il motivo e, se c'era, il registro trovato alterato:
 *  la prossima volta non servira' tirare a indovinare.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>

enum CausaRiparazione : uint8_t {
  RIP_NESSUNA     = 0,
  RIP_REGISTRI    = 1,   // un registro dell'SX1278 non aveva piu' il valore impostato
  RIP_TX_BLOCCATA = 2,   // la trasmissione di un ACK non si e' conclusa in tempo
  RIP_SILENZIO    = 3,   // nessun pacchetto leggibile del nodo da troppo tempo
  RIP_ACK_PERSI   = 4,   // il nodo ritrasmette pacchetti gia' confermati
};

/*
 * Reset hardware, configurazione completa e ascolto continuo.
 * false se il modulo non risponde sul bus SPI: in quel caso radioSorveglia()
 * riprova da sola ogni RADIO_RIPROVA_MS.
 */
bool radioAvvia();

/*
 * Se e' arrivato un pacchetto lo copia in buf come stringa terminata e ne
 * restituisce la lunghezza, insieme a RSSI e SNR. 0 se non c'e' niente (o se
 * il pacchetto aveva il CRC sbagliato). Una sola lettura SPI quando non c'e'
 * niente: si puo' chiamare a ogni giro del loop.
 */
int radioRicevi(char* buf, size_t dim, int& rssi, float& snr);

/*
 * Trasmette e torna in ascolto, verificando subito i registri.
 * false se la trasmissione non si e' conclusa: la radio e' gia' stata
 * riparata e il nodo, senza ACK, ritrasmettera'.
 */
bool radioTrasmetti(const char* pacchetto);

// Da chiamare a ogni giro del loop: verifica periodica dei registri, e nuovo
// tentativo di avvio se il modulo non rispondeva.
void radioSorveglia();

// Reset e riconfigurazione, con il motivo che finisce nella diagnostica.
void radioRipara(CausaRiparazione causa, const char* dettaglio);

// --- Diagnostica ---------------------------------------------------------
bool             radioOk();
uint32_t         radioRiparazioni();
CausaRiparazione radioUltimaCausa();
const char*      radioCausaTesto(CausaRiparazione causa);
const char*      radioUltimoDettaglio();   // "" se non ci sono state riparazioni
// Registri che non tornavano nemmeno a reset appena fatto, e che quindi la
// verifica salta, oppure perche' il modulo non risponde. "" di norma.
const char*      radioAvviso();
uint32_t         radioLettureIncoerenti();
uint32_t         radioCrcErrati();

/*
 * Il filo di reset funziona? 1 si', 0 no, -1 non ancora verificato.
 *
 * Il riavvio software dell'ESP32 (anche quello dopo un OTA) non toglie
 * corrente all'SX1278: il modulo si azzera davvero solo se il suo pin RST,
 * collegato a LORA_RST, viene tenuto basso da LoRa.begin(). Per saperlo,
 * prima del reset si scrive un valore di prova in un registro che il reset
 * riporta al default, e dopo si guarda se c'e' ancora. Si puo' fare solo se
 * il modulo e' gia' in modalita' LoRa, quindi non alla prima accensione.
 */
int8_t radioResetHw();
