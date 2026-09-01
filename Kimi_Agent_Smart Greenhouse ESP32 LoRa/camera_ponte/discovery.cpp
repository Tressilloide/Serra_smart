#include "discovery.h"
#include "config.h"
#include "protocollo.h"

#include <string.h>

static PubSubClient* mqtt = nullptr;

// Blocco "device": raggruppa tutte le entita' sotto un unico dispositivo
// "Serra" nell'interfaccia di Home Assistant.
static const char* DEV =
  "\"dev\":{\"ids\":[\"serra_nodo\"],\"name\":\"Serra\",\"mf\":\"DIY\","
  "\"mdl\":\"ESP32 LoRa\",\"sw\":\"" FW_VERSION_PONTE "\"}";

// ---------------------------------------------------------------------------
//  1. Tabella delle chiavi conosciute
// ---------------------------------------------------------------------------

struct EntitaSensore {
  const char* chiave;
  const char* nome;
  const char* unita;       // nullptr = adimensionale
  const char* devClass;    // nullptr = nessuna
  const char* statClass;   // "measurement" / "total_increasing" / nullptr
  const char* icona;       // nullptr = icona automatica
  bool        testuale;    // true = valore non numerico (niente sentinella -127)
  bool        diagnostica; // true = finisce nella sezione Diagnostica di HA
};

static const EntitaSensore ENTITA[] = {
  // chiave      nome                     unita  devClass          statClass       icona                    txt    diag
  { "temp",     "Temperatura",            "°C", "temperature", "measurement",  nullptr,                 false, false },
  { "hum",      "Umidita' aria",          "%",   "humidity",        "measurement",  nullptr,                 false, false },
  { "pres",     "Pressione",              "hPa", "pressure",        "measurement",  nullptr,                 false, false },
  { "luce",     "Luce",                   "%",   nullptr,           "measurement",  "mdi:brightness-6",      false, false },
  { "volt",     "Tensione batteria",      "V",   "voltage",         "measurement",  nullptr,                 false, false },

  { "soil1",    "Umidita' terreno 1",     "%",   "moisture",        "measurement",  "mdi:water-percent",     false, false },
  { "soil2",    "Umidita' terreno 2",     "%",   "moisture",        "measurement",  "mdi:water-percent",     false, false },
  { "soil3",    "Umidita' terreno 3",     "%",   "moisture",        "measurement",  "mdi:water-percent",     false, false },
  { "soil4",    "Umidita' terreno 4",     "%",   "moisture",        "measurement",  "mdi:water-percent",     false, false },

  { "acqua",    "Acqua ultima irrigazione","L",  "water",           "measurement",  "mdi:water",             false, false },
  // total_increasing: Home Assistant lo tratta come un contatore e ci puo'
  // costruire sopra un utility_meter per i totali giornalieri e mensili.
  { "acquaTot", "Acqua totale",           "L",   "water",           "total_increasing", "mdi:counter",       false, false },

  { "irr",      "Esito irrigazione",      nullptr, nullptr,         nullptr,        "mdi:sprinkler-variant", true,  false },
  { "det",      "Esito ultimo comando",   nullptr, nullptr,         nullptr,        "mdi:message-text",      true,  true  },

  { "rssi",     "Segnale LoRa",           "dBm", "signal_strength", "measurement",  nullptr,                 false, true  },
  { "snr",      "SNR LoRa",               "dB",  nullptr,           "measurement",  "mdi:signal-variant",    false, true  },
  { "bl",       "Record in backlog",      nullptr, nullptr,         "measurement",  "mdi:database-clock",    false, true  },
  { "fw",       "Firmware nodo",          nullptr, nullptr,         nullptr,        "mdi:chip",              true,  true  },
  { "rst",      "Motivo ultimo riavvio",  nullptr, nullptr,         nullptr,        "mdi:restart-alert",     false, true  },
  { "trunc",    "Pacchetto troncato",     nullptr, nullptr,         nullptr,        "mdi:alert",             false, true  },
};

static const uint8_t N_ENTITA = sizeof(ENTITA) / sizeof(ENTITA[0]);

// ---------------------------------------------------------------------------
//  Chiavi di trasporto e di controllo: non devono generare entita'.
//  (sOra/sMin/sDur/sAuto/sSoil/slp alimentano le entita' di COMANDO, che sono
//   pubblicate a parte con il loro tipo corretto: number, switch, time.)
// ---------------------------------------------------------------------------

static const char* IGNORA[] = {
  "v", "s", "t", "now", "c", "o", "a", "h", "res", "rc",
  "sOra", "sMin", "sDur", "sAuto", "sSoil", "slp",
  "cmdL", "cmdS", "ping"
};
static const uint8_t N_IGNORA = sizeof(IGNORA) / sizeof(IGNORA[0]);

