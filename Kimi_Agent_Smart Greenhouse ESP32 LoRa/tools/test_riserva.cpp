/*
 * Prova sul PC della coda di riserva in RAM RTC (serra_nodo/riserva_coda.h).
 *
 * Si compila e lancia da tools/test_riserva.py, oppure a mano:
 *   g++ -std=c++17 -Wall -Wextra -I serra_nodo tools/test_riserva.cpp -o test_riserva
 *   ./test_riserva
 *
 * Mette alla prova proprio i casi che sul nodo non si possono provocare a
 * comando: la coda piena, la memoria lasciata a meta' da un reset, la
 * spazzatura dell'accensione, una consegna che si interrompe a meta'.
 */

#include "riserva_coda.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_errori = 0;

#define VERIFICA(cond, ...)                                     \
  do {                                                          \
    if (!(cond)) {                                              \
      g_errori++;                                               \
      std::printf("FALLITO (riga %d): ", __LINE__);             \
      std::printf(__VA_ARGS__);                                 \
      std::printf("\n");                                        \
    }                                                           \
  } while (0)

// Una riga realistica: un pacchetto principale senza i campi di
// configurazione, come lo prepara accodaInRiserva().
static std::string riga(unsigned seq) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "GH1;bk=1;v=2;s=%u;t=%u;temp=12.34;hum=87.7;pres=909.1;volt=13.06;"
                "luce=43.9;acqua=2.013;acquaTot=199.28;irr=fuori_orario;bl=-1;txp=4",
                seq, 1794295797u + seq * 900u);
  return buf;
}

static std::vector<std::string> svuota(RiservaCoda& c) {
  std::vector<std::string> fuori;
  const char* r;
  while ((r = rcPrima(c)) != nullptr) {
    fuori.push_back(r);
    rcTogliPrima(c);
  }
  return fuori;
}

