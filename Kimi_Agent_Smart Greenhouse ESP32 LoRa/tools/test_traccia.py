# -*- coding: utf-8 -*-
"""
Verifica della scatola nera del nodo (serra_nodo/traccia.*).

La scatola nera serve a una cosa sola: dopo un riavvio anomalo, dire in che
punto del ciclo il nodo si era fermato. Basta poco per renderla inutile senza
accorgersene — una tappa dichiarata e mai segnata, un nome piu' lungo di quanto
il protocollo trasporti, oppure i contatori rimessi in memoria RTC
inizializzata, che il bootloader riazzera proprio nei casi che interessano.

Questo test controlla quelle quattro cose sul sorgente.

    python tools/test_traccia.py
"""

import io
import os
import re
import sys

QUI = os.path.dirname(os.path.abspath(__file__))
RADICE = os.path.dirname(QUI)
NODO = os.path.join(RADICE, "serra_nodo")


def leggi(*parti):
    return io.open(os.path.join(NODO, *parti), encoding="utf-8").read()


intestazione = leggi("traccia.h")
implementazione = leggi("traccia.cpp")
sorgenti = "\n".join(leggi(f) for f in os.listdir(NODO)
                     if f.endswith((".ino", ".cpp")) and f != "traccia.cpp")

errori = []

# --- 1. ogni tappa dichiarata deve avere un nome in chiaro -------------------

blocco = re.search(r"enum Tappa\s*:\s*uint8_t\s*\{(.*?)\};", intestazione, re.S)
if not blocco:
    raise SystemExit("enum Tappa non trovato in traccia.h")

tappe = re.findall(r"^\s*(TAPPA_[A-Z_]+)", blocco.group(1), re.M)
if len(tappe) < 5:
    raise SystemExit("enum Tappa sospettosamente corto: %r" % tappe)

testi = dict(re.findall(r'case\s+(TAPPA_[A-Z_]+):\s*return\s+"([^"]*)";', implementazione))

for t in tappe:
    if t == "TAPPA_IGNOTA":
        continue                      # gestita dal ramo default
    if t not in testi:
        errori.append("%s non ha un nome in tracciaTesto()" % t)

# --- 2. i nomi devono stare nel campo del protocollo ------------------------

m = re.search(r"#define\s+PROTO_LEN_VALORE\s+(\d+)", leggi("protocollo.h"))
maxlen = int(m.group(1)) - 1 if m else 19
for t, testo in sorted(testi.items()):
    if len(testo) > maxlen:
        errori.append("il nome %r di %s supera i %d caratteri utili" % (testo, t, maxlen))

# --- 3. ogni tappa deve essere davvero segnata da qualche parte -------------
# Una tappa mai passata a traccia() e' un'etichetta che non comparira' mai:
# peggio di non averla, perche' leggendo l'elenco si crede di poterla vedere.

segnate = set(re.findall(r"traccia\(\s*(TAPPA_[A-Z_]+)\s*\)", sorgenti))
for t in tappe:
    if t in ("TAPPA_IGNOTA", "TAPPA_AVVIO"):
        continue                      # le imposta tracciaInit()
    if t not in segnate:
        errori.append("%s e' dichiarata ma non viene mai segnata con traccia()" % t)

# --- 4. i contatori devono sopravvivere al watchdog -------------------------
# RTC_DATA_ATTR ha un inizializzatore, e il bootloader lo riapplica a ogni
# avvio che non sia il risveglio dal deep sleep: esattamente i riavvii che
# vogliamo poter contare. Solo RTC_NOINIT_ATTR sopravvive.

ino = leggi("serra_nodo.ino")
for variabile in ("g_seq", "g_risvegli"):
    riga = re.search(r"^.*\b%s\b\s*[;=].*$" % variabile, ino, re.M)
    if not riga:
        errori.append("%s non trovata in serra_nodo.ino" % variabile)
    elif "RTC_NOINIT_ATTR" not in riga.group(0):
        errori.append("%s non e' in RTC_NOINIT_ATTR: si azzera a ogni riavvio anomalo"
                      % variabile)

if "if (tracciaMemoriaPersa()) { g_seq = 0; g_risvegli = 0; }" not in ino:
    errori.append("i contatori in RTC_NOINIT_ATTR non vengono azzerati all'accensione: "
                  "al primo avvio conterrebbero spazzatura")

# --- 5. la tappa deve finire nel pacchetto ----------------------------------

if 'pkt.aggiungi("tp", tracciaTesto(tappaPrec));' not in ino:
    errori.append("il campo \"tp\" non viene aggiunto al pacchetto: la scatola nera "
                  "resterebbe leggibile solo dal monitor seriale")

# --- esito ------------------------------------------------------------------

print("tappe dichiarate: %d" % len(tappe))
piu_lungo = max(testi.values(), key=len)
print("nome piu' lungo:  %r (%d caratteri, limite %d)"
      % (piu_lungo, len(piu_lungo), maxlen))
print()

if errori:
    for e in errori:
        print("FALLITO:", e)
    sys.exit(1)

print("Tutte le tappe sono dichiarate, nominate, segnate e trasmesse.")
