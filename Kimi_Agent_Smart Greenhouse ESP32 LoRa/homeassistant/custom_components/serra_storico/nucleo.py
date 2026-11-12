"""
Il lavoro sul database, senza niente di Home Assistant: riceve una sessione
SQLAlchemy e i record recuperati, e li mette nella tabella degli stati
all'ora in cui il nodo li ha misurati. Cosi' si puo' provare su una copia del
database (tools/test_storico_ha.py).

Home Assistant non ha un modo ufficiale di registrare uno stato nel passato:
gli stati hanno sempre l'ora in cui arrivano. I record che il nodo rimanda
dopo un buio (serra/nodo/storico) hanno invece la loro ora nel campo t, e
senza questo nei grafici resta un buco anche quando i dati ci sono.

Regole:
  - si scrivono solo valori numerici finiti, dentro i limiti del campo;
  - un valore non si scrive se l'entita' ha gia' uno stato valido entro
    VICINO_SEC dall'ora del record: rimandare lo stesso record due volte, o
    recuperare un ciclo che era arrivato anche in diretta, non cambia niente;
  - niente nel futuro e niente piu' vecchio di ETA_MAX_SEC (il recorder tiene
    gli stati una decina di giorni, e un orologio del nodo impazzito non deve
    poter scrivere nel 1970);
  - le statistiche orarie delle ore toccate, se gia' chiuse, si ricalcolano
    dagli stati (media, minima, massima): sono quelle che restano per mesi.
    L'ora in corso la chiude Home Assistant, leggendo gli stessi stati.
"""

from __future__ import annotations

import math
import os

from sqlalchemy import text

VICINO_SEC = 120
ETA_MAX_SEC = 9 * 86400

# campo del record -> (minimo, massimo) accettati
LIMITI = {
    "temp": (-40.0, 85.0),
    "hum": (0.0, 100.0),
    "pres": (300.0, 1100.0),
    "volt": (0.0, 30.0),
    "luce": (0.0, 100.0),
    "soil1": (0.0, 100.0),
    "soil2": (0.0, 100.0),
    "soil3": (0.0, 100.0),
    "soil4": (0.0, 100.0),
}


def numero(valore):
    """Il valore come float, None se non e' un numero finito."""
    try:
        f = float(valore)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


def vpd(temp, hum):
    """Deficit di pressione di vapore in kPa: la formula di sensor.serra_vpd."""
    es = 0.6108 * math.exp(17.27 * temp / (temp + 237.3))
    return round(es * (1 - hum / 100.0), 2)


def valori_del_record(record, mappa):
    """{entity_id: testo dello stato} per un record recuperato."""
    out = {}
    letti = {}
    for campo, (minimo, massimo) in LIMITI.items():
        v = numero(record.get(campo))
        if v is None or not minimo <= v <= massimo:
            continue
        letti[campo] = v
        if mappa.get(campo):
            # Lo stesso testo che arriva in diretta: il numero come lo scrive il ponte
            out[mappa[campo]] = str(record[campo])
    if mappa.get("vpd") and "temp" in letti and "hum" in letti:
        out[mappa["vpd"]] = str(vpd(letti["temp"], letti["hum"]))
    return out


def importa(session, records, mappa, adesso):
    """
    Scrive i record e ricalcola le statistiche orarie toccate.
    Ritorna un riassunto: record validi, stati scritti, gia' presenti, ore.
    """
    esito = {"record": 0, "scritti": 0, "presenti": 0, "scartati": 0,
             "statistiche": 0, "dal": None, "al": None}

    meta, attributi = {}, {}
    ore_toccate = {}   # entity_id -> insieme di inizi d'ora

    def metadati(entita):
        if entita not in meta:
            r = session.execute(text(
                "SELECT metadata_id FROM states_meta WHERE entity_id = :e"), {"e": entita}).first()
            meta[entita] = r[0] if r else None
            if r:
                a = session.execute(text(
                    "SELECT attributes_id FROM states WHERE metadata_id = :m "
                    "AND attributes_id IS NOT NULL AND state NOT IN ('unavailable', 'unknown') "
                    "ORDER BY last_updated_ts DESC LIMIT 1"), {"m": r[0]}).first()
                attributi[entita] = a[0] if a else None
        return meta[entita]

    for record in records:
        t = numero(record.get("t")) if isinstance(record, dict) else None
        if t is None or t > adesso + 60 or t < adesso - ETA_MAX_SEC:
            esito["scartati"] += 1
            continue
        valori = valori_del_record(record, mappa)
        if not valori:
            esito["scartati"] += 1
            continue
        esito["record"] += 1
        esito["dal"] = t if esito["dal"] is None else min(esito["dal"], t)
        esito["al"] = t if esito["al"] is None else max(esito["al"], t)

        for entita, stato in valori.items():
            m = metadati(entita)
            if m is None:
                continue
            gia = session.execute(text(
                "SELECT 1 FROM states WHERE metadata_id = :m "
                "AND last_updated_ts BETWEEN :a AND :b "
                "AND state NOT IN ('unavailable', 'unknown') LIMIT 1"),
                {"m": m, "a": t - VICINO_SEC, "b": t + VICINO_SEC}).first()
            if gia:
                esito["presenti"] += 1
                continue
            session.execute(text(
                "INSERT INTO states (state, last_updated_ts, last_reported_ts, attributes_id, "
                "origin_idx, context_id_bin, metadata_id) "
                "VALUES (:s, :t, :t, :a, 0, :c, :m)"),
                {"s": stato, "t": t, "a": attributi.get(entita), "c": os.urandom(16), "m": m})
            esito["scritti"] += 1
            ore_toccate.setdefault(entita, set()).add(int(t // 3600) * 3600)

    ora_in_corso = int(adesso // 3600) * 3600
    for entita, ore in ore_toccate.items():
        r = session.execute(text(
            "SELECT id FROM statistics_meta WHERE statistic_id = :e AND source = 'recorder' "
            "AND (has_sum IS NULL OR has_sum = 0)"), {"e": entita}).first()
        if not r:
            continue
        sid = r[0]
        for inizio in sorted(ore):
            if inizio >= ora_in_corso:
                continue
            numeri = [n for n in (numero(x[0]) for x in session.execute(text(
                "SELECT state FROM states WHERE metadata_id = :m "
                "AND last_updated_ts >= :a AND last_updated_ts < :b"),
                {"m": meta[entita], "a": inizio, "b": inizio + 3600})) if n is not None]
            if not numeri:
                continue
            p = {"sid": sid, "a": float(inizio), "media": sum(numeri) / len(numeri),
                 "mn": min(numeri), "mx": max(numeri), "ora": adesso}
            c = session.execute(text(
                "UPDATE statistics SET mean = :media, min = :mn, max = :mx "
                "WHERE metadata_id = :sid AND start_ts = :a"), p)
            if not c.rowcount:
                session.execute(text(
                    "INSERT INTO statistics (created_ts, metadata_id, start_ts, mean, min, max) "
                    "VALUES (:ora, :sid, :a, :media, :mn, :mx)"), p)
            esito["statistiche"] += 1
    return esito
