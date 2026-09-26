# -*- coding: utf-8 -*-
"""
Verifica dei record arretrati (serra_nodo.ino: accodaNonConsegnato, marcaArretrato).

Dal firmware 2.5.0 un pacchetto non consegnato va nel backlog con in testa il
marcatore ";bk=1". E' cosi' che il ponte lo riconosce e lo pubblica su
serra/nodo/storico invece che sullo stato attuale: prima lo giudicava solo
dall'eta', e un record vecchio di 15 minuti sovrascriveva in Home Assistant il
pacchetto vero arrivato un attimo prima.

Sono cinque byte, ma il pacchetto ha un tetto rigido (PROTO_MAX_PAYLOAD) e con
"txp" e la scatola nera i margini sono stretti. Questo test ricalca in Python
la serializzazione di protocollo.h e controlla che:

  1. il pacchetto principale piu' lungo che il nodo possa produrre davvero
     passi intero, senza campi omessi, sia in trasmissione diretta sia una
     volta accodato con il marcatore;
  2. lo stesso valga per il pacchetto di esito di un comando;
  3. un pacchetto oltre ogni limite, accodato, resti sotto il tetto e
     conservi marcatore e numero di sequenza;
  4. il ponte riconosca il marcatore e non lo trasformi in un'entita'.

    python tools/test_arretrati.py
"""

import io
import os
import re
import sys

QUI = os.path.dirname(os.path.abspath(__file__))
RADICE = os.path.dirname(QUI)


def leggi(*parti):
    return io.open(os.path.join(RADICE, *parti), encoding="utf-8").read()


protocollo = leggi("serra_nodo", "protocollo.h")
ino = leggi("serra_nodo", "serra_nodo.ino")
config_nodo = leggi("serra_nodo", "config.h")
irrigazione = leggi("serra_nodo", "irrigazione.cpp")
traccia = leggi("serra_nodo", "traccia.cpp")
ponte = leggi("camera_ponte", "camera_ponte.ino")
discovery = leggi("camera_ponte", "discovery.cpp")


def costante(testo, nome):
    m = re.search(r"#define\s+%s\s+(\S+)" % nome, testo)
    if not m:
        raise SystemExit("%s non trovata" % nome)
    return m.group(1).strip('"')


MAX_PAYLOAD = int(costante(protocollo, "PROTO_MAX_PAYLOAD"))
LEN_CHIAVE = int(costante(protocollo, "PROTO_LEN_CHIAVE"))
LEN_VALORE = int(costante(protocollo, "PROTO_LEN_VALORE"))
NODE_ID = costante(config_nodo, "NODE_ID")
FW = costante(config_nodo, "FW_VERSION")
TX_RETRIES = int(costante(config_nodo, "TX_RETRIES"))

m = re.search(r'MARCA_ARRETRATO\[\]\s*=\s*"([^"]*)"', ino)
if not m:
    raise SystemExit("MARCA_ARRETRATO non trovata in serra_nodo.ino")
MARCA = m.group(1)

errori = []

# La riga accodata deve essere serializzata lasciando libero il posto del
# marcatore: e' la sola cosa che garantisce di restare sotto il tetto.
if not re.search(r"serializza\(\s*NODE_ID\s*,\s*buf\s*,\s*PROTO_MAX_PAYLOAD\s*\+\s*1\s*-\s*LUNG_MARCA\s*\)", ino):
    errori.append("accodaNonConsegnato() non riserva lo spazio del marcatore nella "
                  "serializzazione: la riga accodata potrebbe superare il tetto")


# --- Ricalco di PacchettoKV::serializza() (protocollo.h) --------------------

def serializza(prefisso, campi, max_out):
    if len(prefisso) + 1 >= max_out:
        return "", []
    out, pos, omessi, tagliato = prefisso, len(prefisso), [], False
    for chiave, valore in campi:
        chiave, valore = chiave[:LEN_CHIAVE - 1], valore[:LEN_VALORE - 1]
        need = 1 + len(chiave) + 1 + len(valore)
        # Riserva 10 byte per l'eventuale ";trunc=1" finale
        if pos + need + 10 >= max_out or pos + need + 10 >= MAX_PAYLOAD:
            tagliato = True
            omessi.append(chiave)
            continue
        out += ";%s=%s" % (chiave, valore)
        pos += need
    if tagliato and pos + 10 < max_out:
        out += ";trunc=1"
    return out, omessi


def marca(riga):                     # ricalco di marcaArretrato()
    i = riga.find(";")
    return riga if i < 0 else riga[:i] + MARCA + riga[i:]


def diretto(campi):                  # inviaPacchetto(): buf[PROTO_MAX_PAYLOAD + 8]
    return serializza(NODE_ID, campi, MAX_PAYLOAD + 8)


