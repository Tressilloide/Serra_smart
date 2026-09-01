/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : CONFIGURAZIONE
 * ============================================================================
 *  Tutti i parametri modificabili stanno qui. Il resto del codice non contiene
 *  numeri magici.
 *
 *  IMPORTANTE: molti di questi valori sono solo il DEFAULT di primo avvio.
 *  Una volta scritti in NVS (memoria non volatile) diventano modificabili da
 *  Home Assistant tramite i comandi LoRa (vedi comandi.h), e da quel momento
 *  il valore in NVS ha la precedenza su quello scritto qui.
 *  Per tornare ai default: comando RESETCFG oppure cancella la partizione NVS.
 * ============================================================================
 */

#pragma once

// ======================= IDENTITA' DEL NODO =================================

#define NODE_ID          "GH1"    // Identificativo di questo nodo (max 8 char)
#define FW_VERSION       "2.0.0"  // Riportata in Home Assistant

// ======================= RADIO LoRa =========================================
// Questi parametri DEVONO essere identici sul ponte in camera.

#define LORA_BAND        433E6    // Ra-01 = SX1278 a 433 MHz. Se il tuo modulo
                                  // e' a 868 MHz usa 868E6 (anche sul ponte!).
#define LORA_SF          7        // Spreading factor
#define LORA_BW          125E3    // Larghezza di banda
#define LORA_CR          5        // Coding rate 4/5
#define LORA_TX_POWER    17       // dBm su PA_BOOST

#define TX_RETRIES       3        // Tentativi di invio per pacchetto
#define ACK_TIMEOUT_MS   2000     // Attesa ACK dal ponte dopo ogni invio

// ======================= TEMPI ==============================================

#define SLEEP_TIME_SEC   900      // Intervallo di deep sleep in secondi
                                  // (900 = 15 min, 3600 = 1 ora)
#define SLEEP_MIN_SEC    60       // Non dormire mai meno di cosi'
#define WDT_SETUP_SEC    180      // Watchdog globale: se il setup() si blocca
                                  // (SD, LoRa, I2C) il nodo si riavvia da solo

// ======================= IRRIGAZIONE — DEFAULT ==============================
// Modificabili da Home Assistant con il comando SCHED.

#define IRRIG_ORA_DEF    17       // Ora di inizio irrigazione (0-23)
#define IRRIG_MIN_DEF    0        // Minuto di inizio (0-59)
#define IRRIG_SEC_DEF    300      // Durata in secondi (300 = 5 minuti)
#define IRRIG_AUTO_DEF   true     // Irrigazione automatica attiva?

// ---------------------------------------------------------------------------
//  TETTI DI SICUREZZA — NON modificabili da remoto.
//  Sono compilati nel firmware: nessun comando proveniente da Home Assistant
//  (o da chiunque altro trasmetta sulla stessa frequenza) puo' superarli.
//  Sono l'ultima barriera contro l'allagamento della serra.
// ---------------------------------------------------------------------------

#define IRRIG_MAX_SEC          900   // Durata massima assoluta di UNA irrigazione
                                     // (15 min). Ogni richiesta viene clampata.
#define IRRIG_MIN_INTERVALLO_M 30    // Minuti minimi tra due irrigazioni: blocca
                                     // sia i comandi ripetuti sia i bug di logica
#define IRRIG_MAX_AL_GIORNO    4     // Numero massimo di irrigazioni giornaliere
#define BUDGET_LITRI_GIORNO    50.0f // Litri massimi al giorno (0 = disattivato).
                                     // Attivo solo se il flussometro e' presente.

// ======================= SENSORI — FEATURE FLAG =============================
// Metti a 0 quello che non hai ancora collegato: il codice si adatta da solo,
// il pacchetto LoRa si accorcia e Home Assistant non mostra entita' fantasma.

#define USA_BME280       1        // Temperatura / umidita' / pressione (I2C)
#define USA_LUCE         1        // Fotoresistenza analogica su GPIO32
#define USA_TENSIONE     1        // Partitore di tensione su GPIO33

// Molti moduli con fotoresistenza danno tensione ALTA al buio e BASSA in piena
// luce. Se in Home Assistant vedi la luce al 90% di notte, metti 1 qui.
#define LUCE_INVERTITA   0

#define USA_SOIL         1        // Sensori umidita' terreno analogici
#define N_SOIL           2        // Quanti sensori terreno (max 4 su ADC nativo)
#define USA_FLUSSO       1        // Flussometro YF-S201

// Espansione analogica futura: mettendo a 1 i sensori analogici si spostano
// su un ADS1115 sul bus I2C, senza toccare nient'altro nel codice.
#define USA_ADS1115      0
#define ADS1115_ADDR     0x48

// ======================= CALIBRAZIONI — DEFAULT =============================
// Anche queste finiscono in NVS e sono modificabili da HA con il comando CAL.

// Partitore di tensione. I moduli "0-25V" usano 30k + 7.5k -> rapporto 5.0.
// CALIBRAZIONE: misura la batteria col multimetro (V_reale), leggi V_stampata
// dal seriale, poi:  nuovo = attuale * V_reale / V_stampata
#define VOLT_DIVIDER_DEF 5.0f

// Sensori terreno resistivi: valori grezzi ADC (0-4095) letti in aria e in acqua.
// Sui resistivi il valore SCENDE quando il terreno e' bagnato, quindi
// rawSecco > rawBagnato ed e' normale.
// Taratura reale: comando CAL,soil1,<secco>,<bagnato> da Home Assistant.
#define SOIL_RAW_SECCO_DEF   3000
#define SOIL_RAW_BAGNATO_DEF 1300

