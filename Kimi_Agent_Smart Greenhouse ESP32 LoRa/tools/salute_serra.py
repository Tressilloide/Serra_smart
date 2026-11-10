# -*- coding: utf-8 -*-
"""
Rapporto sulla salute della serra, giorno per giorno, letto dal database di
Home Assistant (sola lettura).

PERCHE' SERVE
-------------
Le domande che contano davvero -- quanti cicli arrivano, quanto e' lungo il
buco peggiore, come va il segnale, quanta acqua e' scesa -- finora si
risolvevano con query scritte a mano ogni volta. E' cosi' che il 10/11/2026
si e' scoperto che negli ultimi dieci giorni era arrivato solo l'87% dei
cicli. Questo script fa lo stesso conto in un colpo.

USO
---
Sul server, dentro il container di Home Assistant (che ha gia' il database
e Python):

    docker exec -i homeassistant python3 - < tools/salute_serra.py
    docker exec -i homeassistant python3 - --giorni 30 < tools/salute_serra.py

Oppure su una copia del database:

    python tools/salute_serra.py --db home-assistant_v2.db --giorni 7

COSA LEGGE
----------
- Cicli: i valori distinti di sensor.serra_ora_del_nodo, cioe' l'ora che il
  nodo scrive in ogni pacchetto. Il recorder salva una riga solo quando lo
  stato CAMBIA, ed e' proprio quello che serve: le ripubblicazioni del ponte
  dopo una riconnessione hanno la stessa ora e non contano due volte. Il
  recorder tiene circa 10 giorni: oltre, i cicli risultano "n/d".
- Segnale, batteria, temperatura: le statistiche orarie (tabella
  statistics), che sono medie pesate sul tempo e durano molto piu' a lungo.
  Le medie calcolate sulle righe di stato sarebbero falsate, per lo stesso
  motivo di prima: un valore che si ripete non lascia righe.
- Acqua: differenza giornaliera di sensor.serra_acqua_totale.
- Ponte: aumento giornaliero dei contatori di riconnessioni MQTT, riparazioni
  della radio e pacchetti con CRC errato.

Le giornate sono quelle italiane (Europe/Rome), come nell'interfaccia.
"""

import argparse
import datetime as dt
import sqlite3
import sys

try:
    from zoneinfo import ZoneInfo
    FUSO = ZoneInfo("Europe/Rome")
except Exception:                       # Python vecchio: si ripiega su UTC
    FUSO = dt.timezone.utc

SLEEP_DEF = 900

ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
ap.add_argument("--db", default="/config/home-assistant_v2.db",
                help="database del recorder (default: quello del container)")
ap.add_argument("--giorni", type=int, default=10, help="giorni da mostrare (default 10)")
args = ap.parse_args()

try:
    con = sqlite3.connect("file:%s?mode=ro" % args.db, uri=True)
except sqlite3.Error as e:
    sys.exit("Impossibile aprire %s: %s" % (args.db, e))

adesso = dt.datetime.now(FUSO)
oggi = adesso.date()
giorni = [oggi - dt.timedelta(days=i) for i in range(args.giorni - 1, -1, -1)]
inizio_ts = dt.datetime.combine(giorni[0], dt.time(), FUSO).timestamp()


def giorno_di(ts):
    return dt.datetime.fromtimestamp(ts, FUSO).date()


def righe(entita, da_ts):
    return con.execute(
        "SELECT s.state, s.last_updated_ts FROM states s "
        "JOIN states_meta m ON s.metadata_id = m.metadata_id "
        "WHERE m.entity_id = ? AND s.last_updated_ts >= ? "
        "ORDER BY s.last_updated_ts", (entita, da_ts)).fetchall()


