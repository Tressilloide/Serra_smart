# -*- coding: utf-8 -*-
"""
Prova delle automazioni del pulsante "Durata automatica" (package
homeassistant/packages/serra.yaml) con il VERO motore delle automazioni di
Home Assistant, in un'istanza in memoria: servizi number/input_number
simulati, chiamate "da una persona" (contesto con utente) e "da
un'automazione", stati del nodo che passano da "non disponibile" come nelle
riconnessioni del ponte.

Serve Home Assistant come libreria, che c'e' nel suo container. Sul server
basta lanciarlo come gli altri test: si copia da solo nel container insieme
al package del repository e gira li'.

    python3 tools/test_durata_automatica.py

Non tocca l'installazione vera: nessun file di /config viene letto o scritto.
"""
import os
import shutil
import subprocess
import sys

try:
    import homeassistant  # noqa: F401
except ImportError:
    QUI = os.path.dirname(os.path.abspath(__file__))
    pacchetto = os.path.join(QUI, "..", "homeassistant", "packages", "serra.yaml")
    if not shutil.which("docker"):
        print("Home Assistant non e' installato qui e manca docker: test saltato.")
        sys.exit(0)
    if subprocess.run(["docker", "cp", pacchetto, "homeassistant:/tmp/serra.yaml"]).returncode:
        print("Container homeassistant non raggiungibile: test saltato.")
        sys.exit(0)
    with open(os.path.abspath(__file__), "rb") as f:
        r = subprocess.run(["docker", "exec", "-i", "homeassistant", "timeout", "150",
                            "python3", "-", "/tmp/serra.yaml"], stdin=f)
    subprocess.run(["docker", "exec", "homeassistant", "rm", "-f", "/tmp/serra.yaml"])
    sys.exit(r.returncode)
import asyncio, yaml, tempfile, logging
from homeassistant.core import HomeAssistant, Context
from homeassistant import loader
from homeassistant.setup import async_setup_component
logging.basicConfig(level=logging.ERROR)

