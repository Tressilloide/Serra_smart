# -*- coding: utf-8 -*-
"""
Compila e lancia tools/test_riserva.cpp, la prova della coda di riserva in
RAM RTC del nodo (serra_nodo/riserva_coda.h), con lo stesso RISERVA_BYTE di
serra_nodo/config.h.

Serve un compilatore C++ (g++ o clang++). Se non c'e', il test lo dice e non
fallisce: gli altri script di tools/ girano anche senza.

    python tools/test_riserva.py
"""

import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

QUI = os.path.dirname(os.path.abspath(__file__))
RADICE = os.path.dirname(QUI)
NODO = os.path.join(RADICE, "serra_nodo")

config = io.open(os.path.join(NODO, "config.h"), encoding="utf-8").read()
m = re.search(r"#define\s+RISERVA_BYTE\s+(\d+)", config)
if not m:
    raise SystemExit("RISERVA_BYTE non trovata in serra_nodo/config.h")
riserva_byte = m.group(1)

compilatore = shutil.which("g++") or shutil.which("clang++")
if not compilatore:
    print("Nessun compilatore C++ trovato: test della riserva saltato.")
    sys.exit(0)

with tempfile.TemporaryDirectory() as cartella:
    eseguibile = os.path.join(cartella, "test_riserva")
    comando = [compilatore, "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-DRISERVA_BYTE=%s" % riserva_byte, "-I", NODO,
               os.path.join(QUI, "test_riserva.cpp"), "-o", eseguibile]
    r = subprocess.run(comando)
    if r.returncode != 0:
        raise SystemExit("FALLITO: tools/test_riserva.cpp non compila")
    sys.exit(subprocess.run([eseguibile]).returncode)
