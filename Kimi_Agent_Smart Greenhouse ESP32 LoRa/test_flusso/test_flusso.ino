/*
 * ============================================================================
 *  SERRA SMART — TEST DEL FLUSSOMETRO CON IRRIGAZIONE
 * ============================================================================
 *
 *  Sketch di collaudo da caricare sulla SCHEDA DEL NODO SERRA, con il
 *  flussometro e il relay cablati come in produzione.
 *
 *  Apre la valvola a intervalli, misura il flusso, chiude, e stampa tutto sul
 *  monitor seriale: ti basta il portatile collegato via USB.
 *
 *  A COSA SERVE
 *  ------------
 *   - verificare che il flussometro conti davvero, sulla scheda giusta e sul
 *     pin giusto
 *   - misurare la portata reale dell'impianto, che con la pompa e' diversa da
 *     quella di un litro versato a mano con l'imbuto
 *   - ricavare gli impulsi/litro veri: raccogli l'acqua in un contenitore
 *     graduato e confronta con il conteggio finale
 *
 *  SICUREZZA
 *  ---------
 *  Questo sketch APRE UNA VALVOLA DELL'ACQUA. Le protezioni ci sono e sono
 *  volutamente strette:
 *   - relay forzato chiuso come primissima istruzione, prima di ogni altra cosa
 *   - numero di cicli limitato: finiti quelli si ferma e non riapre piu'
 *   - tetto sui litri totali: superato, chiude e termina
 *   - watchdog hardware: se il codice si impalla la scheda si riavvia e al
 *     boot la valvola si richiude
 *  Restaci comunque davanti mentre gira. E' un test, non un impianto.
 *
 *  Nessuna libreria esterna. Scheda: "ESP32 Dev Module", seriale 115200.
 * ============================================================================
 */

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>

// ======================= CONFIGURAZIONE DEL TEST ============================

// Devono coincidere con serra_nodo/config.h
#define PIN_RELAY        25
#define RELAY_ON         LOW        // la maggior parte dei moduli e' attiva bassa
#define RELAY_OFF        HIGH

// NON usare 16 o 17: sulle schede a 38 pin (WROVER) sono cablati alla PSRAM.
// NON usare 34-39: sono di solo ingresso e non hanno pull-up interno.
#define PIN_FLUSSO       15

#define IMPULSI_LITRO    433.0f     // valore attuale, da verificare col test

#define CICLI            4          // quante aperture fare, poi si ferma
#define SECONDI_ON       15         // durata di ogni apertura
#define SECONDI_OFF      10         // pausa fra un ciclo e il successivo

#define MAX_LITRI_TOTALI 20.0f      // stop di sicurezza sul totale erogato

// ======================= STATO ==============================================

volatile uint32_t g_impulsi = 0;

void IRAM_ATTR isrFlusso() {
  g_impulsi++;
}

static uint32_t impulsi() {
  noInterrupts();
  uint32_t n = g_impulsi;
  interrupts();
  return n;
}

// Campiona il pin a raffica contando i cambi di livello, SENZA usare
// l'interrupt: dice se sul filo arriva un segnale, indipendentemente dal
// fatto che l'interrupt lo raccolga.
static uint32_t sondaTransizioni(uint16_t durataMs) {
  int      precedente  = digitalRead(PIN_FLUSSO);
  uint32_t transizioni = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < durataMs) {
    int adesso = digitalRead(PIN_FLUSSO);
    if (adesso != precedente) { transizioni++; precedente = adesso; }
  }
  return transizioni;
}

static void relayChiudi() {
  gpio_hold_dis((gpio_num_t)PIN_RELAY);
  digitalWrite(PIN_RELAY, RELAY_OFF);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF);
}

// ======================= RISULTATI ==========================================

static float    litriPerCiclo[CICLI];
static uint32_t impulsiPerCiclo[CICLI];
static float    litriTotali = 0.0f;
static uint8_t  cicliFatti  = 0;

static void stampaRiepilogo() {
  uint32_t impulsiTot = 0;

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.println(F("  RIEPILOGO"));
  Serial.println(F("============================================================"));
  Serial.println(F("  ciclo   impulsi     litri    portata media"));

  for (uint8_t i = 0; i < cicliFatti; i++) {
    impulsiTot += impulsiPerCiclo[i];
    Serial.printf("  %3u   %8lu   %7.3f   %6.2f L/min\n",
                  i + 1, (unsigned long)impulsiPerCiclo[i], litriPerCiclo[i],
                  litriPerCiclo[i] * 60.0f / SECONDI_ON);
  }

  Serial.println(F("  ----------------------------------------------------------"));
  Serial.printf("  TOTALE: %lu impulsi = %.3f litri con la taratura attuale "
                "(%.0f impulsi/L)\n",
                (unsigned long)impulsiTot, litriTotali, IMPULSI_LITRO);
  Serial.println();
  Serial.println(F("  TARATURA: misura con un contenitore graduato quanta acqua"));
  Serial.println(F("  e' uscita davvero, poi calcola:"));
  Serial.printf ("     impulsi_per_litro = %lu / <litri misurati>\n",
                 (unsigned long)impulsiTot);
  Serial.println(F("  e mandalo al nodo con:  CAL,acqua,<valore>"));
  Serial.println(F("============================================================"));
}

// ======================= SETUP ==============================================

