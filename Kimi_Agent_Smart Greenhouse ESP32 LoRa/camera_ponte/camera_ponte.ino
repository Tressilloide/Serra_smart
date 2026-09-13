/*
 * ============================================================================
 *  SERRA SMART — PONTE LoRa <-> WiFi/MQTT  (ESP32 in camera)
 * ============================================================================
 *
 *  Firmware 2.0 — vedi docs/Guida_Serra_Smart.md e docs/PROTOCOLLO.md
 *
 *  ---------------------------------------------------------------------------
 *  COMPITI
 *  ---------------------------------------------------------------------------
 *   - Restare SEMPRE in ascolto LoRa: il nodo trasmette per pochi secondi ogni
 *     15 minuti, non possiamo permetterci di perdere quella finestra
 *   - Pubblicare ogni pacchetto su MQTT e rispondere con un ACK SOLO se la
 *     pubblicazione e' riuscita, cosi' un dato non confermato resta al sicuro
 *     sulla microSD della serra
 *   - Consegnare al nodo i comandi che Home Assistant ha messo in coda,
 *     agganciandoli all'ACK: e' l'unico momento in cui la serra ascolta
 *   - Allegare a ogni ACK l'ora NTP, cosi' il DS1307 della serra si
 *     risincronizza da solo quando la batteria tampone si scarica
 *   - Generare da solo le entita' di Home Assistant, comprese quelle nuove
 *     che non conosce (discovery generica)
 *   - Riconnettersi ad automaticamente a WiFi e MQTT senza mai smettere di
 *     ascoltare la radio
 *
 *  ---------------------------------------------------------------------------
 *  LIBRERIE NECESSARIE
 *  ---------------------------------------------------------------------------
 *    - "LoRa" by Sandeep Mistry
 *    - "PubSubClient" by Nick O'Leary
 *
 *  PRIMA DI COMPILARE: copia secrets.h.example in secrets.h e compilalo.
 *
 *  Scheda: "ESP32 Dev Module"
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "secrets.h"
#include "protocollo.h"
#include "discovery.h"
#include "comandi.h"

#if ABILITA_OTA
  #include <ArduinoOTA.h>
#endif

#ifndef ESP_ARDUINO_VERSION_VAL
  #define ESP_ARDUINO_VERSION_VAL(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#endif
#ifndef ESP_ARDUINO_VERSION
  #define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(2, 0, 0)
#endif

// ============================ GLOBALI =======================================

WiFiClient   espClient;
PubSubClient mqtt(espClient);

static uint32_t ultimoTentativoWifi = 0;
static uint32_t ultimoTentativoMqtt = 0;
static uint32_t ultimaDiagnostica   = 0;
static uint32_t ultimaDiscovery     = 0;   // 0 = mai pubblicata
static uint32_t wifiGiuDa           = 0;

static uint32_t riconnessioniWifi   = 0;
static uint32_t riconnessioniMqtt   = 0;
static uint32_t pacchettiRicevuti   = 0;
static uint32_t pacchettiScartati   = 0;
static uint32_t comandiConsegnati   = 0;
static int      ultimoRssi          = 0;
static float    ultimoSnr           = 0.0f;

// Anti-duplicati: il nodo ritrasmette se l'ACK si perde, e senza questo
// controllo il dato finirebbe due volte nello storico e nel totale dell'acqua.
struct ChiaveDedup { uint32_t seq; uint32_t ts; };
static ChiaveDedup dedup[DEDUP_MEMORIA];
static uint8_t     dedupIdx = 0;

// ============================ WATCHDOG ======================================

static void wdtImposta(uint32_t secondi) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = secondi * 1000UL;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;
  esp_err_t e = esp_task_wdt_init(&cfg);
  if (e == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(secondi, true);
#endif
  esp_task_wdt_add(NULL);
}

// ============================ WIFI ==========================================

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  /*
   * Modem-sleep DISATTIVATO, e non e' un dettaglio.
   *
   * Con il modem-sleep la radio WiFi si spegne a intervalli per risparmiare
   * corrente. Su un dispositivo a batteria ha senso; qui il ponte sta attaccato
   * a un alimentatore USB e il risparmio non serve a nulla, mentre il prezzo e'
   * alto: la connessione TCP verso il broker cade in silenzio ogni tanto, e
   * quando il ponte si riconnette il broker pubblica il Last Will della
   * sessione morta. Risultato in Home Assistant: tutte le entita' della serra
   * diventano "non disponibile" per un secondo e poi tornano, a ripetizione.
   */
  WiFi.setSleep(false);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Connessione a %s ...\n", WIFI_SSID);
}

