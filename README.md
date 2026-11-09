# Serra Smart

Monitoraggio e irrigazione automatica di una serra lontana da casa, senza WiFi
in serra: un **ESP32 a batteria** misura e irriga, parla via **LoRa a 433 MHz**
con un secondo ESP32 in casa (il **ponte**), che porta tutto su **MQTT** e
quindi in **Home Assistant**, sul server di casa.

Questo file spiega come funziona l'insieme. I dettagli stanno nei documenti in
[`docs/`](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs):

| Documento | Cosa contiene |
|---|---|
| [Guida_Serra_Smart.md](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs/Guida_Serra_Smart.md) | Installazione passo per passo, collaudo, storia dei bug, risoluzione problemi |
| [PROTOCOLLO.md](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs/PROTOCOLLO.md) | Formato dei pacchetti LoRa, comandi, esiti, topic MQTT |
| [PINOUT.md](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs/PINOUT.md) | Collegamenti, schemi e avvertenze hardware |

---

## 1. Architettura

```
 SERRA                              CASA                                 GARAGE
┌─────────────────────────┐        ┌──────────────────────────┐         ┌──────────────────────────┐
│ NODO (ESP32, batteria)  │  LoRa  │ PONTE (ESP32, USB)       │  WiFi   │ SERVER 192.168.1.36      │
│                         │ 433MHz │                          │  MQTT   │ (Ubuntu + Docker/CasaOS) │
│ BME280  DS1307  luce    │ ─────► │ riceve sempre            │ ──────► │                          │
│ tensione  flussometro   │  dati  │ pubblica su MQTT         │         │  Mosquitto  :1883        │
│ relè → elettrovalvola   │        │ aspetta l'eco del broker │         │  Home Assistant          │
│ microSD (backlog)       │ ◄───── │ ACK + ora NTP + comando  │ ◄────── │  Tailscale (da remoto)   │
│ deep sleep 15 min       │  ACK   │ crea le entità HA        │ comandi │                          │
└─────────────────────────┘        └──────────────────────────┘         └──────────────────────────┘
```

Tre pezzi, ognuno con un compito solo:

- **Nodo serra** (`serra_nodo/`): dorme il 99% del tempo. Ogni 15 minuti si
  sveglia, legge i sensori, trasmette, se è l'ora irriga, poi torna a dormire.
- **Ponte** (`camera_ponte/`): sempre acceso, sempre in ascolto. Traduce LoRa
  in MQTT e viceversa, dà l'ora al nodo e gli consegna i comandi.
- **Server**: il broker Mosquitto fa da punto d'incontro e da coda dei
  comandi; Home Assistant storicizza, calcola, mostra e avvisa.

Tutto è in una direzione sola tranne i comandi, che viaggiano al contrario
**dentro le conferme** (vedi §4).

---

## 2. La serra: il nodo

### 2.1 Hardware montato

| Componente | Funzione |
|---|---|
| ESP32 Dev Module | Cervello del nodo, firmware `serra_nodo` |
| BME280 (I2C) | Temperatura, umidità dell'aria, pressione |
| DS1307 (I2C) | Orologio con batteria tampone, tiene l'ora durante il sonno |
| Fotoresistenza (GPIO 32) | Luce in percentuale |
| Partitore (GPIO 33) | Tensione della batteria |
| Flussometro YF-S201 (GPIO 15) | Litri effettivamente erogati |
| Relè (GPIO 25) → elettrovalvola 12 V | Irrigazione, con diodo di ricircolo 1N4007 sulla bobina |
| microSD (SPI) | Coda dei pacchetti non consegnati (backlog) |
| LoRa Ra-01 SX1278 (SPI dedicato) | Radio a 433 MHz |

**Alimentazione:** batteria 12 V con regolatore di carica. La porta USB del
regolatore alimenta l'ESP32, l'uscita 12 V va al relè e all'elettrovalvola.

I sensori di umidità del terreno sono previsti dal firmware ma **non
collegati** (`USA_SOIL 0`): per questo la soglia del terreno resta a 100 e
l'irrigazione segue solo l'orario.

### 2.2 Un ciclo di veglia

1. **Sicurezza prima di tutto:** la prima istruzione chiude il relè, qualunque
   sia stato il motivo del riavvio. Parte un watchdog di 60 s sull'intero
   ciclo.
