#include "radio.h"
#include "config.h"

#include <SPI.h>
#include <LoRa.h>
#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
//  Registri dell'SX1276/78
//
//  La libreria li tiene privati. Qui servono letti e scritti senza passare da
//  parsePacket(), quindi l'accesso e' replicato in sxLeggi()/sxScrivi(): stesso
//  bus, stesso NSS, stessa frequenza della libreria.
// ---------------------------------------------------------------------------

#define SX_FIFO                 0x00
#define SX_OP_MODE              0x01
#define SX_FRF_MSB              0x06
#define SX_FRF_MID              0x07
#define SX_FRF_LSB              0x08
#define SX_PA_CONFIG            0x09
#define SX_PA_RAMP              0x0A
#define SX_OCP                  0x0B
#define SX_LNA                  0x0C
#define SX_FIFO_ADDR_PTR        0x0D
#define SX_FIFO_TX_BASE_ADDR    0x0E
#define SX_FIFO_RX_BASE_ADDR    0x0F
#define SX_FIFO_RX_CURRENT_ADDR 0x10
#define SX_IRQ_FLAGS_MASK       0x11
#define SX_IRQ_FLAGS            0x12
#define SX_RX_NB_BYTES          0x13
#define SX_MODEM_CONFIG_1       0x1D
#define SX_MODEM_CONFIG_2       0x1E
#define SX_PREAMBLE_MSB         0x20
#define SX_PREAMBLE_LSB         0x21
#define SX_MAX_PAYLOAD_LENGTH   0x23
#define SX_HOP_PERIOD           0x24
#define SX_MODEM_CONFIG_3       0x26
#define SX_DETECTION_OPTIMIZE   0x31
#define SX_INVERTIQ             0x33
#define SX_DETECTION_THRESHOLD  0x37
#define SX_SYNC_WORD            0x39
#define SX_INVERTIQ2            0x3B
#define SX_VERSION              0x42
#define SX_PA_DAC               0x4D

#define SX_MODO_LORA            0x80
#define SX_MODO_RX_CONTINUA     0x05

#define SX_IRQ_TX_DONE          0x08
#define SX_IRQ_CRC_ERRATO       0x20
#define SX_IRQ_RX_DONE          0x40

#define SX_VERSIONE_ATTESA      0x12
#define SX_MARCATORE_RESET      0x5A   // valore di prova per il filo di reset: diverso dal default 0x12

// ---------------------------------------------------------------------------
//  Configurazione attesa
//
//  Ricavata dagli stessi #define di config.h con le stesse regole della
//  libreria 0.8.0, cosi' cambiando SF, banda o potenza la verifica si
//  aggiorna da sola invece di dare l'allarme su un valore ormai giusto.
// ---------------------------------------------------------------------------

// Indice della banda, come in LoRa.setSignalBandwidth()
static constexpr uint8_t indiceBanda(double bw) {
  return bw <= 7.8E3  ? 0 : bw <= 10.4E3  ? 1 : bw <= 15.6E3 ? 2 :
         bw <= 20.8E3 ? 3 : bw <= 31.25E3 ? 4 : bw <= 41.7E3 ? 5 :
         bw <= 62.5E3 ? 6 : bw <= 125E3   ? 7 : bw <= 250E3  ? 8 : 9;
}

// Gli stessi limiti che applicano setSpreadingFactor(), setCodingRate4() e
// setTxPower()
static constexpr int SF_EFF  = LORA_SF < 6 ? 6 : (LORA_SF > 12 ? 12 : LORA_SF);
static constexpr int CR_EFF  = LORA_CR < 5 ? 5 : (LORA_CR > 8 ? 8 : LORA_CR);
static constexpr int POTENZA = LORA_TX_POWER > 20 ? 20 : (LORA_TX_POWER < 2 ? 2 : LORA_TX_POWER);
static constexpr bool PA_ALTA = POTENZA > 17;   // da +18 dBm il PA_DAC passa in alta potenza

static constexpr uint32_t FRF_ATTESA = (uint32_t)(((uint64_t)LORA_BAND << 19) / 32000000ULL);

