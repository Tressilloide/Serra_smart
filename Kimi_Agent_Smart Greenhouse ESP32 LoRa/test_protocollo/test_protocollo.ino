/*
 * ============================================================================
 *  SERRA SMART — AUTOTEST DEL PROTOCOLLO
 * ============================================================================
 *
 *  Sketch di collaudo: caricalo su un ESP32 QUALSIASI (anche senza nulla
 *  collegato), apri il monitor seriale a 115200 e leggi l'esito.
 *
 *  Verifica la logica di protocollo.h senza bisogno di radio, sensori o
 *  microSD: serializzazione, parsing, compatibilita' con il protocollo v1,
 *  estrazione del numero di sequenza, comportamento al superamento del tetto
 *  di dimensione e casi limite.
 *
 *  Da rieseguire ogni volta che modifichi protocollo.h, PRIMA di riflashare
 *  il nodo in serra: un errore qui si manifesterebbe come dati sbagliati in
 *  Home Assistant, che e' molto piu' difficile da diagnosticare.
 *
 *  Scheda: "ESP32 Dev Module"
 * ============================================================================
 */

#include "protocollo.h"

static int g_passati = 0;
static int g_falliti = 0;

// ---------------------------------------------------------------------------

static void verifica(const char* nome, bool condizione, const char* dettaglio = nullptr) {
  if (condizione) {
    g_passati++;
    Serial.printf("  [OK]     %s\n", nome);
  } else {
    g_falliti++;
    Serial.printf("  [FALLITO] %s%s%s\n", nome,
                  dettaglio ? " -> " : "", dettaglio ? dettaglio : "");
  }
}

static void verificaTesto(const char* nome, const char* atteso, const char* ottenuto) {
  bool ok = ottenuto && strcmp(atteso, ottenuto) == 0;
  if (ok) {
    g_passati++;
    Serial.printf("  [OK]     %s\n", nome);
  } else {
    g_falliti++;
    Serial.printf("  [FALLITO] %s\n           atteso  : \"%s\"\n           ottenuto: \"%s\"\n",
                  nome, atteso, ottenuto ? ottenuto : "(null)");
  }
}

// ---------------------------------------------------------------------------
//  1. Serializzazione
// ---------------------------------------------------------------------------

static void testSerializzazione() {
  Serial.println(F("\n--- 1. Serializzazione ---"));

  PacchettoKV p;
  char buf[300];

  p.reset();
  p.aggiungiU("v", 2);
  p.aggiungiU("s", 42);
  p.aggiungiU("t", 1755500400UL);
  p.aggiungiF("temp", 24.1f, 2);
  p.aggiungiF("hum", 61.34f, 1);

  p.serializza("GH1", buf, sizeof(buf));
  verificaTesto("pacchetto dati completo",
                "GH1;v=2;s=42;t=1755500400;temp=24.10;hum=61.3", buf);

  // Il caso che rompeva la vecchia versione: nessun campo, solo il prefisso.
  p.reset();
  p.serializza("GH1", buf, sizeof(buf));
  verificaTesto("pacchetto senza campi (stringa terminata)", "GH1", buf);

  // I sensori assenti valgono NAN e devono diventare la sentinella -127,
  // non "nan" che non e' JSON valido e romperebbe Home Assistant.
  p.reset();
  p.aggiungiF("temp", NAN, 2);
  p.aggiungiF("pres", INFINITY, 1);
  p.serializza("GH1", buf, sizeof(buf));
  verificaTesto("NAN e infinito diventano -127", "GH1;temp=-127;pres=-127", buf);

  // Valori negativi (la soglia terreno disattivata vale -1)
  p.reset();
  p.aggiungiI("sSoil", -1);
  p.serializza("GH1", buf, sizeof(buf));
  verificaTesto("interi negativi", "GH1;sSoil=-1", buf);
}

// ---------------------------------------------------------------------------
//  2. Parsing v2
// ---------------------------------------------------------------------------