class L(yaml.SafeLoader): pass
pkg = yaml.load(open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/serra.yaml'), Loader=L)
IDS = {'serra_irrigazione_adattiva','serra_durata_automatica_spenta','serra_durata_normale_ricorda','serra_durata_normale_applica','serra_durata_normale_iniziale'}
autos = [a for a in pkg['automation'] if a['id'] in IDS]
assert len(autos) == 5

NUM, BASE, BOOL, CONS = 'number.serra_durata_irrigazione', 'input_number.serra_durata_base_irrigazione', 'input_boolean.serra_irrigazione_adattiva', 'sensor.serra_durata_irrigazione_consigliata'
errori = []
def verifica(c, msg):
    print(('ok    ' if c else 'FALLITO ') + msg)
    if not c: errori.append(msg)

async def main():
    hass = HomeAssistant(tempfile.mkdtemp())
    loader.async_setup(hass)
    await hass.config.async_set_time_zone('Europe/Rome')
    from homeassistant.config_entries import ConfigEntries
    from homeassistant.helpers import entity_registry as er, device_registry as dr, area_registry as ar, floor_registry as fr, label_registry as lr, category_registry as cr, issue_registry as ir, translation
    hass.config_entries = ConfigEntries(hass, {})
    await hass.config_entries.async_initialize()
    for m in (ar, fr, lr, cr, dr, er, ir):
        try: await m.async_load(hass)
        except TypeError: m.async_load(hass)
    comandi, notifiche = [], []
    async def number_set(call):
        e = call.data['entity_id']; e = e[0] if isinstance(e, list) and len(e) == 1 else e
        comandi.append((e, call.data['value'], call.context.user_id))
    async def base_set(call):
        hass.states.async_set(BASE, str(float(call.data['value'])), context=call.context)
    async def notifica(call):
        notifiche.append(call.data['title'])
    hass.services.async_register('number', 'set_value', number_set)
    hass.services.async_register('input_number', 'set_value', base_set)
    hass.services.async_register('persistent_notification', 'create', notifica)

    hass.states.async_set(NUM, '50'); hass.states.async_set(BASE, '10.0'); hass.states.async_set(BOOL, 'off')
    assert await async_setup_component(hass, 'automation', {'automation': autos})
    await hass.async_start(); await hass.async_block_till_done()
    verifica(hass.states.get(BASE).state == '50.0', f"1. avvio: durata normale presa dal nodo (50), e' {hass.states.get(BASE).state}")

    # 2. acceso di notte, consigliata non pronta
    hass.states.async_set(CONS, 'unavailable')
    hass.states.async_set(BOOL, 'on'); await hass.async_block_till_done()
    verifica(not comandi and notifiche[-1:] == ['Serra: durata automatica accesa'], f"2. acceso senza consigliata: nessun comando, avviso ({comandi}, {notifiche[-1:]})")

    # 3. spento e riacceso con la consigliata pronta
    hass.states.async_set(BOOL, 'off'); await hass.async_block_till_done()
    verifica(not comandi, f"3a. spento con nodo gia' a 50 = normale: nessun comando ({comandi})")
    hass.states.async_set(CONS, '30', {'domanda_kpa_h': 4.2, 'fonte': 'notte + previsione met.no'})
    hass.states.async_set(BOOL, 'on'); await hass.async_block_till_done()
    verifica(comandi[-1:] == [(NUM, 30, None)], f"3b. acceso: sul nodo 30 s ({comandi[-1:]})")
    verifica(hass.states.get(BASE).state == '50.0', '3c. la durata normale resta 50')

    # 4. il nodo riferisce 30, poi il ponte si riconnette (unavailable -> 30): la normale non si tocca
    hass.states.async_set(NUM, '30'); hass.states.async_set(NUM, 'unavailable'); hass.states.async_set(NUM, '30')
    await hass.async_block_till_done()
    verifica(hass.states.get(BASE).state == '50.0', f"4. riconnessioni del ponte: normale ancora 50 ({hass.states.get(BASE).state})")

    # 5. un'automazione (senza utente) chiama number.set_value: non e' una scelta a mano
    n = len(comandi)
    await hass.services.async_call('number', 'set_value', {'entity_id': NUM, 'value': 40}, blocking=True, context=Context())
    await hass.async_block_till_done()
    verifica(hass.states.get(BASE).state == '50.0', '5. comando senza utente: la normale non cambia')

    # 6. una persona imposta 70 col pulsante acceso: diventa la normale, e avviso
    await hass.services.async_call('number', 'set_value', {'entity_id': NUM, 'value': 70}, blocking=True, context=Context(user_id='eric'))
    await hass.async_block_till_done()
    verifica(hass.states.get(BASE).state == '70.0', f"6a. scelta a mano: normale 70 ({hass.states.get(BASE).state})")
    verifica(notifiche[-1] == 'Serra: durata impostata a mano', f"6b. avviso della scelta a mano col pulsante acceso ({notifiche[-1]})")
    hass.states.async_set(NUM, '70')

    # 7. 6:40 del giorno dopo: torna la consigliata (simulata con automation.trigger)
    hass.states.async_set(CONS, '20', {'domanda_kpa_h': 1.5, 'fonte': 'notte + previsione met.no'})
    await hass.services.async_call('automation', 'trigger', {'entity_id': 'automation.serra_durata_automatica', 'skip_condition': False}, blocking=True)
    await hass.async_block_till_done()
    verifica(comandi[-1] == (NUM, 20, None), f"7. alle 6:40: sul nodo la consigliata 20 ({comandi[-1]})")
    hass.states.async_set(NUM, '20')

    # 8. spento: torna la normale 70
    hass.states.async_set(BOOL, 'off'); await hass.async_block_till_done()
    verifica(comandi[-1] == (NUM, 70, None), f"8. spento: sul nodo la normale 70 ({comandi[-1]})")
    verifica(notifiche[-1] == 'Serra: durata automatica spenta', '8b. avviso di spegnimento')
    hass.states.async_set(NUM, '70')

    # 9. col pulsante spento una persona cambia la normale dal suo riquadro: va sul nodo
    await hass.services.async_call('input_number', 'set_value', {'entity_id': BASE, 'value': 90}, blocking=True, context=Context(user_id='eric'))
    await hass.async_block_till_done()
    verifica(comandi[-1] == (NUM, 90, None), f"9. normale cambiata a mano, pulsante spento: sul nodo 90 ({comandi[-1]})")

    # 10. col pulsante spento una persona cambia la durata del nodo: la normale la segue, nessun avviso
    k = len(notifiche)
    await hass.services.async_call('number', 'set_value', {'entity_id': [NUM], 'value': 60}, blocking=True, context=Context(user_id='eric'))
    await hass.async_block_till_done()
    verifica(hass.states.get(BASE).state == '60.0' and len(notifiche) == k, f"10. scelta a mano col pulsante spento: normale 60, nessun avviso ({hass.states.get(BASE).state})")
    n = len(comandi)
    await hass.async_block_till_done()
    verifica(len(comandi) == n, '10b. nessun comando in piu\' (niente rimbalzo fra le automazioni)')

    # 11. ponte giu' quando si spegne: aspetta, poi applica
    hass.states.async_set(BOOL, 'on'); await hass.async_block_till_done()
    hass.states.async_set(NUM, 'unavailable')
    n = len(comandi)
    hass.states.async_set(BOOL, 'off'); await asyncio.sleep(0.3)
    verifica(len(comandi) == n, '11a. ponte giu\': nessun comando perso nel vuoto')
    hass.states.async_set(NUM, '20'); await asyncio.sleep(0.3); await hass.async_block_till_done()
    verifica(comandi[-1] == (NUM, 60, None), f"11b. ponte tornato: sul nodo la normale 60 ({comandi[-1]})")

    # 12. un altro number cambiato a mano non tocca la normale
    await hass.services.async_call('number', 'set_value', {'entity_id': 'number.serra_orario_irrigazione_ora', 'value': 8}, blocking=True, context=Context(user_id='eric'))
    await hass.async_block_till_done()
    verifica(hass.states.get(BASE).state == '60.0', '12. un altro number non tocca la normale')

    await hass.async_stop(force=True)
    print()
    print('Tutti i casi passano.' if not errori else '%d verifiche fallite.' % len(errori))
    return len(errori)
sys.exit(1 if asyncio.run(asyncio.wait_for(main(), 120)) else 0)