// Non bloccante: il LoRa deve continuare ad ascoltare anche mentre il WiFi
// e' giu'. Nessuna delle funzioni chiamate qui puo' fermare il loop.
static void wifiMantieni() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiGiuDa != 0) {
      // Stesso ragionamento del contatore MQTT: all'avvio il WiFi risulta
      // "giu'" per i primi secondi, prima ancora di essersi mai collegato.
      // Quell'aggancio iniziale non e' una riconnessione.
      static bool primoAggancio = true;
      if (primoAggancio) primoAggancio = false;
      else               riconnessioniWifi++;

      Serial.printf("[WiFi] Riconnesso dopo %lu s: %s (RSSI %d dBm) - riconnessione n.%lu\n",
                    (unsigned long)((millis() - wifiGiuDa) / 1000UL),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                    (unsigned long)riconnessioniWifi);
      wifiGiuDa = 0;
    }
    return;
  }

  if (wifiGiuDa == 0) wifiGiuDa = millis();

  /*
   * Riavvio SOLO se il WiFi resta giu' a lungo, non piu' a intervalli fissi.
   * La versione precedente faceva ESP.restart() ogni ora a prescindere:
   * durante i secondi di riavvio la radio non ascolta, e con il nodo che
   * trasmette ogni 15 minuti c'era una probabilita' concreta di perdere
   * proprio quel pacchetto.
   */
  if (millis() - wifiGiuDa > REBOOT_WIFI_DOWN_MS) {
    Serial.println(F("[WiFi] Giu' da troppo tempo: riavvio il ponte."));
    delay(500);
    ESP.restart();
  }

  if (millis() - ultimoTentativoWifi < WIFI_RETRY_MS) return;
  ultimoTentativoWifi = millis();

  Serial.println(F("[WiFi] Disconnesso, ritento..."));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================ ORA (NTP) =====================================

// L'ora del ponte viaggia su ogni ACK ed e' quella con cui la serra corregge
// il proprio DS1307: e' l'unico orologio affidabile del sistema.
static uint32_t oraCorrente() {
  time_t adesso = time(nullptr);
  return (adesso > 1700000000L) ? (uint32_t)adesso : 0;
}

// ============================ MQTT ==========================================

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  codaMessaggioMqtt(topic, payload, len);
}

static void mqttMantieni() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;
  if (millis() - ultimoTentativoMqtt < MQTT_RETRY_MS) return;
  ultimoTentativoMqtt = millis();

  Serial.print(F("[MQTT] Connessione al broker... "));

  // Last Will: se il ponte muore, Home Assistant segna subito le entita'
  // come non disponibili invece di mostrare valori vecchi come se fossero attuali.
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                   TOPIC_PONTE, 1, true, "offline")) {
    // La PRIMA connessione non e' una riconnessione: contarla farebbe
    // sembrare instabile un ponte appena avviato che invece sta benissimo.
    // Questi contatori servono a rispondere a "sta reggendo?", quindi devono
    // partire da zero e restarci finche' non succede davvero qualcosa.
    static bool primaConnessione = true;
    if (primaConnessione) primaConnessione = false;
    else                  riconnessioniMqtt++;

    Serial.printf("OK! (riconnessioni finora: %lu, uptime %lu s)\n",
                  (unsigned long)riconnessioniMqtt,
                  (unsigned long)(millis() / 1000UL));
    mqtt.publish(TOPIC_PONTE, "online", true);
    mqtt.subscribe(TOPIC_CMD_SUB, 1);
    // Subito dopo la sottoscrizione il broker riversa i messaggi ritenuti:
    // i comandi di azione che arrivano adesso sono residui, non richieste.
    codaSegnalaSottoscrizione();

    /*
     * La discovery NON si ripubblica a ogni riconnessione.
     *
     * Sono tredici messaggi ritenuti da ~400 byte: una raffica di 5 KB spinta
     * nel socket alla massima velocita'. Su un WiFi debole basta a far cadere
     * la connessione appena stabilita, il che provoca una riconnessione, che
     * rilancia la raffica. Nel log del ponte si vedeva nitidamente:
     * connessione riuscita, tredici pubblicazioni, caduta dopo cinque secondi,
     * e tutto da capo.
     *
     * Ripubblicare ogni volta era comunque inutile: quei messaggi sono
     * RITENUTI, quindi e' il broker a conservarli. Serve farlo al primo avvio
     * e, per sicurezza, ogni tanto, nel caso il broker sia stato reinstallato.
     */
    uint32_t adessoMs = millis();
    bool servePubblicare = (ultimaDiscovery == 0) ||
                           (adessoMs - ultimaDiscovery > DISCOVERY_RIPUBBLICA_MS);

    if (servePubblicare) {
      discoveryReset();
      if (discoveryPubblicaComandi() && discoveryPubblicaPonte())
        ultimaDiscovery = adessoMs ? adessoMs : 1;   // 0 e' riservato a "mai"
      else
        Serial.println(F("[HA] Discovery interrotta: riprovo alla prossima connessione."));
    } else {
      Serial.printf("[HA] Discovery gia' pubblicata %lu min fa: la salto.\n",
                    (unsigned long)((adessoMs - ultimaDiscovery) / 60000UL));
    }

    codaPubblicaPending();
  } else {
    Serial.printf("fallita (rc=%d), riprovo tra %lu s.\n",
                  mqtt.state(), MQTT_RETRY_MS / 1000UL);
  }
}