bool discoveryDaIgnorare(const char* chiave) {
  for (uint8_t i = 0; i < N_IGNORA; i++)
    if (strcmp(chiave, IGNORA[i]) == 0) return true;
  return false;
}

// ---------------------------------------------------------------------------
//  Cache delle chiavi gia' annunciate
// ---------------------------------------------------------------------------

#define MAX_CHIAVI_ANNUNCIATE 32
static char    annunciate[MAX_CHIAVI_ANNUNCIATE][PROTO_LEN_CHIAVE];
static uint8_t nAnnunciate = 0;

static bool giaAnnunciata(const char* chiave) {
  for (uint8_t i = 0; i < nAnnunciate; i++)
    if (strcmp(annunciate[i], chiave) == 0) return true;
  return false;
}

static void segnaAnnunciata(const char* chiave) {
  if (nAnnunciate >= MAX_CHIAVI_ANNUNCIATE) return;
  strncpy(annunciate[nAnnunciate], chiave, PROTO_LEN_CHIAVE - 1);
  annunciate[nAnnunciate][PROTO_LEN_CHIAVE - 1] = '\0';
  nAnnunciate++;
}

void discoveryReset() { nAnnunciate = 0; }

void discoveryInit(PubSubClient* client) {
  mqtt = client;
  discoveryReset();
}

// ---------------------------------------------------------------------------
//  Pubblicazione di un sensore
// ---------------------------------------------------------------------------

static void pubblicaSensore(const EntitaSensore& e) {
  if (!mqtt || !mqtt->connected()) return;

  char topic[96];
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/sensor/serra_%s/config", e.chiave);

  /*
   * Template del valore. Il nodo invia -127 quando un sensore non e'
   * disponibile (e' JSON valido, a differenza di "nan"). Qui lo convertiamo
   * in stringa vuota: Home Assistant scarta gli aggiornamenti vuoti, quindi
   * l'entita' conserva l'ultimo valore buono invece di mostrare un -127 che
   * sporcherebbe grafici e statistiche.
   */
  char tpl[220];
  if (e.testuale) {
    snprintf(tpl, sizeof(tpl), "{{ value_json.%s | default('') }}", e.chiave);
  } else {
    snprintf(tpl, sizeof(tpl),
             "{%% set v = value_json.%s | float(-127) %%}{{ '' if v == -127 else v }}",
             e.chiave);
  }

  char payload[820];
  int n = snprintf(payload, sizeof(payload),
    "{\"name\":\"%s\","
    "\"uniq_id\":\"serra_%s\","
    "\"stat_t\":\"%s\","
    "\"val_tpl\":\"%s\","
    "%s%s%s"                       // unita'
    "%s%s%s"                       // device_class
    "%s%s%s"                       // state_class
    "%s%s%s"                       // icona
    "%s"                           // categoria diagnostica
    "\"exp_aft\":7200,"
    "\"avty_t\":\"%s\","
    "%s}",
    e.nome, e.chiave, TOPIC_STATO, tpl,
    e.unita     ? "\"unit_of_meas\":\"" : "", e.unita     ? e.unita     : "", e.unita     ? "\"," : "",
    e.devClass  ? "\"dev_cla\":\""      : "", e.devClass  ? e.devClass  : "", e.devClass  ? "\"," : "",
    e.statClass ? "\"stat_cla\":\""     : "", e.statClass ? e.statClass : "", e.statClass ? "\"," : "",
    e.icona     ? "\"ic\":\""           : "", e.icona     ? e.icona     : "", e.icona     ? "\"," : "",
    e.diagnostica ? "\"ent_cat\":\"diagnostic\"," : "",
    TOPIC_PONTE, DEV);

  if (n <= 0) return;
  if (n >= (int)sizeof(payload)) {
    Serial.printf("[HA] Payload discovery troppo lungo per %s: salto.\n", e.chiave);
    return;
  }

  if (mqtt->publish(topic, (const uint8_t*)payload, n, true)) {
    Serial.printf("[HA] Entita' pubblicata: %s (%s)\n", e.chiave, e.nome);
  } else {
    Serial.printf("[HA] ERRORE nella pubblicazione di %s (payload %d byte, "
                  "buffer MQTT insufficiente?)\n", e.chiave, n);
  }
}

