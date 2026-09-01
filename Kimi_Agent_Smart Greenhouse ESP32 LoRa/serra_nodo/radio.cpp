#include "radio.h"
#include "config.h"
#include "watchdog.h"

#include <SPI.h>
#include <LoRa.h>

static SPIClass loraSPI(HSPI);
static bool g_radioOk = false;

// ---------------------------------------------------------------------------

bool radioInit() {
  loraSPI.begin(LORA_SCK2, LORA_MISO2, LORA_MOSI2, LORA_NSS);
  LoRa.setSPI(loraSPI);                                  // usa l'HSPI, non il VSPI
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

static bool trasmetti(const char* pacchetto) {
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(pacchetto);
  bool ok = LoRa.endPacket();     // bloccante fino a fine trasmissione
  LoRa.receive();                 // subito in ascolto per l'ACK
  return ok;
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

    delay(300UL * tentativo);      // backoff crescente tra i tentativi
  }

  Serial.println(F("[LoRa] Nessun ACK: il pacchetto andra' nel backlog su SD."));
  return false;
}