static void testParsing() {
  Serial.println(F("\n--- 2. Parsing v2 ---"));

  PacchettoKV p;
  char prefisso[12];

  verifica("parse riesce",
           p.parse("GH1;v=2;s=42;t=1755500400;temp=24.10;soil1=42.5", prefisso, sizeof(prefisso)));
  verificaTesto("prefisso estratto", "GH1", prefisso);
  verifica("numero di campi", p.n() == 5);
  verifica("s = 42", p.valoreU("s") == 42);
  verifica("t = 1755500400", p.valoreU("t") == 1755500400UL);
  verifica("temp = 24.10", fabsf(p.valoreF("temp") - 24.10f) < 0.001f);
  verifica("soil1 = 42.5", fabsf(p.valoreF("soil1") - 42.5f) < 0.001f);
  verifica("chiave assente -> nullptr", p.valore("inesistente") == nullptr);
  verifica("chiave assente -> valore di default", p.valoreU("inesistente", 99) == 99);
  verifica("ha() funziona", p.ha("temp") && !p.ha("uv"));

  // ACK con comando accodato: e' il messaggio su cui si regge tutta la
  // comunicazione bidirezionale.
  PacchettoKV a;
  a.parse("ACK;s=42;now=1755500402;c=7;o=IRR;a=120", prefisso, sizeof(prefisso));
  verificaTesto("prefisso ACK", "ACK", prefisso);
  verifica("ACK: sequenza", a.valoreU("s") == 42);
  verifica("ACK: ora del ponte", a.valoreU("now") == 1755500402UL);
  verifica("ACK: id comando", a.valoreU("c") == 7);
  verificaTesto("ACK: opcode", "IRR", a.valore("o"));
  verificaTesto("ACK: argomenti", "120", a.valore("a"));

  // ACK senza comando: "o" deve essere assente, non stringa vuota
  PacchettoKV b;
  b.parse("ACK;s=43;now=1755500500", prefisso, sizeof(prefisso));
  verifica("ACK senza comando", b.valore("o") == nullptr);

  // Comando con piu' argomenti
  PacchettoKV c;
  c.parse("ACK;s=1;c=9;o=CAL;a=soil1,3010,1290");
  verificaTesto("argomenti multipli", "soil1,3010,1290", c.valore("a"));

  // Casi limite
  PacchettoKV d;
  verifica("stringa vuota rifiutata", !d.parse(""));
  verifica("puntatore nullo rifiutato", !d.parse(nullptr));

  PacchettoKV e;
  e.parse("GH1;;;temp=20", prefisso, sizeof(prefisso));
  verifica("separatori doppi ignorati", e.n() == 1 && e.valoreF("temp") == 20.0f);
}

// ---------------------------------------------------------------------------
//  3. Compatibilita' con il protocollo v1
// ---------------------------------------------------------------------------

static void testCompatibilitaV1() {
  Serial.println(F("\n--- 3. Compatibilita' v1 ---"));

  const char* v1 = "GH1,42,1755500400,24.10,61.30,1012.50,78.4,12.35,0";
  const char* v2 = "GH1;v=2;s=42;t=1755500400;temp=24.10";

  verifica("riconoscimento v2", protoEV2(v2));
  verifica("riconoscimento v1", !protoEV2(v1));

  PacchettoKV p;
  char prefisso[12];
  verifica("parse v1 riesce", protoParseV1(v1, p, prefisso, sizeof(prefisso)));
  verificaTesto("v1: prefisso", "GH1", prefisso);
  verificaTesto("v1: marcato come versione 1", "1", p.valore("v"));
  verifica("v1: sequenza", p.valoreU("s") == 42);
  verifica("v1: timestamp", p.valoreU("t") == 1755500400UL);
  verifica("v1: temperatura", fabsf(p.valoreF("temp") - 24.10f) < 0.001f);
  verifica("v1: umidita'", fabsf(p.valoreF("hum") - 61.30f) < 0.001f);
  verifica("v1: pressione", fabsf(p.valoreF("pres") - 1012.50f) < 0.01f);
  verifica("v1: luce", fabsf(p.valoreF("luce") - 78.4f) < 0.01f);
  verifica("v1: tensione", fabsf(p.valoreF("volt") - 12.35f) < 0.001f);
  verifica("v1: flags", p.valoreU("flags") == 0);
}

// ---------------------------------------------------------------------------
//  4. Estrazione del numero di sequenza
// ---------------------------------------------------------------------------

static void testEstrazioneSeq() {
  Serial.println(F("\n--- 4. Estrazione sequenza ---"));

  verifica("seq da pacchetto v2",
           protoEstraiSeq("GH1;v=2;s=42;t=1755500400;temp=24.1") == 42);
  verifica("seq da pacchetto v1",
           protoEstraiSeq("GH1,4242,1755500400,24.10,61.30,1012.50,78.4,12.35,0") == 4242);
  verifica("seq assente -> 0", protoEstraiSeq("GH1;v=2;temp=24.1") == 0);
  verifica("puntatore nullo -> 0", protoEstraiSeq(nullptr) == 0);

  // Le chiavi di stato iniziano tutte per "s" (sAuto, sOra, sMin, sDur,
  // sSoil, slp): la ricerca di ";s=" non deve confondersi con nessuna di loro.
  verifica("nessuna confusione con sAuto/sOra/sDur/slp",
           protoEstraiSeq("GH1;v=2;sAuto=1;sOra=17;sDur=300;slp=900;s=77") == 77);
}

// ---------------------------------------------------------------------------
//  5. Tetto di dimensione
// ---------------------------------------------------------------------------

