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
static uint32_t wifiGiuDa           = 0;

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
  WiFi.setSleep(true);              // modem-sleep: risparmio energetico
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] Connessione a %s ...\n", WIFI_SSID);
}

// Non bloccante: il LoRa deve continuare ad ascoltare anche mentre il WiFi
// e' giu'. Nessuna delle funzioni chiamate qui puo' fermare il loop.
static void wifiMantieni() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiGiuDa != 0) {
      Serial.printf("[WiFi] Riconnesso: %s (RSSI %d dBm)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
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
    Serial.println(F("OK!"));
    mqtt.publish(TOPIC_PONTE, "online", true);
    mqtt.subscribe(TOPIC_CMD_SUB, 1);

    // Ripubblicazione completa: cosi' le entita' si ricreano anche se il
    // broker ha perso i retained (per esempio dopo un suo riavvio).
    discoveryReset();
    discoveryPubblicaComandi();
    discoveryPubblicaPonte();
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
  size_t pos = 0;
  pos += snprintf(out + pos, maxOut - pos, "{");

  for (uint8_t i = 0; i < pkt.n() && pos < maxOut - 48; i++) {
    const CampoKV& c = pkt.campo(i);
    if (eNumerico(c.valore))
      pos += snprintf(out + pos, maxOut - pos, "%s\"%s\":%s", i ? "," : "", c.chiave, c.valore);
    else
      pos += snprintf(out + pos, maxOut - pos, "%s\"%s\":\"%s\"", i ? "," : "", c.chiave, c.valore);
  }

  // Dati aggiunti dal ponte: qualita' del collegamento radio e ora di ricezione
  pos += snprintf(out + pos, maxOut - pos,
                  ",\"rssi\":%d,\"snr\":%.1f,\"ts_ponte\":%lu}",
                  rssi, snr, (unsigned long)oraCorrente());
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
  int  pos = snprintf(ack, sizeof(ack), "%s;s=%lu", PROTO_PREFIX_ACK, (unsigned long)seq);

  uint32_t adesso = oraCorrente();
  if (adesso > 0)
    pos += snprintf(ack + pos, sizeof(ack) - pos, ";now=%lu", (unsigned long)adesso);

  if (allegaComando && !codaVuota()) {
    ComandoInCoda cmd;
    if (codaEstrai(cmd)) {
      pos += snprintf(ack + pos, sizeof(ack) - pos, ";c=%lu;o=%s;a=%s",
                      (unsigned long)cmd.id, cmd.opcode, cmd.args);
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

  char json[640];
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

  char payload[320];
  snprintf(payload, sizeof(payload),
    "{\"uptime\":%lu,\"pkt\":%lu,\"scartati\":%lu,\"cmd_consegnati\":%lu,"
    "\"coda\":%u,\"wifi_rssi\":%d,\"heap\":%lu,\"lora_rssi\":%d,\"lora_snr\":%.1f,"
    "\"ip\":\"%s\",\"fw\":\"%s\"}",
    (unsigned long)(millis() / 1000UL),
    (unsigned long)pacchettiRicevuti, (unsigned long)pacchettiScartati,
    (unsigned long)comandiConsegnati, codaConta(),
    WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(),
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
  mqtt.setBufferSize(1024);       // i payload di discovery sono lunghi
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
