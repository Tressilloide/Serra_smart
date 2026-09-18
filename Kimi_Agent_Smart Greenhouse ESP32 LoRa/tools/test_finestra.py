# -*- coding: utf-8 -*-
"""
Modello dell'aritmetica della finestra oraria di irrigazioneValuta().

Non e' il codice compilato: e' la trascrizione delle stesse espressioni, e
serve a verificare i due punti che possono essere sottilmente sbagliati, cioe'
il giro delle 24 ore e la tolleranza sul risveglio anticipato.
Le espressioni vengono ricontrollate contro il sorgente prima di girare.

    python tools/test_finestra.py
"""
import io, os, re, sys

QUI      = os.path.dirname(os.path.abspath(__file__))
RADICE   = os.path.dirname(QUI)
SORGENTE = os.path.join(RADICE, "serra_nodo", "irrigazione.cpp")
CONFIG   = os.path.join(RADICE, "serra_nodo", "config.h")

src = io.open(SORGENTE, encoding="utf-8").read()
cfg = io.open(CONFIG,   encoding="utf-8").read()

# --- le righe che il modello riproduce devono esistere davvero nel sorgente ---
ATTESE = [
    "uint32_t daSched = (secOra + 86400UL - secSched) % 86400UL;",
    "if (daSched > 86400UL - (uint32_t)IRRIG_ANTICIPO_SEC) daSched = 0;",
    "uint32_t finestraSec = g_cfg.sleepSec + 60UL + (uint32_t)IRRIG_RECUPERO_SEC;",
    "if (daSched >= finestraSec) return IRR_NO_ORARIO;",
]
for r in ATTESE:
    if r not in src:
        print("DISALLINEATO, il sorgente non contiene:", r); sys.exit(1)
print("Le espressioni del modello coincidono con serra_nodo/irrigazione.cpp")

ANTICIPO = int(re.search(r"#define IRRIG_ANTICIPO_SEC\s+(\d+)", cfg).group(1))
RECUPERO = int(re.search(r"#define IRRIG_RECUPERO_SEC\s+(\d+)", cfg).group(1))
print("IRRIG_ANTICIPO_SEC=%d  IRRIG_RECUPERO_SEC=%d\n" % (ANTICIPO, RECUPERO))


def dentro_finestra(hms, sched_hm, sleep_sec=900):
    h, m, s = hms
    secOra   = h * 3600 + m * 60 + s
    secSched = (sched_hm[0] * 3600 + sched_hm[1] * 60) % 86400
    daSched  = (secOra + 86400 - secSched) % 86400
    if daSched > 86400 - ANTICIPO:
        daSched = 0
    finestra = min(sleep_sec + 60 + RECUPERO, 86400)
    return daSched < finestra, daSched


def vecchio(hms, sched_hm, sleep_sec=900):
    """La logica precedente, per confronto."""
    h, m, _ = hms
    minutiOra   = h * 60 + m
    minutiSched = sched_hm[0] * 60 + sched_hm[1]
    finestraMin = min(sleep_sec // 60 + 1, 1440)
    daSched     = (minutiOra + 1440 - minutiSched % 1440) % 1440
    return daSched < finestraMin


CASI = [
    # (descrizione,                         risveglio,     orario,  atteso)
    ("risveglio 4 s PRIMA delle 6:00",      (5, 59, 56),   (6, 0),  True),
    ("risveglio in ritardo di 16 min",      (6, 16, 58),   (6, 0),  True),
    ("risveglio in ritardo di 2 min",       (6,  2,  0),   (6, 0),  True),
    ("risveglio in ritardo di 59 min",      (6, 59,  0),   (6, 0),  True),
    ("risveglio in ritardo di 1h17",        (7, 17,  0),   (6, 0),  False),
    ("risveglio 10 min PRIMA (troppo)",     (5, 50,  0),   (6, 0),  False),
    ("meta' pomeriggio, niente a che fare", (15, 0,  0),   (6, 0),  False),
    ("mezzanotte, orario 23:50, 4 s prima", (23, 49, 56),  (23, 50), True),
    ("orario 23:50, recupero alle 00:05",   (0,  5,  0),   (23, 50), True),
    ("orario 23:50, alle 02:00 e' tardi",   (2,  0,  0),   (23, 50), False),
    ("orario 00:00, 4 s prima (23:59:56)",  (23, 59, 56),  (0, 0),  True),
]

print("%-38s %-10s %-8s %-8s %s" % ("caso", "daSched", "nuovo", "atteso", "vecchio"))
print("-" * 82)
errori = 0
for desc, risveglio, sched, atteso in CASI:
    ok, da = dentro_finestra(risveglio, sched)
    old = vecchio(risveglio, sched)
    esito = "IRRIGA" if ok else "no"
    if ok != atteso:
        errori += 1
        esito += "  <<< SBAGLIATO"
    print("%-38s %-10s %-8s %-8s %s" % (
        desc, "%d s" % da, esito, "IRRIGA" if atteso else "no",
        "IRRIGA" if old else "no"))

print()
if errori:
    print("%d casi falliti" % errori); sys.exit(1)
print("Tutti i casi passano.")
