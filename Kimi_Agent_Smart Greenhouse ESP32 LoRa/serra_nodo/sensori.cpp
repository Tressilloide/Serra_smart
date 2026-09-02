#include "sensori.h"
#include "impostazioni.h"
#include "irrigazione.h"

#include <Wire.h>
#include <driver/gpio.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#if USA_ADS1115
  #include <Adafruit_ADS1X15.h>
  static Adafruit_ADS1115 ads;
  static bool g_adsOk = false;
#endif

// ---------------------------------------------------------------------------
//  Stato interno
// ---------------------------------------------------------------------------

static Adafruit_BME280 bme;
static bool g_bmeOk = false;

// ---------------------------------------------------------------------------
//  Alimentazione commutata dei sensori
// ---------------------------------------------------------------------------

void sensoriAlimenta(bool acceso) {
#if PIN_PWR_SENSORI >= 0
  static bool inizializzato = false;
  if (!inizializzato) {
    // Sblocca il mantenimento impostato prima del deep sleep, poi scrive il
    // livello di riposo PRIMA di configurare il pin come uscita: cosi' un
    // relay attivo basso non riceve mai un impulso spurio all'avvio.
    gpio_hold_dis((gpio_num_t)PIN_PWR_SENSORI);
    digitalWrite(PIN_PWR_SENSORI, !PWR_SENSORI_ON);
    pinMode(PIN_PWR_SENSORI, OUTPUT);
    digitalWrite(PIN_PWR_SENSORI, !PWR_SENSORI_ON);
    inizializzato = true;
  }
  digitalWrite(PIN_PWR_SENSORI, acceso ? PWR_SENSORI_ON : !PWR_SENSORI_ON);
  if (acceso) delay(PWR_SETTLE_MS);   // attuazione relay + assestamento partitore
#else
  (void)acceso;
#endif
}

// ---------------------------------------------------------------------------
//  Lettura canale analogico: unico punto che sa se sotto c'e' l'ADC nativo
//  dell'ESP32 o un ADS1115 esterno. Cambiando USA_ADS1115 in config.h tutti
//  i sensori analogici si spostano sul bus I2C senza altre modifiche.
// ---------------------------------------------------------------------------

uint16_t leggiCanaleRaw(uint8_t canale) {
#if USA_ADS1115
  if (!g_adsOk) return 0;
  int16_t v = ads.readADC_SingleEnded(canale);
  return v < 0 ? 0 : (uint16_t)v;
#else
  uint32_t acc = 0;
  for (uint8_t i = 0; i < ADC_CAMPIONI; i++) {
    acc += analogRead(canale);
    delay(ADC_DELAY_MS);
  }
  return (uint16_t)(acc / ADC_CAMPIONI);
#endif
}

float leggiCanaleMilliVolt(uint8_t canale) {
#if USA_ADS1115
  if (!g_adsOk) return NAN;
  return ads.computeVolts(ads.readADC_SingleEnded(canale)) * 1000.0f;
#else
  // analogReadMilliVolts applica la curva di calibrazione di fabbrica salvata
  // negli eFuse: molto piu' accurato di (raw/4095)*3300.
  uint32_t acc = 0;
  for (uint8_t i = 0; i < ADC_CAMPIONI; i++) {
    acc += analogReadMilliVolts(canale);
    delay(ADC_DELAY_MS);
  }
  return (float)acc / (float)ADC_CAMPIONI;
#endif
}

// ---------------------------------------------------------------------------
//  Flussometro YF-S201
// ---------------------------------------------------------------------------

static volatile uint32_t s_impulsi   = 0;
static volatile int64_t  s_ultimoImp = 0;
static bool              s_flussoAttivo = false;

// Filtro anti-rimbalzo: alla portata massima del YF-S201 (~30 L/min) gli impulsi
// distano ~4,4 ms, quindi tutto cio' che arriva a meno di 1 ms e' rumore.
#define FLUSSO_MIN_INTERVALLO_US 1000

static void IRAM_ATTR isrFlusso() {
  int64_t ora = esp_timer_get_time();
  if (ora - s_ultimoImp < FLUSSO_MIN_INTERVALLO_US) return;
  s_ultimoImp = ora;
  s_impulsi++;
}

bool flussoDisponibile() {
#if USA_FLUSSO
  return true;
#else
  return false;
#endif
}

void flussoAzzera() {
#if USA_FLUSSO
  noInterrupts();
  s_impulsi   = 0;
  s_ultimoImp = 0;
  interrupts();
  if (!s_flussoAttivo) {
    // Con FLUSSO_PULLUP il livello alto lo definisce l'ESP32 a 3,3 V e il
    // sensore si limita a tirare la linea a massa: nessun partitore esterno
    // e nessun rischio che i 5 V arrivino sul GPIO.
    // Senza, il livello deve arrivare dal partitore o dal level shifter.
  #if FLUSSO_PULLUP
    pinMode(PIN_FLUSSO, INPUT_PULLUP);
  #else
    pinMode(PIN_FLUSSO, INPUT);
  #endif
    attachInterrupt(digitalPinToInterrupt(PIN_FLUSSO), isrFlusso, FALLING);
    s_flussoAttivo = true;
  }
#endif
}

