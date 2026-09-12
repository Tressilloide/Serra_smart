#include "backlog.h"
#include "impostazioni.h"
#include "watchdog.h"

#include <SPI.h>
#include <SD.h>

// Flag globale: prima era una variabile locale del setup() e logSD() provava a
// indovinare lo stato della SD con SD.exists("/"), che non e' affidabile.
static bool g_sdOk = false;

bool backlogDisponibile() { return g_sdOk; }

// ---------------------------------------------------------------------------

bool backlogInit() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  g_sdOk = SD.begin(SD_CS);

  if (!g_sdOk) {
    Serial.println(F("[SD] ERRORE: scheda non trovata. Nessun backlog disponibile."));
    Serial.println(F("[SD] Il nodo continua a funzionare, ma i pacchetti non"));
    Serial.println(F("[SD] consegnati andranno persi. Controlla: FAT32, CS=GPIO4, 3,3 V."));
    return false;
  }

  /*
   * Recupero dopo un'interruzione a meta' operazione.
   *
   * Il riordino della coda avviene copiando i record non consegnati in un
   * file temporaneo, cancellando l'originale e rinominando il temporaneo.
   * Se la corrente va via fra la cancellazione e il rinomino — o se il
   * rinomino fallisce — resta solo il temporaneo, e tutti i record ancora
   * da consegnare sarebbero abbandonati li' senza che nessuno li guardi piu'.
   * Proprio lo scenario contro cui il backlog esiste.
   */
  if (!SD.exists(FILE_BACKLOG) && SD.exists(FILE_BACKLOG_TMP)) {
    Serial.println(F("[SD] Trovato un backlog temporaneo orfano: lo recupero."));
    if (SD.rename(FILE_BACKLOG_TMP, FILE_BACKLOG))
      backlogLog("BACKLOG recuperato da file temporaneo orfano");
    else
      Serial.println(F("[SD] ERRORE: recupero del temporaneo fallito."));
  }

  Serial.printf("[SD] Scheda montata (%llu MB). Record in coda: %lu\n",
                SD.cardSize() / (1024ULL * 1024ULL), (unsigned long)backlogConta());
  return true;
}

// ---------------------------------------------------------------------------

/*
 * Il conteggio richiede di scorrere tutto il file, quindi viene memorizzato.
 * Serve davvero: finisce nel blocco di stato di OGNI pacchetto, e durante una
 * finestra di manutenzione i pacchetti sono uno ogni cinque secondi. Con una
 * coda da qualche centinaio di kilobyte significherebbe rileggere la microSD
 * da capo in continuazione, consumando proprio i minuti della manutenzione.
 * La cache viene invalidata dalle sole tre funzioni che cambiano il file.
 */
static uint32_t g_conta       = 0;
static bool     g_contaValida = false;

static void invalidaConta() { g_contaValida = false; }

uint32_t backlogConta() {
  if (g_contaValida) return g_conta;
  if (!g_sdOk || !SD.exists(FILE_BACKLOG)) { g_conta = 0; g_contaValida = true; return 0; }

  File f = SD.open(FILE_BACKLOG, FILE_READ);
  if (!f) return 0;

  uint32_t righe = 0;
  uint8_t  buf[512];
  int      letti;
  while ((letti = f.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < letti; i++) if (buf[i] == '\n') righe++;
    wdtNutri();
  }
  f.close();

  g_conta       = righe;
  g_contaValida = true;
  return righe;
}

// ---------------------------------------------------------------------------
//  Copia la porzione di file da 'daOffset' a fine file in un temporaneo, poi
//  sostituisce l'originale. E' l'operazione su cui si regge tutto: nessun
//  record viene mai ricostruito dalla RAM, quindi non se ne possono perdere.
// ---------------------------------------------------------------------------

