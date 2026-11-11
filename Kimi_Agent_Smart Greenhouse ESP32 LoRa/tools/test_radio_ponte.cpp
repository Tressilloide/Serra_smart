/*
 * Prova sul PC della radio del ponte (camera_ponte/radio.cpp) contro un
 * SX1278 finto (tools/sim_radio/), con la libreria LoRa vera in mezzo.
 * La lancia tools/test_radio_ponte.py, uno scenario per processo perche'
 * radio.cpp tiene il suo stato in variabili statiche.
 *
 * Gli scenari sono i guasti che il ponte ha incontrato davvero: il modulo
 * che non risponde, quello che tiene la configurazione ma non esce da sleep
 * (11/11/2026), il registro alterato in servizio. Per ognuno si controlla
 * che il ponte non si dichiari a posto da sordo, che dica perche', e che
 * riparta da solo appena il modulo torna a funzionare.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Arduino.h"
#include "SPI.h"
#include "config.h"
#include "radio.h"

uint32_t     simOraMs = 1000;
bool         simSerialeVisibile = false;
Sx1278Sim    sim;
SerialeFinta Serial;
SPIClass     SPI;

static int g_falliti = 0;

#define VERIFICA(cond, ...)                                   \
  do {                                                        \
    if (!(cond)) {                                            \
      g_falliti++;                                            \
      printf("    FALLITO riga %d: %s -- ", __LINE__, #cond); \
      printf(__VA_ARGS__);                                    \
      printf("\n");                                           \
    }                                                         \
  } while (0)

static const uint8_t RICEZIONE = 0x85;

// Il loop() del ponte per `ms` millisecondi finti, un giro ogni 100.
static int g_ricevuti = 0;
static void gira(uint32_t ms) {
  const uint32_t fine = simOraMs + ms;
  char buf[256];
  int rssi = 0;
  float snr = 0;
  while (simOraMs < fine) {
    if (radioRicevi(buf, sizeof(buf), rssi, snr) > 0) g_ricevuti++;
    radioSorveglia();
    simOraMs += 100;
  }
}

static bool riceve(const char* testo) {
  if (!sim.inietta(testo)) return false;
  char buf[256];
  int rssi = 0;
  float snr = 0;
  const int n = radioRicevi(buf, sizeof(buf), rssi, snr);
  return n == (int)strlen(testo) && strcmp(buf, testo) == 0 && rssi == -100 && snr == 7.0f;
}

// ---------------------------------------------------------------------------

static void sano() {
  const uint32_t t0 = simOraMs;
  VERIFICA(radioAvvia(), "un modulo sano deve partire (%s)", radioAvviso());
  VERIFICA(simOraMs - t0 <= 25, "l'avvio di un modulo sano non deve aspettare: %u ms",
           (unsigned)(simOraMs - t0));
  VERIFICA(radioOk(), "radio_ok");
  VERIFICA(radioAvviso()[0] == '\0', "avviso inatteso: %s", radioAvviso());
  VERIFICA(radioAvviiFalliti() == 0, "avvii falliti %u", (unsigned)radioAvviiFalliti());
  VERIFICA((sim.modo() & 0xC7) == RICEZIONE, "modo 0x%02X", sim.modo());
  VERIFICA(sim.reg[0x39] == LORA_SYNC_WORD, "sync word 0x%02X", sim.reg[0x39]);
  VERIFICA(radioResetHw() == -1, "alla prima accensione il filo di reset non si puo' provare");

  VERIFICA(riceve("GH1;v=2;s=7;temp=12.98"), "un pacchetto deve arrivare intatto");

  VERIFICA(radioTrasmetti("ACK1;s=7;t=1794394830"), "l'ACK deve partire");
  VERIFICA((sim.modo() & 0xC7) == RICEZIONE, "dopo l'ACK si torna in ascolto: modo 0x%02X", sim.modo());

  const unsigned scritture = sim.scritture;
  gira(10UL * 60UL * 1000UL);
  VERIFICA(radioRiparazioni() == 0, "riparazioni %u su un modulo sano", (unsigned)radioRiparazioni());
  VERIFICA(radioLettureIncoerenti() == 0, "letture incoerenti %u", (unsigned)radioLettureIncoerenti());
  VERIFICA(sim.scritture == scritture, "in ascolto non si deve scrivere nessun registro");

  // Secondo avvio, a modulo gia' in LoRa: adesso il filo di reset si puo' provare
  VERIFICA(radioAvvia(), "secondo avvio");
  VERIFICA(radioResetHw() == 1, "reset collegato: rst_hw %d", (int)radioResetHw());
  VERIFICA(riceve("GH1;v=2;s=8"), "pacchetto dopo il secondo avvio");
}

static void senzaReset() {
  sim.resetCollegato = false;
  VERIFICA(radioAvvia(), "primo avvio");
  VERIFICA(radioAvvia(), "secondo avvio (%s)", radioAvviso());
  VERIFICA(radioResetHw() == 0, "filo di reset staccato: rst_hw %d", (int)radioResetHw());
  VERIFICA(radioOk() && radioAvviso()[0] == '\0', "senza reset la radio va lo stesso (%s)", radioAvviso());
  VERIFICA(riceve("GH1;v=2;s=9"), "pacchetto");
}

// L'11/11/2026: tutta la configurazione a posto, il modo fermo a 0x80.
static void fermoInSleep() {
  VERIFICA(radioAvvia(), "primo avvio da sano");
  sim.resetCollegato = false;               // come sul ponte vero
  sim.guasto = Sx1278Sim::FERMO_IN_SLEEP;
  sim.cadeInSleep();

  const uint32_t t0 = simOraMs;
  VERIFICA(!radioAvvia(), "una radio in sleep non e' una radio avviata");
  VERIFICA(simOraMs - t0 <= 400, "l'avvio fallito blocca il loop per %u ms", (unsigned)(simOraMs - t0));
  VERIFICA(!radioOk(), "radio_ok deve andare a 0");
  VERIFICA(strcmp(radioAvviso(), "la radio resta in sleep (modo 0x80)") == 0, "avviso: %s", radioAvviso());
  VERIFICA(radioAvviiFalliti() == 1, "avvii falliti %u", (unsigned)radioAvviiFalliti());
  VERIFICA(!sim.inietta("GH1;v=2;s=1"), "in sleep il modulo non riceve");
  VERIFICA(!radioTrasmetti("ACK1;s=1"), "niente ACK con la radio ferma");

  // Cinque minuti cosi': riprova ogni RADIO_RIPROVA_MS, senza contarle come riparazioni
  gira(5UL * 60UL * 1000UL);
  const unsigned attesi = 1 + (5UL * 60UL * 1000UL) / RADIO_RIPROVA_MS;
  VERIFICA(radioAvviiFalliti() >= attesi - 1 && radioAvviiFalliti() <= attesi + 1,
           "avvii falliti %u, attesi circa %u", (unsigned)radioAvviiFalliti(), attesi);
  VERIFICA(radioRiparazioni() == 0, "i nuovi tentativi non sono riparazioni: %u", (unsigned)radioRiparazioni());

  // Il contatto torna buono: entro un giro di RADIO_RIPROVA_MS il ponte ascolta di nuovo
  sim.guasto = Sx1278Sim::SANO;
  gira(RADIO_RIPROVA_MS + 1000UL);
  VERIFICA(radioOk(), "a modulo guarito la radio deve ripartire da sola (%s)", radioAvviso());
  VERIFICA(radioAvviso()[0] == '\0', "avviso rimasto: %s", radioAvviso());
  VERIFICA(radioAvviiFalliti() == 0, "avvii falliti non azzerati: %u", (unsigned)radioAvviiFalliti());
  VERIFICA(riceve("GH1;v=2;s=2"), "pacchetto dopo la guarigione");
}

// Un modulo che ha solo bisogno di tempo per uscire da sleep.
static void oscillatoreLento() {
  sim.guasto = Sx1278Sim::OSCILLATORE_LENTO;
  sim.oscillatoreMs = 20;
  VERIFICA(radioAvvia(), "con la sequenza sleep-standby-ricezione deve partire (%s)", radioAvviso());
  VERIFICA((sim.modo() & 0xC7) == RICEZIONE, "modo 0x%02X", sim.modo());
  VERIFICA(radioAvviso()[0] == '\0', "avviso: %s", radioAvviso());
  VERIFICA(riceve("GH1;v=2;s=3"), "pacchetto");
}

static void oscillatoreTroppoLento() {
  sim.guasto = Sx1278Sim::OSCILLATORE_LENTO;
  sim.oscillatoreMs = 500;
  VERIFICA(!radioAvvia(), "oltre RADIO_MODO_MS e' un avvio fallito");
  VERIFICA(strncmp(radioAvviso(), "la radio resta in sleep", 23) == 0, "avviso: %s", radioAvviso());
}

static void muto() {
  sim.guasto = Sx1278Sim::MUTO;
  VERIFICA(!radioAvvia(), "un modulo muto non parte");
  VERIFICA(strcmp(radioAvviso(), "il modulo non risponde (versione 0x00)") == 0, "avviso: %s", radioAvviso());
  VERIFICA(!radioOk(), "radio_ok");
  gira(60000UL);
  VERIFICA(radioAvviiFalliti() >= 2, "deve riprovare: %u", (unsigned)radioAvviiFalliti());

  sim.guasto = Sx1278Sim::SANO;
  sim.accendi();
  gira(RADIO_RIPROVA_MS + 1000UL);
  VERIFICA(radioOk() && radioAvviso()[0] == '\0', "deve ripartire da solo (%s)", radioAvviso());
  VERIFICA(riceve("GH1;v=2;s=4"), "pacchetto");
}

// In servizio, il modulo cade in sleep e ci resta.
static void cadeERestaInSleep() {
  VERIFICA(radioAvvia(), "avvio");
  gira(60000UL);
  sim.guasto = Sx1278Sim::FERMO_IN_SLEEP;
  sim.cadeInSleep();
  gira(RADIO_VERIFICA_MS + 1000UL);
  VERIFICA(radioRiparazioni() == 1, "la verifica periodica deve accorgersene: riparazioni %u",
           (unsigned)radioRiparazioni());
  VERIFICA(radioUltimaCausa() == RIP_REGISTRI, "causa %s", radioCausaTesto(radioUltimaCausa()));
  VERIFICA(strcmp(radioUltimoDettaglio(), "reg 0x01 & 0xC7 = 0x80, atteso 0x85") == 0,
           "dettaglio: %s", radioUltimoDettaglio());
  VERIFICA(!radioOk(), "radio_ok deve andare a 0");
  VERIFICA(strncmp(radioAvviso(), "la radio resta in sleep", 23) == 0, "avviso: %s", radioAvviso());

  gira(10UL * 60UL * 1000UL);
  VERIFICA(radioRiparazioni() == 1, "una sola riparazione, poi solo tentativi: %u", (unsigned)radioRiparazioni());

  sim.guasto = Sx1278Sim::SANO;
  gira(RADIO_RIPROVA_MS + 1000UL);
  VERIFICA(radioOk(), "deve ripartire (%s)", radioAvviso());
  VERIFICA(riceve("GH1;v=2;s=5"), "pacchetto");
}

// In servizio, il modo cade una volta sola: basta un reset.
static void cadeESiRialza() {
  VERIFICA(radioAvvia(), "avvio");
  gira(60000UL);
  sim.cadeInSleep();
  gira(RADIO_VERIFICA_MS + 1000UL);
  VERIFICA(radioRiparazioni() == 1, "riparazioni %u", (unsigned)radioRiparazioni());
  VERIFICA(radioOk(), "dopo la riparazione deve ascoltare (%s)", radioAvviso());
  VERIFICA(radioAvviiFalliti() == 0, "avvii falliti %u", (unsigned)radioAvviiFalliti());
  VERIFICA(riceve("GH1;v=2;s=6"), "pacchetto");
}

// Un registro di configurazione che non tiene il valore: si esclude e si va avanti.
static void registroTestardo() {
  sim.testardi[0x0A] = 0x08;                 // rampa del PA
  VERIFICA(radioAvvia(), "un registro che non torna non deve fermare la radio (%s)", radioAvviso());
  VERIFICA(radioOk(), "radio_ok");
  VERIFICA(strcmp(radioAvviso(), "reg 0x0A & 0x0F = 0x08, atteso 0x09") == 0, "avviso: %s", radioAvviso());
  gira(5UL * 60UL * 1000UL);
  VERIFICA(radioRiparazioni() == 0, "il registro escluso non va riparato ogni dieci secondi: %u",
           (unsigned)radioRiparazioni());
  VERIFICA(riceve("GH1;v=2;s=10"), "pacchetto");
}

// Troppi registri che non tornano: non e' la tabella, e' il modulo.
static void troppiRegistri() {
  sim.testardi[0x0A] = 0x00;
  sim.testardi[0x0B] = 0x00;
  sim.testardi[0x1D] = 0x00;
  sim.testardi[0x1E] = 0x00;
  sim.testardi[0x39] = 0x00;
  VERIFICA(RADIO_ESCLUSI_MAX < 5, "lo scenario presume RADIO_ESCLUSI_MAX < 5");
  VERIFICA(!radioAvvia(), "con cinque registri sbagliati la radio non e' in servizio");
  VERIFICA(!radioOk(), "radio_ok");
  VERIFICA(strstr(radioAvviso(), "(+4)") != nullptr, "avviso: %s", radioAvviso());

  sim.testardi.clear();
  gira(RADIO_RIPROVA_MS + 1000UL);
  VERIFICA(radioOk() && radioAvviso()[0] == '\0', "deve ripartire (%s)", radioAvviso());
}

// Il registro della versione che legge storto a reset appena fatto.
static void versioneSbagliataDopo() {
  VERIFICA(radioAvvia(), "avvio");
  gira(30000UL);
  sim.guasto = Sx1278Sim::MUTO;
  gira(RADIO_VERIFICA_MS + 1000UL);
  VERIFICA(radioRiparazioni() == 1, "riparazioni %u", (unsigned)radioRiparazioni());
  VERIFICA(strcmp(radioUltimoDettaglio(), "reg 0x42 = 0x00, atteso 0x12") == 0, "dettaglio: %s", radioUltimoDettaglio());
  VERIFICA(!radioOk(), "radio_ok");
  VERIFICA(strcmp(radioAvviso(), "il modulo non risponde (versione 0x00)") == 0, "avviso: %s", radioAvviso());
}

// ---------------------------------------------------------------------------

struct Scenario { const char* nome; void (*fn)(); };

static const Scenario SCENARI[] = {
  { "sano",                     sano },
  { "senza_reset",              senzaReset },
  { "fermo_in_sleep",           fermoInSleep },
  { "oscillatore_lento",        oscillatoreLento },
  { "oscillatore_troppo_lento", oscillatoreTroppoLento },
  { "muto",                     muto },
  { "cade_e_resta_in_sleep",    cadeERestaInSleep },
  { "cade_e_si_rialza",         cadeESiRialza },
  { "registro_testardo",        registroTestardo },
  { "troppi_registri",          troppiRegistri },
  { "muto_in_servizio",         versioneSbagliataDopo },
};

int main(int argc, char** argv) {
  if (argc < 2) {
    for (const Scenario& s : SCENARI) printf("%s\n", s.nome);
    return 0;
  }
  simSerialeVisibile = argc > 2;
  sim.pinNss = LORA_NSS;
  sim.pinRst = LORA_RST;
  for (const Scenario& s : SCENARI) {
    if (strcmp(s.nome, argv[1]) != 0) continue;
    s.fn();
    printf("  %-26s %s\n", s.nome, g_falliti ? "FALLITO" : "ok");
    return g_falliti ? 1 : 0;
  }
  printf("scenario sconosciuto: %s\n", argv[1]);
  return 2;
}
