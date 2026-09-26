/*
 * ============================================================================
 *  SERRA SMART — PROTOCOLLO LoRa v2  (file CONDIVISO)
 * ============================================================================
 *
 *  >>> QUESTO FILE DEVE ESSERE IDENTICO IN serra_nodo/, camera_ponte/ E <<<
 *  >>> test_protocollo/                                                  <<<
 *      Se lo modifichi, ricopialo nelle altre due cartelle e riflasha nodo e
 *      ponte.
 *
 *  ---------------------------------------------------------------------------
 *  PERCHE' chiave=valore e non piu' CSV posizionale
 *  ---------------------------------------------------------------------------
 *  Il protocollo v1 era "GH1,seq,epoch,temp,hum,pres,luce,volt,flags": la
 *  posizione nella stringa determinava il significato del campo. Aggiungere un
 *  sensore voleva dire cambiare il codice del nodo E del ponte insieme, e
 *  riflasharli in modo coordinato o i dati venivano interpretati male.
 *
 *  Con "chiave=valore" il ponte non ha bisogno di sapere quali sensori esistono:
 *  copia ogni coppia nel JSON MQTT cosi' com'e'. Aggiungere un sensore alla
 *  serra diventa una modifica al SOLO nodo, e in Home Assistant il sensore
 *  compare da solo grazie alla discovery generica del ponte.
 *
 *  ---------------------------------------------------------------------------
 *  FORMATO
 *  ---------------------------------------------------------------------------
 *  Dati (nodo -> ponte):
 *      GH1;v=2;s=42;t=1755500400;temp=24.10;hum=61.3;soil1=42.5;acqua=3.21;bl=0
 *
 *  ACK semplice (ponte -> nodo):
 *      ACK;s=42;now=1755500402
 *
 *  ACK con comando accodato (ponte -> nodo):
 *      ACK;s=42;now=1755500402;c=7;o=IRR;a=120
 *
 *  Esito di un comando (nodo -> ponte): e' un normale pacchetto dati con in piu'
 *      ...;res=7;rc=0
 *
 *  Record arretrato, cioe' uscito dal backlog del nodo (dal firmware 2.5.0):
 *      GH1;bk=1;v=2;s=41;t=1755499500;...
 *  Il ponte lo pubblica su serra/nodo/storico e mai sullo stato attuale.
 *
 *  Chiavi riservate al trasporto:
 *      v    versione protocollo        s    numero di sequenza
 *      t    timestamp Unix del dato    now  ora corrente secondo il ponte (NTP)
 *      c    id del comando             o    opcode del comando
 *      a    argomenti del comando      res  id del comando eseguito
 *      rc   esito (0 = OK)             h    tag di autenticazione (riservato)
 *      tz   scarto del fuso (ACK)      bk   record arretrato dal backlog
 *  Tutte le altre chiavi sono letture di sensori e viaggiano fino a HA intatte.
 *
 *  ---------------------------------------------------------------------------
 *  ESTENSIONE FUTURA — autenticazione
 *  ---------------------------------------------------------------------------
 *  Il campo "h" e' riservato a un tag HMAC. Oggi non viene ne' generato ne'
 *  verificato (scelta esplicita: LoRa in chiaro), ma il parser lo tratta come
 *  un campo qualunque: potrai attivarlo in futuro senza rompere la compatibilita'.
 *  Nel frattempo la protezione contro comandi ostili o malfunzionanti sta nei
 *  tetti di sicurezza compilati nel nodo (vedi config.h).
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------

#define PROTO_VERSIONE      2
#define PROTO_SEP           ';'
#define PROTO_MAX_PAYLOAD   250   // Tetto operativo; il limite fisico LoRa e' 255.
                                  // A SF7/BW125 un pacchetto pieno occupa la radio
                                  // ~400 ms, quattro volte l'ora: irrilevante.
                                  // Con 4 sensori terreno il pacchetto arriva a
                                  // ~240 byte, quindi il margine serve tutto.
#define PROTO_MAX_CAMPI     28
#define PROTO_LEN_CHIAVE    10
#define PROTO_LEN_VALORE    20

#define PROTO_PREFIX_ACK    "ACK"

// Codici di esito dei comandi (campo "rc")
#define RC_OK               0
#define RC_OPCODE_IGNOTO    1
#define RC_ARGOMENTI        2
#define RC_NEGATO_SICUREZZA 3   // Comando valido ma bloccato da un tetto di sicurezza
#define RC_HW_ASSENTE       4   // Serve un componente che non c'e' (es. flussometro)
#define RC_ERRORE_INTERNO   5

struct CampoKV {
  char chiave[PROTO_LEN_CHIAVE];
  char valore[PROTO_LEN_VALORE];
};

/*
 * Contenitore di coppie chiave=valore, senza allocazioni dinamiche
 * (~840 byte in stack/BSS: sicuro anche dentro un ISR-free setup()).
 */