void discoveryAssicuraSensore(const char* chiave) {
  if (!chiave || !*chiave) return;
  if (discoveryDaIgnorare(chiave)) return;
  if (giaAnnunciata(chiave)) return;

  for (uint8_t i = 0; i < N_ENTITA; i++) {
    if (strcmp(chiave, ENTITA[i].chiave) == 0) {
      pubblicaSensore(ENTITA[i]);
      segnaAnnunciata(chiave);
      return;
    }
  }

  // --- 2. Fallback generico: chiave sconosciuta, entita' creata lo stesso ---
  // E' quello che permette di aggiungere un sensore al nodo senza toccare
  // il firmware del ponte.
  Serial.printf("[HA] Chiave sconosciuta '%s': creo una entita' generica.\n", chiave);

  EntitaSensore generica = {
    chiave, chiave, nullptr, nullptr, "measurement", "mdi:help-circle-outline", false, false
  };
  pubblicaSensore(generica);
  segnaAnnunciata(chiave);
}

// ---------------------------------------------------------------------------
//  Entita' di comando
// ---------------------------------------------------------------------------

/*
 * Nota sui template: rendono STRINGA VUOTA quando la chiave manca, invece di
 * un valore di ripiego. Se un pacchetto arriva senza il blocco di stato
 * (per troncamento, o perche' e' un pacchetto di esito comando), Home Assistant
 * scarta l'aggiornamento vuoto e il controllo conserva il valore precedente.
 * Con un default numerico il cursore salterebbe a zero e poi tornerebbe
 * indietro a ogni pacchetto incompleto.
 *
 * "retain":true e' essenziale: Home Assistant pubblica il comando come
 * messaggio ritenuto, quindi il broker lo conserva finche' il ponte non lo
 * consegna al nodo. La coda sopravvive a un riavvio del ponte senza bisogno
 * di scriverla da nessuna parte.
 */
static void pubblica(const char* topic, const char* payload) {
  if (!mqtt || !mqtt->connected()) return;
  if (!mqtt->publish(topic, (const uint8_t*)payload, strlen(payload), true))
    Serial.printf("[HA] ERRORE pubblicando %s\n", topic);
}

void discoveryPubblicaComandi() {
  if (!mqtt || !mqtt->connected()) return;
  Serial.println(F("[HA] Pubblico le entita' di comando..."));

  char topic[96];
  char payload[820];

  // --- Bottone: irriga ora (durata = quella configurata sul nodo) ----------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/button/serra_irriga/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Irriga ora\",\"uniq_id\":\"serra_irriga\","
    "\"cmd_t\":\"%s/IRR\",\"payload_press\":\"0\",\"retain\":true,"
    "\"ic\":\"mdi:sprinkler\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Bottone: blocca l'irrigazione per oggi ------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/button/serra_stop/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Blocca irrigazione per oggi\",\"uniq_id\":\"serra_stop\","
    "\"cmd_t\":\"%s/STOP\",\"payload_press\":\"1\",\"retain\":true,"
    "\"ic\":\"mdi:water-off\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Numero: durata irrigazione ------------------------------------------
  // Il massimo coincide con il tetto compilato nel nodo (IRRIG_MAX_SEC):
  // richieste superiori verrebbero comunque clampate dal firmware.
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/number/serra_durata/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Durata irrigazione\",\"uniq_id\":\"serra_durata\","
    "\"cmd_t\":\"%s/DUR\",\"retain\":true,"
    "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.sDur | default('') }}\","
    "\"min\":10,\"max\":900,\"step\":10,\"unit_of_meas\":\"s\",\"mode\":\"box\","
    "\"ic\":\"mdi:timer-sand\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_STATO, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Orario programmato ---------------------------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/time/serra_orario/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Orario irrigazione\",\"uniq_id\":\"serra_orario\","
    "\"cmd_t\":\"%s/ORA\",\"retain\":true,"
    "\"stat_t\":\"%s\","
    "\"val_tpl\":\"{%% if value_json.sOra is defined %%}{{ '%%02d:%%02d:00' | format(value_json.sOra | int(0), value_json.sMin | int(0)) }}{%% endif %%}\","
    "\"ic\":\"mdi:clock-outline\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_STATO, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Interruttore: irrigazione automatica --------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/switch/serra_auto/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Irrigazione automatica\",\"uniq_id\":\"serra_auto\","
    "\"cmd_t\":\"%s/AUTO\",\"retain\":true,"
    "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.sAuto | default('') }}\","
    "\"pl_on\":\"1\",\"pl_off\":\"0\",\"stat_on\":\"1\",\"stat_off\":\"0\","
    "\"ic\":\"mdi:calendar-clock\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_STATO, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Numero: soglia umidita' terreno (-1 = irrigazione non condizionata) --
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/number/serra_soglia/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Soglia umidita' terreno\",\"uniq_id\":\"serra_soglia\","
    "\"cmd_t\":\"%s/SOIL\",\"retain\":true,"
    "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.sSoil | default('') }}\","
    "\"min\":-1,\"max\":100,\"step\":1,\"unit_of_meas\":\"%%\",\"mode\":\"slider\","
    "\"ic\":\"mdi:water-alert\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_STATO, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Numero: intervallo di risveglio -------------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/number/serra_sleep/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Intervallo risveglio\",\"uniq_id\":\"serra_sleep\","
    "\"cmd_t\":\"%s/SLEEP\",\"retain\":true,"
    "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.slp | default('') }}\","
    "\"min\":60,\"max\":3600,\"step\":60,\"unit_of_meas\":\"s\",\"mode\":\"box\","
    "\"ic\":\"mdi:sleep\",\"ent_cat\":\"config\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_STATO, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Bottone: finestra di manutenzione -----------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/button/serra_wake/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Finestra manutenzione (2 min)\",\"uniq_id\":\"serra_wake\","
    "\"cmd_t\":\"%s/WAKE\",\"payload_press\":\"120\",\"retain\":true,"
    "\"ic\":\"mdi:tools\",\"ent_cat\":\"config\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Bottone: svuota backlog ---------------------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/button/serra_clrbl/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Svuota backlog\",\"uniq_id\":\"serra_clrbl\","
    "\"cmd_t\":\"%s/CLRBL\",\"payload_press\":\"1\",\"retain\":true,"
    "\"ic\":\"mdi:database-remove\",\"ent_cat\":\"config\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Bottone: riavvia il nodo --------------------------------------------
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/button/serra_reset/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Riavvia nodo serra\",\"uniq_id\":\"serra_reset\","
    "\"cmd_t\":\"%s/RESET\",\"payload_press\":\"1\",\"retain\":true,"
    "\"dev_cla\":\"restart\",\"ent_cat\":\"config\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_BASE, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  // --- Sensore: comandi in attesa di consegna ------------------------------
  // Serve alla dashboard per dire "comando in coda, verra' eseguito al
  // prossimo risveglio" invece di sembrare che non sia successo nulla.
  snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/sensor/serra_pending/config");
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Comandi in coda\",\"uniq_id\":\"serra_pending\","
    "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.n | default(0) }}\","
    "\"json_attr_t\":\"%s\","
    "\"ic\":\"mdi:playlist-play\",\"avty_t\":\"%s\",%s}",
    TOPIC_CMD_PEND, TOPIC_CMD_PEND, TOPIC_PONTE, DEV);
  pubblica(topic, payload);

  Serial.println(F("[HA] Entita' di comando pubblicate."));
}

