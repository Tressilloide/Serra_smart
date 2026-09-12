#include "comandi.h"

#include <string.h>

static PubSubClient* mqtt = nullptr;

static ComandoInCoda coda[CODA_CMD_MAX];
static uint8_t       nCoda = 0;

// Gli id partono da un valore diverso a ogni avvio, cosi' un esito in ritardo
// relativo a una sessione precedente non viene scambiato per quello attuale.
static uint32_t prossimoId = 1;

// ---------------------------------------------------------------------------

// Istante dell'ultima sottoscrizione a serra/nodo/cmd/+. Subito dopo, il
// broker riversa tutti i messaggi ritenuti: quelli sono residui, non
// richieste appena fatte dall'utente.
static uint32_t istanteSottoscrizione = 0;

void codaInit(PubSubClient* client) {
  mqtt = client;
  nCoda = 0;
  prossimoId = (millis() & 0x0FFF) + 1;
}

void codaSegnalaSottoscrizione() {
  istanteSottoscrizione = millis();
}

/*
 * Comandi che FANNO qualcosa adesso, contrapposti a quelli che IMPOSTANO un
 * valore. Solo questi scadono: una durata impostata ieri e' comunque la
 * durata giusta, un "irriga ora" di ieri no.
 *
 * TIME e' nell'elenco perche' porta con se' un timestamp: consegnato in
 * ritardo metterebbe l'orologio del nodo a un'ora sbagliata.
 */
bool comandoEffimero(const char* opcode) {
  static const char* EFFIMERI[] = {
    "IRR", "IRRVOL", "STOP", "WAKE", "RESET", "CLRBL", "TIME"
  };
  for (uint8_t i = 0; i < sizeof(EFFIMERI) / sizeof(EFFIMERI[0]); i++)
    if (strcmp(opcode, EFFIMERI[i]) == 0) return true;
  return false;
}

// Cancella il messaggio ritenuto di un opcode sul broker.
static void cancellaRetained(const char* opcode) {
  if (!mqtt || !mqtt->connected()) return;
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/%s", TOPIC_CMD_BASE, opcode);
  mqtt->publish(topic, (const uint8_t*)"", 0, true);
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

  /*
   * Protezione 1: residui ritenuti sul broker.
   *
   * Appena il ponte si sottoscrive, il broker gli riversa tutti i messaggi
   * ritenuti presenti sui topic dei comandi. Se fra questi c'e' un "irriga
   * ora" rimasto li' da ore o da giorni — perche' il ponte era spento quando
   * lo hai premuto — consegnarlo adesso significa annaffiare la serra a un
   * orario a caso. Un comando di azione che arriva in questa finestra viene
   * quindi scartato e il suo retained cancellato.
   *
   * Le impostazioni invece passano: quelle vanno bene anche in ritardo, anzi
   * servono proprio a riallineare il nodo dopo un riavvio.
   */
  if (comandoEffimero(ultimo) &&
      (millis() - istanteSottoscrizione) < CODA_FINESTRA_RETAINED_MS) {
    Serial.printf("[CMD] %s scartato: e' un residuo ritenuto sul broker, "
                  "non una richiesta appena fatta.\n", ultimo);
    cancellaRetained(ultimo);
    return;
  }

  // Un solo comando per opcode: se ce n'e' gia' uno in coda si sovrascrive.
  // "L'ultimo vince" e' il comportamento giusto per durata, orario, soglia...
  for (uint8_t i = 0; i < nCoda; i++) {
    if (strcmp(coda[i].opcode, ultimo) == 0) {
      strncpy(coda[i].args, args, sizeof(coda[i].args) - 1);
      coda[i].args[sizeof(coda[i].args) - 1] = '\0';
      coda[i].ricevutoMs = millis();   // richiesta nuova: il TTL riparte
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
  c.ricevutoMs = millis();
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
  const uint32_t ttlMs = (uint32_t)CMD_TTL_AZIONE_MIN * 60UL * 1000UL;

  while (nCoda > 0) {
    ComandoInCoda c = coda[0];

    // Rimozione dalla testa, comunque vada
    for (uint8_t i = 1; i < nCoda; i++) coda[i - 1] = coda[i];
    nCoda--;

    /*
     * Protezione 2: scadenza dei comandi di azione.
     *
     * Il nodo dorme fino a 15 minuti, quindi un po' di attesa e' normale. Ma
     * se un "irriga ora" resta in coda per mezz'ora vuol dire che qualcosa non
     * ha funzionato (nodo irraggiungibile, link LoRa giu'), e consegnarlo
     * adesso farebbe partire l'acqua in un momento che non c'entra piu' nulla
     * con quando hai premuto il bottone.
     */
    if (comandoEffimero(c.opcode) && (millis() - c.ricevutoMs) > ttlMs) {
      Serial.printf("[CMD] %s SCADUTO dopo %lu minuti in coda: scartato.\n",
                    c.opcode, (unsigned long)((millis() - c.ricevutoMs) / 60000UL));
      cancellaRetained(c.opcode);
      codaPubblicaPending();
      continue;                       // prova con il prossimo della coda
    }

    c.id = prossimoId++;
    out  = c;

    // Il comando e' stato consegnato: via il retained, altrimenti al prossimo
    // avvio del ponte il broker glielo riproporrebbe da capo.
    cancellaRetained(out.opcode);

    Serial.printf("[CMD] Consegno al nodo: id=%lu %s(%s)\n",
                  (unsigned long)out.id, out.opcode, out.args);
    codaPubblicaPending();
    return true;
  }
  return false;
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