def accodato(campi):                 # accodaNonConsegnato()
    riga, omessi = serializza(NODE_ID, campi, MAX_PAYLOAD + 1 - len(MARCA))
    return marca(riga), omessi


def estrai_seq(riga):                # ricalco di protoEstraiSeq()
    m = re.search(r";s=(\d+)", riga)
    return int(m.group(1)) if m else 0


def chiavi_di(riga):
    return [t.split("=", 1)[0] for t in riga.split(";")[1:] if "=" in t]


# --- I valori piu' lunghi che il nodo puo' produrre davvero ----------------

testi_esito = re.findall(r'case\s+IRR_\w+:\s*return\s+"([^"]+)";', irrigazione)
testi_tappa = re.findall(r'case\s+TAPPA_\w+:\s*return\s+"([^"]+)";', traccia)
esito_orologio_ok = max((t for t in testi_esito if t != "ora_non_attendibile"), key=len)
tappa_lunga = max(testi_tappa, key=len)

# Ipotesi, volutamente larghe:
#   s        6 cifre: 999999 pacchetti sono 28 anni a uno ogni 15 minuti
#   acqua    36 L: il tetto di durata (900 s) a 2,4 L/min
#   acquaTot 13 140 L: dieci anni a 3,6 L al giorno
#   bl       2000 record: il tetto del backlog (BACKLOG_MAX_BYTE)
#   irr      l'esito piu' lungo compatibile con un orologio valido: con
#            "ora_non_attendibile" il nodo manda t=0, che e' piu' corto
PRINCIPALE = [
    ("v", "2"), ("s", "999999"), ("t", "1790109896"),
    ("temp", "-12.34"), ("hum", "100.0"), ("pres", "1013.2"),
    ("volt", "14.64"), ("luce", "100.0"),
    ("acqua", "36.000"), ("acquaTot", "13140.00"),
    ("sAuto", "1"), ("sOra", "23"), ("sMin", "59"), ("sDur", "900"),
    ("sSoil", "100"), ("slp", "3600"),
    ("irr", esito_orologio_ok), ("bl", "2000"),
    ("fw", FW), ("rst", "15"), ("tp", tappa_lunga),
    ("txp", str(TX_RETRIES + 1)),
]

ESITO = [
    ("v", "2"), ("s", "999999"), ("t", "1790109896"),
    ("res", "65535"), ("rc", "5"), ("det", "x" * (LEN_VALORE - 1)),
    ("cmdL", "36.000"), ("cmdS", "900"),
    ("acqua", "36.000"), ("acquaTot", "13140.00"),
    ("sAuto", "1"), ("sOra", "23"), ("sMin", "59"), ("sDur", "900"),
    ("sSoil", "100"), ("slp", "3600"),
    ("irr", esito_orologio_ok), ("bl", "2000"),
]

# Ogni chiave usata qui deve esistere davvero nei sorgenti del nodo, o il test
# starebbe misurando un pacchetto che non esiste piu'.
sorgenti_nodo = ino + leggi("serra_nodo", "sensori.cpp") + leggi("serra_nodo", "comandi.cpp")
for chiave, _ in PRINCIPALE + ESITO:
    if chiave in ("v", "s", "t", "hum", "pres", "volt", "luce"):
        continue                      # intestazione e tabella dei sensori
    if '"%s"' % chiave not in sorgenti_nodo:
        errori.append("la chiave %r del test non compare nei sorgenti del nodo" % chiave)

print("tetto del pacchetto: %d byte, marcatore %r" % (MAX_PAYLOAD, MARCA))
print()
print("%-34s %8s %8s  %s" % ("caso", "diretto", "accodato", "campi omessi"))
print("-" * 70)

for nome, campi in (("pacchetto principale (crash + txp)", PRINCIPALE),
                    ("esito di un comando", ESITO)):
    d, omessi_d = diretto(campi)
    a, omessi_a = accodato(campi)
    print("%-34s %8d %8d  %s" % (nome, len(d), len(a), ", ".join(omessi_d + omessi_a) or "-"))
    if omessi_d:
        errori.append("%s: in trasmissione diretta verrebbero omessi %s" % (nome, omessi_d))
    if omessi_a:
        errori.append("%s: una volta accodato verrebbero omessi %s" % (nome, omessi_a))
    if len(a) > MAX_PAYLOAD:
        errori.append("%s: la riga accodata supera il tetto (%d byte)" % (nome, len(a)))
    if not a.startswith(NODE_ID + MARCA + ";"):
        errori.append("%s: il marcatore non e' subito dopo il prefisso" % nome)
    if estrai_seq(a) != 999999:
        errori.append("%s: protoEstraiSeq non ritrova la sequenza nella riga accodata" % nome)
    if chiavi_di(a)[1:] != chiavi_di(d):
        errori.append("%s: la riga accodata non ha gli stessi campi del pacchetto" % nome)

