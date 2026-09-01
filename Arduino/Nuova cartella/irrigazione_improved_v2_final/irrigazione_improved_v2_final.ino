#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <RTClib.h>
#include <EEPROM.h>

// ==========================================
// DEFINIZIONE PIN (ESP32)
// ==========================================
// I2C
#define I2C_SDA 21
#define I2C_SCL 22

// SPI (MicroSD)
#define SD_CS   4
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18

// Relay (IRRIGAZIONE)
#define PIN_RELAY 4  // Pin per controllare il relay
// IMPORTANTE: Questo relay è attivato con logica INVERTITA
// LOW (0V) = Relay ON (acceso) → Pompa funziona
// HIGH (3.3V) = Relay OFF (spento) → Pompa ferma

// ==========================================
// COSTANTI E PARAMETRI
// ==========================================
// Intervallo tra letture (in secondi) - usato per deep sleep
#define INTERVALLO_LETTURA_SEC 1800  // 30 minuti

// FINESTRA ORARIA DI IRRIGAZIONE
#define ORA_INIZIO_IRRIGAZIONE 6     // Inizio: 6:00 AM
#define ORA_FINE_IRRIGAZIONE 7       // Fine: 7:00 AM

// FINESTRA RESET FLAG MEZZANOTTE
#define ORA_INIZIO_RESET 0            // Inizio: 0:00 (mezzanotte)
#define ORA_FINE_RESET 1              // Fine: 1:00

// Durata dell'irrigazione (in millisecondi) - PROTEZIONE SICUREZZA
#define DURATA_IRRIGAZIONE_MS 120000  // 120 secondi MAX

// EEPROM addresses per flags (2 bytes)
#define EEPROM_ADDR_IRRIGAZIONE_OGGI 0      // Flag irrigazione
#define EEPROM_ADDR_RESET_MEZZANOTTE 1      // Flag reset mezzanotte
#define EEPROM_SIZE 2

// ==========================================
// VARIABILI GLOBALI E OGGETTI
// ==========================================
Adafruit_BME280 bme; 
RTC_DS1307 rtc;

// Struttura dati per i sensori
struct DatiSensori {
  float temperatura;
  float umiditaAria;
  float pressione;
  String timestamp;
} dati;

bool sdInizializzata = false;
bool rtcInizializzato = false;
bool bmeInizializzato = false;

// Flags persistenti in EEPROM per evitare multiple azioni al giorno
bool irrigazioneEseguitaOggi = false;
bool resetEseguitoOggi = false;  // Flag per il reset della finestra mezzanotte

// ==========================================
// PROTOTIPI FUNZIONI
// ==========================================
void inizializzaPeriferiche();
void leggiSensori();
void salvaSuSD();
void verificaEIrriga();
void attivaRelay(uint32_t durata_ms);
void disattivaRelay();
void logActionSD(String azione);
String formattaDataOra(DateTime dt);
void entrareModoSonno();
void caricaFlagIrrigazione();
void salviFlagIrrigazione();
void caricaFlagReset();
void salviFlagReset();
void verificaEResetFlagMezzanotte();
void logSeriale(String messaggio);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  logSeriale("\n===== SERRA SMART - AVVIO (v2.1 con log seriale) =====");

  // Setup del pin del relay (FONDAMENTALE: subito HIGH per sicurezza)
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH);  // Relay SEMPRE OFF all'avvio (HIGH = OFF)
  
  // Inizializza EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  inizializzaPeriferiche();
  
  // Carica flags dal EEPROM (persistenti tra riavvii)
  caricaFlagIrrigazione();
  logSeriale("[Flag] Irrigazione oggi: " + String(irrigazioneEseguitaOggi ? "SÌ" : "NO"));
  
  caricaFlagReset();
  logSeriale("[Flag] Reset mezzanotte: " + String(resetEseguitoOggi ? "SÌ" : "NO"));
  
  // Verifica e resetta flag se siamo in finestra 0:00-1:00
  verificaEResetFlagMezzanotte();
  
  // Leggi dati dai sensori
  leggiSensori();
  salvaSuSD();
  
  // Verifica se è il momento di irrigare
  verificaEIrriga();
  
  logSeriale("Setup completato. Entro in deep sleep...");
  delay(500);
  
  // Entra in deep sleep
  entrareModoSonno();
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  // Il loop non viene mai raggiunto a causa del deep sleep
  // Tutto il codice è nel setup
}