def numero(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def statistiche(entita):
    """{giorno: (minimo, media, massimo)} dalle statistiche orarie."""
    q = con.execute(
        "SELECT st.start_ts, st.min, st.mean, st.max FROM statistics st "
        "JOIN statistics_meta sm ON st.metadata_id = sm.id "
        "WHERE sm.statistic_id = ? AND st.start_ts >= ? ORDER BY st.start_ts",
        (entita, inizio_ts)).fetchall()
    per_giorno = {}
    for start, mn, media, mx in q:
        if media is None:
            continue
        per_giorno.setdefault(giorno_di(start), []).append((mn, media, mx))
    return {g: (min(r[0] for r in v), sum(r[1] for r in v) / len(v), max(r[2] for r in v))
            for g, v in per_giorno.items()}


def aumenti(entita):
    """{giorno: aumento} per un contatore che cresce (azzeramenti compresi)."""
    per_giorno, prec = {}, None
    for stato, ts in righe(entita, inizio_ts - 86400):
        v = numero(stato)
        if v is None:
            continue
        if prec is not None and ts >= inizio_ts:
            d = v - prec if v >= prec else v      # sceso: il ponte si e' riavviato
            per_giorno[giorno_di(ts)] = per_giorno.get(giorno_di(ts), 0) + d
        prec = v
    return per_giorno


# --- Cicli -----------------------------------------------------------------

intervallo = SLEEP_DEF
ultimo_slp = righe("number.serra_intervallo_risveglio", 0)
for stato, _ in reversed(ultimo_slp):
    if numero(stato) and numero(stato) >= 60:
        intervallo = int(numero(stato))
        break

istanti = set()
for stato, _ in righe("sensor.serra_ora_del_nodo", inizio_ts - 2 * intervallo):
    if stato and stato[:2] == "20":
        try:
            istanti.add(dt.datetime.fromisoformat(stato).timestamp())
        except ValueError:
            pass
istanti = sorted(istanti)
primo_dato = istanti[0] if istanti else None

cicli, buco = {}, {}
for a, b in zip(istanti, istanti[1:]):
    g = giorno_di(b)
    if b - a > 1.5 * intervallo:
        buco[g] = max(buco.get(g, 0), b - a)
for t in istanti:
    if t >= inizio_ts:
        cicli[giorno_di(t)] = cicli.get(giorno_di(t), 0) + 1


def attesi(g):
    """Risvegli attesi nel giorno g, contando solo da quando il recorder ha dati."""
    da = dt.datetime.combine(g, dt.time(), FUSO).timestamp()
    a = min(da + 86400, adesso.timestamp())
    if primo_dato is None or primo_dato > a:
        return 0
    return max(0, round((a - max(da, primo_dato)) / intervallo))


# --- Il resto ----------------------------------------------------------------

rssi = statistiche("sensor.serra_segnale_lora")
snr = statistiche("sensor.serra_snr_lora")
batt = statistiche("sensor.serra_tensione_batteria")
temp = statistiche("sensor.serra_temperatura")
acqua = aumenti("sensor.serra_acqua_totale")
cadute = aumenti("sensor.serra_riconnessioni_mqtt")
riparazioni = aumenti("sensor.serra_riparazioni_radio")
crc = aumenti("sensor.serra_pacchetti_crc_errato")


def f(v, fmt):
    return fmt % v if v is not None else "n/d"


print("Serra - salute degli ultimi %d giorni (intervallo del nodo %d s, ora italiana)"
      % (args.giorni, intervallo))
print()
intest = ("giorno", "cicli", "%", "buco max", "RSSI med/min", "SNR",
          "batt min-max V", "temp min-max C", "acqua L", "cadute", "rip.", "crc")
print("%-10s %8s %4s %8s %12s %5s %14s %14s %7s %6s %4s %4s" % intest)
print("-" * 112)

tot_c = tot_a = 0
for g in giorni:
    a = attesi(g)
    c = cicli.get(g, 0)
    tot_c += c
    tot_a += a
    perc = "%3.0f" % (100.0 * c / a) if a else "n/d"
    r, s, b, t = rssi.get(g), snr.get(g), batt.get(g), temp.get(g)
    print("%-10s %8s %4s %8s %12s %5s %14s %14s %7s %6s %4s %4s" % (
        g.strftime("%d/%m %a"),
        "%d/%d" % (c, a) if a else "n/d",
        perc,
        "%d min" % (buco[g] / 60) if g in buco else "-",
        "%s/%s" % (f(r and r[1], "%.0f"), f(r and r[0], "%.0f")) if r else "n/d",
        f(s and s[1], "%.1f"),
        "%s-%s" % (f(b[0], "%.2f"), f(b[2], "%.2f")) if b else "n/d",
        "%s-%s" % (f(t[0], "%.1f"), f(t[2], "%.1f")) if t else "n/d",
        f(acqua.get(g), "%.2f"),
        f(cadute.get(g), "%.0f"),
        f(riparazioni.get(g), "%.0f"),
        f(crc.get(g), "%.0f"),
    ))

print("-" * 112)
if tot_a:
    print("Cicli arrivati: %d su %d attesi (%.1f%%)." % (tot_c, tot_a, 100.0 * tot_c / tot_a))
if buco:
    g, durata = max(buco.items(), key=lambda kv: kv[1])
    print("Buco piu' lungo: %.1f ore, chiuso il %s." % (durata / 3600, g.strftime("%d/%m")))
print()
print("cicli = valori distinti dell'ora del nodo; buco max = intervallo piu' lungo fra due")
print("cicli; cadute = riconnessioni MQTT del ponte; rip. = riparazioni della radio del")
print("ponte; crc = pacchetti ricevuti con CRC errato (dal ponte 2.6.0).")