void flussoStacca() {
#if USA_FLUSSO
  if (s_flussoAttivo) {
    detachInterrupt(digitalPinToInterrupt(PIN_FLUSSO));
    s_flussoAttivo = false;
  }
#endif
}

uint32_t flussoImpulsi() {
  noInterrupts();
  uint32_t n = s_impulsi;
  interrupts();
  return n;
}

float flussoLitri() {
  if (g_cfg.flussoImpLitro <= 0.0f) return 0.0f;
  return (float)flussoImpulsi() / g_cfg.flussoImpLitro;
}

// ---------------------------------------------------------------------------
//  Funzioni di lettura dei singoli sensori
//  Convenzione: ritornano NAN quando il sensore non e' disponibile.
// ---------------------------------------------------------------------------

static bool initBme(void*) {
#if USA_BME280
  g_bmeOk = bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire);
  if (!g_bmeOk) {
    Serial.println(F("[BME] ERRORE: BME280 non trovato (provati 0x76 e 0x77)."));
    return false;
  }
  // Modalita' forced: il sensore misura solo quando glielo chiediamo e poi
  // torna in sleep a ~0,1 uA. Su un nodo a batteria e' la scelta giusta.
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,   // temperatura
                  Adafruit_BME280::SAMPLING_X1,   // pressione
                  Adafruit_BME280::SAMPLING_X1,   // umidita'
                  Adafruit_BME280::FILTER_OFF);
  return true;
#else
  return false;
#endif
}

// Una sola misura forzata serve tutte e tre le grandezze: la eseguiamo alla
// prima lettura del ciclo e le altre due leggono i registri gia' aggiornati.
static void bmeMisuraSeServe() {
  static uint32_t ultimaMisura = 0;
  if (!g_bmeOk) return;
  if (ultimaMisura != 0 && millis() - ultimaMisura < 1000) return;
  bme.takeForcedMeasurement();
  ultimaMisura = millis();
}

static float leggiTemp(void*) {
  if (!g_bmeOk) return NAN;
  bmeMisuraSeServe();
  return bme.readTemperature();
}

static float leggiHum(void*) {
  if (!g_bmeOk) return NAN;
  bmeMisuraSeServe();
  return bme.readHumidity();
}

static float leggiPres(void*) {
  if (!g_bmeOk) return NAN;
  bmeMisuraSeServe();
  return bme.readPressure() / 100.0f;   // Pa -> hPa
}

static bool initLuce(void*) {
#if USA_LUCE && !USA_ADS1115
  analogSetPinAttenuation(PIN_LUM, ADC_11db);
#endif
  return true;
}

static float leggiLuce(void*) {
#if USA_LUCE
  float pct = leggiCanaleRaw(PIN_LUM) * 100.0f / 4095.0f;
  #if LUCE_INVERTITA
    pct = 100.0f - pct;
  #endif
  return pct;
#else
  return NAN;
#endif
}

static bool initTensione(void*) {
#if USA_TENSIONE && !USA_ADS1115
  analogSetPinAttenuation(PIN_VOLT, ADC_11db);
#endif
  return true;
}

static float leggiTensione(void*) {
#if USA_TENSIONE
  float mv = leggiCanaleMilliVolt(PIN_VOLT);
  if (isnan(mv)) return NAN;
  return (mv / 1000.0f) * g_cfg.voltDivider;
#else
  return NAN;
#endif
}

/*
 * Lettura di un sensore analogico calibrato, espressa in percentuale.
 *
 * La direzione la determinano i valori di calibrazione, non il codice:
 * sui sensori terreno resistivi (e anche sui capacitivi v1.2) il valore grezzo
 * SCENDE quando il terreno e' bagnato, quindi rawSecco > rawBagnato. Se il tuo
 * sensore si comporta al contrario basta invertire i due valori di calibrazione
 * con il comando CAL: la formula funziona in entrambi i versi.
 */
/*
 * Cache delle letture del terreno.
 *
 * Il terreno viene letto due volte per risveglio: una prima dell'irrigazione
 * (per decidere se serve) e una durante la composizione del pacchetto. Sono
 * due accensioni del rail dei sensori a distanza di pochi secondi, con valori
 * praticamente identici.
 *
 * Se l'interruttore e' un MOSFET la cosa e' gratis, ma con un canale di relay
 * significa il doppio degli scatti: circa 70.000 all'anno invece di 35.000.
 * La cache elimina la seconda accensione, e viene invalidata dopo
 * un'irrigazione — l'unico momento in cui il valore cambia davvero e in cui
 * rileggere ha senso, perche' serve a verificare che l'acqua sia arrivata.
 */