// ============================ DEDUP =========================================

static bool giaVisto(uint32_t seq, uint32_t ts) {
  for (uint8_t i = 0; i < DEDUP_MEMORIA; i++)
    if (dedup[i].seq == seq && dedup[i].ts == ts) return true;
  return false;
}

static void ricorda(uint32_t seq, uint32_t ts) {
  dedup[dedupIdx].seq = seq;
  dedup[dedupIdx].ts  = ts;
  dedupIdx = (dedupIdx + 1) % DEDUP_MEMORIA;
}

// ============================ JSON ==========================================

// true se la stringa e' un numero valido per intero (e non, per esempio,
// "2.0.0" o "ok", che nel JSON vanno virgolettati).
static bool eNumerico(const char* s) {
  if (!s || !*s) return false;
  char* fine = nullptr;
  strtod(s, &fine);
  return fine && *fine == '\0';
}

/*
 * Converte il pacchetto in JSON copiando OGNI coppia chiave=valore cosi'
 * com'e'. E' qui che si vede il vantaggio del protocollo v2: il ponte non ha
 * bisogno di sapere quali sensori esistono, quindi aggiungerne uno alla serra
 * non richiede di riflashare anche lui.
 */
static size_t costruisciJson(const PacchettoKV& pkt, char* out, size_t maxOut,
                             int rssi, float snr) {
  /*
   * La coda del JSON viene preparata PRIMA e il suo spazio riservato per tutto
   * il ciclo. Non e' pignoleria: se il buffer si riempisse con i campi, la
   * graffa di chiusura verrebbe troncata e Home Assistant scarterebbe l'intero
   * pacchetto come JSON non valido — tutti i sensori fermi, senza un errore
   * che spieghi perche'. Meglio perdere qualche campo che l'intero messaggio.
   */
  char coda[96];
  int  lCoda = snprintf(coda, sizeof(coda),
                        ",\"rssi\":%d,\"snr\":%.1f,\"ts_ponte\":%lu}",
                        rssi, snr, (unsigned long)oraCorrente());
  if (lCoda < 0 || (size_t)lCoda >= sizeof(coda) || maxOut < (size_t)lCoda + 4) {
    out[0] = '\0';
    return 0;
  }

  size_t pos = 0;
  out[pos++] = '{';
  out[pos]   = '\0';

  bool primo = true, tagliato = false;
  char campo[PROTO_LEN_CHIAVE + PROTO_LEN_VALORE + 8];

  for (uint8_t i = 0; i < pkt.n(); i++) {
    const CampoKV& c = pkt.campo(i);

    int l = eNumerico(c.valore)
          ? snprintf(campo, sizeof(campo), "%s\"%s\":%s",
                     primo ? "" : ",", c.chiave, c.valore)
          : snprintf(campo, sizeof(campo), "%s\"%s\":\"%s\"",
                     primo ? "" : ",", c.chiave, c.valore);
    if (l < 0 || (size_t)l >= sizeof(campo)) continue;   // campo anomalo: saltato

    // Il campo ci sta solo se dopo di lui ci sta ancora tutta la coda.
    if (pos + (size_t)l + (size_t)lCoda + 1 > maxOut) { tagliato = true; break; }

    memcpy(out + pos, campo, (size_t)l);
    pos += (size_t)l;
    out[pos] = '\0';
    primo = false;
  }

  // Se non e' entrato nessun campo va tolta la virgola iniziale della coda,
  // o verrebbe fuori {,"rssi":...} che non e' JSON valido.
  const char* codaDa  = primo ? coda + 1 : coda;
  size_t      codaLen = primo ? (size_t)lCoda - 1 : (size_t)lCoda;

  memcpy(out + pos, codaDa, codaLen);
  pos += codaLen;
  out[pos] = '\0';

  if (tagliato)
    Serial.printf("[MQTT] ATTENZIONE: JSON oltre i %u byte, alcuni campi omessi. "
                  "Alza la dimensione del buffer in gestisciLoRa().\n", (unsigned)maxOut);

  return pos;
}