// Banda, coding rate e header esplicito (bit 0 a zero)
static constexpr uint8_t MC1_ATTESO = (uint8_t)((indiceBanda(LORA_BW) << 4) | ((CR_EFF - 4) << 1));
// Spreading factor e CRC; TX continua e bit alti del timeout a zero
static constexpr uint8_t MC2_ATTESO = (uint8_t)((SF_EFF << 4) | 0x04);
// AGC automatico, piu' LowDataRateOptimize se un simbolo dura oltre 16 ms
// (lo stesso conto, intero, di setLdoFlag())
static constexpr bool    LDO        = (1000L / ((long)LORA_BW / (1L << SF_EFF))) > 16;
static constexpr uint8_t MC3_ATTESO = (uint8_t)(0x04 | (LDO ? 0x08 : 0x00));

static constexpr uint8_t PA_CONFIG_ATTESO = (uint8_t)(0x80 | ((PA_ALTA ? POTENZA - 3 : POTENZA) - 2));
static constexpr uint8_t PA_DAC_ATTESO    = PA_ALTA ? 0x07 : 0x04;   // i soli 3 bit bassi di 0x87 / 0x84
static constexpr uint8_t OCP_ATTESO       = (uint8_t)(0x20 | (0x1F & (PA_ALTA ? (140 + 30) / 10
                                                                              : (100 - 45) / 5)));
static constexpr uint8_t DET_OPT_ATTESO   = SF_EFF == 6 ? 0x05 : 0x03;   // 3 bit bassi di 0xC5 / 0xC3
static constexpr uint8_t DET_THR_ATTESO   = SF_EFF == 6 ? 0x0C : 0x0A;

// Valori che la libreria non scrive mai, quindi quelli di fabbrica: rampa del
// PA di 40 us, guadagno LNA massimo con il boost che begin() aggiunge.
static constexpr uint8_t PA_RAMP_ATTESO = 0x09;
static constexpr uint8_t LNA_ATTESO     = 0x23;

struct RegistroAtteso {
  uint8_t reg;
  uint8_t maschera;   // i bit che contano: i riservati restano fuori
  uint8_t valore;     // gia' mascherato
};

static const RegistroAtteso ATTESI[] = {
  // registro                 maschera  valore
  { SX_VERSION,                 0xFF,  SX_VERSIONE_ATTESA },
  // LoRa, pagina dei registri LoRa (bit 6 a zero), ricezione continua
  { SX_OP_MODE,                 0xC7,  SX_MODO_LORA | SX_MODO_RX_CONTINUA },
  { SX_FRF_MSB,                 0xFF,  (uint8_t)(FRF_ATTESA >> 16) },
  { SX_FRF_MID,                 0xFF,  (uint8_t)(FRF_ATTESA >> 8) },
  { SX_FRF_LSB,                 0xFF,  (uint8_t)(FRF_ATTESA) },
  { SX_MODEM_CONFIG_1,          0xFF,  MC1_ATTESO },
  { SX_MODEM_CONFIG_2,          0xFF,  MC2_ATTESO },
  { SX_MODEM_CONFIG_3,          0x0C,  MC3_ATTESO },
  { SX_PA_CONFIG,               0xFF,  PA_CONFIG_ATTESO },
  { SX_PA_DAC,                  0x07,  PA_DAC_ATTESO },
  { SX_OCP,                     0x3F,  OCP_ATTESO },
  { SX_PA_RAMP,                 0x0F,  PA_RAMP_ATTESO },
  { SX_LNA,                     0x1B,  LNA_ATTESO & 0x1B },
  { SX_FIFO_TX_BASE_ADDR,       0xFF,  0x00 },
  { SX_FIFO_RX_BASE_ADDR,       0xFF,  0x00 },
  // Un bit a 1 qui fa sparire il flag corrispondente: RxDone mascherato e'
  // una radio che riceve senza dirlo a nessuno
  { SX_IRQ_FLAGS_MASK,          0xFF,  0x00 },
  { SX_PREAMBLE_MSB,            0xFF,  (uint8_t)(LORA_PREAMBOLO >> 8) },
  { SX_PREAMBLE_LSB,            0xFF,  (uint8_t)(LORA_PREAMBOLO) },
  { SX_MAX_PAYLOAD_LENGTH,      0xFF,  0xFF },
  { SX_HOP_PERIOD,              0xFF,  0x00 },
  { SX_DETECTION_OPTIMIZE,      0x07,  DET_OPT_ATTESO },
  { SX_DETECTION_THRESHOLD,     0xFF,  DET_THR_ATTESO },
  // I/Q dritti sia in ricezione (bit 6 a zero) sia in trasmissione (bit 0 a
  // uno): con quello di trasmissione invertito il ponte sentirebbe il nodo e
  // il nodo non sentirebbe nessun ACK
  { SX_INVERTIQ,                0x41,  0x01 },
  { SX_INVERTIQ2,               0xFF,  0x1D },
  { SX_SYNC_WORD,               0xFF,  LORA_SYNC_WORD },
};

