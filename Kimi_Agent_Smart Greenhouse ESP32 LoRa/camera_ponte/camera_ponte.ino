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

/*
 * Perche' e' caduta l'ultima connessione MQTT, e con che segnale WiFi.
 *
 * Finora si sapeva solo QUANTE volte era caduta. Il log del broker diceva
 * sempre "exceeded timeout", cioe' "non ho piu' ricevuto niente da te", e
 * da li' non si poteva distinguere fra due cose molto diverse: il ponte che
 * smette di trasmettere, e il ponte che trasmette senza che arrivi.
 *
 * mqtt.state() letto NEL MOMENTO in cui ci si accorge della caduta separa i
 * due casi: -4 (timeout) significa che e' stato il ponte a chiudere perche'
 * il suo ping non ha avuto risposta, -3 (lost) che se l'e' vista chiudere
 * sotto. Il RSSI dello stesso istante dice se quando succede il segnale era
 * gia' in difficolta'.
 */
static int      statoUltimaCaduta   = 0;
static int      rssiUltimaCaduta    = 0;

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
  // Si prova PRIMA a riconfigurare e solo dopo a inizializzare.
  // Nel core ESP32 3.x il TWDT e' gia' avviato dal framework, quindi init()
  // per primo fallisce sempre e stampa un "E (...) task_wdt: TWDT already
  // initialized" rosso nel log di avvio: sembra un guasto grave e invece e'
  // rumore. Correzione gia' applicata al nodo in watchdog.h; qui il ponte
  // aveva la sua copia locale ed era rimasta indietro.
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);
  if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&cfg);
  if (e != ESP_OK) Serial.printf("[WDT] Configurazione fallita (err=%d)\n", (int)e);
#else
  esp_task_wdt_init(secondi, true);
#endif
  // Anche qui: iscrivere un task gia' iscritto stampa un altro errore rosso.
  if (esp_task_wdt_status(NULL) != ESP_OK) esp_task_wdt_add(NULL);
}

// ============================ WIFI ==========================================

/*
 * Scansione delle reti visibili, stampata all'avvio.
 *
 * Serve a scegliere l'access point guardando i numeri invece che a intuito:
 * il segnale va misurato DALLA POSIZIONE DEL PONTE, non da dove sei tu col
 * telefono. Mostra anche se la rete configurata e' effettivamente visibile,
 * il che smaschera subito i due errori piu' comuni: SSID scritto male e rete
 * a 5 GHz, che l'ESP32 non puo' vedere perche' ha solo la radio a 2,4 GHz.
 */
#if SCANSIONE_WIFI_AVVIO
static void wifiScansione() {
  Serial.println(F("[WiFi] Scansione delle reti visibili da qui..."));
  int n = WiFi.scanNetworks();

  if (n <= 0) {
    Serial.println(F("[WiFi] Nessuna rete trovata."));
    return;
  }

  bool trovataLaNostra = false;
  Serial.println(F("       RSSI  canale  SSID"));
  for (int i = 0; i < n && i < 12; i++) {
    bool nostra = (WiFi.SSID(i) == WIFI_SSID);
    if (nostra) trovataLaNostra = true;
    Serial.printf("      %4d  %6d  %s%s\n", WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.SSID(i).c_str(), nostra ? "   <== configurata" : "");
  }

  if (!trovataLaNostra) {
    Serial.printf("[WiFi] ATTENZIONE: \"%s\" non e' fra le reti visibili.\n", WIFI_SSID);
    Serial.println(F("[WiFi] Controlla il nome (occhio a maiuscole e spazi) e"));
    Serial.println(F("[WiFi] ricorda che l'ESP32 vede SOLO i 2,4 GHz, mai i 5 GHz."));
  }
  WiFi.scanDelete();
}
#endif

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
#if SCANSIONE_WIFI_AVVIO
  wifiScansione();
