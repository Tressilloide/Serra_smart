# -*- coding: utf-8 -*-
"""
Taratura dell'irrigazione adattiva sui dati veri di Home Assistant (sola
lettura). Rifa' i conti da cui vengono le costanti della sezione "NOTTE,
PREVISIONE E DURATA DELL'IRRIGAZIONE" di homeassistant/packages/serra.yaml.

USO, sul server:

    docker exec -i homeassistant python3 - < tools/calibra_irrigazione.py

COSA CALCOLA
------------
Per ogni giorno, dalle statistiche orarie del recorder (che durano mesi):

  notte    22:00-06:00: temperatura media e minima, umidita' media, e da
           queste la pressione di vapore (quanta acqua c'e' nell'aria)
  giorno   08:00-18:00: la "domanda" in kPa·h, somma oraria del deficit di
           pressione di vapore (VPD), e la massima

Il sensore sta in una scatola di plastica: di giorno la scatola al sole
gonfia temperatura e VPD, di notte e' in equilibrio con l'aria. Per questo
il modello prende dalla notte l'umidita' (la pressione di vapore) e usa il
giorno solo come forma: la domanda e' un indice relativo, tarato sulle
durate scelte a mano, non un'evapotraspirazione in millimetri.

Poi:
  1. adatta il modello fisico (curva della temperatura fra minima notturna e
     massima alle 13, pressione di vapore notturna moltiplicata per k) e
     dice quanto rifa' la domanda vera (R²);
  2. la regressione di ripiego, dalla sola notte (Tn, Un);
  3. la retta fra domanda media e durata scelta, sui periodi in cui la
     durata e' rimasta fissa (PERIODI qui sotto: aggiornali quando cambi la
     durata a mano e vuoi che il modello impari dalla scelta nuova).
"""

import datetime as dt
import math
import sqlite3
import statistics as st
import sys

try:
    from zoneinfo import ZoneInfo
    FUSO = ZoneInfo("Europe/Rome")
except Exception:
    FUSO = dt.timezone.utc

DB = sys.argv[1] if len(sys.argv) > 1 else "/config/home-assistant_v2.db"

# Periodi a durata fissa scelta a mano: (dal, al, secondi). Dalle
# irrigazioni registrate: 4,4 L al giorno fino al 10/10 (120 s), 3,3 L fino
# al 21/10 (90 s), 2,2 L dopo (50 s).
PERIODI = [
    ("2026-09-25", "2026-10-10", 120),
    ("2026-10-11", "2026-10-21", 90),
    ("2026-10-22", "2026-11-10", 50),
]

con = sqlite3.connect("file:%s?mode=ro" % DB, uri=True)


def orarie(*entita):
    h = {}
    for e in entita:
        for ts, mn, me, mx in con.execute(
                "SELECT st.start_ts, st.min, st.mean, st.max FROM statistics st "
                "JOIN statistics_meta sm ON st.metadata_id = sm.id "
                "WHERE sm.statistic_id = ? ORDER BY st.start_ts", (e,)):
            if me is not None:
                h[int(ts)] = (mn, me, mx)
    return h


def es(t):
    return 0.6108 * math.exp(17.27 * t / (t + 237.3))


# Le entita' "serra_serra_*" sono quelle del primo firmware, fino al 1/09.
T = orarie("sensor.serra_serra_temperatura", "sensor.serra_temperatura")
U = orarie("sensor.serra_serra_umidita", "sensor.serra_umidita_aria")

per_giorno = {}
for ts in T:
    if ts not in U:
        continue
    x = dt.datetime.fromtimestamp(ts, FUSO)
    t, u = T[ts], U[ts]
    if x.hour >= 22:
        per_giorno.setdefault(x.date() + dt.timedelta(days=1), {"n": [], "g": []})["n"].append((t, u))
    elif x.hour < 6:
        per_giorno.setdefault(x.date(), {"n": [], "g": []})["n"].append((t, u))
    elif 8 <= x.hour < 18:
        per_giorno.setdefault(x.date(), {"n": [], "g": []})["g"].append((t, u))