static const size_t N_ATTESI = sizeof(ATTESI) / sizeof(ATTESI[0]);

// ---------------------------------------------------------------------------
//  Stato
// ---------------------------------------------------------------------------

static SPISettings      g_spi(LORA_SPI_HZ, MSBFIRST, SPI_MODE0);
static bool             g_ok                = false;
static bool             g_escluso[N_ATTESI];
static uint32_t         g_riparazioni       = 0;
static CausaRiparazione g_causa             = RIP_NESSUNA;
static char             g_dettaglio[48]     = "";
static char             g_avviso[48]        = "";
static uint32_t         g_incoerenti        = 0;
static uint32_t         g_crcErrati         = 0;
static int8_t           g_resetHw           = -1;
static uint32_t         g_ultimaVerificaMs  = 0;
static uint32_t         g_ultimoTentativoMs = 0;

// ---------------------------------------------------------------------------
//  Accesso diretto ai registri
// ---------------------------------------------------------------------------

// Copia del singleTransfer() della libreria, compreso l'ordine fra NSS e
// transazione: le due strade si alternano sullo stesso bus.
static uint8_t sxTrasferisci(uint8_t indirizzo, uint8_t valore) {
  digitalWrite(LORA_NSS, LOW);
  SPI.beginTransaction(g_spi);
  SPI.transfer(indirizzo);
  uint8_t risposta = SPI.transfer(valore);
  SPI.endTransaction();
  digitalWrite(LORA_NSS, HIGH);
  return risposta;
}

static uint8_t sxLeggi(uint8_t reg)                { return sxTrasferisci(reg & 0x7F, 0x00); }
static void    sxScrivi(uint8_t reg, uint8_t valore) { sxTrasferisci(reg | 0x80, valore); }

// ---------------------------------------------------------------------------
//  Verifica dei registri
// ---------------------------------------------------------------------------

/*
 * Indice del primo registro che non ha il valore atteso nemmeno rileggendolo,
 * -1 se sono tutti a posto.
 *
 * Una lettura sbagliata da sola non basta ad accusare il registro: se anche
 * una sola delle due riletture da' il valore giusto, a sbagliare era il bus,
 * e il registro non va toccato. Quelle letture smentite le conta
 * radioLettureIncoerenti(), che e' la misura diretta di quanto e' disturbato
 * il collegamento fra ESP32 e modulo.
 */
static int registroSbagliato(uint8_t& letto) {
  for (size_t i = 0; i < N_ATTESI; i++) {
    if (g_escluso[i]) continue;
    const RegistroAtteso& r = ATTESI[i];
    if ((sxLeggi(r.reg) & r.maschera) == r.valore) continue;

    const uint8_t a = sxLeggi(r.reg);
    const uint8_t b = sxLeggi(r.reg);
    if ((a & r.maschera) == r.valore || (b & r.maschera) == r.valore) {
      g_incoerenti++;
      continue;
    }
    letto = b;
    return (int)i;
  }
  return -1;
}

static void descrivi(char* out, size_t dim, const RegistroAtteso& r, uint8_t letto) {
  if (r.maschera == 0xFF)
    snprintf(out, dim, "reg 0x%02X = 0x%02X, atteso 0x%02X", r.reg, letto, r.valore);
  else
    snprintf(out, dim, "reg 0x%02X & 0x%02X = 0x%02X, atteso 0x%02X",
             r.reg, r.maschera, letto & r.maschera, r.valore);
}

