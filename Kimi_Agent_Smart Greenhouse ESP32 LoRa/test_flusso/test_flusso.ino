/*
 * ============================================================================
 *  SERRA SMART — TEST DEL FLUSSOMETRO
 * ============================================================================
 *
 *  A COSA SERVE
 *  ------------
 *  Caricalo sulla SCHEDA DEL NODO SERRA, quella che non conta gli impulsi,
 *  lasciando il flussometro cablato esattamente com'e' adesso.
 *
 *  E' il test che separa le due possibilita' che restano:
 *
 *    - se anche questo sketch conta ZERO  -> il problema e' nell'hardware:
 *      cablaggio, massa non in comune, pin danneggiato o scheda diversa da
 *      quella che credi. Il firmware della serra e' innocente.
 *
 *    - se questo sketch conta CORRETTAMENTE -> l'hardware va bene e il
 *      problema e' nel firmware della serra. Dimmelo e lo troviamo.
 *
 *  Nessuna libreria esterna, nessun display: solo il monitor seriale.
 *  Scheda: "ESP32 Dev Module", monitor seriale a 115200.
 * ============================================================================
 */

#include <Arduino.h>

#define PIN_FLUSSO   17     // lo stesso pin del nodo serra
#define IMPULSI_LITRO 433.0f

volatile uint32_t g_impulsi = 0;

void IRAM_ATTR isrFlusso() {
  g_impulsi++;
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

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.printf ("  TEST FLUSSOMETRO — pin %d, pull-up interno attivo\n", PIN_FLUSSO);
  Serial.println(F("  Fai scorrere l'acqua e guarda le colonne.\n"));
  Serial.println(F("  impulsi     : contati dall'interrupt"));
  Serial.println(F("  transizioni : cambi di livello visti campionando il pin"));
  Serial.println(F("  livello     : stato del pin in questo istante"));
  Serial.println(F("============================================================"));
  Serial.println();

  pinMode(PIN_FLUSSO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLUSSO), isrFlusso, FALLING);
}

void loop() {
  static uint32_t ultimoReport = 0;
  static uint32_t impulsiPrec  = 0;

  uint32_t transizioni = sondaTransizioni(200);
  delay(800);

  if (millis() - ultimoReport < 1000) return;
  ultimoReport = millis();

  noInterrupts();
  uint32_t impulsi = g_impulsi;
  interrupts();

  uint32_t nelSecondo = impulsi - impulsiPrec;
  impulsiPrec = impulsi;

  float litri   = impulsi / IMPULSI_LITRO;
  float portata = (nelSecondo * 60.0f) / IMPULSI_LITRO;   // L/min

  Serial.printf("impulsi=%-8lu (+%-4lu)  transizioni=%-4lu  livello=%d   %.3f L   %.2f L/min",
                (unsigned long)impulsi, (unsigned long)nelSecondo,
                (unsigned long)transizioni, digitalRead(PIN_FLUSSO),
                litri, portata);

  // Interpretazione automatica, cosi' non devi ricordarti la tabella
  if (nelSecondo == 0 && transizioni == 0)
    Serial.print("   <- nessun segnale sul filo");
  else if (nelSecondo == 0 && transizioni > 0)
    Serial.print("   <- SEGNALE PRESENTE ma l'interrupt non conta!");

  Serial.println();
}