class PacchettoKV {
public:
  void reset() { _n = 0; _troncato = false; }

  uint8_t          n()        const { return _n; }
  bool             troncato() const { return _troncato; }
  const CampoKV&   campo(uint8_t i) const { return _campi[i]; }

  // --- Scrittura -----------------------------------------------------------

  bool aggiungi(const char* chiave, const char* valore) {
    if (_n >= PROTO_MAX_CAMPI) { _troncato = true; return false; }
    strncpy(_campi[_n].chiave, chiave, PROTO_LEN_CHIAVE - 1);
    _campi[_n].chiave[PROTO_LEN_CHIAVE - 1] = '\0';
    strncpy(_campi[_n].valore, valore, PROTO_LEN_VALORE - 1);
    _campi[_n].valore[PROTO_LEN_VALORE - 1] = '\0';
    _n++;
    return true;
  }

  // I sensori assenti valgono NAN: in quel caso viene inviata la sentinella
  // -127, che Home Assistant riconosce come "non disponibile" e che soprattutto
  // e' JSON valido (a differenza di "nan", che romperebbe il parsing).
  bool aggiungiF(const char* chiave, float v, uint8_t decimali) {
    char buf[PROTO_LEN_VALORE];
    if (isnan(v) || isinf(v)) strncpy(buf, "-127", sizeof(buf));
    else                      snprintf(buf, sizeof(buf), "%.*f", (int)decimali, v);
    buf[sizeof(buf) - 1] = '\0';
    return aggiungi(chiave, buf);
  }

  bool aggiungiU(const char* chiave, uint32_t v) {
    char buf[PROTO_LEN_VALORE];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
    return aggiungi(chiave, buf);
  }

  bool aggiungiI(const char* chiave, int32_t v) {
    char buf[PROTO_LEN_VALORE];
    snprintf(buf, sizeof(buf), "%ld", (long)v);
    return aggiungi(chiave, buf);
  }

  // --- Lettura -------------------------------------------------------------

  const char* valore(const char* chiave) const {
    for (uint8_t i = 0; i < _n; i++)
      if (strcmp(_campi[i].chiave, chiave) == 0) return _campi[i].valore;
    return nullptr;
  }

  bool ha(const char* chiave) const { return valore(chiave) != nullptr; }

  float valoreF(const char* chiave, float def = NAN) const {
    const char* v = valore(chiave);
    return v ? atof(v) : def;
  }

  long valoreL(const char* chiave, long def = 0) const {
    const char* v = valore(chiave);
    return v ? atol(v) : def;
  }

  uint32_t valoreU(const char* chiave, uint32_t def = 0) const {
    const char* v = valore(chiave);
    return v ? (uint32_t)strtoul(v, nullptr, 10) : def;
  }

  // --- Serializzazione -----------------------------------------------------

  /*
   * Scrive "<prefisso>;k1=v1;k2=v2;..." in out.
   * Se il pacchetto supererebbe PROTO_MAX_PAYLOAD i campi in eccesso vengono
   * OMESSI e viene aggiunto "trunc=1": meglio un pacchetto valido e incompleto
   * che uno troncato a meta' e non parsabile. Per questo i campi vanno aggiunti
   * in ordine di importanza (prima trasporto e sensori critici, poi diagnostica).
   * Ritorna la lunghezza scritta.
   */
  size_t serializza(const char* prefisso, char* out, size_t maxOut) const {
    size_t pos = 0;
    out[0] = '\0';

    size_t lp = strlen(prefisso);
    if (lp + 1 >= maxOut) return 0;
    memcpy(out, prefisso, lp);
    pos = lp;
    out[pos] = '\0';   // senza questo, un pacchetto con zero campi non
                       // verrebbe terminato: il memcpy sovrascrive lo zero
                       // messo all'inizio e nessuna snprintf lo rimetterebbe.

    bool tagliato = false;
    for (uint8_t i = 0; i < _n; i++) {
      size_t need = 1 + strlen(_campi[i].chiave) + 1 + strlen(_campi[i].valore);
      // Riserva 10 byte per l'eventuale ";trunc=1" finale
      if (pos + need + 10 >= maxOut || pos + need + 10 >= PROTO_MAX_PAYLOAD) {
        tagliato = true;
        continue;
      }
      pos += snprintf(out + pos, maxOut - pos, "%c%s=%s",
                      PROTO_SEP, _campi[i].chiave, _campi[i].valore);
    }

    if (tagliato && pos + 10 < maxOut)
      pos += snprintf(out + pos, maxOut - pos, "%ctrunc=1", PROTO_SEP);

    return pos;
  }