2. **Lettura dei sensori**, con controllo di plausibilità sul BME280.
3. **Trasmissione** del pacchetto (`GH1;v=2;s=<seq>;t=<epoch>;temp=…;hum=…`),
   fino a 3 tentativi, aspettando l'ACK per 2 s ogni volta.
4. **ACK ricevuto:** il nodo risincronizza l'orologio sull'ora NTP del ponte
   (se lo scarto supera 30 s) ed esegue l'eventuale comando allegato.
5. **ACK mancato:** il pacchetto finisce nel backlog su microSD e viene
   ritrasmesso ai risvegli successivi, marcato `bk=1` (fino a 60 record per
   ciclo).
6. **Irrigazione**, se è l'ora (vedi sotto).
7. **Deep sleep** per 900 s, con il pin del relè bloccato a OFF.

Il pacchetto riporta anche lo stato del nodo: impostazioni correnti (`sOra`,
`sMin`, `sDur`, `sAuto`, `sSoil`, `slp`), esito dell'ultima irrigazione
(`irr`), litri (`acqua`, `acquaTot`), record in backlog (`bl`, **-1 = microSD
assente o guasta**), tentativi del pacchetto precedente (`txp`), qualità del
segnale. Dopo un riavvio anomalo aggiunge `fw`, `rst` (motivo) e `tp` (tappa
in cui si era bloccato), e li ripete finché non arrivano.

### 2.3 Irrigazione

- **Quando:** all'ora `sOra:sMin` **locale** (il ponte manda anche il fuso
  orario), per `sDur` secondi. La finestra accetta fino a 5 minuti di anticipo
  e un'ora di ritardo, così un risveglio ogni 15 minuti non la manca mai.
- **Valori in uso:** si cambiano da Home Assistant e restano nella NVS del
  nodo, che ha la precedenza sui default compilati. Al 09/11/2026: alle 12:00
  per 50 s, circa 2 L (portata misurata circa 0,037-0,04 L/s).
- **Limiti compilati che nessun comando può superare:** 900 s per singola
  irrigazione, almeno 30 minuti fra due irrigazioni, massimo 4 al giorno,
  budget di 50 L al giorno (con il flussometro la valvola si chiude anche a
  metà).
- **Mai due volte:** il giorno viene segnato come fatto in NVS **prima** di
  aprire la valvola. Se il nodo muore a valvola aperta, al riavvio non riprova:
  meglio un giorno senza acqua che una serra allagata.
- **Esito** (`irr`): `ok`, `fuori_orario` (il caso normale), `gia_fatta`,
  `nessun_flusso` (valvola aperta ma niente acqua: serbatoio vuoto, tubo
  staccato…), `interrotta` (reset a valvola aperta), `ora_non_attendibile`,
  ecc. L'elenco completo è in [PROTOCOLLO.md §5](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs/PROTOCOLLO.md).

---

## 3. Il ponte

ESP32 alimentato via USB in casa, firmware `camera_ponte`, aggiornabile via
WiFi (OTA, hostname `serra-ponte`).

**Cosa fa:**

- **Ascolta in continuo** la radio e decodifica i pacchetti del nodo: scarta
  rumore e pacchetti con CRC errato, riconosce le ritrasmissioni (stessi `s` e
  `t`) e le conferma di nuovo senza ripubblicarle.
- **Pubblica** i dati freschi su `serra/nodo/stato` (retained) e i record
  arretrati su `serra/nodo/storico`, così una lettura vecchia non passa mai
  per attuale.
- **Conferma solo ciò che il broker ha davvero ricevuto.** Dopo i dati
  pubblica un gettone su `serra/ponte/eco`, un topic a cui è iscritto lui
  stesso, e manda l'ACK al nodo solo quando il gettone torna (entro 800 ms).
  Se il WiFi è in stallo l'ACK non parte e il dato resta al sicuro sul nodo.
- **Allega all'ACK** l'ora NTP, il fuso orario e, se c'è, un comando.
- **Crea da solo le entità in Home Assistant** (MQTT Discovery), anche per
  chiavi che non conosce: un sensore nuovo sul nodo compare in HA senza
  toccare il ponte.
- **Sorveglia la propria radio** (dalla 2.5.0): rilegge i registri del modulo
  ogni 10 s e lo riconfigura se li trova alterati, se il nodo tace per due
  cicli o se il nodo smette di sentire gli ACK per tre cicli. Nasce dal guasto
  del 9-20/10/2026, quando la radio restava configurata male finché non si
  riflashava il ponte.
