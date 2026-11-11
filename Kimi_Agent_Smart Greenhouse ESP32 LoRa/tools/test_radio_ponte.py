# -*- coding: utf-8 -*-
"""
Compila e lancia tools/test_radio_ponte.cpp: la radio del ponte
(camera_ponte/radio.cpp) con la libreria LoRa vera, contro un SX1278 finto
(tools/sim_radio/) che sa guastarsi come si e' guastato quello vero.

Servono un compilatore C++ (g++ o clang++) e i sorgenti della libreria LoRa
di Sandeep Mistry, cercati nelle cartelle di Arduino o in LORA_SRC. Se manca
una delle due cose il test lo dice e non fallisce.

    python tools/test_radio_ponte.py            tutti gli scenari
    python tools/test_radio_ponte.py -v muto    uno solo, con la seriale del ponte
"""

import os
import shutil
import subprocess
import sys
import tempfile

QUI = os.path.dirname(os.path.abspath(__file__))
RADICE = os.path.dirname(QUI)
PONTE = os.path.join(RADICE, "camera_ponte")
SIM = os.path.join(QUI, "sim_radio")

casa = os.path.expanduser("~")
candidati = [os.environ.get("LORA_SRC", ""),
             os.path.join(casa, "Arduino", "libraries", "LoRa", "src"),
             os.path.join(casa, "Documents", "Arduino", "libraries", "LoRa", "src"),
             os.path.join(casa, "OneDrive", "Documents", "Arduino", "libraries", "LoRa", "src")]
lora = next((c for c in candidati if c and os.path.isfile(os.path.join(c, "LoRa.cpp"))), None)
if not lora:
    print("Libreria LoRa non trovata (imposta LORA_SRC): test della radio del ponte saltato.")
    sys.exit(0)

compilatore = shutil.which("g++") or shutil.which("clang++")
if not compilatore:
    print("Nessun compilatore C++ trovato: test della radio del ponte saltato.")
    sys.exit(0)

argomenti = [a for a in sys.argv[1:] if a != "-v"]
verboso = "-v" in sys.argv[1:]

with tempfile.TemporaryDirectory() as cartella:
    eseguibile = os.path.join(cartella, "test_radio_ponte")
    # La cartella dei finti va per prima: Arduino.h, SPI.h ed esp_task_wdt.h
    # devono essere quelli, non quelli veri.
    comando = [compilatore, "-std=c++17", "-Wall", "-Wextra",
               "-I", SIM, "-I", PONTE, "-I", lora,
               os.path.join(QUI, "test_radio_ponte.cpp"),
               os.path.join(PONTE, "radio.cpp"),
               os.path.join(lora, "LoRa.cpp"),
               "-o", eseguibile]
    r = subprocess.run(comando, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    testo = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0:
        print(testo)
        raise SystemExit("FALLITO: la radio del ponte non compila contro il modulo simulato")
    # Gli avvisi del compilatore contano solo per il codice nostro
    nostri = [riga for riga in testo.splitlines()
              if "warning" in riga and ("radio.cpp" in riga or "test_radio_ponte.cpp" in riga
                                        or "sim_radio" in riga)]
    if nostri:
        print("\n".join(nostri))
        raise SystemExit("FALLITO: avvisi del compilatore in radio.cpp o nel test")

    scenari = argomenti or subprocess.run(
        [eseguibile], stdout=subprocess.PIPE).stdout.decode().split()
    falliti = 0
    print("Radio del ponte contro il modulo simulato:", flush=True)
    for nome in scenari:
        falliti += subprocess.run([eseguibile, nome] + (["-v"] if verboso else [])).returncode != 0
    if falliti:
        raise SystemExit("FALLITO: %d scenari su %d" % (falliti, len(scenari)))
    print("Tutti e %d gli scenari passano." % len(scenari))
