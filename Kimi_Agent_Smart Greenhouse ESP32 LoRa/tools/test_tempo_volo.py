# -*- coding: utf-8 -*-
"""
Verifica del tempo di volo LoRa calcolato dal nodo.

tempoDiVoloMs() in serra_nodo/radio.cpp decide da sola quanto aspettare prima
di passare all'ascolto: se sbagliasse per difetto il nodo troncherebbe i propri
pacchetti, e sarebbe un guasto difficile da vedere (sembrerebbero solo ACK
mancanti). Qui la formula viene ricalcolata in modo indipendente, partendo dal
datasheet SX1276/78 paragrafo 4.1.1.7, e confrontata con quella del firmware.

Il test controlla anche che le espressioni cercate siano ancora presenti nel
sorgente: se qualcuno cambia radio.cpp senza aggiornare questo file, il test
fallisce invece di continuare a promuovere una formula che non esiste piu'.

    python tools/test_tempo_volo.py
"""

import io
import math
import os
import re
import sys

QUI = os.path.dirname(os.path.abspath(__file__))
RADICE = os.path.dirname(QUI)
RADIO = os.path.join(RADICE, "serra_nodo", "radio.cpp")
CONFIG = os.path.join(RADICE, "serra_nodo", "config.h")


def leggi(percorso):
    return io.open(percorso, encoding="utf-8").read()


def costante(sorgente, nome):
    m = re.search(r"#define\s+%s\s+([0-9.E+]+)" % nome, sorgente)
    if not m:
        raise SystemExit("costante %s non trovata in config.h" % nome)
    return float(m.group(1))


# --- il sorgente deve ancora contenere la formula che stiamo verificando -----

ATTESE = [
    "const float tSimbolo = (float)(1UL << LORA_SF) / (float)LORA_BW;",
    "int32_t numeratore  = 8 * (int32_t)lunghezza - 4 * LORA_SF + 28 + 16;",
    "int32_t denominatore = 4 * LORA_SF;",
    "simboli += ((numeratore + denominatore - 1) / denominatore) * LORA_CR;",
    "const float preambolo = (8.0f + 4.25f) * tSimbolo;",
    "return (uint32_t)((preambolo + simboli * tSimbolo) * 1000.0f) + 1;",
    "LoRa.endPacket(true);",
    "if (LoRa.beginPacket() == 1) { finita = true; break; }",
    "TX_GUARDIA_X",
]

sorgente = leggi(RADIO)
mancanti = [a for a in ATTESE if a not in sorgente]
if mancanti:
    print("radio.cpp non contiene piu' le espressioni verificate da questo test:")
    for a in mancanti:
        print("  -", a)
    raise SystemExit(1)

cfg = leggi(CONFIG)
SF = int(costante(cfg, "LORA_SF"))
BW = costante(cfg, "LORA_BW")
CR = int(costante(cfg, "LORA_CR"))       # 5 = coding rate 4/5, cioe' (CR + 4)
GUARDIA_X  = int(costante(cfg, "TX_GUARDIA_X"))
GUARDIA_MS = int(costante(cfg, "TX_GUARDIA_MS"))
MAX_PAYLOAD = int(re.search(r"#define\s+PROTO_MAX_PAYLOAD\s+(\d+)",
                            leggi(os.path.join(RADICE, "serra_nodo", "protocollo.h"))).group(1))


def firmware(lunghezza):
    """Rifa' passo per passo quello che fa tempoDiVoloMs() in radio.cpp."""
    t_simbolo = float(1 << SF) / BW
    numeratore = 8 * lunghezza - 4 * SF + 28 + 16
    denominatore = 4 * SF
    simboli = 8
    if numeratore > 0:
        simboli += ((numeratore + denominatore - 1) // denominatore) * CR
    preambolo = (8.0 + 4.25) * t_simbolo
    return int((preambolo + simboli * t_simbolo) * 1000.0) + 1


def datasheet(lunghezza):
    """Formula del datasheet, scritta in modo indipendente (float e ceil)."""
    t_simbolo = (2.0 ** SF) / BW
    t_preambolo = (8 + 4.25) * t_simbolo
    grezzo = (8.0 * lunghezza - 4.0 * SF + 28 + 16) / (4.0 * SF)
    n_payload = 8 + max(math.ceil(grezzo) * CR, 0)
    return t_preambolo + n_payload * t_simbolo


falliti = 0
print("SF%d  BW %.0f kHz  CR 4/%d  guardia = tempo di volo x%d + %d ms"
      % (SF, BW / 1000.0, CR, GUARDIA_X, GUARDIA_MS))
print()
print(" byte   datasheet   firmware   guardia   verdetto")

for lunghezza in (20, 60, 100, 150, 200, MAX_PAYLOAD):
    vero_ms = datasheet(lunghezza) * 1000.0
    calcolato = firmware(lunghezza)
    guardia = calcolato * GUARDIA_X + GUARDIA_MS

    # Il calcolo non deve mai sottostimare il tempo di volo reale...
    ok = (calcolato >= vero_ms - 0.001) and (calcolato <= vero_ms + 2.0)
    # ...la guardia deve lasciare largo spazio a una trasmissione onesta...
    ok = ok and guardia > vero_ms * 2
    # ...e deve restare molto sotto il watchdog di setup(), altrimenti tre
    # tentativi falliti basterebbero comunque a far riavviare il nodo.
    ok = ok and guardia * 3 < costante(cfg, "WDT_SETUP_SEC") * 1000 / 2

    if not ok:
        falliti += 1
    print("  %3d   %7.1f ms   %5d ms   %5d ms   %s"
          % (lunghezza, vero_ms, calcolato, guardia, "ok" if ok else "FALLITO"))

print()
if falliti:
    print("%d casi falliti." % falliti)
    sys.exit(1)
print("Tutti i casi passano: la guardia e' sempre larga e sempre finita.")