// ============================ LORA ==========================================

/*
 * Costruisce e invia l'ACK. E' il messaggio piu' importante del sistema:
 *   - conferma al nodo che il dato e' arrivato fino al broker
 *   - porta l'ora corrente, con cui la serra corregge il proprio orologio
 *   - porta l'eventuale comando da eseguire
 * Tutto questo senza un solo pacchetto in piu' rispetto a prima.
 */
static void inviaAck(uint32_t seq, bool allegaComando) {
  char ack[PROTO_MAX_PAYLOAD + 1];

  /*
   * snprintf ritorna quanti caratteri AVREBBE scritto, non quanti ne ha
   * scritti davvero. Accumulando quel valore senza limitarlo, "pos" puo'
   * superare la dimensione del buffer e la sottrazione successiva va in
   * underflow (sono size_t), passando a snprintf una dimensione enorme.
   * Da qui in poi si scriverebbe oltre il buffer. Questa lambda tiene "pos"
   * dentro i limiti qualunque cosa succeda.
   */
  size_t pos = 0;
  auto accoda = [&](const char* fmt, auto... args) {
    if (pos >= sizeof(ack) - 1) return;
    int n = snprintf(ack + pos, sizeof(ack) - pos, fmt, args...);
    if (n < 0) return;
    pos = (size_t)n >= sizeof(ack) - pos ? sizeof(ack) - 1 : pos + (size_t)n;
  };

  accoda("%s;s=%lu", PROTO_PREFIX_ACK, (unsigned long)seq);

  uint32_t adesso = oraCorrente();
  if (adesso > 0) accoda(";now=%lu", (unsigned long)adesso);

  if (allegaComando && !codaVuota()) {
    ComandoInCoda cmd;
    if (codaEstrai(cmd)) {
      accoda(";c=%lu;o=%s;a=%s", (unsigned long)cmd.id, cmd.opcode, cmd.args);
      comandiConsegnati++;
    }
  }

  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(ack);
  LoRa.endPacket();
  LoRa.receive();                 // subito di nuovo in ascolto

  Serial.printf("[LoRa] ACK -> %s\n", ack);
}