void setup() {
  // Primissima istruzione: valvola chiusa, sempre.
  relayChiudi();

  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.println(F("  TEST FLUSSOMETRO CON IRRIGAZIONE"));
  Serial.println(F("============================================================"));
  Serial.printf ("  Relay      : GPIO %d (attivo %s)\n",
                 PIN_RELAY, RELAY_ON == LOW ? "basso" : "alto");
  Serial.printf ("  Flussometro: GPIO %d (pull-up interno)\n", PIN_FLUSSO);
  Serial.printf ("  Cicli      : %d da %d s, pausa %d s\n",
                 CICLI, SECONDI_ON, SECONDI_OFF);
  Serial.printf ("  Tetto      : %.1f litri totali\n", MAX_LITRI_TOTALI);
  Serial.printf ("  Chip %s, PSRAM: %s\n",
                 ESP.getChipModel(),
                 psramFound() ? "SI (WROVER: GPIO 16 e 17 NON usabili)" : "no");
  Serial.println(F("============================================================"));
  Serial.println();
  Serial.println(F("  Colonne della riga di avanzamento:"));
  Serial.println(F("    impulsi     contati dall'interrupt"));
  Serial.println(F("    transizioni cambi di livello visti campionando il pin"));
  Serial.println(F("    pin         livello logico in questo istante"));
  Serial.println();
  Serial.println(F("  Se restano tutti a zero con pin=1 fisso, il segnale non"));
  Serial.println(F("  arriva proprio: cablaggio, massa in comune, o pin sbagliato."));
  Serial.println();

  pinMode(PIN_FLUSSO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLUSSO), isrFlusso, FALLING);

  // Watchdog: se il codice si blocca a valvola aperta, la scheda si riavvia
  // e relayChiudi() in cima al setup() la richiude.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = (SECONDI_ON + 30) * 1000UL;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;
  if (esp_task_wdt_reconfigure(&cfg) == ESP_ERR_INVALID_STATE)
    esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(SECONDI_ON + 30, true);
#endif
  if (esp_task_wdt_status(NULL) != ESP_OK) esp_task_wdt_add(NULL);

  Serial.println(F("  Primo ciclo fra 5 secondi. Stacca l'alimentazione per fermare tutto."));
  for (int i = 5; i > 0; i--) { Serial.printf("  %d...\n", i); esp_task_wdt_reset(); delay(1000); }
}

// ======================= CICLO DI PROVA =====================================

static void eseguiCiclo(uint8_t n) {
  Serial.println();
  Serial.printf("--- CICLO %u di %d - apro la valvola per %d s ---\n",
                n + 1, CICLI, SECONDI_ON);

  uint32_t impulsiInizio = impulsi();
  uint32_t t0 = millis();
  uint32_t ultimoLog = 0;

  digitalWrite(PIN_RELAY, RELAY_ON);

  while (true) {
    esp_task_wdt_reset();

    uint32_t trascorso = millis() - t0;
    if (trascorso >= (uint32_t)SECONDI_ON * 1000UL) break;

    float litriCiclo = (impulsi() - impulsiInizio) / IMPULSI_LITRO;

    // Tetto di sicurezza sul totale erogato
    if (litriTotali + litriCiclo >= MAX_LITRI_TOTALI) {
      Serial.println(F("  TETTO LITRI RAGGIUNTO: chiudo subito."));
      break;
    }

    if (trascorso - ultimoLog >= 1000UL) {
      ultimoLog = trascorso;
      uint32_t transizioni = sondaTransizioni(200);
      Serial.printf("  %2lus  impulsi=%-6lu  %6.3f L  %5.2f L/min  pin=%d  transizioni=%lu\n",
                    (unsigned long)(trascorso / 1000UL),
                    (unsigned long)(impulsi() - impulsiInizio),
                    litriCiclo,
                    trascorso > 0 ? litriCiclo * 60000.0f / trascorso : 0.0f,
                    digitalRead(PIN_FLUSSO),
                    (unsigned long)transizioni);
    }
    delay(50);
  }

  digitalWrite(PIN_RELAY, RELAY_OFF);

  // La turbina continua a girare per inerzia e il tubo si svuota: si aspetta
  // un paio di secondi prima di fermare il conteggio, o si perdono impulsi.
  for (int i = 0; i < 20; i++) { esp_task_wdt_reset(); delay(100); }

  uint32_t impulsiCiclo = impulsi() - impulsiInizio;
  float    litriCiclo   = impulsiCiclo / IMPULSI_LITRO;

  impulsiPerCiclo[n] = impulsiCiclo;
  litriPerCiclo[n]   = litriCiclo;
  litriTotali       += litriCiclo;
  cicliFatti         = n + 1;

  Serial.printf("--- Valvola CHIUSA. Ciclo %u: %lu impulsi, %.3f L "
                "(totale %.3f L) ---\n",
                n + 1, (unsigned long)impulsiCiclo, litriCiclo, litriTotali);

  if (impulsiCiclo == 0)
    Serial.println(F("    ATTENZIONE: nessun impulso. Acqua ferma, oppure il "
                     "segnale non arriva al pin."));
}

void loop() {
  static uint8_t ciclo = 0;
  static bool    finito = false;

  esp_task_wdt_reset();

  if (finito) { delay(1000); return; }

  if (ciclo >= CICLI || litriTotali >= MAX_LITRI_TOTALI) {
    relayChiudi();
    stampaRiepilogo();
    Serial.println(F("\n  Test concluso. La valvola resta chiusa."));
    Serial.println(F("  Per rifarlo, premi il pulsante di reset della scheda."));
    finito = true;
    return;
  }

  eseguiCiclo(ciclo);
  ciclo++;

  if (ciclo < CICLI) {
    Serial.printf("\n  Pausa di %d s prima del prossimo ciclo...\n", SECONDI_OFF);
    for (int i = 0; i < SECONDI_OFF * 10; i++) { esp_task_wdt_reset(); delay(100); }
  }
}