static void radioVerifica() {
  uint8_t letto = 0;
  const int i = registroSbagliato(letto);
  g_ultimaVerificaMs = millis();
  if (i < 0) return;

  char det[48];
  descrivi(det, sizeof(det), ATTESI[i], letto);
  radioRipara(RIP_REGISTRI, det);
}

// ---------------------------------------------------------------------------
//  Tempo di volo
// ---------------------------------------------------------------------------

/*
 * Tempo di volo di un pacchetto, in millisecondi. Formula del datasheet
 * SX1276/78 paragrafo 4.1.1.7: header esplicito, CRC attivo, low data rate
 * optimization spenta. LORA_CR vale gia' 5, cioe' e' il (CR + 4) della
 * formula. Gemella di quella in serra_nodo/radio.cpp: tools/test_tempo_volo.py
 * le verifica entrambe.
 */
static uint32_t tempoDiVoloMs(size_t lunghezza) {
  const float tSimbolo = (float)(1UL << LORA_SF) / (float)LORA_BW;   // secondi

  int32_t numeratore  = 8 * (int32_t)lunghezza - 4 * LORA_SF + 28 + 16;
  int32_t denominatore = 4 * LORA_SF;
  int32_t simboli = 8;
  if (numeratore > 0)
    simboli += ((numeratore + denominatore - 1) / denominatore) * LORA_CR;

  const float preambolo = (8.0f + 4.25f) * tSimbolo;
  return (uint32_t)((preambolo + simboli * tSimbolo) * 1000.0f) + 1;
}

// ---------------------------------------------------------------------------
//  Avvio
// ---------------------------------------------------------------------------

bool radioAvvia() {
  // Anche il bus SPI dell'ESP32 riparte da capo, come in un riavvio. Con NSS
  // alto il modulo ignora quello che passa sui fili nel frattempo.
  pinMode(LORA_NSS, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);
  SPI.end();
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);

  // Prova del filo di reset (vedi radioResetHw() in radio.h): solo con il
  // modulo in modalita' LoRa e sulla sua pagina di registri, altrimenti il
  // valore di prova finirebbe in un registro FSK e non direbbe niente.
  bool provaReset = false;
  if (sxLeggi(SX_VERSION) == SX_VERSIONE_ATTESA &&
      (sxLeggi(SX_OP_MODE) & 0xC0) == SX_MODO_LORA) {
    sxScrivi(SX_SYNC_WORD, SX_MARCATORE_RESET);
    provaReset = (sxLeggi(SX_SYNC_WORD) == SX_MARCATORE_RESET);
  }

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(LORA_SPI_HZ);
  if (!LoRa.begin(LORA_BAND)) {          // qui dentro il reset hardware
    snprintf(g_avviso, sizeof(g_avviso), "il modulo non risponde (versione 0x%02X)",
             sxLeggi(SX_VERSION));
    g_ok = false;
    return false;
  }

  if (provaReset) g_resetHw = (sxLeggi(SX_SYNC_WORD) == SX_MARCATORE_RESET) ? 0 : 1;

  // Gli stessi identici parametri radio del nodo serra
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setPreambleLength(LORA_PREAMBOLO);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.disableInvertIQ();
  LoRa.enableCrc();

  /*
   * Registri che la libreria lascia al valore di fabbrica, o modifica solo in
   * parte con una lettura-modifica-scrittura. Scritti per intero, la
   * configurazione e' completa anche se il reset hardware non avesse
   * funzionato: dopo un riavvio software l'SX1278 resta acceso, e senza reset
   * conserverebbe qualunque valore avesse prima.
   */
  sxScrivi(SX_MODEM_CONFIG_2,     MC2_ATTESO);
  sxScrivi(SX_LNA,                LNA_ATTESO);
  sxScrivi(SX_PA_RAMP,            PA_RAMP_ATTESO);
  sxScrivi(SX_IRQ_FLAGS_MASK,     0x00);
  sxScrivi(SX_MAX_PAYLOAD_LENGTH, 0xFF);
  sxScrivi(SX_HOP_PERIOD,         0x00);

  sxScrivi(SX_IRQ_FLAGS, 0xFF);
  LoRa.receive();                        // ricezione continua

  /*
   * Verifica a reset appena fatto. Un registro che non torna nemmeno adesso
   * non e' stato alterato da un disturbo: o la radio non accetta quella
   * scrittura, o la tabella si aspetta un valore che questo modulo non tiene.
   * In entrambi i casi il registro viene escluso dalla verifica periodica,
   * che altrimenti lo "riparerebbe" ogni dieci secondi per sempre, e finisce
   * in radioAvviso(). La radio resta in servizio comunque: un errore nella
   * tabella non deve poter rendere sordo il ponte, cioe' produrre proprio il
   * guasto che questo file esiste per curare. Se poi la radio non funziona
   * davvero, ci pensa la sorveglianza del silenzio.
   */
  for (size_t i = 0; i < N_ATTESI; i++) g_escluso[i] = false;
  g_avviso[0] = '\0';

  uint8_t  letto   = 0;
  unsigned esclusi = 0;
  int      i;
  while ((i = registroSbagliato(letto)) >= 0) {
    g_escluso[i] = true;
    if (esclusi++ == 0) descrivi(g_avviso, sizeof(g_avviso), ATTESI[i], letto);
    Serial.printf("[LoRa] ATTENZIONE: registro 0x%02X diverso dall'atteso subito dopo "
                  "il reset (letto 0x%02X): escluso dalla verifica.\n", ATTESI[i].reg, letto);
  }
  if (esclusi > 1) {
    const size_t l = strlen(g_avviso);
    snprintf(g_avviso + l, sizeof(g_avviso) - l, " (+%u)", esclusi - 1);
  }

  g_ok = true;
  g_ultimaVerificaMs = millis();
  return true;
}