// ==========================================
// IMPLEMENTAZIONE DELLE FUNZIONI
// ==========================================

void inizializzaPeriferiche() {
  // Setup I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // Inizializzazione BME280
  if (!bme.begin(0x76, &Wire)) {
    logSeriale("[ERRORE] BME280 non trovato (provo indirizzo 0x77)");
    if (!bme.begin(0x77, &Wire)) {
      logSeriale("[ERRORE] BME280 non disponibile");
      bmeInizializzato = false;
    } else {
      bmeInizializzato = true;
      logSeriale("[OK] BME280 inizializzato (0x77)");
    }
  } else {
    bmeInizializzato = true;
    logSeriale("[OK] BME280 inizializzato (0x76)");
  }

  // Inizializzazione RTC
  if (!rtc.begin(&Wire)) {
    logSeriale("[ERRORE] RTC DS1307 non trovato");
    rtcInizializzato = false;
  } else {
    rtcInizializzato = true;
    if (!rtc.isrunning()) {
      logSeriale("[WARN] RTC non calibrato, uso ora di compilazione");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    logSeriale("[OK] RTC inizializzato");
  }

  // Inizializzazione SD
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    logSeriale("[ERRORE] MicroSD non inizializzata");
    sdInizializzata = false;
  } else {
    sdInizializzata = true;
    logSeriale("[OK] MicroSD inizializzata");
  }
}

void leggiSensori() {
  if (rtcInizializzato) {
    DateTime now = rtc.now();
    dati.timestamp = formattaDataOra(now);
    logSeriale("[LETTURA] Timestamp: " + dati.timestamp);
  } else {
    dati.timestamp = "Data_Non_Disponibile";
  }

  // Lettura BME280
  if (bmeInizializzato) {
    dati.temperatura = bme.readTemperature();
    dati.umiditaAria = bme.readHumidity();
    dati.pressione = bme.readPressure() / 100.0F; // hPa
  } else {
    dati.temperatura = -99.9;
    dati.umiditaAria = -99.9;
    dati.pressione = -99.9;
  }

  // Log sensori
  char buffer[100];
  sprintf(buffer, "  T=%.2f°C | H=%.2f%% | P=%.2fhPa", 
          dati.temperatura, dati.umiditaAria, dati.pressione);
  logSeriale(String(buffer));
}

// ==========================================
// GESTION IRRIGAZIONE CON FINESTRA ORARIA
// ==========================================

void verificaEIrriga() {
  if (!rtcInizializzato) {
    logSeriale("[WARN] RTC non disponibile, salto irrigazione");
    return;
  }

  DateTime now = rtc.now();
  int oraAttuale = now.hour();
  int minutoAttuale = now.minute();
  
  logSeriale("[IRRIGAZIONE] Ora attuale: " + String(oraAttuale) + ":" + (minutoAttuale < 10 ? "0" : "") + String(minutoAttuale));
  logSeriale("[IRRIGAZIONE] Finestra: " + String(ORA_INIZIO_IRRIGAZIONE) + ":00 - " + String(ORA_FINE_IRRIGAZIONE) + ":59");

  // ✅ Controlla se siamo DENTRO la finestra oraria
  bool inFinestraOraria = (oraAttuale >= ORA_INIZIO_IRRIGAZIONE && 
                            oraAttuale < ORA_FINE_IRRIGAZIONE);

  if (inFinestraOraria) {
    logSeriale("[IRRIGAZIONE] ✓ Siamo dentro la finestra oraria!");
    
    // Controlla flag: hai già irrigato oggi?
    if (!irrigazioneEseguitaOggi) {
      logSeriale("[AZIONE] Prima irrigazione oggi! Attivazione relay!");
      logActionSD("INIZIO_IRRIGAZIONE");
      
      attivaRelay(DURATA_IRRIGAZIONE_MS);
      
      // Setta flag e salva in EEPROM
      irrigazioneEseguitaOggi = true;
      salviFlagIrrigazione();
      
      logActionSD("FINE_IRRIGAZIONE");
      logSeriale("[Flag] Irrigazione marcata come completata oggi");
    } else {
      logSeriale("[INFO] Irrigazione già effettuata oggi - skip");
    }
  } else {
    logSeriale("[IRRIGAZIONE] ✗ Fuori dalla finestra oraria");
  }
}

