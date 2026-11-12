# -*- coding: utf-8 -*-
"""
Prova di homeassistant/custom_components/serra_storico/nucleo.py, il pezzo
che scrive nel database di Home Assistant i record recuperati dal nodo, su un
database finto con le sole tabelle che servono.

Serve SQLAlchemy. Sul server, dove Python non ce l'ha, il test si rilancia
da solo dentro il container di Home Assistant:

    python3 tools/test_storico_ha.py
"""

import importlib.util
import os
import shutil
import subprocess
import sys

QUI = os.path.dirname(os.path.abspath(__file__))
NUCLEO = os.path.join(os.path.dirname(QUI), "homeassistant", "custom_components",
                      "serra_storico", "nucleo.py")

try:
    from sqlalchemy import create_engine, text
    from sqlalchemy.orm import Session
except ImportError:
    if os.environ.get("NUCLEO") or not shutil.which("docker"):
        print("SQLAlchemy non c'e': test dello storico saltato.")
        sys.exit(0)
    sorgente = open(__file__, "rb").read()
    subprocess.run(["docker", "exec", "-i", "homeassistant", "sh", "-c",
                    "cat > /tmp/serra_nucleo.py"], input=open(NUCLEO, "rb").read(), check=True)
    sys.exit(subprocess.run(["docker", "exec", "-i", "-e", "NUCLEO=/tmp/serra_nucleo.py",
                             "homeassistant", "python3", "-"], input=sorgente).returncode)

spec = importlib.util.spec_from_file_location("nucleo", os.environ.get("NUCLEO", NUCLEO))
nucleo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nucleo)

ADESSO = 1794484800.0          # un'ora tonda
MAPPA = {"temp": "sensor.t", "hum": "sensor.u", "vpd": "sensor.vpd", "pres": "sensor.manca"}
falliti = 0


def verifica(cond, cosa):
    global falliti
    if not cond:
        falliti += 1
        print("  FALLITO:", cosa)


def nuovo():
    e = create_engine("sqlite://")
    s = Session(e)
    for sql in (
        "CREATE TABLE states_meta (metadata_id INTEGER PRIMARY KEY, entity_id TEXT)",
        "CREATE TABLE states (state_id INTEGER PRIMARY KEY, state TEXT, last_updated_ts FLOAT, "
        "last_changed_ts FLOAT, last_reported_ts FLOAT, old_state_id INTEGER, attributes_id INTEGER, "
        "origin_idx INTEGER, context_id_bin BLOB, metadata_id INTEGER)",
        "CREATE TABLE statistics_meta (id INTEGER PRIMARY KEY, statistic_id TEXT, source TEXT, has_sum INTEGER)",
        "CREATE TABLE statistics (id INTEGER PRIMARY KEY, created_ts FLOAT, metadata_id INTEGER, "
        "start_ts FLOAT, mean FLOAT, min FLOAT, max FLOAT, state FLOAT, sum FLOAT)",
        "INSERT INTO states_meta VALUES (1, 'sensor.t'), (2, 'sensor.u'), (3, 'sensor.vpd')",
        "INSERT INTO statistics_meta VALUES (10, 'sensor.t', 'recorder', 0), (11, 'sensor.u', 'recorder', NULL)",
        # ultimo dato in diretta 6 ore fa, poi non disponibile
        "INSERT INTO states (state, last_updated_ts, attributes_id, metadata_id) VALUES "
        "('12.98', %f, 7, 1), ('unavailable', %f, NULL, 1), ('86.7', %f, 8, 2)"
        % (ADESSO - 6 * 3600, ADESSO - 4 * 3600, ADESSO - 6 * 3600),
    ):
        s.execute(text(sql))
    return s


def stati(s, m):
    return s.execute(text("SELECT state, last_updated_ts, attributes_id FROM states WHERE metadata_id = %d "
                          "ORDER BY last_updated_ts" % m)).fetchall()