# Un pacchetto oltre ogni limite: 28 campi pieni. Deve uscire troncato ma
# valido, sotto il tetto, con marcatore e sequenza.
ENORME = [("v", "2"), ("s", "123456")] + [("k%02d" % i, "9" * (LEN_VALORE - 1)) for i in range(26)]
a, omessi = accodato(ENORME)
print("%-34s %8s %8d  %d campi omessi (atteso)" % ("pacchetto oltre ogni limite", "-", len(a), len(omessi)))
if len(a) > MAX_PAYLOAD:
    errori.append("pacchetto enorme: la riga accodata supera il tetto (%d byte)" % len(a))
if not a.startswith(NODE_ID + MARCA + ";") or estrai_seq(a) != 123456 or "trunc=1" not in a:
    errori.append("pacchetto enorme: la riga accodata ha perso marcatore, sequenza o trunc=1")

# --- Il ponte deve riconoscere il marcatore ---------------------------------

chiave_marca = MARCA.lstrip(";").split("=")[0]
if not re.search(r'pkt\.ha\(\s*"%s"\s*\)' % chiave_marca, ponte):
    errori.append("il ponte non guarda %r per decidere se un record e' storico" % chiave_marca)
ignora = re.search(r"IGNORA\[\]\s*=\s*\{(.*?)\};", discovery, re.S)
if not ignora or '"%s"' % chiave_marca not in ignora.group(1):
    errori.append("%r non e' fra le chiavi IGNORA della discovery: diventerebbe "
                  "un'entita' in Home Assistant" % chiave_marca)
# --- Nodi senza marcatore (fino alla 2.4.0): la regola del confronto --------
# Il ponte 2.4.0 puo' lavorare con un nodo vecchio. Ricalco della decisione
# in gestisciLoRa(), su una sequenza realistica di un risveglio.

config_ponte = leggi("camera_ponte", "config.h")
SOGLIA = int(costante(config_ponte, "SOGLIA_STORICO_SEC").rstrip("UL"))
MARGINE = int(costante(config_ponte, "STORICO_MARGINE_SEC").rstrip("UL"))
PLAUSIBILE = int(costante(config_ponte, "STORICO_PLAUSIBILE_SEC").rstrip("UL"))
if not re.search(r"ts\s*\+\s*STORICO_MARGINE_SEC\s*<\s*ultimoTsFresco", ponte):
    errori.append("il ponte non confronta i record con l'ultimo pacchetto fresco")

rif = {"ts": 0}


def classifica(ts, adesso, bk=False):
    storico = bk or (ts > 0 and adesso > ts and adesso - ts > SOGLIA) or \
        (ts > 0 and rif["ts"] > 0 and ts + MARGINE < rif["ts"])
    if not storico and ts > rif["ts"] and abs(ts - adesso) <= PLAUSIBILE:
        rif["ts"] = ts
    return "storico" if storico else "fresco"


T = 1790200000          # ora del ponte; l'orologio del nodo e' 25 s avanti
casi = [
    ("pacchetto fresco",                         T + 25,        T,        "fresco"),
    ("record del ciclo prima, senza bk",         T + 25 - 900,  T + 1,    "storico"),
    ("esito di un comando dopo la correzione",   T - 6 + 30,    T + 30,   "fresco"),
    ("pacchetto fresco del risveglio dopo",      T + 900,       T + 900,  "fresco"),
    ("timestamp impazzito (DS1307 nel 2099)",    4070908800,    T + 1800, "fresco"),
    ("fresco dopo il timestamp impazzito",       T + 2700,      T + 2700, "fresco"),
    ("record arretrato con bk=1",                T + 1800,      T + 2701, "storico"),
]
print()
for nome, ts, adesso, atteso in casi[:6]:
    esito = classifica(ts, adesso)
    print("%-44s %-8s %s" % (nome, esito, "ok" if esito == atteso else "SBAGLIATO"))
    if esito != atteso:
        errori.append("regola dello storico: %s classificato %s" % (nome, esito))
nome, ts, adesso, atteso = casi[6]
if classifica(ts, adesso, bk=True) != atteso:
    errori.append("regola dello storico: il marcatore bk=1 non basta")

m = re.search(r"char\s+buf\[\s*PROTO_MAX_PAYLOAD\s*\+\s*1\s*\]", ponte)
if not m:
    errori.append("il buffer di ricezione del ponte non e' PROTO_MAX_PAYLOAD + 1: "
                  "controlla che una riga accodata ci stia intera")

print()
if errori:
    for e in errori:
        print("FALLITO:", e)
    sys.exit(1)

print("Tutti i casi passano: i record arretrati restano interi, sotto il tetto")
print("e riconoscibili dal ponte.")