// ---------------------------------------------------------------------------
//  Entita' diagnostiche del ponte
// ---------------------------------------------------------------------------

struct DiagPonte {
  const char* id;
  const char* nome;
  const char* campo;      // chiave nel JSON di serra/ponte/diag
  const char* unita;
  const char* devClass;
  const char* icona;
};

static const DiagPonte DIAG[] = {
  { "ponte_uptime", "Ponte uptime",        "uptime",         "s",   "duration",        "mdi:timer-outline"   },
  { "ponte_wifi",   "Ponte segnale WiFi",  "wifi_rssi",      "dBm", "signal_strength", nullptr               },
  { "ponte_pkt",    "Pacchetti ricevuti",  "pkt",            nullptr, nullptr,         "mdi:package-down"    },
  { "ponte_heap",   "Ponte memoria libera","heap",           "B",   "data_size",       "mdi:memory"          },
  { "ponte_cmd",    "Comandi consegnati",  "cmd_consegnati", nullptr, nullptr,         "mdi:send-check"      },
};

void discoveryPubblicaPonte() {
  if (!mqtt || !mqtt->connected()) return;

  char topic[96];
  char payload[700];

  for (uint8_t i = 0; i < sizeof(DIAG) / sizeof(DIAG[0]); i++) {
    const DiagPonte& d = DIAG[i];
    snprintf(topic, sizeof(topic), HA_DISCOVERY_PREFIX "/sensor/%s/config", d.id);
    snprintf(payload, sizeof(payload),
      "{\"name\":\"%s\",\"uniq_id\":\"%s\",\"stat_t\":\"%s\","
      "\"val_tpl\":\"{{ value_json.%s | default(0) }}\","
      "%s%s%s%s%s%s%s%s%s"
      "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\","
      "\"avty_t\":\"%s\",%s}",
      d.nome, d.id, TOPIC_DIAG, d.campo,
      d.unita    ? "\"unit_of_meas\":\"" : "", d.unita    ? d.unita    : "", d.unita    ? "\"," : "",
      d.devClass ? "\"dev_cla\":\""      : "", d.devClass ? d.devClass : "", d.devClass ? "\"," : "",
      d.icona    ? "\"ic\":\""           : "", d.icona    ? d.icona    : "", d.icona    ? "\"," : "",
      TOPIC_PONTE, DEV);
    pubblica(topic, payload);
  }

  Serial.println(F("[HA] Entita' diagnostiche del ponte pubblicate."));
}