void verificaEResetFlagMezzanotte() {
  if (!rtcInizializzato) return;

  DateTime now = rtc.now();
  int oraAttuale = now.hour();
  
  // ✅ NUOVA LOGICA: Finestra di reset 0:00-1:00 (come l'irrigazione)
  bool inFinestraReset = (oraAttuale >= ORA_INIZIO_RESET && oraAttuale < ORA_FINE_RESET);

  if (inFinestraReset) {
    logSeriale("[Reset] Siamo nella finestra di reset (0:00-1:00)");
    
    // Se non abbiamo ancora resettato oggi...
    if (!resetEseguitoOggi) {
      logSeriale("[Reset] Reset flag irrigazione per nuovo giorno!");
      
      // Resetta il flag di irrigazione
      irrigazioneEseguitaOggi = false;
      salviFlagIrrigazione();
      
      // Marca come resettato oggi
      resetEseguitoOggi = true;
      salviFlagReset();
      
      logSeriale("[Reset] ✓ Flags resettati per nuovo giorno");
    } else {
      logSeriale("[Reset] Reset già effettuato oggi - skip");
    }
  }
}

// ==========================================
// CONTROLLO RELAY
// ==========================================

void attivaRelay(uint32_t durata_ms) {
  logSeriale("[RELAY] Attivazione per " + String(durata_ms) + " ms");
  
  // Doppio controllo di sicurezza
  digitalWrite(PIN_RELAY, LOW);  // LOW = accende il relay
  delayMicroseconds(100);  // Stabilizzazione
  
  if (digitalRead(PIN_RELAY) != LOW) {  // Verifica che sia acceso (LOW)
    logSeriale("[ERRORE CRITICO] Relay non si è attivato!");
    logActionSD("ERRORE_RELAY_NON_ATTIVATO");
    return;
  }
  
  logSeriale("[RELAY] ✓ Attivato");
  
  // Attesa con controllo periodico
  uint32_t tempoInizio = millis();
  while (millis() - tempoInizio < durata_ms) {
    // Controlla periodicamente che il relay sia ancora attivo
    if (digitalRead(PIN_RELAY) != LOW) {  // Verifica che rimanga acceso (LOW)
      logSeriale("[ERRORE] Relay disattivato inaspettatamente!");
      logActionSD("ERRORE_RELAY_INASPETTATO");
      break;
    }
    delay(1000);  // Check ogni 1 secondo
  }
  
  disattivaRelay();
}

void disattivaRelay() {
  logSeriale("[RELAY] Disattivazione");
  digitalWrite(PIN_RELAY, HIGH);  // HIGH = spegne il relay
  delayMicroseconds(100);  // Stabilizzazione
  
  if (digitalRead(PIN_RELAY) != HIGH) {  // Verifica che sia spento (HIGH)
    logSeriale("[ERRORE CRITICO] Relay non si è disattivato!");
    logActionSD("ERRORE_RELAY_NON_DISATTIVATO");
    digitalWrite(PIN_RELAY, HIGH);  // Forza offline (HIGH = spento)
  }
  
  logSeriale("[RELAY] ✓ Disattivato (SAFE)");
}

// ==========================================
// SD CARD LOGGING
// ==========================================

void salvaSuSD() {
  if (!sdInizializzata) {
    logSeriale("[WARN] SD non disponibile, dati non salvati");
    return;
  }

  // Creazione stringa CSV
  String record = dati.timestamp + ",";
  record += String(dati.temperatura, 2) + ",";
  record += String(dati.umiditaAria, 2) + ",";
  record += String(dati.pressione, 2);

  // Scrittura su File
  File file = SD.open("/datalog.csv", FILE_APPEND);
  if (!file) {
    logSeriale("[ERRORE] Impossibile aprire datalog.csv");
    return;
  }

  // Se il file è vuoto, scrivo l'intestazione
  if (file.size() == 0) {
    file.println("Timestamp,Temperatura(C),Umidita_Aria(%),Pressione(hPa)");
  }

  file.println(record);
  file.close();
  logSeriale("[SD] ✓ Dati salvati");
}