- **Pubblica la propria diagnostica** ogni 60 s su `serra/ponte/diag`: uptime,
  WiFi, memoria, riconnessioni, eco mancate, riparazioni della radio,
  pacchetti scartati.
- **Last Will** su `serra/ponte/stato`: se il ponte muore, HA segna subito le
  entità come non disponibili.

**Protezioni:** watchdog del loop a 30 s, keepalive MQTT a 60 s, riconnessioni
WiFi e MQTT non bloccanti (la radio non smette di ascoltare), riavvio solo se
il WiFi resta giù per 15 minuti.

---

## 4. Come arrivano i comandi a un nodo che dorme

Il nodo non può restare in ascolto: l'unico momento in cui la sua radio
riceve è l'attesa dell'ACK, subito dopo aver trasmesso. **Il comando viaggia
lì dentro.**

```
Home Assistant ── serra/nodo/cmd/DUR = 50 (retained) ──► Mosquitto ──► ponte (in coda)

nodo  ── dati ──────────────────────────────────►  ponte
nodo  ◄── ACK;s=42;now=…;tz=3600;c=7;o=DUR;a=50 ──  ponte   (il retained viene cancellato)
        [il nodo esegue e salva in NVS]
nodo  ── dati;res=7;rc=0;det=durata=50s ────────►  ponte ──► serra/nodo/cmd/res ──► HA
```

- **Latenza:** fino a un intervallo di sonno, cioè 15 minuti. In HA il sensore
  "Comandi in coda" mostra l'attesa.
- **Coda persistente:** Home Assistant pubblica con `retain`, quindi è il
  broker a conservare il comando, anche se il ponte si riavvia.
- **Più comandi in un risveglio:** anche il pacchetto di esito riceve un ACK,
  che può portare il comando successivo.
- **Consegna at-most-once:** se l'esito si perde, il comando non viene
  ripetuto. È la direzione sicura per un'irrigazione.
- **Comandi d'azione con scadenza:** un "Irriga ora" rimasto in coda più di 30
  minuti, o trovato come residuo ritenuto all'avvio del ponte, viene scartato.
  Le impostazioni invece aspettano quanto serve.
- **Esito** (`rc`): 0 eseguito, 1 opcode sconosciuto, 2 argomenti errati,
  3 bloccato da un limite di sicurezza, 4 hardware assente, 5 errore interno.

I comandi principali sono `IRR` (irriga ora), `STOP`, `AUTO`, `ORAH`/`ORAM`,
`DUR`, `SOIL`, `SLEEP`, `WAKE`, `CLRBL`, `CAL`, `RESET`. Elenco completo in
[PROTOCOLLO.md §4](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs/PROTOCOLLO.md).

Il collegamento LoRa è in chiaro e senza autenticazione, per scelta: la
protezione sta nei limiti compilati nel nodo.

---

## 5. Il server

Server Linux in garage (`192.168.1.36`), con Docker e CasaOS.

| Servizio | Ruolo |
|---|---|
| **Mosquitto** (`eclipse-mosquitto:2`) | Broker MQTT sulla porta 1883, solo rete locale e Tailscale, autenticazione obbligatoria, messaggi persistenti. Configurazione in [`server/`](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/server) |
| **Home Assistant** | Storico, calcoli, dashboard, automazioni, notifiche |
| **Tailscale** | Accesso da fuori casa senza aprire porte |

Le credenziali (WiFi, MQTT, OTA) stanno in `camera_ponte/secrets.h`, che **non**
è nel repository: c'è solo `secrets.h.example` come modello.

### Topic MQTT

| Topic | Direzione | Contenuto |
|---|---|---|
| `serra/nodo/stato` | ponte → HA | Ultima lettura del nodo, JSON, retained |
| `serra/nodo/storico` | ponte → HA | Record arretrati dal backlog |
| `serra/nodo/cmd/<OPCODE>` | HA → ponte | Comandi, retained |
| `serra/nodo/cmd/res` | ponte → HA | Esito dei comandi |
| `serra/nodo/cmd/pending` | ponte → HA | Comandi ancora in coda |
| `serra/ponte/stato` | ponte → HA | `online`/`offline` (Last Will) |
| `serra/ponte/diag` | ponte → HA | Diagnostica del ponte e della radio |
| `serra/ponte/log` | ponte → HA | Ultima riparazione della radio |
| `serra/ponte/eco` | ponte → ponte | Eco di conferma |
| `homeassistant/…/config` | ponte → HA | MQTT Discovery |