// ---------------------------------------------------------------------------
//  Ricezione
// ---------------------------------------------------------------------------

int radioRicevi(char* buf, size_t dim, int& rssi, float& snr) {
  if (!g_ok || dim == 0) return 0;

  // Il caso di gran lunga piu' frequente: niente di arrivato, una lettura e via.
  const uint8_t irq = sxLeggi(SX_IRQ_FLAGS);
  if (!(irq & SX_IRQ_RX_DONE)) return 0;

  // Si azzerano solo i flag: la radio resta in ricezione continua.
  sxScrivi(SX_IRQ_FLAGS, 0xFF);

  if (irq & SX_IRQ_CRC_ERRATO) {
    g_crcErrati++;
    return 0;
  }

  const uint8_t lunghezza = sxLeggi(SX_RX_NB_BYTES);
  sxScrivi(SX_FIFO_ADDR_PTR, sxLeggi(SX_FIFO_RX_CURRENT_ADDR));

  size_t n = 0;
  while (n < lunghezza && n < dim - 1) buf[n++] = (char)sxLeggi(SX_FIFO);
  buf[n] = '\0';

  rssi = LoRa.packetRssi();
  snr  = LoRa.packetSnr();
  return (int)n;
}

// ---------------------------------------------------------------------------
//  Trasmissione
// ---------------------------------------------------------------------------

/*
 * Trasmette e torna in ascolto, senza attese illimitate.
 *
 * La versione sincrona di LoRa.endPacket() e' un ciclo che aspetta il flag
 * di TxDone senza timeout e senza nutrire il watchdog: sul nodo si e'
 * impiantata piu' volte. Qui si parte in modo asincrono e si legge il flag
 * di TxDone, con una guardia proporzionata alla lunghezza del pacchetto.
 *
 * Fino alla 2.4.0, se la guardia scadeva, il ponte stampava un avviso e
 * tornava in ascolto con la radio nello stato in cui era: se il modulo non
 * riusciva piu' a trasmettere, non ci riusciva nemmeno all'ACK dopo, ne' a
 * quelli dei giorni seguenti. Ora una trasmissione che non si conclude vuol
 * dire reset e riconfigurazione.
 */