static void testTetto() {
  Serial.println(F("\n--- 5. Tetto di dimensione ---"));

  PacchettoKV p;
  char buf[400];

  // Molti campi lunghi: si deve superare il tetto e scattare il troncamento
  p.reset();
  for (uint8_t i = 0; i < 20; i++) {
    char chiave[10];
    snprintf(chiave, sizeof(chiave), "sens%u", i);
    p.aggiungiF(chiave, 1234.5678f, 4);
  }
  size_t n = p.serializza("GH1", buf, sizeof(buf));

  verifica("il pacchetto resta entro il tetto", n <= PROTO_MAX_PAYLOAD,
           String(String("lunghezza ") + n).c_str());
  verifica("il troncamento e' segnalato", strstr(buf, "trunc=1") != nullptr);

  // Il punto cruciale: un pacchetto troncato deve restare INTERPRETABILE.
  // Meglio incompleto che tagliato a meta' e inutilizzabile.
  PacchettoKV r;
  char prefisso[12];
  verifica("il pacchetto troncato e' ancora parsabile",
           r.parse(buf, prefisso, sizeof(prefisso)) && strcmp(prefisso, "GH1") == 0);
  verificaTesto("primo campo intatto dopo il troncamento", "1234.5678", r.valore("sens0"));

  Serial.printf("           (%u byte, %u campi su 20 trasmessi)\n",
                (unsigned)n, r.n() - 1);

  // Nessun campo deve risultare tagliato a meta'
  bool tuttiInteri = true;
  for (uint8_t i = 0; i < r.n(); i++)
    if (strlen(r.campo(i).valore) == 0) tuttiInteri = false;
  verifica("nessun campo tagliato a meta'", tuttiInteri);

  // Superamento del numero massimo di campi in memoria
  PacchettoKV q;
  q.reset();
  for (uint8_t i = 0; i < PROTO_MAX_CAMPI + 5; i++) q.aggiungiU("x", i);
  verifica("il numero di campi non sfora la struttura", q.n() == PROTO_MAX_CAMPI);
  verifica("il sovraccarico e' segnalato", q.troncato());
}

// ---------------------------------------------------------------------------
//  6. Andata e ritorno
// ---------------------------------------------------------------------------

static void testAndataERitorno() {
  Serial.println(F("\n--- 6. Andata e ritorno ---"));

  // Un pacchetto realistico come quello che il nodo invia davvero
  PacchettoKV out;
  out.reset();
  out.aggiungiU("v", 2);
  out.aggiungiU("s", 1234);
  out.aggiungiU("t", 1755500400UL);
  out.aggiungiF("temp", 24.13f, 2);
  out.aggiungiF("hum", 61.3f, 1);
  out.aggiungiF("pres", 1012.5f, 1);
  out.aggiungiF("volt", 12.35f, 2);
  out.aggiungiF("luce", 78.4f, 1);
  out.aggiungiF("soil1", 42.5f, 1);
  out.aggiungiF("soil2", 51.2f, 1);
  out.aggiungiF("acqua", 3.21f, 3);
  out.aggiungiF("acquaTot", 126.66f, 2);
  out.aggiungi ("irr", "ok");
  out.aggiungiU("bl", 0);
  out.aggiungiU("sAuto", 1);
  out.aggiungiU("sOra", 17);
  out.aggiungiU("sMin", 0);
  out.aggiungiU("sDur", 300);
  out.aggiungiI("sSoil", -1);
  out.aggiungiU("slp", 900);

  char buf[300];
  size_t n = out.serializza("GH1", buf, sizeof(buf));

  Serial.printf("  Pacchetto reale (%u byte):\n  %s\n", (unsigned)n, buf);
  verifica("il pacchetto reale sta nel tetto", n <= PROTO_MAX_PAYLOAD);
  verifica("il pacchetto reale non viene troncato", strstr(buf, "trunc=1") == nullptr);

  PacchettoKV in;
  char prefisso[12];
  in.parse(buf, prefisso, sizeof(prefisso));

  verifica("stesso numero di campi", in.n() == out.n());
  verifica("temp conservata", fabsf(in.valoreF("temp") - 24.13f) < 0.001f);
  verifica("acquaTot conservata", fabsf(in.valoreF("acquaTot") - 126.66f) < 0.01f);
  verificaTesto("irr conservata", "ok", in.valore("irr"));
  verifica("sSoil negativa conservata", in.valoreL("sSoil") == -1);
  verifica("slp conservata", in.valoreU("slp") == 900);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1500);          // tempo per aprire il monitor seriale

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.println(F("  SERRA SMART — AUTOTEST DEL PROTOCOLLO"));
  Serial.printf ("  versione %d, tetto %d byte, max %d campi\n",
                 PROTO_VERSIONE, PROTO_MAX_PAYLOAD, PROTO_MAX_CAMPI);
  Serial.println(F("============================================================"));

  testSerializzazione();
  testParsing();
  testCompatibilitaV1();
  testEstrazioneSeq();
  testTetto();
  testAndataERitorno();

  Serial.println(F("\n============================================================"));
  Serial.printf ("  RISULTATO: %d superati, %d falliti\n", g_passati, g_falliti);
  if (g_falliti == 0)
    Serial.println(F("  Tutto a posto: protocollo.h e' coerente."));
  else
    Serial.println(F("  ATTENZIONE: correggi prima di riflashare il nodo in serra!"));
  Serial.println(F("============================================================"));
}

void loop() {
  delay(10000);
}
