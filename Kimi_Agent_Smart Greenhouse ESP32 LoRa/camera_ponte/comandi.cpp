#include "comandi.h"

#include <string.h>

static PubSubClient* mqtt = nullptr;

static ComandoInCoda coda[CODA_CMD_MAX];
static uint8_t       nCoda = 0;

// Gli id partono da un valore diverso a ogni avvio, cosi' un esito in ritardo
// relativo a una sessione precedente non viene scambiato per quello attuale.
static uint32_t prossimoId = 1;

// ---------------------------------------------------------------------------

void codaInit(PubSubClient* client) {
  mqtt = client;
  nCoda = 0;
  prossimoId = (millis() & 0x0FFF) + 1;
}

bool    codaVuota() { return nCoda == 0; }
uint8_t codaConta() { return nCoda; }

// ---------------------------------------------------------------------------
//  Ricezione da MQTT
// ---------------------------------------------------------------------------

void codaMessaggioMqtt(const char* topic, const uint8_t* payload, unsigned int len) {
  // L'opcode e' l'ultimo segmento del topic: serra/nodo/cmd/<OPCODE>
  const char* ultimo = strrchr(topic, '/');
  if (!ultimo) return;
  ultimo++;

  // Sono topic nostri, non comandi: vanno ignorati o si crea un ciclo.
  if (strcmp(ultimo, "res") == 0 || strcmp(ultimo, "pending") == 0) return;

  // Payload vuoto = cancellazione di un retained. E' la nostra stessa
  // cancellazione che ci torna indietro: niente da fare.
  if (len == 0) {
    Serial.printf("[CMD] Retained cancellato su %s\n", ultimo);
    return;
  }

  char args[sizeof(coda[0].args)];
  unsigned int n = len < sizeof(args) - 1 ? len : sizeof(args) - 1;
  memcpy(args, payload, n);
  args[n] = '\0';

  Serial.printf("[CMD] Ricevuto da Home Assistant: %s = \"%s\"\n", ultimo, args);

  // Un solo comando per opcode: se ce n'e' gia' uno in coda si sovrascrive.
  // "L'ultimo vince" e' il comportamento giusto per durata, orario, soglia...
  for (uint8_t i = 0; i < nCoda; i++) {
    if (strcmp(coda[i].opcode, ultimo) == 0) {
      strncpy(coda[i].args, args, sizeof(coda[i].args) - 1);
      coda[i].args[sizeof(coda[i].args) - 1] = '\0';
      Serial.printf("[CMD] Aggiornato il comando %s gia' in coda.\n", ultimo);
      codaPubblicaPending();
      return;
    }
  }

  if (nCoda >= CODA_CMD_MAX) {
    Serial.println(F("[CMD] Coda piena: comando scartato."));
    return;
  }

  ComandoInCoda& c = coda[nCoda];
  c.id = 0;                                  // assegnato alla consegna
  strncpy(c.opcode, ultimo, sizeof(c.opcode) - 1);
  c.opcode[sizeof(c.opcode) - 1] = '\0';
  strncpy(c.args, args, sizeof(c.args) - 1);
  c.args[sizeof(c.args) - 1] = '\0';
  nCoda++;

  Serial.printf("[CMD] In coda (%u in attesa del risveglio del nodo).\n", nCoda);
  codaPubblicaPending();
}

// ---------------------------------------------------------------------------
//  Consegna
// ---------------------------------------------------------------------------

bool codaEstrai(ComandoInCoda& out) {
  if (nCoda == 0) return false;

  out = coda[0];
  out.id = prossimoId++;

  // Scorrimento della coda
  for (uint8_t i = 1; i < nCoda; i++) coda[i - 1] = coda[i];
  nCoda--;

  // Cancellazione del retained sul broker: il comando e' stato consegnato e
  // non deve essere riproposto al prossimo avvio del ponte.
  if (mqtt && mqtt->connected()) {
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", TOPIC_CMD_BASE, out.opcode);
    mqtt->publish(topic, (const uint8_t*)"", 0, true);
  }

  Serial.printf("[CMD] Consegno al nodo: id=%lu %s(%s)\n",
                (unsigned long)out.id, out.opcode, out.args);
  codaPubblicaPending();
  return true;
}

// ---------------------------------------------------------------------------
//  Pubblicazioni verso Home Assistant
// ---------------------------------------------------------------------------

void codaPubblicaEsito(uint32_t id, uint32_t rc, const char* dettaglio) {
  if (!mqtt || !mqtt->connected()) return;

  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"id\":%lu,\"rc\":%lu,\"esito\":\"%s\",\"ok\":%s}",
           (unsigned long)id, (unsigned long)rc,
           dettaglio ? dettaglio : "", rc == 0 ? "true" : "false");

  mqtt->publish(TOPIC_CMD_RES, payload);
  Serial.printf("[CMD] Esito pubblicato: %s\n", payload);
}

void codaPubblicaPending() {
  if (!mqtt || !mqtt->connected()) return;

  char payload[256];
  int pos = snprintf(payload, sizeof(payload), "{\"n\":%u,\"coda\":[", nCoda);

  for (uint8_t i = 0; i < nCoda && pos < (int)sizeof(payload) - 40; i++) {
    pos += snprintf(payload + pos, sizeof(payload) - pos, "%s\"%s:%s\"",
                    i ? "," : "", coda[i].opcode, coda[i].args);
  }
  snprintf(payload + pos, sizeof(payload) - pos, "]}");

  mqtt->publish(TOPIC_CMD_PEND, (const uint8_t*)payload, strlen(payload), true);
}