static void gestisciLoRa() {
  int sz = LoRa.parsePacket();
  if (sz <= 0) return;

  char buf[PROTO_MAX_PAYLOAD + 1];
  int  n = 0;
  while (LoRa.available() && n < (int)sizeof(buf) - 1) buf[n++] = (char)LoRa.read();
  buf[n] = '\0';

  ultimoRssi = LoRa.packetRssi();
  ultimoSnr  = LoRa.packetSnr();
  pacchettiRicevuti++;

  Serial.printf("\n[LoRa] #%lu (RSSI %d dBm, SNR %.1f dB, %d byte): %s\n",
                (unsigned long)pacchettiRicevuti, ultimoRssi, ultimoSnr, n, buf);

  // --- Parsing: v2 (chiave=valore) oppure v1 (CSV posizionale) -------------
  // Il supporto al v1 serve durante l'aggiornamento: sulla microSD del nodo
  // possono esserci record accodati dalla versione precedente del firmware.
  char        prefisso[12] = {0};
  PacchettoKV pkt;
  bool        ok;

  if (protoEV2(buf)) ok = pkt.parse(buf, prefisso, sizeof(prefisso));
  else               ok = protoParseV1(buf, pkt, prefisso, sizeof(prefisso));

  if (!ok || strcmp(prefisso, "GH1") != 0) {
    Serial.println(F("[LoRa] Pacchetto non riconosciuto, ignorato."));
    pacchettiScartati++;
    return;
  }

  uint32_t seq = pkt.valoreU("s", 0);
  uint32_t ts  = pkt.valoreU("t", 0);

  // --- Duplicato? Si conferma comunque, ma non si ripubblica ---------------
  if (giaVisto(seq, ts)) {
    Serial.println(F("[LoRa] Duplicato (ACK perso in precedenza): confermo senza ripubblicare."));
    inviaAck(seq, false);
    return;
  }

  // --- Fresco o storico? ---------------------------------------------------
  // Un record ripescato dal backlog non deve finire su serra/nodo/stato:
  // il nodo trasmette prima il pacchetto attuale e poi la coda arretrata,
  // quindi Home Assistant finirebbe per mostrare come "valore corrente" una
  // lettura di ore prima. ts == 0 significa che il nodo non conosce ancora
  // l'ora: e' un pacchetto fresco, e anzi ha bisogno del "now" nell'ACK.
  uint32_t adesso = oraCorrente();
  bool storico = (ts > 0) && (adesso > 0) && (adesso > ts) &&
                 ((adesso - ts) > SOGLIA_STORICO_SEC);

  const char* topic = storico ? TOPIC_STORICO : TOPIC_STATO;

  /*
   * 900 byte e non 640: un pacchetto LoRa pieno (250 byte, fino a 28 campi)
   * espanso in JSON con virgolette e nomi di chiave puo' superare
   * abbondantemente i 640, e il buffer precedente era piu' piccolo di quello
   * che il protocollo stesso permette di ricevere.
   */
  char json[900];
  costruisciJson(pkt, json, sizeof(json), ultimoRssi, ultimoSnr);
  Serial.printf("[MQTT] %s <- %s\n", topic, json);

  // Retained solo per lo stato attuale: dopo un riavvio di Home Assistant i
  // sensori hanno subito un valore invece di restare vuoti fino al risveglio
  // successivo del nodo (fino a 15 minuti di buco).
  bool pubblicato = mqtt.connected() &&
                    mqtt.publish(topic, (const uint8_t*)json, strlen(json), !storico);

  if (!pubblicato) {
    // Niente ACK: la serra conserva il dato su microSD e lo rimandera'.
    // E' cosi' che la catena "nessun dato perso" resta intatta.
    Serial.println(F("[MQTT] Pubblicazione fallita: NIENTE ACK, la serra terra' il dato su SD."));
    return;
  }

  ricorda(seq, ts);

  // --- Discovery: crea le entita' mancanti, anche per chiavi sconosciute ---
  for (uint8_t i = 0; i < pkt.n(); i++)
    discoveryAssicuraSensore(pkt.campo(i).chiave);

  /*
   * rssi e snr vanno annunciati a parte, ed e' un errore che e' costato due
   * entita' mai nate: non arrivano dal nodo, li aggiunge il ponte in fondo al
   * JSON. Il ciclo qui sopra scorre i campi del PACCHETTO, dove quei due non
   * compaiono, quindi la loro discovery non veniva pubblicata mai — pur
   * essendo regolarmente presenti nella tabella e usati dalle dashboard.
   */
  discoveryAssicuraSensore("rssi");
  discoveryAssicuraSensore("snr");

  // --- Esito di un comando eseguito dal nodo -------------------------------
  if (pkt.ha("res")) {
    const char* det = pkt.valore("det");
    codaPubblicaEsito(pkt.valoreU("res", 0), pkt.valoreU("rc", 0), det ? det : "");
  }

  // --- ACK, con eventuale comando ------------------------------------------
  // Ai record storici NON si allegano comandi: il nodo li invia con una
  // funzione che non li interpreta, quindi il comando andrebbe perso.
  inviaAck(seq, !storico);
}

// ============================ DIAGNOSTICA ===================================