int main() {
  static RiservaCoda c;
  const size_t lungRiga = riga(1000).size() + 1;
  const size_t capienza = RISERVA_BYTE / lungRiga;

  std::printf("riserva di %d byte, riga tipica di %zu byte: %zu righe, %.1f ore a 15 min\n\n",
              RISERVA_BYTE, lungRiga, capienza, capienza * 0.25);

  // --- Accensione: memoria piena di spazzatura -----------------------------
  std::srand(42);
  for (size_t i = 0; i < sizeof(c); i++) ((unsigned char*)&c)[i] = (unsigned char)std::rand();
  VERIFICA(!rcValida(c), "la spazzatura dell'accensione passa per una coda valida");
  rcAzzera(c);
  VERIFICA(rcValida(c) && c.record == 0 && rcPrima(c) == nullptr, "coda azzerata non vuota");

  // --- Ordine FIFO ----------------------------------------------------------
  for (unsigned s = 1; s <= 5; s++) VERIFICA(rcAccoda(c, riga(s).c_str()), "accodamento %u rifiutato", s);
  VERIFICA(rcValida(c) && c.record == 5, "5 righe accodate, ne risultano %u", c.record);
  std::vector<std::string> fuori = svuota(c);
  VERIFICA(fuori.size() == 5, "uscite %zu righe su 5", fuori.size());
  for (unsigned s = 1; s <= fuori.size(); s++)
    VERIFICA(fuori[s - 1] == riga(s), "riga %u uscita fuori ordine o alterata", s);
  VERIFICA(rcValida(c) && c.usati == 0, "coda svuotata ma usati=%u", c.usati);

  // --- Coda piena: si sacrificano le piu' vecchie ---------------------------
  const unsigned tante = (unsigned)capienza + 7;
  for (unsigned s = 1; s <= tante; s++) rcAccoda(c, riga(s).c_str());
  VERIFICA(rcValida(c), "coda piena non piu' coerente");
  VERIFICA(c.usati <= RISERVA_BYTE, "sforato il tetto: %u byte", c.usati);
  VERIFICA(c.record + c.scartati == tante, "righe perse per strada: %u + %u scartate su %u",
           c.record, (unsigned)c.scartati, tante);
  VERIFICA(c.record == capienza, "in coda %u righe, ne stavano %zu", c.record, capienza);
  fuori = svuota(c);
  VERIFICA(!fuori.empty() && fuori.front() == riga(tante - (unsigned)fuori.size() + 1),
           "dopo lo scarto la piu' vecchia rimasta non e' quella giusta");
  VERIFICA(!fuori.empty() && fuori.back() == riga(tante), "l'ultima accodata non e' in fondo");
  std::printf("coda piena: %u accodate, %u in coda, %u scartate (le piu' vecchie)\n",
              tante, (unsigned)fuori.size(), (unsigned)c.scartati);

  // --- Righe impossibili ----------------------------------------------------
  rcAzzera(c);
  std::string gigante(RISERVA_BYTE, 'x');
  VERIFICA(!rcAccoda(c, gigante.c_str()), "accettata una riga piu' lunga dell'intera riserva");
  VERIFICA(!rcAccoda(c, ""), "accettata una riga vuota");
  VERIFICA(!rcAccoda(c, nullptr), "accettato un puntatore nullo");
  VERIFICA(rcValida(c) && c.record == 0, "un rifiuto ha sporcato la coda");
  std::string quasi(RISERVA_BYTE - 1, 'y');     // terminatore compreso: esattamente piena
  VERIFICA(rcAccoda(c, quasi.c_str()) && c.usati == RISERVA_BYTE && rcValida(c),
           "una riga che riempie esattamente la riserva non entra");
  VERIFICA(rcAccoda(c, riga(1).c_str()) && c.record == 1 && c.scartati == 1 && rcValida(c),
           "la riga successiva non ha preso il posto di quella che riempiva tutto");

  // --- Reset a meta' scrittura ----------------------------------------------
  rcAzzera(c);
  for (unsigned s = 1; s <= 3; s++) rcAccoda(c, riga(s).c_str());
  RiservaCoda copia = c;
  copia.dati[copia.usati] = 'Z';            // scrittura iniziata, contatori non ancora aggiornati
  copia.usati += 1;
  VERIFICA(!rcValida(copia), "una scrittura interrotta passa per coerente");
  copia = c;
  copia.dati[10] ^= 0x01;                   // un bit cambiato nei dati
  VERIFICA(!rcValida(copia), "un bit cambiato nei dati non viene notato");
  copia = c;
  copia.record = 2;                         // contatore che non torna con i terminatori
  rcSigilla(copia);
  VERIFICA(!rcValida(copia), "contatore incoerente con i terminatori non viene notato");
  copia = c;
  copia.usati = RISERVA_BYTE + 100;         // contatore oltre il buffer
  VERIFICA(!rcValida(copia), "usati oltre il buffer passa per coerente");

  // --- Consegna interrotta: le righe consegnate non tornano ------------------
  rcAzzera(c);
  for (unsigned s = 1; s <= 6; s++) rcAccoda(c, riga(s).c_str());
  unsigned consegnate = 0;
  while (consegnate < 2 && rcPrima(c)) { rcTogliPrima(c); consegnate++; }   // il link cade alla terza
  RiservaCoda dopoReset = c;                 // la RAM RTC al risveglio successivo
  VERIFICA(rcValida(dopoReset), "dopo una consegna parziale la coda non e' piu' coerente");
  fuori = svuota(dopoReset);
  VERIFICA(fuori.size() == 4 && fuori.front() == riga(3) && fuori.back() == riga(6),
           "dopo una consegna parziale restano le righe sbagliate");

  std::printf("\n");
  if (g_errori) {
    std::printf("%d verifiche fallite.\n", g_errori);
    return 1;
  }
  std::printf("Tutti i casi passano: ordine, tetto, scarto dei piu' vecchi, memoria sporca\n"
              "e consegna interrotta.\n");
  return 0;
}