  // --- Parsing -------------------------------------------------------------

  /*
   * Interpreta "<prefisso>;k=v;k=v;...". Il primo token (quello senza '=')
   * viene copiato in prefissoOut, se richiesto. Ritorna false solo se il testo
   * e' vuoto: un pacchetto senza coppie valide e' comunque "parsato" (0 campi).
   */
  bool parse(const char* testo, char* prefissoOut = nullptr, size_t maxPrefisso = 0) {
    reset();
    if (!testo || !*testo) return false;

    if (prefissoOut && maxPrefisso) prefissoOut[0] = '\0';

    const char* p = testo;
    bool primo = true;

    while (*p) {
      const char* fine = strchr(p, PROTO_SEP);
      size_t lung = fine ? (size_t)(fine - p) : strlen(p);

      if (lung > 0) {
        char token[PROTO_LEN_CHIAVE + PROTO_LEN_VALORE + 2];
        size_t copia = lung < sizeof(token) - 1 ? lung : sizeof(token) - 1;
        memcpy(token, p, copia);
        token[copia] = '\0';

        char* uguale = strchr(token, '=');
        if (uguale) {
          *uguale = '\0';
          aggiungi(token, uguale + 1);
        } else if (primo && prefissoOut && maxPrefisso) {
          strncpy(prefissoOut, token, maxPrefisso - 1);
          prefissoOut[maxPrefisso - 1] = '\0';
        }
      }

      primo = false;
      if (!fine) break;
      p = fine + 1;
    }
    return true;
  }

private:
  CampoKV _campi[PROTO_MAX_CAMPI];
  uint8_t _n = 0;
  bool    _troncato = false;
};

// ---------------------------------------------------------------------------
//  Riconoscimento della versione e compatibilita' con il v1
// ---------------------------------------------------------------------------

// Il v2 usa ';', il v1 usava ','. Basta guardare quale separatore compare.
inline bool protoEV2(const char* testo) {
  return testo && strchr(testo, PROTO_SEP) != nullptr;
}

/*
 * Converte un pacchetto v1 "GH1,seq,epoch,temp,hum,pres,luce,volt,flags"
 * nella rappresentazione v2. Serve durante l'aggiornamento: sulla microSD del
 * nodo possono esserci record accodati dalla versione precedente del firmware,
 * e vanno consegnati lo stesso invece di essere buttati.
 */
inline bool protoParseV1(const char* testo, PacchettoKV& out,
                         char* prefissoOut = nullptr, size_t maxPrefisso = 0) {
  static const char* CHIAVI_V1[] = {"s", "t", "temp", "hum", "pres", "luce", "volt", "flags"};
  const uint8_t N_V1 = sizeof(CHIAVI_V1) / sizeof(CHIAVI_V1[0]);

  out.reset();
  if (!testo || !*testo) return false;

  const char* p = testo;
  uint8_t idx = 0;   // 0 = prefisso (NODE_ID), poi le chiavi in ordine

  while (*p && idx <= N_V1) {
    const char* fine = strchr(p, ',');
    size_t lung = fine ? (size_t)(fine - p) : strlen(p);

    char token[PROTO_LEN_VALORE + 4];
    size_t copia = lung < sizeof(token) - 1 ? lung : sizeof(token) - 1;
    memcpy(token, p, copia);
    token[copia] = '\0';

    if (idx == 0) {
      if (prefissoOut && maxPrefisso) {
        strncpy(prefissoOut, token, maxPrefisso - 1);
        prefissoOut[maxPrefisso - 1] = '\0';
      }
      out.aggiungi("v", "1");
    } else {
      out.aggiungi(CHIAVI_V1[idx - 1], token);
    }

    idx++;
    if (!fine) break;
    p = fine + 1;
  }
  return idx > 1;
}

/*
 * Estrae il numero di sequenza da un pacchetto in QUALSIASI versione, senza
 * costruire un PacchettoKV. Il nodo la usa per riconoscere l'ACK dei record
 * di backlog, che possono essere v1 o v2.
 */
inline uint32_t protoEstraiSeq(const char* testo) {
  if (!testo) return 0;

  if (protoEV2(testo)) {
    const char* p = strstr(testo, ";s=");
    if (!p) return 0;
    return (uint32_t)strtoul(p + 3, nullptr, 10);
  }
  // v1: il seq e' il secondo campo separato da virgola
  const char* virgola = strchr(testo, ',');
  if (!virgola) return 0;
  return (uint32_t)strtoul(virgola + 1, nullptr, 10);
}
