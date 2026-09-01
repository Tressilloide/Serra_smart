#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <RTClib.h>

// ==========================================
// DEFINIZIONE PIN (ESP32)
// ==========================================
// I2C
#define I2C_SDA 21
#define I2C_SCL 22

// SPI (MicroSD)
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18

// Sensore Analogico
#define PIN_SENS_TERRENO 33

// ==========================================
// COSTANTI E PARAMETRI
// ==========================================
// Intervallo di salvataggio dati su SD (in millisecondi)
#define INTERVALLO_SALVATAGGIO_MS 300000 // 5 minuti

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
  int umiditaTerreno;
  String timestamp;
} dati;

bool sdInizializzata = false;
bool rtcInizializzato = false;
bool bmeInizializzato = false;

unsigned long tempoUltimoSalvataggio = 0;

// ==========================================
// PROTOTIPI FUNZIONI
// ==========================================
void inizializzaPeriferiche();
void leggiSensori();
void salvaSuSD();
String formattaDataOra(DateTime dt);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Avvio Datalogger Serra ESP32 ---");

  inizializzaPeriferiche();
  
  // Esegue una prima lettura e salvataggio immediato all'avvio
  leggiSensori();
  salvaSuSD();
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  // Salva i dati a intervalli regolari senza bloccare il microcontrollore
  if (millis() - tempoUltimoSalvataggio >= INTERVALLO_SALVATAGGIO_MS) {
    tempoUltimoSalvataggio = millis();
    
    leggiSensori();
    salvaSuSD();
  }
}

// ==========================================
// IMPLEMENTAZIONE DELLE FUNZIONI
// ==========================================

void inizializzaPeriferiche() {
  // Setup I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // Inizializzazione BME280
  if (!bme.begin(0x76, &Wire)) { // Indirizzo standard, potrebbe essere 0x77
    Serial.println("Errore: BME280 non trovato.");
  } else {
    bmeInizializzato = true;
    Serial.println("BME280 inizializzato.");
  }

  // Inizializzazione RTC
  if (!rtc.begin(&Wire)) {
    Serial.println("Errore: DS1307 non trovato.");
  } else {
    rtcInizializzato = true;
    if (!rtc.isrunning()) {
      Serial.println("RTC non impostato, calibro con l'ora di compilazione.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.println("RTC inizializzato.");
  }

  // Inizializzazione SD
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("Errore: Impossibile leggere la MicroSD.");
  } else {
    sdInizializzata = true;
    Serial.println("MicroSD inizializzata.");
  }
}

void leggiSensori() {
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

  // Lettura Sensore Terreno
  dati.umiditaTerreno = analogRead(PIN_SENS_TERRENO);
  
  // Lettura RTC
  if (rtcInizializzato) {
    DateTime now = rtc.now();
    dati.timestamp = formattaDataOra(now);
  } else {
    dati.timestamp = "Data_Non_Disponibile";
  }

  // Stampa di debug su monitor seriale
  Serial.printf("[%s] T: %.2f C | H: %.2f %% | P: %.2f hPa | Terr: %d\n", 
                dati.timestamp.c_str(), dati.temperatura, dati.umiditaAria, dati.pressione, dati.umiditaTerreno);
}

void salvaSuSD() {
  if (!sdInizializzata) {
    Serial.println("Salvataggio su SD saltato (Memoria non inizializzata).");
    return;
  }

  // Creazione stringa CSV
  String record = dati.timestamp + ",";
  record += String(dati.temperatura) + ",";
  record += String(dati.umiditaAria) + ",";
  record += String(dati.pressione) + ",";
  record += String(dati.umiditaTerreno);

  // Scrittura su File (FILE_APPEND aggiunge in coda!)
  File file = SD.open("/datalog.csv", FILE_APPEND);
  if (!file) {
    Serial.println("Errore nell'apertura del file datalog.csv");
    return;
  }

  // Se il file è vuoto (appena creato), scrivo l'intestazione
  if (file.size() == 0) {
    file.println("Timestamp,Temperatura(C),Umidita_Aria(%),Pressione(hPa),Umidita_Terreno");
  }

  file.println(record);
  file.close();
  Serial.println("-> Dati salvati in coda su SD.");
}

// Funzione di utilità per l'RTC
String formattaDataOra(DateTime dt) {
  // Aumentato da 20 a 32 per prevenire "Stack smashing protect failure"
  char buffer[32]; 
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
  return String(buffer);
}