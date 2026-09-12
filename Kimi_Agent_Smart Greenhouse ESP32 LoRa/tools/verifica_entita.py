#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Verifica che ogni entita' richiamata dalle dashboard e dal package esista
davvero, confrontandola con quelle generate dalla MQTT Discovery del ponte.

PERCHE' SERVE
-------------
Home Assistant ricava l'entity_id dal campo "name", non da "unique_id".
Un nome cambiato senza aggiornare i riferimenti non produce nessun errore:
l'entita' resta semplicemente "non disponibile" per sempre, e capire il
perche' richiede parecchio tempo. E' gia' successo tre volte in questo
progetto, da cui questo controllo.

USO
---
    python tools/verifica_entita.py

dalla cartella del progetto. Esce con codice 1 se qualcosa non risolve.

CONFRONTO CON L'INSTALLAZIONE VERA (facoltativo)
------------------------------------------------
    python tools/verifica_entita.py --ha http://192.168.1.36:8123 --token <TOKEN>

Con un token di accesso a lunga durata (profilo utente in fondo alla
pagina) interroga /api/states e confronta con le entita' realmente
presenti, che e' l'unica verifica che non puo' sbagliarsi.
"""

import argparse
import glob
import io
import json
import os
import re
import sys
import urllib.request

DOMINI = "sensor|binary_sensor|button|number|switch|time"


def slug(nome):
    """Riproduce la slugify di Home Assistant.

    Tutto in minuscolo, e ogni sequenza di caratteri che non siano lettere
    o cifre diventa un singolo underscore. Attenzione agli apostrofi: NON
    vengono tolti ma trasformati in underscore, quindi "dall'ultimo" da'
    "dall_ultimo" e non "dallultimo". Verificato su un'installazione reale.
    """
    return re.sub(r"[^a-z0-9]+", "_", nome.lower()).strip("_")


def senza_commenti(testo):
    return "\n".join(l for l in testo.splitlines() if not l.lstrip().startswith("#"))


def entita_dal_ponte(percorso):
    """Entita' generate dalla discovery, dedotte dal sorgente del ponte."""
    src = io.open(percorso, encoding="utf-8").read()
    trovate = set()

    # Tabelle C: { "chiave", "Nome leggibile", ... }
    for nome in re.findall(r'\{\s*"[A-Za-z_][A-Za-z0-9_]*",\s*"([^"]+)"', src):
        trovate.add("sensor.serra_" + slug(nome))

    # Entita' di comando: nei sorgenti il JSON e' con le virgolette protette,
    # quindi si cerca la sequenza  name\":\"  e si legge fino alla successiva.
    apri, chiudi = 'name' + chr(92) + '":' + chr(92) + '"', chr(92) + '"'
    i = 0
    while True:
        i = src.find(apri, i)
        if i < 0:
            break
        i += len(apri)
        j = src.find(chiudi, i)
        nome = src[i:j]
        # Il dominio non e' deducibile con certezza dal solo nome: si accetta
        # in tutti i domini possibili. Il controllo resta utile perche' cio'
        # che conta e' che lo SLUG corrisponda.
        for dom in DOMINI.split("|"):
            trovate.add(dom + ".serra_" + slug(nome))
        i = j
    return trovate


def entita_dal_package(percorso):
    pkg = io.open(percorso, encoding="utf-8").read()
    trovate = set()

    # template e mqtt: l'entity_id viene dal name
    for nome in re.findall(r'-\s+name:\s*"([^"]+)"', pkg):
        trovate.add("sensor." + slug(nome))
        trovate.add("binary_sensor." + slug(nome))

    # utility_meter: anche qui l'entity_id viene dal name, NON dalla chiave.
    # (La chiave determina invece l'unique_id: rinominarla crea un'entita'
    #  nuova che, trovando l'id occupato, finisce con "_2" e perde lo storico.)
    for _chiave, nome in re.findall(
            r"^  ([a-z_]+):\r?\n    name:\s*(.+?)\r?$", pkg, re.M):
        trovate.add("sensor." + slug(nome.strip()))

    return trovate


def entita_reali(base, token):
    req = urllib.request.Request(
        base.rstrip("/") + "/api/states",
        headers={"Authorization": "Bearer " + token})
    stati = json.loads(urllib.request.urlopen(req, timeout=20).read().decode())
    return {s["entity_id"] for s in stati}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ha", help="URL di Home Assistant, es. http://192.168.1.36:8123")
    ap.add_argument("--token", help="token di accesso a lunga durata")
    args = ap.parse_args()

    radice = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(radice)

    esistenti = entita_dal_ponte("camera_ponte/discovery.cpp")
    esistenti |= entita_dal_package("homeassistant/packages/serra.yaml")

    reali = None
    if args.ha and args.token:
        try:
            reali = entita_reali(args.ha, args.token)
            print("Home Assistant raggiunto: %d entita' presenti.\n" % len(reali))
        except Exception as e:
            print("Impossibile interrogare Home Assistant (%s): "
                  "proseguo con la sola analisi statica.\n" % e)

    problemi = 0
    file_da_controllare = sorted(glob.glob("homeassistant/lovelace/*.yaml"))
    file_da_controllare.append("homeassistant/packages/serra.yaml")

    for f in file_da_controllare:
        testo = senza_commenti(io.open(f, encoding="utf-8").read())
        usate = sorted(set(re.findall(r"\b((?:%s)\.[a-z0-9_]+)" % DOMINI, testo)))

        confronto = reali if reali is not None else esistenti
        mancanti = [u for u in usate if u not in confronto]

        if mancanti:
            problemi += len(mancanti)
            print("%s -> %d riferimenti NON risolti:" % (f, len(mancanti)))
            for u in mancanti:
                print("      " + u)
        else:
            print("%s -> tutti i %d riferimenti risolvono" % (f, len(usate)))

    if reali is not None:
        orfane = sorted(e for e in reali
                        if ("serra_serra" in e or e.endswith("_2"))
                        and e.split(".")[0] in DOMINI.split("|"))
        if orfane:
            print("\nEntita' probabilmente ORFANE in Home Assistant "
                  "(residui di versioni precedenti):")
            for e in orfane:
                print("      " + e)
            print("  Si rimuovono da Impostazioni -> Dispositivi e servizi -> Entita'.")

    print()
    if problemi:
        print("%d riferimenti da sistemare." % problemi)
        return 1
    print("Nessun riferimento rotto.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
