#include "radio.h"
#include "config.h"
#include "watchdog.h"

#include <SPI.h>
#include <LoRa.h>

static SPIClass loraSPI(HSPI);
static bool g_radioOk   = false;
static bool g_spiPronto = false;   // l'HSPI si inizializza una volta sola

// ---------------------------------------------------------------------------

/*
 * Inizializza (o re-inizializza) la radio.
 *
 * E' richiamabile a caldo: LoRa.begin() tiene basso il pin RST per 10 ms,
 * quindi ripassare di qui equivale a spegnere e riaccendere l'SX1278, e
 * riporta i registri nello stato noto. Solo il bus SPI non va ritoccato:
 * ri-aprirlo mentre e' gia' aperto lascerebbe in giro il vecchio bus mai
 * liberato.
 */
bool radioInit() {
  if (!g_spiPronto) {
    loraSPI.begin(LORA_SCK2, LORA_MISO2, LORA_MOSI2, LORA_NSS);
    LoRa.setSPI(loraSPI);                                // usa l'HSPI, non il VSPI
    g_spiPronto = true;
  }
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_BAND)) {
    Serial.println(F("[LoRa] ERRORE: modulo non trovato!"));
    Serial.println(F("[LoRa] Controlla: cablaggio SPI, NSS=GPIO5, alimentazione 3,3 V,"));
    Serial.println(F("[LoRa] e soprattutto che l'ANTENNA sia montata."));
    g_radioOk = false;
    return false;
  }

  // Questi parametri devono coincidere esattamente con quelli del ponte,
  // altrimenti i due moduli non si sentono nemmeno standosi accanto.
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.enableCrc();
  LoRa.setTxPower(LORA_TX_POWER);

  g_radioOk = true;
  Serial.printf("[LoRa] Radio pronta (%.1f MHz, SF%d).\n", LORA_BAND / 1E6, LORA_SF);
  return true;
}

void radioSpegni() {
  if (!g_radioOk) return;
  LoRa.sleep();
}

// ---------------------------------------------------------------------------

/*
 * Tempo di volo (time on air) di un pacchetto, in millisecondi.
 *
 * Serve a dimensionare il limite di guardia della trasmissione: sapendo
 * quanto DEVE durare, si riconosce subito una trasmissione che non finira'
 * mai, senza dover scegliere a occhio un timeout buono per tutte le
 * lunghezze.
 *
 * Formula del datasheet SX1276/78, paragrafo 4.1.1.7, nel nostro caso:
 * header esplicito, CRC attivo, low data rate optimization spenta.
 * LORA_CR vale gia' 5 per il coding rate 4/5, cioe' e' il (CR + 4) della
 * formula. Con SF7 e BW 125 kHz un pacchetto pieno da 250 byte sta in aria
 * 389 ms; uno corto da 60 byte, 113 ms.
 */
static uint32_t tempoDiVoloMs(size_t lunghezza) {
  const float tSimbolo = (float)(1UL << LORA_SF) / (float)LORA_BW;   // secondi

  int32_t numeratore  = 8 * (int32_t)lunghezza - 4 * LORA_SF + 28 + 16;
  int32_t denominatore = 4 * LORA_SF;
  int32_t simboli = 8;
  if (numeratore > 0)
    simboli += ((numeratore + denominatore - 1) / denominatore) * LORA_CR;

  const float preambolo = (8.0f + 4.25f) * tSimbolo;
  return (uint32_t)((preambolo + simboli * tSimbolo) * 1000.0f) + 1;
}

/*
 * Trasmette un pacchetto e si rimette in ascolto per l'ACK.
 *
 * Perche' non si usa LoRa.endPacket() e basta: nella libreria di Sandeep
 * Mistry la versione sincrona e' un "while (manca il flag di TxDone)
 * yield();" senza ne' timeout ne' wdtNutri(). Se l'SX1278 non alza mai quel
 * flag -- perche' un disturbo gli ha scombinato i registri, o perche' il bus
 * SPI e' piantato e ogni lettura torna sempre lo stesso valore -- quel ciclo
 * gira per sempre. Il nodo resta appeso dentro setup(), il watchdog globale
 * lo azzera dopo WDT_SETUP_SEC (180 s) e si perde l'intero ciclo di misure.
 * E' la firma dei riavvii rst=6 visti in Home Assistant, compresi due
 * avvenuti a valvola ferma, cioe' senza che il solenoide c'entrasse nulla.
 *
 * Qui la trasmissione parte in modo asincrono e si sorveglia con un limite di
 * tempo proporzionato al pacchetto. Nessuna attesa e' piu' illimitata.
 *
 * Come si sa che la trasmissione e' finita: chiedendolo a beginPacket(), che
 * risponde 0 finche' c'e' una trasmissione in corso e 1 quando il modulo e'
 * libero. E' l'unica strada pubblica, perche' isTransmitting() nella libreria
 * 0.8.0 e' dichiarata privata. Gli effetti collaterali della chiamata che
 * riesce -- modalita' standby e puntatori del FIFO azzerati -- sono innocui:
 * subito dopo si passa comunque in ricezione, e parsePacket() riposiziona da
 * solo il puntatore sul buffer di ricezione.
 *
 * Si esce entro un millisecondo dalla fine della trasmissione, e questo conta:
 * il ponte risponde in fretta, e una finestra cieca di qualche decina di
 * millisecondi basterebbe a perdere l'ACK e a far ritrasmettere tutto.
 */