static bool copiaCodaESostituisci(uint32_t daOffset) {
  invalidaConta();

  File src = SD.open(FILE_BACKLOG, FILE_READ);
  if (!src) return false;

  uint32_t dim = src.size();
  if (daOffset >= dim) {           // consegnato tutto: basta cancellare
    src.close();
    SD.remove(FILE_BACKLOG);
    return true;
  }

  SD.remove(FILE_BACKLOG_TMP);
  File dst = SD.open(FILE_BACKLOG_TMP, FILE_WRITE);
  if (!dst) {
    src.close();
    Serial.println(F("[SD] ERRORE: impossibile creare il file temporaneo."));
    return false;
  }

  src.seek(daOffset);
  uint8_t buf[512];
  int letti;
  while ((letti = src.read(buf, sizeof(buf))) > 0) {
    dst.write(buf, letti);
    wdtNutri();
  }

  src.close();
  dst.close();

  SD.remove(FILE_BACKLOG);
  if (!SD.rename(FILE_BACKLOG_TMP, FILE_BACKLOG)) {
    Serial.println(F("[SD] ERRORE: rename del backlog fallito!"));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Tetto di dimensione: scarta i record piu' vecchi mantenendo i piu' recenti.
//  Si taglia al primo '\n' dopo un quarto del file, cosi' l'operazione non
//  scatta a ogni singolo accodamento successivo.
// ---------------------------------------------------------------------------

static void applicaTettoDimensione() {
  if (!SD.exists(FILE_BACKLOG)) return;

  File f = SD.open(FILE_BACKLOG, FILE_READ);
  if (!f) return;
  uint32_t dim = f.size();
  if (dim <= BACKLOG_MAX_BYTE) { f.close(); return; }

  // Allinea il punto di taglio a un confine di record
  uint32_t taglio = dim / 4;
  f.seek(taglio);
  uint32_t scartati = 0;
  while (f.available() && f.read() != '\n') { /* avanza fino a fine riga */ }
  taglio = f.position();

  /*
   * Conta quanti record stiamo perdendo, per poterlo dire a Home Assistant.
   * A blocchi da 512 byte e non byte per byte: il taglio puo' valere decine
   * di migliaia di byte, e altrettante chiamate di lettura attraverso lo
   * strato FAT terrebbero occupato il nodo per svariati secondi, per giunta
   * senza nutrire il watchdog.
   */
  f.seek(0);
  uint32_t letto = 0;
  uint8_t  buf[512];
  while (letto < taglio) {
    uint32_t quanti = taglio - letto;
    if (quanti > sizeof(buf)) quanti = sizeof(buf);
    int n = f.read(buf, quanti);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) if (buf[i] == '\n') scartati++;
    letto += (uint32_t)n;
    wdtNutri();
  }
  f.close();

  Serial.printf("[SD] Backlog oltre il tetto (%lu byte): scarto %lu record vecchi.\n",
                (unsigned long)dim, (unsigned long)scartati);

  if (copiaCodaESostituisci(taglio)) {
    g_cfg.backlogScartati += scartati;
    impostazioniModificate();
    backlogLog(String("BACKLOG TETTO scartati=") + scartati);
  }
}

// ---------------------------------------------------------------------------

bool backlogAccoda(const char* riga) {
  if (!g_sdOk || !riga || !*riga) return false;

  File f = SD.open(FILE_BACKLOG, FILE_APPEND);
  if (!f) {
    Serial.println(F("[SD] ERRORE: impossibile aprire il backlog in append."));
    return false;
  }
  f.print(riga);
  f.print('\n');          // solo LF: println scriverebbe CRLF
  f.close();

  invalidaConta();
  applicaTettoDimensione();
  return true;
}

// ---------------------------------------------------------------------------

uint32_t backlogDrena(FnInvioRecord invia) {
  if (!g_sdOk || !invia || !SD.exists(FILE_BACKLOG)) return 0;

  File f = SD.open(FILE_BACKLOG, FILE_READ);
  if (!f) return 0;

  uint32_t consegnati = 0;
  uint32_t offsetNonConsegnato = 0;
  bool     linkCaduto = false;

  while (f.available() && consegnati < BACKLOG_MAX_INVII) {
    wdtNutri();

    uint32_t posPrima = f.position();
    String riga = f.readStringUntil('\n');
    riga.trim();                              // toglie eventuali \r residui

    if (riga.length() < 5) {                  // riga vuota o corrotta: si salta
      offsetNonConsegnato = f.position();
      continue;
    }

    if (!invia(riga.c_str())) {
      // Link caduto: questo record e TUTTI quelli successivi restano in coda.
      offsetNonConsegnato = posPrima;
      linkCaduto = true;
      break;
    }

    consegnati++;
    offsetNonConsegnato = f.position();
    delay(150);                               // respiro tra un pacchetto e l'altro
  }

  bool esaurito = !linkCaduto && !f.available();
  f.close();

  if (esaurito) {
    SD.remove(FILE_BACKLOG);
    Serial.printf("[SD] Backlog svuotato: %lu record consegnati.\n",
                  (unsigned long)consegnati);
  } else if (consegnati > 0) {
    // Qui sta la differenza con la versione precedente: la coda non consegnata
    // viene copiata dal FILE, non ricostruita dalla RAM.
    copiaCodaESostituisci(offsetNonConsegnato);
    Serial.printf("[SD] %lu record consegnati, %lu ancora in coda.\n",
                  (unsigned long)consegnati, (unsigned long)backlogConta());
  } else {
    Serial.println(F("[SD] Nessun record consegnato: la coda resta intatta."));
  }

  return consegnati;
}

// ---------------------------------------------------------------------------

void backlogSvuota() {
  if (!g_sdOk) return;
  invalidaConta();
  SD.remove(FILE_BACKLOG);
  SD.remove(FILE_BACKLOG_TMP);
  Serial.println(F("[SD] Backlog svuotato su richiesta."));
  backlogLog("BACKLOG SVUOTATO su comando");
}

// ---------------------------------------------------------------------------

void backlogLog(const String& messaggio) {
  if (!g_sdOk) return;

  // Tetto sul file di log: senza, dopo mesi riempirebbe la scheda.
  if (SD.exists(FILE_LOG)) {
    File chk = SD.open(FILE_LOG, FILE_READ);
    if (chk) {
      bool troppoGrande = chk.size() > LOG_MAX_BYTE;
      chk.close();
      if (troppoGrande) SD.remove(FILE_LOG);
    }
  }

  File f = SD.open(FILE_LOG, FILE_APPEND);
  if (!f) return;
  f.printf("%lu %s\n", (unsigned long)millis(), messaggio.c_str());
  f.close();
}
