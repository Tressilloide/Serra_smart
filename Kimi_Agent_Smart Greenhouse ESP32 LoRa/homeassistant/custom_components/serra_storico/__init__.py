"""
Serra: i dati recuperati finiscono nei grafici.

Ascolta serra/nodo/storico, dove il ponte pubblica i record che il nodo ha
tenuto da parte mentre non riusciva a consegnarli (riserva in RAM RTC o
backlog su microSD), e li registra in Home Assistant all'ora in cui sono
stati misurati, non a quella in cui arrivano. Il lavoro sul database e' in
nucleo.py e gira nel thread del recorder, in coda con le sue scritture.

Configurazione (tutto facoltativo), in configuration.yaml o in un package:

    serra_storico:
      topic: serra/nodo/storico
      campi:
        temp: sensor.serra_temperatura

Sul topic <topic>/reimporta si possono ripubblicare a mano dei record gia'
passati (stesso JSON): servono a recuperare un buio di prima che il
componente fosse installato.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import time

from homeassistant.components import mqtt
from homeassistant.components.recorder import get_instance
from homeassistant.components.recorder.tasks import RecorderTask
from homeassistant.components.recorder.util import session_scope
from homeassistant.const import EVENT_HOMEASSISTANT_STARTED
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.event import async_call_later
from homeassistant.util import dt as dt_util

from . import nucleo

DOMAIN = "serra_storico"
_LOGGER = logging.getLogger(__name__)

TOPIC_DEF = "serra/nodo/storico"
ATTESA_SEC = 20          # i record arrivano a raffica: si aspetta che finiscano
STATO = "sensor.serra_ultimo_recupero_nei_grafici"

CAMPI_DEF = {
    "temp": "sensor.serra_temperatura",
    "hum": "sensor.serra_umidita_aria",
    "pres": "sensor.serra_pressione",
    "volt": "sensor.serra_tensione_batteria",
    "luce": "sensor.serra_luce",
    "soil1": "sensor.serra_umidita_terreno_1",
    "soil2": "sensor.serra_umidita_terreno_2",
    "soil3": "sensor.serra_umidita_terreno_3",
    "soil4": "sensor.serra_umidita_terreno_4",
    "vpd": "sensor.serra_vpd",
}


@dataclass(slots=True)
class ImportaTask(RecorderTask):
    """Un lotto di record recuperati, da scrivere nel thread del recorder."""

    hass: HomeAssistant
    records: list
    mappa: dict

    def run(self, instance) -> None:
        try:
            with session_scope(session=instance.get_session()) as session:
                esito = nucleo.importa(session, self.records, self.mappa, time.time())
        except Exception:  # noqa: BLE001 - un lotto storto non deve fermare il recorder
            _LOGGER.exception("Record recuperati non scritti (%d)", len(self.records))
            return
        _LOGGER.info("Record recuperati nei grafici: %s", esito)
        self.hass.loop.call_soon_threadsafe(_pubblica, self.hass, esito)


@callback
def _pubblica(hass: HomeAssistant, esito: dict) -> None:
    def ora(t):
        return dt_util.utc_from_timestamp(t).isoformat() if t else None

    hass.states.async_set(STATO, esito["scritti"], {
        "friendly_name": "Serra ultimo recupero nei grafici",
        "icon": "mdi:chart-timeline-variant",
        "unit_of_measurement": "valori",
        "record": esito["record"],
        "gia_presenti": esito["presenti"],
        "scartati": esito["scartati"],
        "ore_di_statistiche": esito["statistiche"],
        "dal": ora(esito["dal"]),
        "al": ora(esito["al"]),
        "quando": dt_util.utcnow().isoformat(),
    })


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    conf = config.get(DOMAIN) or {}
    topic = conf.get("topic") or TOPIC_DEF
    mappa = {**CAMPI_DEF, **(conf.get("campi") or {})}
    coda: list = []
    annulla = None

    @callback
    def scarica(_adesso) -> None:
        nonlocal annulla
        annulla = None
        lotto = coda[:]
        coda.clear()
        if lotto:
            get_instance(hass).queue_task(ImportaTask(hass, lotto, mappa))

    @callback
    def arrivato(msg) -> None:
        nonlocal annulla
        try:
            record = json.loads(msg.payload)
        except (TypeError, ValueError):
            return
        if not isinstance(record, dict):
            return
        coda.append(record)
        if annulla:
            annulla()
        annulla = async_call_later(hass, ATTESA_SEC, scarica)

    async def avvia(_evento) -> None:
        await mqtt.async_subscribe(hass, topic, arrivato)
        await mqtt.async_subscribe(hass, topic + "/reimporta", arrivato)
        _LOGGER.info("In ascolto su %s", topic)

    hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STARTED, avvia)
    return True