---

## 6. Home Assistant

La parte HA è divisa in tre strati: quello che crea il ponte da solo, quello
che aggiunge il package YAML e la dashboard.

### 6.1 Entità create dal ponte (MQTT Discovery)

Nessun file YAML: compaiono da sole sotto il dispositivo **Serra**.

| Gruppo | Entità |
|---|---|
| **Clima** | Temperatura, Umidità aria, Pressione, Luce |
| **Energia** | Tensione batteria |
| **Acqua** | Acqua totale, Acqua ultima irrigazione, Esito irrigazione |
| **Collegamento nodo** | Segnale LoRa (RSSI), SNR LoRa, Record in backlog, Tentativi trasmissione |
| **Diagnostica nodo** | Firmware nodo, Motivo ultimo riavvio, Tappa ultimo blocco (valorizzate solo dopo un riavvio anomalo) |
| **Ponte** | Uptime, Segnale WiFi, Pacchetti ricevuti/scartati, Memoria libera/minima/blocco, Riconnessioni WiFi/MQTT, Comandi consegnati, Conferme MQTT mancate, Riparazioni radio, ACK persi, Letture SPI incoerenti, Ultima riparazione radio |
| **Comandi** | Pulsanti *Irriga ora*, *Blocca irrigazione per oggi*, *Finestra manutenzione*, *Svuota backlog*, *Riavvia nodo*; numeri *Orario irrigazione ora/minuto*, *Durata irrigazione*, *Soglia umidità terreno*, *Intervallo risveglio*; interruttore *Irrigazione automatica*; sensori *Comandi in coda* ed *Esito ultimo comando* |

Gli entity_id li ricava HA dal **nome** alla prima registrazione (per esempio
`sensor.serra_temperatura`, `number.serra_durata_irrigazione`). Il registro
entità sul server è la fonte di verità: se un id finisce con `_2`, i
riferimenti nel package non lo trovano più.

### 6.2 Package `packages/serra.yaml`

Aggiunge quello che la discovery non può fare
([sorgente](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/homeassistant/packages/serra.yaml)):

- **Contatori dell'acqua** (`utility_meter`): litri di oggi e del mese, dal
  totale cumulativo che manda il nodo.