#endif
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

      int rssi = WiFi.RSSI();
      Serial.printf("[WiFi] Connesso dopo %lu s: %s (RSSI %d dBm) - riconnessioni: %lu\n",
                    (unsigned long)((millis() - wifiGiuDa) / 1000UL),
                    WiFi.localIP().toString().c_str(), rssi,
                    (unsigned long)riconnessioniWifi);

      // Segnalato all'avvio invece di lasciarlo scoprire dai sintomi: sotto
      // questa soglia le cadute di connessione sono attese, non un guasto.
      if (rssi < WIFI_RSSI_DEBOLE)
        Serial.printf("[WiFi] ATTENZIONE: %d dBm e' un segnale debole. Aspettati "
                      "riconnessioni: avvicina il ponte o cambia access point.\n", rssi);

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

/*
 * Scarto in secondi fra l'ora civile e UTC in questo istante: +7200 con l'ora
 * legale, +3600 con quella solare.
 *
 * Serve perche' time() restituisce UTC e il nodo scrive quel valore tale e
 * quale nel DS1307, per poi confrontare rtc.now().hour() con l'orario
 * programmato da Home Assistant. Senza questo campo "irriga alle 6:00"
 * significava le 6:00 UTC, cioe' le 8:00 italiane d'estate e le 7:00
 * d'inverno: l'acqua arrivava ogni giorno due ore dopo il previsto.
 *
 * Le regole dell'ora legale stanno solo qui, dentro NTP_TZ, ed e' giusto che
 * ci restino: il nodo non ha ne' modo ne' motivo di conoscerle, gli basta il
 * numero che il ponte gli passa a ogni ACK.
 *
 * Il calcolo NON usa tm_gmtoff: e' un'estensione che newlib espone solo con
 * certe combinazioni di macro di visibilita', e una compilazione che oggi
 * funziona potrebbe smettere di farlo al prossimo aggiornamento del core.
 * Confrontare le due rappresentazioni dello stesso istante e' equivalente e
 * non dipende da nulla.
 */
static int32_t offsetFuso() {
  time_t adesso = time(nullptr);
  if (adesso < 1700000000L) return 0;

  struct tm loc, utc;
  localtime_r(&adesso, &loc);
  gmtime_r   (&adesso, &utc);

  int32_t scarto = (int32_t)(loc.tm_hour - utc.tm_hour) * 3600L
                 + (int32_t)(loc.tm_min  - utc.tm_min ) * 60L
                 + (int32_t)(loc.tm_sec  - utc.tm_sec );

  // Le due date possono cadere a cavallo della mezzanotte: in quel caso la
  // sola differenza oraria e' sbagliata di un giorno intero. Lo scarto fra i
  // giorni dell'anno vale +-1 in condizioni normali e +-364/365 a Capodanno,
  // quando l'anno cambia sotto i piedi al confronto.
  int giorni = loc.tm_yday - utc.tm_yday;
  if (giorni ==  1 || giorni < -1) scarto += 86400L;
  if (giorni == -1 || giorni >  1) scarto -= 86400L;

  return scarto;
}

// ============================ MQTT ==========================================

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  codaMessaggioMqtt(topic, payload, len);
}