# tre record nella stessa ora chiusa, uno nell'ora in corso
records = [{"bk": 1, "t": ADESSO - 3 * 3600 + 60 + i * 900, "temp": 8.0 + i, "hum": 90.0} for i in range(3)]
records.append({"bk": 1, "t": ADESSO + 300 - 3600 + 3600 - 200, "temp": 11.5, "hum": 88.0})

s = nuovo()
e = nucleo.importa(s, records, MAPPA, ADESSO + 600)
verifica(e["record"] == 4 and e["scritti"] == 12, "4 record, 12 valori scritti: %s" % e)
t = stati(s, 1)
verifica(len(t) == 6, "sei stati di temperatura: %d" % len(t))
verifica([x[0] for x in t if x[1] > ADESSO - 3.5 * 3600] == ["8.0", "9.0", "10.0", "11.5"], "valori e ordine: %s" % t)
verifica(all(x[2] == 7 for x in t if x[0] not in ("unavailable",)), "attributi dell'ultimo stato valido")
verifica(stati(s, 3)[0][0] == str(nucleo.vpd(8.0, 90.0)), "VPD calcolato")
st = s.execute(text("SELECT metadata_id, start_ts, mean, min, max FROM statistics ORDER BY metadata_id")).fetchall()
verifica(len(st) == 2, "statistiche solo per l'ora chiusa e solo per chi le ha: %s" % st)
verifica(st[0][1:] == (ADESSO - 3 * 3600, 9.0, 8.0, 10.0), "media, minima e massima: %s" % (st[0],))
verifica(e["statistiche"] == 2, "ore di statistiche: %s" % e)

# rimandare gli stessi record non cambia niente
e2 = nucleo.importa(s, records, MAPPA, ADESSO + 700)
verifica(e2["scritti"] == 0 and e2["presenti"] == 12, "seconda passata: %s" % e2)
verifica(len(stati(s, 1)) == 6, "nessun doppione")

# un'ora che ha gia' la sua statistica viene aggiornata, non raddoppiata
s = nuovo()
s.execute(text("INSERT INTO statistics (metadata_id, start_ts, mean, min, max) VALUES (10, %f, 12.98, 12.98, 12.98)"
               % (ADESSO - 3 * 3600)))
nucleo.importa(s, records[:3], MAPPA, ADESSO + 600)
st = s.execute(text("SELECT mean, min, max FROM statistics WHERE metadata_id = 10")).fetchall()
verifica(st == [(9.0, 8.0, 10.0)], "statistica aggiornata sul posto: %s" % st)

# un ciclo arrivato anche in diretta non si riscrive
s = nuovo()
s.execute(text("INSERT INTO states (state, last_updated_ts, attributes_id, metadata_id) VALUES ('8.1', %f, 7, 1)"
               % (records[0]["t"] + 4)))
e = nucleo.importa(s, records[:1], MAPPA, ADESSO)
verifica(e["presenti"] == 1 and e["scritti"] == 2, "temperatura gia' presente, umidita' e VPD scritti: %s" % e)

# robaccia: niente ora, futuro, troppo vecchio, fuori limiti, non numeri
s = nuovo()
robaccia = [{"temp": 5}, {"t": ADESSO + 3600, "temp": 5}, {"t": ADESSO - 30 * 86400, "temp": 5},
            {"t": ADESSO - 100, "temp": 180.0, "hum": "nan"}, {"t": "ieri", "temp": 5}, "testo", None,
            {"t": ADESSO - 100, "irr": "ok"}]
e = nucleo.importa(s, robaccia, MAPPA, ADESSO)
verifica(e["scritti"] == 0 and e["scartati"] == len(robaccia), "tutto scartato: %s" % e)
verifica(len(stati(s, 1)) == 2, "database intatto")

# entita' sconosciuta: si salta senza errori
s = nuovo()
e = nucleo.importa(s, [{"t": ADESSO - 100, "pres": 900.1}], MAPPA, ADESSO)
verifica(e["scritti"] == 0 and e["record"] == 1, "entita' che non esiste: %s" % e)

if falliti:
    sys.exit("FALLITO: %d controlli" % falliti)
print("Tutti i casi passano.")