giorni = []
for g, v in sorted(per_giorno.items()):
    if len(v["n"]) < 6 or len(v["g"]) < 8:
        continue
    tmax = max(t[2] for t, _ in v["g"])
    if tmax > 80:                      # letture I2C impazzite (es. 180 °C il 20/09)
        continue
    tn = st.mean(t[1] for t, _ in v["n"])
    un = st.mean(min(u[1], 100) for _, u in v["n"])
    giorni.append(dict(
        g=g, tn=tn, un=un, tmin=min(t[0] for t, _ in v["n"]), tmax=tmax,
        en=st.mean(es(t[1]) * min(u[1], 100) / 100 for t, u in v["n"]),
        d=sum(es(t[1]) * (1 - min(u[1], 100) / 100) for t, u in v["g"])))

if len(giorni) < 15:
    sys.exit("Troppo pochi giorni completi (%d) per una taratura." % len(giorni))


def modello(r, tmax, k, picco, larg, scala):
    d = 0.0
    for h in range(8, 18):
        f = max(0.0, math.cos(math.pi * (h + 0.5 - picco) / (2 * larg)))
        th = r["tmin"] + (tmax - r["tmin"]) * f
        d += max(0.0, es(th) - k * r["en"])
    return scala * d


def r2(veri, previsti):
    m = st.mean(veri)
    return 1 - sum((a - b) ** 2 for a, b in zip(veri, previsti)) / sum((a - m) ** 2 for a in veri)


veri = [r["d"] for r in giorni]
migliore = None
for k in [x / 10 for x in range(10, 31)]:
    for larg in (2, 2.5, 3, 3.5, 4, 5, 6):
        for picco in (12.5, 13.0, 13.5, 14.0):
            for scala in (0.7, 0.8, 0.9, 1.0, 1.1):
                q = r2(veri, [modello(r, r["tmax"], k, picco, larg, scala) for r in giorni])
                if migliore is None or q > migliore[0]:
                    migliore = (q, k, picco, larg, scala)

q, k, picco, larg, scala = migliore
print("Giorni completi: %d (%s - %s)" % (len(giorni), giorni[0]["g"], giorni[-1]["g"]))
print()
print("1. Modello fisico con la massima vera:  R² = %.2f" % q)
print("   k = %.1f  (pressione di vapore di giorno / di notte)" % k)
print("   massima alle %.1f, mezza larghezza della campana %g h (nel package: cos(pi*(h+0.5-%g)/%g))"
      % (picco, larg, picco, 2 * larg))
print("   scala = %.1f" % scala)

# Ripiego dalla sola notte: d = a + b*Tn + c*Un, minimi quadrati
X = [(1.0, r["tn"], r["un"]) for r in giorni]
A = [[sum(x[i] * x[j] for x in X) for j in range(3)] for i in range(3)]
B = [sum(x[i] * y for x, y in zip(X, veri)) for i in range(3)]
M = [A[i] + [B[i]] for i in range(3)]
for c in range(3):
    p = max(range(c, 3), key=lambda r: abs(M[r][c]))
    M[c], M[p] = M[p], M[c]
    for r in range(3):
        if r != c:
            f = M[r][c] / M[c][c]
            M[r] = [a - f * b for a, b in zip(M[r], M[c])]
coef = [M[i][3] / M[i][i] for i in range(3)]
print()
print("2. Ripiego dalla sola notte: domanda = %.1f %+.3f*Tn %+.2f*Un   R² = %.2f"
      % (coef[0], coef[1], coef[2], r2(veri, [coef[0] + coef[1] * r["tn"] + coef[2] * r["un"] for r in giorni])))

punti = []
print()
print("3. Durate scelte a mano")
for dal, al, sec in PERIODI:
    sel = [r["d"] for r in giorni if dal <= str(r["g"]) <= al]
    if sel:
        punti.append((st.mean(sel), sec))
        print("   %s - %s  %3d s   domanda media %.1f kPa·h (%d giorni)" % (dal, al, sec, st.mean(sel), len(sel)))
if len(punti) >= 2:
    mx, my = st.mean(p[0] for p in punti), st.mean(p[1] for p in punti)
    b = sum((x - mx) * (y - my) for x, y in punti) / sum((x - mx) ** 2 for x, _ in punti)
    a = my - b * mx
    print("   retta: durata = %.1f s + %.2f s per kPa·h" % (a, b))
    rif = punti[-1][0]
    print("   riferimento per la durata base: %.1f kPa·h (ultimo periodo)" % rif)