- **Sensori MQTT:** *Ultimo contatto* (istante di ricezione del ponte, più
  affidabile di `last_changed`) e *Ora del nodo* (per vedere se l'orologio
  della serra ha perso l'ora).
- **Sensori calcolati:**
  - **VPD** (deficit di pressione di vapore, kPa): sotto 0,4 troppo umido,
    0,8-1,2 ideale, sopra 1,6 stress.
  - **Minuti dall'ultimo contatto.**
  - **Stato generale**, una parola per la dashboard: *Tutto ok*, *Ponte
    offline*, *Nodo non risponde*, *Batteria scarica*, *MicroSD guasta*, *Dati
    in attesa*, *Comando in coda*.
  - **Ponte online** e **Anomalia irrigazione** (valvola aperta senza flusso).
  - Sul server anche **temperatura massima e minima di oggi**, con l'ora.
- **Automazioni di allarme:**

  | Allarme | Quando |
  |---|---|
  | Batteria scarica | Sotto 11,2 V per 30 minuti |
  | Nodo muto | Nessun dato da oltre 2 ore, con il ponte online |
  | Irrigazione a vuoto | Esito `nessun_flusso` |
  | Backlog in crescita | Oltre 50 record in attesa per un'ora |
  | Esito comando | A ogni messaggio su `serra/nodo/cmd/res`, eseguito o rifiutato |

  Creano notifiche persistenti in HA; il package `notifiche_telegram.yaml` sul
  server le inoltra anche su Telegram.

### 6.3 Dashboard

- In uso: la **Panoramica** di HA, rifatta il 26/10/2026 per telefono ed Echo
  Show 8, con una vista *Casa* e una vista *Serra* (schema dell'impianto con
  valori dal vivo, grafici, irrigazione con conferma, radio, manutenzione). I
  sorgenti sono sul server, non in questo repository.
- Nel repository restano due dashboard alternative:
  [`serra_core.yaml`](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/homeassistant/lovelace/serra_core.yaml)
  (solo schede standard) e
  [`serra_hacs.yaml`](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/homeassistant/lovelace/serra_hacs.yaml)
  (Mushroom e ApexCharts).

### 6.4 Storico

Il recorder di HA tiene circa 10 giorni di stati (default). Le statistiche
orarie (medie, minimi, massimi) restano molto più a lungo e servono per i
confronti fra mesi. Il recorder salva una riga solo quando un valore cambia;
le coppie `unavailable` → valore sono riconnessioni MQTT del ponte, non eventi
della serra.

---

## 7. Cosa garantisce il sistema

| Situazione | Cosa succede |
|---|---|
| Ponte spento o WiFi giù | Il nodo non riceve ACK, accoda su microSD e ritrasmette quando il ponte torna |
| Stallo WiFi del ponte | L'eco non torna, l'ACK non parte, il dato resta sul nodo |
| Server o HA riavviati | I dati sono retained sul broker e ricompaiono subito |
| Reset del nodo durante l'irrigazione | Relè chiuso alla prima istruzione, nessun secondo tentativo, esito `interrotta` |
| Orologio del nodo perso | Il nodo lo dichiara, non irriga e si risincronizza al primo ACK |
| Radio del ponte configurata male | Il ponte se ne accorge e si ripara da solo (dalla 2.5.0) |
| Comando premuto mentre il nodo dorme | Resta sul broker ed è eseguito al risveglio successivo |

Il punto debole: **senza microSD** (`bl = -1`) un pacchetto non confermato non
ha dove andare e si perde.

---

## 8. Struttura del repository

```
Serra_smart/
├── README.md                         ← questo file
├── Arduino/                          ← sketch della prima versione (storici)
└── Kimi_Agent_Smart Greenhouse ESP32 LoRa/
    ├── serra_nodo/                   ← firmware del nodo (config.h = parametri e pinout)
    ├── camera_ponte/                 ← firmware del ponte (secrets.h non versionato)
    ├── homeassistant/
    │   ├── packages/serra.yaml       ← contatori, sensori calcolati, automazioni
    │   └── lovelace/                 ← due dashboard alternative
    ├── server/                       ← docker-compose e configurazione di Mosquitto
    ├── docs/                         ← guida, protocollo, pinout
    ├── test_protocollo/              ← autotest del protocollo su un ESP32 qualsiasi
    ├── test_flusso/                  ← test del flussometro sulla scheda del nodo
    └── tools/                        ← test Python (finestra di irrigazione, arretrati, tempo di volo…)
```

Nodo e ponte si compilano con l'IDE Arduino o `arduino-cli`, scheda **ESP32
Dev Module**. Il file `protocollo.h` esiste in tre copie (nodo, ponte, test) e
deve restare identico. Il pacchetto più lungo è praticamente al limite per
essere accodato: ogni campo nuovo va verificato con `tools/test_arretrati.py`.

---

## 9. Stato al 9/11/2026

| Parte | Stato |
|---|---|
| Firmware | Nodo **2.5.0**, ponte **2.5.0** (flashato il 20/10/2026, senza riavvii da allora) |
| Irrigazione | Automatica alle 12:00 per 50 s; ultima erogazione 2,0 L |
| Batteria | 13,1 V stasera; il 21/10 oscillava fra 13,1 e 14,5 V, senza segni di calo |

Questioni aperte:

- **microSD del nodo non rilevata** (`bl = -1`) dal 20/10/2026. Sospetto
  principale: modulo alimentato a 3,3 V invece di 5 V.
- **Segnale LoRa in calo:** circa -82 dBm a fine settembre, -99 il 21/10, -110
  oggi (SNR ancora positivo). Da controllare antenne e connettori. Il ponte ha
  dovuto riparare la radio 20 volte.
- **Filo di reset della radio del ponte** (RST → GPIO 14) da verificare:
  `rst_hw = 0` nella diagnostica.
- **Package HA sul server diverso da quello del repository:** sul server ci
  sono i sensori di temperatura massima/minima e gli id
  `sensor.serra_minuti_dall_ultimo_contatto` / `sensor.serra_ora_del_nodo`;
  nel repo ci sono la riga *MicroSD guasta* e gli id rinominati. Prima di
  copiare uno sull'altro vanno riallineati, altrimenti si rompono l'allarme
  "nodo muto" e lo stato generale.
- **Contatori dell'acqua:** l'`utility_meter` ignora l'aumento del totale
  arrivato dopo un periodo `unavailable`, quindi il mensile può sottostimare.