bool radioTrasmetti(const char* pacchetto) {
  if (!g_ok || !pacchetto) return false;

  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print(pacchetto);
  sxScrivi(SX_IRQ_FLAGS, 0xFF);    // il TxDone da aspettare dev'essere di questo pacchetto
  LoRa.endPacket(true);            // asincrona: scrive un registro e torna

  const uint32_t limite = tempoDiVoloMs(strlen(pacchetto)) * (uint32_t)TX_GUARDIA_X
                        + (uint32_t)TX_GUARDIA_MS;
  const uint32_t t0     = millis();
  bool           finita = false;

  while (millis() - t0 < limite) {
    esp_task_wdt_reset();
    if (sxLeggi(SX_IRQ_FLAGS) & SX_IRQ_TX_DONE) { finita = true; break; }
    delay(1);
  }

  if (!finita) {
    char det[48];
    snprintf(det, sizeof(det), "trasmissione non conclusa in %lu ms", (unsigned long)limite);
    radioRipara(RIP_TX_BLOCCATA, det);
    return false;
  }

  /*
   * Di nuovo in ascolto, con tutti i flag azzerati. Mentre il ponte aspettava
   * l'eco la radio e' rimasta in ricezione, ma il nodo in quella finestra non
   * trasmette, ascolta: un pacchetto arrivato li' sarebbe solo rumore, e il
   * FIFO adesso contiene l'ACK.
   */
  sxScrivi(SX_IRQ_FLAGS, 0xFF);
  LoRa.receive();

  // La trasmissione e' il momento piu' rumoroso, e la libreria ci riscrive
  // dei registri: si controlla subito invece di aspettare il turno.
  radioVerifica();
  return true;
}

// ---------------------------------------------------------------------------
//  Sorveglianza e riparazione
// ---------------------------------------------------------------------------

void radioSorveglia() {
  const uint32_t adesso = millis();

  if (!g_ok) {
    if (adesso - g_ultimoTentativoMs < RADIO_RIPROVA_MS) return;
    g_ultimoTentativoMs = adesso;
    if (radioAvvia()) Serial.println(F("[LoRa] Il modulo risponde di nuovo: radio in ascolto."));
    return;
  }

  if (adesso - g_ultimaVerificaMs < RADIO_VERIFICA_MS) return;
  radioVerifica();
}

void radioRipara(CausaRiparazione causa, const char* dettaglio) {
  g_riparazioni++;
  g_causa = causa;
  snprintf(g_dettaglio, sizeof(g_dettaglio), "%s", dettaglio ? dettaglio : "");

  Serial.printf("[LoRa] RIPARAZIONE #%lu (%s): %s. Reset e riconfigurazione della radio.\n",
                (unsigned long)g_riparazioni, radioCausaTesto(causa), g_dettaglio);

  g_ok = radioAvvia();
  g_ultimoTentativoMs = millis();

  if (g_ok) Serial.println(F("[LoRa] Radio di nuovo in ascolto."));
  else      Serial.printf("[LoRa] La radio non riparte (%s): riprovo ogni %lu s.\n",
                          g_avviso, (unsigned long)(RADIO_RIPROVA_MS / 1000UL));
}

// ---------------------------------------------------------------------------
//  Diagnostica
// ---------------------------------------------------------------------------

const char* radioCausaTesto(CausaRiparazione causa) {
  switch (causa) {
    case RIP_REGISTRI:    return "registri";
    case RIP_TX_BLOCCATA: return "tx_bloccata";
    case RIP_SILENZIO:    return "silenzio";
    case RIP_ACK_PERSI:   return "ack_persi";
    default:              return "";
  }
}

bool             radioOk()                { return g_ok; }
uint32_t         radioRiparazioni()       { return g_riparazioni; }
CausaRiparazione radioUltimaCausa()       { return g_causa; }
const char*      radioUltimoDettaglio()   { return g_dettaglio; }
const char*      radioAvviso()            { return g_avviso; }
uint32_t         radioLettureIncoerenti() { return g_incoerenti; }
uint32_t         radioCrcErrati()         { return g_crcErrati; }
int8_t           radioResetHw()           { return g_resetHw; }