static float s_soilCache[4];
static bool  s_soilCacheValida = false;

void sensoriInvalidaCache() { s_soilCacheValida = false; }

static float leggiPercentuale(void* ctx) {
  CtxAnalogico* c = (CtxAnalogico*)ctx;
  if (!c) return NAN;

  if (s_soilCacheValida && c->indiceCal < 4) return s_soilCache[c->indiceCal];

  float secco   = (float)g_cfg.soilSecco[c->indiceCal];
  float bagnato = (float)g_cfg.soilBagnato[c->indiceCal];
  if (fabsf(secco - bagnato) < 1.0f) return NAN;   // calibrazione non valida

  float raw = (float)leggiCanaleRaw(c->canale);
  float pct = 100.0f * (secco - raw) / (secco - bagnato);

  if (c->invertito) pct = 100.0f - pct;

  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;

  if (c->indiceCal < 4) s_soilCache[c->indiceCal] = pct;
  return pct;
}

// --- "Sensori" che riportano stato invece di leggere hardware ---------------
// Passano dalla stessa tabella cosi' viaggiano nel pacchetto, finiscono nel
// backlog e compaiono in Home Assistant esattamente come gli altri.

static float leggiAcquaUltima(void*) {
  if (!flussoDisponibile()) return NAN;
  return irrigazioneLitriUltima();
}

static float leggiAcquaTotale(void*) {
  if (!flussoDisponibile()) return NAN;
  return g_cfg.litriTotali;
}

// ---------------------------------------------------------------------------
//  TABELLA DEI SENSORI
//  Per aggiungerne uno: una riga qui + una funzione di lettura sopra.
//  L'ordine conta: se il pacchetto supera PROTO_MAX_PAYLOAD vengono omessi
//  i campi in fondo, quindi tieni in alto i sensori piu' importanti.
// ---------------------------------------------------------------------------

#if USA_SOIL
static CtxAnalogico ctxSoil1 = { PIN_SOIL1, 0, false };
static CtxAnalogico ctxSoil2 = { PIN_SOIL2, 1, false };
  #if N_SOIL > 2
static CtxAnalogico ctxSoil3 = { PIN_SOIL3, 2, false };
  #endif
  #if N_SOIL > 3
static CtxAnalogico ctxSoil4 = { PIN_SOIL4, 3, false };
  #endif
#endif

DescrittoreSensore SENSORI[] = {
  //  chiave       bit             dec  init          leggi              ctx
  {  "temp",      BIT_TEMP,        2,   initBme,      leggiTemp,         nullptr    },
  {  "hum",       BIT_HUM,         1,   nullptr,      leggiHum,          nullptr    },
  {  "pres",      BIT_PRES,        1,   nullptr,      leggiPres,         nullptr    },
  {  "volt",      BIT_VOLT,        2,   initTensione, leggiTensione,     nullptr    },
  {  "luce",      BIT_LUCE,        1,   initLuce,     leggiLuce,         nullptr    },
#if USA_SOIL
  {  "soil1",     BIT_SOIL1,       1,   nullptr,      leggiPercentuale,  &ctxSoil1  },
  {  "soil2",     BIT_SOIL2,       1,   nullptr,      leggiPercentuale,  &ctxSoil2  },
  #if N_SOIL > 2
  {  "soil3",     BIT_SOIL3,       1,   nullptr,      leggiPercentuale,  &ctxSoil3  },
  #endif
  #if N_SOIL > 3
  {  "soil4",     BIT_SOIL4,       1,   nullptr,      leggiPercentuale,  &ctxSoil4  },
  #endif
#endif
#if USA_FLUSSO
  // "acqua" = litri dell'ultima irrigazione; "acquaTot" = contatore cumulativo.
  // Il totale giornaliero/mensile NON viene trasmesso: lo calcola molto meglio
  // un utility_meter di Home Assistant a partire da acquaTot, senza occupare
  // byte preziosi nel pacchetto LoRa.
  {  "acqua",     BIT_ACQUA,       3,   nullptr,      leggiAcquaUltima,  nullptr    },
  {  "acquaTot",  BIT_ACQUA_TOT,   2,   nullptr,      leggiAcquaTotale,  nullptr    },
#endif
  // <-- AGGIUNGI QUI I TUOI SENSORI (UV, piranometro, pH, EC, ...)
};

const uint8_t N_SENSORI = sizeof(SENSORI) / sizeof(SENSORI[0]);

// ---------------------------------------------------------------------------
//  API
// ---------------------------------------------------------------------------