static void pubblicaDiagnostica() {
  if (!mqtt.connected()) return;
  if (millis() - ultimaDiagnostica < DIAG_INTERVALLO_MS) return;
  ultimaDiagnostica = millis();

  // 448 e non 320: con tutti i contatori ai valori massimi il JSON arriva a
  // 284 byte, e 36 byte di margine sono troppo pochi per aggiungerci un altro
  // campo domani senza accorgersi del troncamento.
  char payload[448];
  snprintf(payload, sizeof(payload),
    "{\"uptime\":%lu,\"pkt\":%lu,\"scartati\":%lu,\"cmd_consegnati\":%lu,"
    "\"riconn_wifi\":%lu,\"riconn_mqtt\":%lu,"
    "\"coda\":%u,\"wifi_rssi\":%d,\"heap\":%lu,"
    "\"heap_blocco\":%lu,\"heap_minimo\":%lu,"
    "\"lora_rssi\":%d,\"lora_snr\":%.1f,"
    "\"ip\":\"%s\",\"fw\":\"%s\"}",
    (unsigned long)(millis() / 1000UL),
    (unsigned long)pacchettiRicevuti, (unsigned long)pacchettiScartati,
    (unsigned long)comandiConsegnati,
    (unsigned long)riconnessioniWifi, (unsigned long)riconnessioniMqtt,
    codaConta(),
    WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(),
    /*
     * heap_blocco e heap_minimo servono a rispondere alla domanda che la sola
     * memoria libera NON risolve: la heap si sta frammentando?
     *
     * Un uso intenso di String alloca e libera blocchi di dimensioni sempre
     * diverse, lasciando buchi. Dopo settimane il totale libero puo' restare
     * identico mentre il piu' grande blocco CONTIGUO si rimpicciolisce, finche'
     * un'allocazione fallisce e il dispositivo si riavvia. Guardando solo la
     * memoria libera non si vede arrivare nulla.
     *
     *   heap_blocco  = il piu' grande blocco allocabile in questo momento.
     *                  Se cala giorno dopo giorno mentre "heap" resta stabile,
     *                  quella e' frammentazione.
     *   heap_minimo  = il minimo storico di memoria libera dall'avvio, utile a
     *                  sapere quanto margine c'e' stato davvero nei picchi.
     */
    (unsigned long)ESP.getMaxAllocHeap(),
    (unsigned long)ESP.getMinFreeHeap(),
    ultimoRssi, ultimoSnr,
    WiFi.localIP().toString().c_str(), FW_VERSION_PONTE);

  mqtt.publish(TOPIC_DIAG, (const uint8_t*)payload, strlen(payload), true);
}

// ============================ SETUP / LOOP ==================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.printf ("  PONTE LoRa <-> MQTT — firmware %s\n", FW_VERSION_PONTE);
  Serial.println(F("============================================================"));

  // --- LoRa: gli stessi identici parametri radio del nodo serra ------------
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_BAND)) {
    Serial.println(F("[LoRa] ERRORE: modulo non trovato! Riavvio tra 5 s..."));
    Serial.println(F("[LoRa] Controlla cablaggio SPI, NSS=GPIO5, 3,3 V e ANTENNA montata."));
    delay(5000);
    ESP.restart();
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.enableCrc();
  LoRa.receive();
  Serial.println(F("[LoRa] In ascolto continuo."));

  wifiSetup();

  configTzTime(NTP_TZ, NTP_SERVER1, NTP_SERVER2);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  // Deve contenere il piu' grande fra: payload di discovery (~400 byte) e
   // JSON di stato (fino a 900), piu' topic e intestazione MQTT.
  mqtt.setBufferSize(1536);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  discoveryInit(&mqtt);
  codaInit(&mqtt);

#if ABILITA_OTA
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println(F("[OTA] Aggiornamento in corso..."));
    esp_task_wdt_delete(NULL);    // l'upload puo' superare il timeout
  });
  ArduinoOTA.begin();
  Serial.printf("[OTA] Attivo come \"%s\".\n", OTA_HOSTNAME);
#endif

  // Watchdog sul loop. Sostituisce il riavvio orario a tappeto: interviene
  // solo quando il ponte e' davvero bloccato, non ogni ora a prescindere.
  wdtImposta(WDT_LOOP_SEC);
}

void loop() {
  esp_task_wdt_reset();

  // Priorita' assoluta: la radio va servita per prima e a ogni giro.
  // Il nodo trasmette per pochi secondi ogni 15 minuti: quella finestra
  // non si puo' perdere.
  gestisciLoRa();

  wifiMantieni();
  mqttMantieni();
  mqtt.loop();

#if ABILITA_OTA
  ArduinoOTA.handle();
#endif

  pubblicaDiagnostica();

  delay(2);                       // un respiro allo stack WiFi
}