// Soglia di umidita' terreno sotto la quale irrigare (%). -1 = irrigazione
// non condizionata al terreno (si comporta come prima: solo orario).
#define SOIL_SOGLIA_DEF  -1

// Flussometro YF-S201: impulsi per litro. Datasheet: F = 7.5 * Q(L/min),
// cioe' 450 impulsi/litro. Varia parecchio da esemplare a esemplare:
// taralo riempiendo un contenitore da 1 litro e leggendo il conteggio.
#define FLUSSO_IMP_LITRO_DEF 450.0f

// Sicurezza idraulica: se durante l'irrigazione non arriva NESSUN impulso
// entro questo tempo, qualcosa non va (pompa guasta, serbatoio vuoto, filtro
// otturato). Il nodo chiude il relay e segnala l'anomalia a Home Assistant.
#define FLUSSO_TIMEOUT_SEC   30

// ======================= MICROSD / BACKLOG ==================================

#define FILE_BACKLOG     "/backlog.txt"
#define FILE_BACKLOG_TMP "/backlog.tmp"
#define FILE_LOG         "/log.txt"

#define BACKLOG_MAX_INVII   60        // Max record inviati per ciclo di veglia
#define BACKLOG_MAX_BYTE    262144UL  // Tetto dimensione file (256 KB, ~2000 rec).
                                      // Oltre, i record piu' VECCHI vengono scartati.
#define LOG_MAX_BYTE        65536UL   // Tetto del file di log (64 KB)

// ======================= COMANDI ============================================

#define MAX_CMD_PER_RISVEGLIO 5       // Quanti comandi eseguire per ogni veglia
#define WAKE_MAX_SEC          300     // Tetto della finestra di manutenzione WAKE
#define RTC_DRIFT_MAX_SEC     30      // Deriva oltre la quale il DS1307 viene
                                      // risincronizzato dall'ora del ponte

// ============================ PINOUT ========================================

// --- I2C (BME280 + DS1307, e in futuro ADS1115 / sensori UV) ---
#define I2C_SDA 21
#define I2C_SCL 22

// --- SPI #1 (VSPI): microSD ---
#define SD_CS    4
#define SD_MOSI  23
#define SD_MISO  19
#define SD_SCK   18

// --- SPI #2 (HSPI): LoRa Ra-01 su bus dedicato ---
// ATTENZIONE GPIO12: e' lo strapping pin MTDI. Se e' ALTO al reset, l'ESP32
// imposta VDD_SDIO a 1.8 V e puo' non avviarsi. Monta un pulldown da 10k su
// GPIO12 e un pull-up da 10k su GPIO5 (NSS), cosi' il modulo LoRa resta
// deselezionato durante il boot e non pilota la linea MISO.
#define LORA_SCK2  27
#define LORA_MISO2 12
#define LORA_MOSI2 13
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_NSS   5

// --- Relay irrigazione ---
// GPIO25 e' un pin del dominio RTC: puo' mantenere attivamente il livello
// logico anche in deep sleep (gpio_deep_sleep_hold_en), quindi nessun glitch.
// La maggior parte dei moduli relay e' ATTIVA BASSA (LOW = relay eccitato).
// Se il tuo e' attivo alto, inverti le due righe qui sotto.
#define PIN_RELAY 25
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// --- Sensori analogici su ADC1 (l'unico sempre disponibile) ---
#define PIN_LUM   32     // Fotoresistenza
#define PIN_VOLT  33     // Partitore di tensione
#define PIN_SOIL1 34     // Umidita' terreno 1 (solo input)
#define PIN_SOIL2 35     // Umidita' terreno 2 (solo input)
#define PIN_SOIL3 36     // Riserva (VP, solo input)
#define PIN_SOIL4 39     // Riserva (VN, solo input)

// --- Flussometro e alimentazione commutata dei sensori ---
// ATTENZIONE MODULI WROVER: sui moduli ESP32-WROVER i GPIO 16 e 17 sono
// occupati dalla PSRAM e NON sono utilizzabili. Il firmware lo rileva al boot
// con psramFound() e stampa un avviso. In quel caso usa le alternative:
//   PIN_FLUSSO -> 15   |   PIN_PWR_SENSORI -> 2
#define PIN_FLUSSO       17   // Impulsi YF-S201 (interrupt su fronte di discesa)
#define PIN_PWR_SENSORI  16   // Gate del MOSFET che alimenta i sensori terreno.
                              // -1 per alimentarli permanentemente (sconsigliato:
                              // i sensori resistivi si corrodono in poche settimane)
#define PWR_SENSORI_ON   HIGH
#define PWR_SETTLE_MS    250  // Attesa dopo l'accensione, prima di leggere

// Il sensore terreno va alimentato a 3,3 V e NON a 5 V: la sua uscita analogica
// segue la tensione di alimentazione e a 5 V danneggerebbe l'ADC dell'ESP32.

// ======================= LETTURE ANALOGICHE =================================

#define ADC_CAMPIONI     16       // Campioni mediati per ogni lettura analogica
#define ADC_DELAY_MS     3        // Pausa tra un campione e l'altro

// Sentinella "sensore non disponibile". Viene inviata al posto di NaN perche'
// "nan" non e' JSON valido e romperebbe il parsing in Home Assistant.
#define VAL_NON_DISPONIBILE -127.0f