static void mqttMantieni() {
  if (WiFi.status() != WL_CONNECTED) return;

  static bool eraConnesso = false;
  if (mqtt.connected()) { eraConnesso = true; return; }

  // Passaggio da connesso a caduto: e' l'unico istante in cui mqtt.state()
  // contiene ancora il motivo, prima che il tentativo di riconnessione lo
  // sovrascriva con il proprio esito.
  if (eraConnesso) {
    eraConnesso       = false;
    statoUltimaCaduta = mqtt.state();
    rssiUltimaCaduta  = WiFi.RSSI();
    Serial.printf("[MQTT] Connessione caduta (stato=%d, RSSI %d dBm, "
                  "uptime %lu s).\n",
                  statoUltimaCaduta, rssiUltimaCaduta,
                  (unsigned long)(millis() / 1000UL));
  }

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

    /*
     * Algoritmo di Nagle disattivato, e va rifatto a OGNI connessione
     * perche' il socket e' nuovo ogni volta.
     *
     * Nagle trattiene i pacchetti piccoli finche' i dati precedenti non
     * sono stati confermati, per non sprecare rete con tanti invii minuscoli.
     * Il PINGREQ di MQTT e' lungo DUE byte, ed e' esattamente il pacchetto
     * che Nagle ama trattenere. Se la conferma TCP di una pubblicazione
     * tarda -- e con il WiFi tarda spesso, perche' anche dall'altra parte
     * c'e' un ritardo volontario sulle conferme -- il ping resta fermo nel
     * buffer di uscita. Il broker non riceve piu' niente, e dopo una volta e
     * mezza il keepalive chiude la sessione: nel suo log si legge
     * "exceeded timeout", che sembra un ponte morto e invece e' un ponte
     * vivissimo con due byte in ostaggio.
     *
     * Qui non c'e' niente da risparmiare: i messaggi sono pochi e radi.
     */
    espClient.setNoDelay(true);

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
/*
 * Tempo di volo di un pacchetto, in millisecondi. Formula del datasheet
 * SX1276/78 paragrafo 4.1.1.7: header esplicito, CRC attivo, low data rate
 * optimization spenta. LORA_CR vale gia' 5, cioe' e' il (CR + 4) della
 * formula. Gemella di quella in serra_nodo/radio.cpp, verificata da
 * tools/test_tempo_volo.py.
 */
static uint32_t tempoDiVoloMs(size_t lunghezza) {
  const float tSimbolo = (float)(1UL << LORA_SF) / (float)LORA_BW;   // secondi

  int32_t numeratore   = 8 * (int32_t)lunghezza - 4 * LORA_SF + 28 + 16;
  int32_t denominatore = 4 * LORA_SF;
  int32_t simboli = 8;
  if (numeratore > 0)
    simboli += ((numeratore + denominatore - 1) / denominatore) * LORA_CR;

  const float preambolo = (8.0f + 4.25f) * tSimbolo;
  return (uint32_t)((preambolo + simboli * tSimbolo) * 1000.0f) + 1;
}

/*
 * Trasmette e torna in ascolto, senza attese illimitate.
 *
 * La versione sincrona di LoRa.endPacket() e' un ciclo che aspetta il flag
 * di TxDone senza timeout e senza nutrire il watchdog: sul nodo si e'
 * impiantata piu' volte. Qui si parte in modo asincrono e si chiede a
 * beginPacket() quando il modulo e' di nuovo libero -- risponde 0 finche'
 * trasmette, 1 quando ha finito -- perche' isTransmitting() nella libreria
 * 0.8.0 e' privata. Gli effetti collaterali della chiamata che riesce
 * (standby e puntatori del FIFO azzerati) sono innocui: subito dopo si
 * passa comunque in ricezione.
 */
static void trasmettiAck(const char* pacchetto) {
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(pacchetto);
  LoRa.endPacket(true);           // asincrona: scrive un registro e torna

  const uint32_t limite = tempoDiVoloMs(strlen(pacchetto)) * (uint32_t)TX_GUARDIA_X
                        + (uint32_t)TX_GUARDIA_MS;
  const uint32_t t0     = millis();
  bool           finita = false;

  while (millis() - t0 < limite) {
    esp_task_wdt_reset();
    if (LoRa.beginPacket() == 1) { finita = true; break; }
    delay(1);
  }

  if (!finita)
    Serial.printf("[LoRa] ACK non concluso in %lu ms: il modulo non risponde.\n",
                  (unsigned long)limite);

  LoRa.receive();                 // subito di nuovo in ascolto
}

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
  if (adesso > 0) {
    accoda(";now=%lu", (unsigned long)adesso);
    accoda(";tz=%ld",  (long)offsetFuso());
  }

  if (allegaComando && !codaVuota()) {
    ComandoInCoda cmd;
    if (codaEstrai(cmd)) {
      accoda(";c=%lu;o=%s;a=%s", (unsigned long)cmd.id, cmd.opcode, cmd.args);
      comandiConsegnati++;
    }
  }

  trasmettiAck(ack);

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
    "\"mqtt_caduta\":%d,\"rssi_caduta\":%d,"
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
    statoUltimaCaduta, rssiUltimaCaduta,
    WiFi.localIP().toString().c_str(), FW_VERSION_PONTE);

  mqtt.publish(TOPIC_DIAG, (const uint8_t*)payload, strlen(payload), true);
}

// ============================ SETUP / LOOP ==================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.printf ("  PONTE LoRa <-> MQTT - firmware %s\n", FW_VERSION_PONTE);
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