bool sensoreAbilitato(uint8_t bit) {
  if (bit >= 32) return true;
  return (g_cfg.sensoriAbilitati >> bit) & 0x1;
}

void sensoreImpostaAbilitato(uint8_t bit, bool abilitato) {
  if (bit >= 32) return;
  if (abilitato) g_cfg.sensoriAbilitati |=  (1UL << bit);
  else           g_cfg.sensoriAbilitati &= ~(1UL << bit);
  impostazioniModificate();
}

int8_t sensoreIndiceDaChiave(const char* chiave) {
  for (uint8_t i = 0; i < N_SENSORI; i++)
    if (strcmp(SENSORI[i].chiave, chiave) == 0) return (int8_t)i;
  return -1;
}

uint8_t sensoriInit() {
#if USA_ADS1115
  ads.setGain(GAIN_ONE);                  // +-4,096 V a fondo scala
  g_adsOk = ads.begin(ADS1115_ADDR, &Wire);
  Serial.printf("[ADS] ADS1115 @0x%02X: %s\n", ADS1115_ADDR, g_adsOk ? "OK" : "NON TROVATO");
#endif

  // L'inizializzazione viene eseguita per TUTTI i sensori, anche per quelli
  // disabilitati. Motivo: piu' voci della tabella condividono lo stesso
  // hardware (temp/hum/pres sono tutte il BME280, e solo la prima ha una
  // funzione di init). Saltare l'init di un sensore disabilitato lascerebbe
  // il chip non inizializzato e farebbe fallire anche i suoi "fratelli"
  // rimasti attivi.
  uint8_t pronti = 0;
  for (uint8_t i = 0; i < N_SENSORI; i++) {
    if (SENSORI[i].init && !SENSORI[i].init(SENSORI[i].ctx)) continue;
    if (sensoreAbilitato(SENSORI[i].bit)) pronti++;
  }
  Serial.printf("[SENS] %u sensori attivi su %u in tabella.\n", pronti, N_SENSORI);
  return pronti;
}

/*
 * Umidita' del sensore terreno piu' SECCO, letta prima di decidere se irrigare.
 * Si usa il minimo e non la media di proposito: se anche una sola zona e'
 * sotto soglia conviene bagnare, mentre una media alta potrebbe nascondere
 * una zona completamente asciutta.
 * Ritorna NAN se nessun sensore terreno e' disponibile: in quel caso
 * l'irrigazione condizionata non si attiva e vale il solo criterio orario.
 */
float sensoriSoilMin() {
#if USA_SOIL
  bool trovato = false;
  float minimo = 200.0f;

  sensoriAlimenta(true);
  for (uint8_t i = 0; i < N_SENSORI; i++) {
    if (SENSORI[i].leggi != leggiPercentuale) continue;
    if (!sensoreAbilitato(SENSORI[i].bit)) continue;
    float v = leggiPercentuale(SENSORI[i].ctx);
    if (isnan(v)) continue;
    if (v < minimo) minimo = v;
    trovato = true;
  }
  sensoriAlimenta(false);

  if (!trovato) return NAN;

  // Da qui in poi le letture sono in cache: la composizione del pacchetto,
  // pochi secondi dopo, non riaccendera' il rail una seconda volta.
  s_soilCacheValida = true;

  Serial.printf("[SENS] Terreno piu' secco: %.1f%%\n", minimo);
  return minimo;
#else
  return NAN;
#endif
}

void sensoriLeggiTutti(PacchettoKV& pkt) {
  // I sensori terreno vengono alimentati solo per il tempo della misura:
  // i resistivi a due punte si corrodono se lasciati costantemente sotto
  // tensione, e in poche settimane diventano inutilizzabili.
  // Se le letture del terreno sono gia' in cache non serve riaccendere nulla:
  // con un canale di relay questo dimezza gli scatti.
  bool serveAlimentazione = false;
  if (!s_soilCacheValida)
    for (uint8_t i = 0; i < N_SENSORI; i++)
      if (SENSORI[i].leggi == leggiPercentuale && sensoreAbilitato(SENSORI[i].bit))
        serveAlimentazione = true;

  if (serveAlimentazione) sensoriAlimenta(true);

  for (uint8_t i = 0; i < N_SENSORI; i++) {
    if (!sensoreAbilitato(SENSORI[i].bit)) continue;
    float v = SENSORI[i].leggi ? SENSORI[i].leggi(SENSORI[i].ctx) : NAN;
    pkt.aggiungiF(SENSORI[i].chiave, v, SENSORI[i].decimali);

    if (isnan(v)) Serial.printf("[SENS] %-9s : non disponibile\n", SENSORI[i].chiave);
    else          Serial.printf("[SENS] %-9s : %.*f\n", SENSORI[i].chiave,
                                (int)SENSORI[i].decimali, v);
  }

  if (serveAlimentazione) sensoriAlimenta(false);
}