static bool trasmetti(const char* pacchetto) {
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(pacchetto);
  LoRa.endPacket(true);           // asincrona: scrive un registro e torna

  const uint32_t limite = tempoDiVoloMs(strlen(pacchetto)) * (uint32_t)TX_GUARDIA_X
                        + (uint32_t)TX_GUARDIA_MS;
  const uint32_t t0     = millis();
  bool           finita = false;

  while (millis() - t0 < limite) {
    wdtNutri();
    if (LoRa.beginPacket() == 1) { finita = true; break; }
    delay(1);
  }

  if (!finita) {
    Serial.printf("[LoRa] Trasmissione non conclusa in %lu ms: reset della radio.\n",
                  (unsigned long)limite);
    radioInit();                  // reset hardware dell'SX1278
    return false;
  }

  LoRa.receive();                 // in ascolto per l'ACK
  return true;
}

/*
 * Attende un ACK con il numero di sequenza atteso.
 * Un ACK con sequenza diversa (eco di una ritrasmissione precedente) viene
 * scartato e l'attesa prosegue: non deve far fallire il tentativo in corso.
 */
static bool attendiAck(uint32_t seqAttesa, uint32_t timeoutMs, RispostaAck& out) {
  uint32_t t0 = millis();

  while (millis() - t0 < timeoutMs) {
    wdtNutri();

    int sz = LoRa.parsePacket();
    if (sz <= 0) { delay(2); continue; }

    char buf[PROTO_MAX_PAYLOAD + 1];
    int  n = 0;
    while (LoRa.available() && n < (int)sizeof(buf) - 1) buf[n++] = (char)LoRa.read();
    buf[n] = '\0';

    // static: non serve una copia per chiamata e tiene questo frame leggero,
    // visto che siamo in fondo a una catena di chiamate profonda.
    static PacchettoKV pkt;
    char prefisso[12];

    if (!pkt.parse(buf, prefisso, sizeof(prefisso))) continue;
    if (strcmp(prefisso, PROTO_PREFIX_ACK) != 0) continue;

    uint32_t seq = pkt.valoreU("s", 0);
    if (seq != seqAttesa) {
      Serial.printf("[LoRa] ACK per seq=%lu (attendevo %lu): ignorato.\n",
                    (unsigned long)seq, (unsigned long)seqAttesa);
      continue;
    }

    out.ricevuto   = true;
    out.seq        = seq;
    out.epochPonte = pkt.valoreU("now", 0);
    out.rssi       = LoRa.packetRssi();
    out.snr        = LoRa.packetSnr();

    // Fuso orario: presente solo se il ponte e' aggiornato (vedi RispostaAck).
    const char* tz  = pkt.valore("tz");
    out.tzValido    = (tz != nullptr && *tz != '\0');
    out.tzOffsetSec = out.tzValido ? (int32_t)atol(tz) : 0;

    // Comando eventualmente accodato all'ACK dal ponte
    const char* opcode = pkt.valore("o");
    if (opcode && *opcode) {
      out.haComando = true;
      out.cmdId     = pkt.valoreU("c", 0);
      strncpy(out.opcode, opcode, sizeof(out.opcode) - 1);
      out.opcode[sizeof(out.opcode) - 1] = '\0';

      const char* a = pkt.valore("a");
      if (a) {
        strncpy(out.args, a, sizeof(out.args) - 1);
        out.args[sizeof(out.args) - 1] = '\0';
      }
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------

bool radioInviaConAck(const char* pacchetto, RispostaAck& out) {
  memset(&out, 0, sizeof(RispostaAck));

  if (!g_radioOk || !pacchetto) return false;

  uint32_t seq = protoEstraiSeq(pacchetto);

  for (int tentativo = 1; tentativo <= TX_RETRIES; tentativo++) {
    wdtNutri();
    Serial.printf("[LoRa] TX seq=%lu tentativo %d/%d (%u byte)\n",
                  (unsigned long)seq, tentativo, TX_RETRIES, (unsigned)strlen(pacchetto));

    if (trasmetti(pacchetto) && attendiAck(seq, ACK_TIMEOUT_MS, out)) {
      Serial.printf("[LoRa] ACK ricevuto (RSSI %d dBm, SNR %.1f dB).\n", out.rssi, out.snr);
      return true;
    }

    /*
     * Tentativo fallito: prima di riprovare si resetta la radio.
     * Se l'ACK e' mancato per un motivo normale (collisione, disturbo) sono
     * venti millisecondi buttati; se invece l'SX1278 si e' impuntato, e'
     * l'unica cosa che lo rimette in sesto, e senza questo il secondo e il
     * terzo tentativo sarebbero identici al primo e altrettanto inutili.
     */
    if (tentativo < TX_RETRIES) {
      radioInit();
      if (!g_radioOk) break;       // modulo davvero morto: inutile insistere
    }

    delay(300UL * tentativo);      // backoff crescente tra i tentativi
  }

  Serial.println(F("[LoRa] Nessun ACK: il pacchetto andra' nel backlog su SD."));
  return false;
}