void logActionSD(String azione) {
  if (!sdInizializzata) {
    logSeriale("[WARN] SD non disponibile, azione non loggata: " + azione);
    return;
  }

  File file = SD.open("/azioni.log", FILE_APPEND);
  if (!file) {
    logSeriale("[ERRORE] Impossibile aprire azioni.log");
    return;
  }

  if (file.size() == 0) {
    file.println("Timestamp,Azione");
  }

  if (rtcInizializzato) {
    DateTime now = rtc.now();
    String timestamp = formattaDataOra(now);
    file.printf("%s,%s\n", timestamp.c_str(), azione.c_str());
  } else {
    file.printf("Data_Non_Disponibile,%s\n", azione.c_str());
  }

  file.close();
  logSeriale("[LOG] ✓ Azione registrata: " + azione);
}

// ==========================================
// LOGGING SERIALE + SD
// ==========================================

void logSeriale(String messaggio) {
  // Stampa su monitor seriale
  Serial.println(messaggio);
  
  // Salva anche su SD in formato semplificato
  if (!sdInizializzata) return;
  
  File file = SD.open("/serial.log", FILE_APPEND);
  if (!file) return;
  
  if (file.size() == 0) {
    file.println("Timestamp,Messaggio");
  }
  
  if (rtcInizializzato) {
    DateTime now = rtc.now();
    String timestamp = formattaDataOra(now);
    file.printf("%s,%s\n", timestamp.c_str(), messaggio.c_str());
  } else {
    file.printf("?,%s\n", messaggio.c_str());
  }
  
  file.close();
}

// ==========================================
// UTILITY FUNCTIONS
// ==========================================

String formattaDataOra(DateTime dt) {
  char buffer[32]; 
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          dt.year(), dt.month(), dt.day(), 
          dt.hour(), dt.minute(), dt.second());
  return String(buffer);
}

void caricaFlagIrrigazione() {
  byte valore = EEPROM.read(EEPROM_ADDR_IRRIGAZIONE_OGGI);
  irrigazioneEseguitaOggi = (valore == 1);
  Serial.printf("[EEPROM] Flag irrigazione caricato: %d\n", valore);
}

void salviFlagIrrigazione() {
  byte valore = irrigazioneEseguitaOggi ? 1 : 0;
  EEPROM.write(EEPROM_ADDR_IRRIGAZIONE_OGGI, valore);
  EEPROM.commit();
  Serial.printf("[EEPROM] Flag irrigazione salvato: %d\n", valore);
}

void caricaFlagReset() {
  byte valore = EEPROM.read(EEPROM_ADDR_RESET_MEZZANOTTE);
  resetEseguitoOggi = (valore == 1);
  Serial.printf("[EEPROM] Flag reset caricato: %d\n", valore);
}

void salviFlagReset() {
  byte valore = resetEseguitoOggi ? 1 : 0;
  EEPROM.write(EEPROM_ADDR_RESET_MEZZANOTTE, valore);
  EEPROM.commit();
  Serial.printf("[EEPROM] Flag reset salvato: %d\n", valore);
}

// ==========================================
// DEEP SLEEP
// ==========================================

void entrareModoSonno() {
  logSeriale("[SLEEP] Entro in deep sleep per " + String(INTERVALLO_LETTURA_SEC) + " secondi");
  
  // Disattiva tutti i componenti non essenziali
  disattivaRelay();  // Assicurati che il relay sia OFF
  
  // Configura il timer di wake
  esp_sleep_enable_timer_wakeup(INTERVALLO_LETTURA_SEC * 1000000ULL);  // Converte a microsecondi
  
  logSeriale("[SLEEP] Prossimo risveglio tra 30 minuti");
  delay(100);  // Attesa per flush seriale
  
  // Entra in deep sleep
  esp_deep_sleep_start();
  // Dopo il deep sleep, il chip si riavvia da setup()
}
