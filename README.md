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
   (se lo scarto supera 5 s) ed esegue l'eventuale comando allegato.
5. **ACK mancato:** il pacchetto finisce nel backlog su microSD e viene
   ritrasmesso ai risvegli successivi, marcato `bk=1` (fino a 60 record per
   ciclo). Se la microSD manca o rifiuta la scrittura, dalla 2.6.0 il
   pacchetto aspetta nella **riserva in RAM RTC** (6 KB, circa 43 pacchetti,
   11 ore), che sopravvive a deep sleep e riavvii ma non a una mancanza di
   corrente, ed è la prima a essere svuotata al ritorno del collegamento.
6. **Irrigazione**, se è l'ora (vedi sotto).
7. **Deep sleep** per 900 s, con il pin del relè bloccato a OFF.

Il pacchetto riporta anche lo stato del nodo: impostazioni correnti (`sOra`,
`sMin`, `sDur`, `sAuto`, `sSoil`, `slp`), esito dell'ultima irrigazione
(`irr`), litri (`acqua`, `acquaTot`), record in backlog (`bl`, **-1 = microSD
assente o guasta**), tentativi del pacchetto precedente (`txp`), pacchetti in
riserva RTC (`rs`, solo se ce ne sono), qualità del segnale. Dopo un riavvio anomalo aggiunge `fw`, `rst` (motivo) e `tp` (tappa
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
| **Collegamento nodo** | Segnale LoRa (RSSI), SNR LoRa, Record in backlog, Pacchetti in riserva, Tentativi trasmissione |
| **Diagnostica nodo** | Firmware nodo, Motivo ultimo riavvio, Tappa ultimo blocco (valorizzate solo dopo un riavvio anomalo) |
| **Ponte** | Uptime, Segnale WiFi, Pacchetti ricevuti/scartati, Memoria libera/minima/blocco, Riconnessioni WiFi/MQTT, Comandi consegnati, Conferme MQTT mancate, Riparazioni radio, ACK persi, Letture SPI incoerenti, Pacchetti CRC errato, Record arretrati recuperati, Ultima riparazione radio |
| **Comandi** | Pulsanti *Irriga ora*, *Blocca irrigazione per oggi*, *Finestra manutenzione*, *Svuota backlog*, *Riavvia nodo*; numeri *Orario irrigazione ora/minuto*, *Durata irrigazione*, *Soglia umidità terreno*, *Intervallo risveglio*; interruttore *Irrigazione automatica*; sensori *Comandi in coda* ed *Esito ultimo comando* |

Gli entity_id li ricava HA dal **nome** alla prima registrazione (per esempio
`sensor.serra_temperatura`, `number.serra_durata_irrigazione`). Il registro
entità sul server è la fonte di verità: se un id finisce con `_2`, i
riferimenti nel package non lo trovano più.

### 6.2 Package `packages/serra.yaml`

Aggiunge quello che la discovery non può fare
([sorgente](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/homeassistant/packages/serra.yaml)):

- **Contatori dell'acqua** (`utility_meter`): litri di oggi e del mese, dal
  totale cumulativo che manda il nodo, con `periodically_resetting: false`
  perché non perdano i litri arrivati dopo un periodo `unavailable`.
- **Sensori MQTT:** *Ultimo contatto* (istante di ricezione del ponte, più
  affidabile di `last_changed`) e *Ora del nodo* (per vedere se l'orologio
  della serra ha perso l'ora).
- **Sensori calcolati:**
  - **VPD** (deficit di pressione di vapore, kPa): sotto 0,4 troppo umido,
    0,8-1,2 ideale, sopra 1,6 stress.
  - **Minuti dall'ultimo contatto.**
  - **Stato generale**, una parola per la dashboard: *Tutto ok*, *Ponte
    offline*, *Nodo non risponde*, *Batteria scarica*, *Collegamento debole*,
    *MicroSD guasta*, *Dati in attesa*, *Comando in coda*.
  - **Ponte online** e **Anomalia irrigazione** (valvola aperta senza flusso).
  - **Temperatura massima e minima di oggi**, con l'ora e l'estremo di ieri.
  - **Cicli ricevuti 24h**: la percentuale dei risvegli attesi che hanno
    lasciato un dato, in tempo reale o recuperato dallo storico.
- **Irrigazione su misura** (dall'11/11/2026): ogni mattina alle 6:00 il
  package fotografa la notte appena passata (temperatura e umidità medie
  22-6, minima, pressione di vapore) e calcola la **durata consigliata** per
  la giornata, combinandola con la massima prevista in serra. Il BME280 sta
  in una scatola di plastica che di giorno fa da forno (massime di 40-50 °C
  anche a ottobre), mentre di notte è in equilibrio con l'aria: per questo
  l'umidità si prende dalla notte. La taratura (`tools/calibra_irrigazione.py`)
  usa i dati da agosto e le durate scelte a mano come metro: 120 s a fine
  settembre, 90 a metà ottobre, 50 a novembre, che stanno su una retta,
  durata = 14,4 s + 3,78 s per kPa·h di domanda (fra 20 e 300 s).
- **Pulsante *Durata automatica*** (sezione Irrigazione della vista Serra,
  spento di default). Acceso: il nodo riceve subito la durata consigliata, e
  poi ogni mattina alle 6:40. Spento: torna la **durata normale**, quella
  impostata a mano, che Home Assistant ricorda da solo: ogni durata cambiata a
  mano diventa la nuova durata normale (la riconosce dalla persona che ha
  fatto la chiamata, non dai valori che riferisce il nodo, che passano da
  "non disponibile" a ogni riconnessione del ponte). Se il ponte è giù quando
  lo si spegne, il ripristino aspetta che torni. Non è *Irrigazione
  automatica*: quella decide **se** irrigare, questo **quanto**.
- **Previsione per la serra:** dalle previsioni orarie di met.no la massima
  prevista *dentro* la serra (esterna + correzione imparata ogni sera + fino a
  19 °C col sole), la minima della notte, la pioggia; avviso *gelo previsto*
  alle 18:30. Più un **barometro Zambretti** dal BME280, che funziona anche
  senza internet.
- **Automazioni di allarme:**

  | Allarme | Quando |
  |---|---|
  | Batteria scarica | Sotto 11,2 V per 30 minuti |
  | Nodo muto | Nessun dato da oltre 2 ore, con il ponte online |
  | Irrigazione a vuoto | Esito `nessun_flusso` |
  | Irrigazione quasi a secco | Esito `ok` ma meno di 0,1 L, o meno di un quarto della volta prima |
  | Backlog in crescita | Oltre 50 record in attesa per un'ora |
  | Rischio gelo / troppo caldo | Sotto 2 °C / sopra 40 °C, al massimo ogni 12 ore |
  | Collegamento debole | Meno dell'85% dei cicli in 24 ore, per un'ora |
  | MicroSD guasta | `bl` sotto zero per 3 ore |
  | Esito comando | A ogni messaggio su `serra/nodo/cmd/res`, eseguito o rifiutato |

  Creano notifiche persistenti in HA; il package `notifiche_telegram.yaml` sul
  server le inoltra anche su Telegram.

### 6.3 Dashboard

- La vista *Serra* della Panoramica si apre con il **modello 3D
  interattivo** (`custom:serra-3d-card`, sorgente in
  [`homeassistant/www/serra3d/`](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/homeassistant/www/serra3d)):
  sole vero da `sun.sun`, pioggia/neve/nebbia/nuvole da met.no, aria colorata
  dalla temperatura, vetri appannati o brinati, onde dell'antenna in base al
  segnale, gocce dopo l'irrigazione; si gira col dito e un tocco apre il
  dettaglio. three.js è copiato in locale, niente internet necessario.
  Dalla 1.1 è pensata per l'Echo Show 8: oggetti fermi uniti in poche mesh
  (da 181 a 17-21 draw call, CPU per fotogramma da 1,8 a 0,4 ms), ombre
  ricalcolate solo quando il sole si sposta, tre livelli di qualità con
  discesa automatica sotto i 22 fotogrammi al secondo (`qualita: auto`, il
  default; si può forzare con `alta`, `media`, `bassa`).
  Sotto, la sezione *Previsione e irrigazione su misura*.

  ![La serra in 3D](Kimi_Agent_Smart%20Greenhouse%20ESP32%20LoRa/docs/serra3d-giorno.png)

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
| Ponte spento o WiFi giù | Il nodo non riceve ACK, accoda su microSD (o nella riserva RTC) e ritrasmette quando il ponte torna |
| Stallo WiFi del ponte | L'eco non torna, l'ACK non parte, il dato resta sul nodo |
| Server o HA riavviati | I dati sono retained sul broker e ricompaiono subito |
| Reset del nodo durante l'irrigazione | Relè chiuso alla prima istruzione, nessun secondo tentativo, esito `interrotta` |
| Orologio del nodo perso | Il nodo lo dichiara, non irriga e si risincronizza al primo ACK |
| Radio del ponte configurata male | Il ponte se ne accorge e si ripara da solo (dalla 2.5.0) |
| Comando premuto mentre il nodo dorme | Resta sul broker ed è eseguito al risveglio successivo |

Il punto debole: **senza microSD** (`bl = -1`) resta solo la riserva in RAM
RTC del nodo (dalla 2.6.0): circa 11 ore di pacchetti, che una mancanza di
corrente cancella. Fino alla 2.5.0 un pacchetto non confermato, senza
microSD, era perso.

Un altro limite: i record arretrati arrivano su `serra/nodo/storico`, ma Home
Assistant non sa scrivere stati nel passato, quindi i grafici restano con il
buco. Quei record contano però in *Cicli ricevuti 24h* e nel contatore
*Record arretrati recuperati*, e gli eventi importanti (esito
dell'irrigazione, riavvii) il nodo li ripete nel pacchetto attuale finché non
arrivano.

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
    └── tools/                        ← test (finestra di irrigazione, arretrati, tempo di volo, riserva, radio del ponte…)
                                        e salute_serra.py, il rapporto giornaliero dal recorder di HA
```

Nodo e ponte si compilano con l'IDE Arduino o `arduino-cli`, scheda **ESP32
Dev Module**. Il file `protocollo.h` esiste in tre copie (nodo, ponte, test) e
deve restare identico. Il pacchetto più lungo è praticamente al limite per
essere accodato: ogni campo nuovo va verificato con `tools/test_arretrati.py`.

---

## 9. Stato all'11/11/2026

| Parte | Stato |
|---|---|
| Firmware in funzione | Nodo **2.6.0**, ponte **2.6.0**, flashati l'11/11/2026 alle 11 |
| Firmware nel repository | Nodo **2.6.0**, ponte **2.6.1** (da flashare, vedi sotto) |
| Radio del ponte | **Guasta dall'11/11 alle 12:08**: il modulo LoRa del ponte non esce da sleep, dalla serra non arriva niente. Guasto elettrico sul ponte, cominciato il 10/11 con la 2.5.0 (vedi le questioni aperte) |
| Package HA | **Installato l'11/11/2026** (backup `packages/serra.yaml.bak-20261111*`), con la Panoramica nuova, il pulsante *Durata automatica* e la card 3D 1.1. La sera dell'11/11 aggiunti il sensore *Radio del ponte* e il suo avviso (backup `/config/serra.yaml.bak-20261111-radio`) |
| Quota di Home Assistant | Portata da 0 a **1000 m** l'11/11: met.no correggeva le temperature sul livello del mare e le dava 6,0 °C troppo alte |
| Irrigazione | Automatica alle 12:00 per 50 s, circa 2,0 L; il 4/11 la valvola si è aperta ma sono passati 0,002 L |
| Collegamento | Dal 30/10 al 10/11 arrivato l'87,2% dei cicli (853 su 978); 24% il 4/11 e 61% il 5/11, buco più lungo 9,3 ore |
| Batteria | 13,0-14,5 V, nessun segno di calo |

Per rifare il conto in qualunque momento, sul server:

```
docker exec -i homeassistant python3 - --giorni 10 < "Kimi_Agent_Smart Greenhouse ESP32 LoRa/tools/salute_serra.py"
```

### Cosa cambia con i firmware 2.6.0

- **Nodo:** riserva in RAM RTC quando la microSD manca; secondo tentativo di
  montaggio della microSD a 1 MHz; byte scritti sulla microSD controllati;
  orologio corretto oltre 5 s invece di 30 (il DS1307 va avanti di circa 5 s
  al giorno); anomalia di flusso anche con pochi impulsi (`FLUSSO_MIN_IMPULSI`),
  non solo con zero; `CLRBL` svuota anche la riserva.
- **Ponte:** entità *Pacchetti in riserva*, *Pacchetti CRC errato* e *Record
  arretrati recuperati* (diagnostica `storici`).
- Un nodo 2.6.0 con il ponte 2.5.0 funziona, ma `rs` nascerebbe come entità
  generica: per questo l'ordine è ponte e poi nodo.
- Il nodo 2.6.0 compila a 436.706 byte con il core esp32 3.3.12, il ponte a
  1.011.501. Una cifra molto diversa vuol dire sketch sbagliato.

### Cosa cambia con il ponte 2.6.1

L'11/11/2026 il modulo LoRa del ponte ha smesso di uscire da sleep: teneva
tutta la configurazione, rispondeva sul bus, ma non entrava in ricezione. Il
ponte 2.6.0 annotava il registro del modo fra quelli "esclusi dalla verifica",
restava con `radio_ok` a 1 e riprovava solo quando scattava la sorveglianza
del silenzio: dopo 35 minuti, poi 70, 140, 280.

- L'avvio della radio **controlla di essere entrato in ricezione**, con fino
  a tre passate sleep → standby → ricezione. Se non ci riesce è un avvio
  fallito: `radio_ok` a 0, il motivo in `radio_avv` (`la radio resta in sleep
  (modo 0x80)`, `il modulo non risponde (versione 0x00)`), e un nuovo
  tentativo **ogni 30 secondi** finché non riparte. Lo stesso se a reset
  appena fatto non tornano più di tre registri.
- Diagnostica `radio_falliti`: gli avvii falliti di fila.
- Package HA: `binary_sensor.serra_radio_del_ponte` (Problema quando la radio
  del ponte non ascolta, con il motivo fra gli attributi), l'avviso *Radio del
  ponte guasta*, e l'avviso *Nessun dato dal nodo* che in quel caso manda a
  guardare il ponte invece di nodo e antenna.
- `tools/test_radio_ponte.py` fa girare sul PC `camera_ponte/radio.cpp` con
  la libreria LoRa vera contro un SX1278 simulato (`tools/sim_radio/`) che si
  guasta negli stessi modi: 11 scenari.
- Compila a 1.012.053 byte. Il nodo non cambia.

Il firmware non ripara un filo: serve a ripartire da solo entro 30 secondi
quando il contatto torna, e a dire subito in Home Assistant dov'è il guasto.

### Dati recuperati nei grafici (12/11/2026)

Home Assistant registra ogni stato all'ora in cui arriva, quindi i record
che il nodo rimanda dopo un buio (`serra/nodo/storico`) non comparivano nei
grafici. Il componente `homeassistant/custom_components/serra_storico` li
scrive nel database del recorder all'ora in cui sono stati misurati (campo
`t`): temperatura, umidità, pressione, batteria, luce e VPD, più le
statistiche orarie delle ore toccate. Non scrive due volte lo stesso ciclo
e scarta ore impossibili. `sensor.serra_ultimo_recupero_nei_grafici` dice
quanti valori ha scritto l'ultima volta e per quale periodo.

Installazione: copiare la cartella in `/config/custom_components/`, tenere
la riga `serra_storico:` in fondo al package, riavviare. Per recuperare un
buio precedente si ripubblicano i record su `serra/nodo/storico/reimporta`.
Prova: `tools/test_storico_ha.py`. Scrive direttamente nelle tabelle del
recorder (non esiste un modo ufficiale): a un aggiornamento importante di
Home Assistant conviene rilanciare la prova.

### Installare il package HA

Il file del repository ora parte da quello installato il 27/10, quindi si può
copiare sopra senza rinominare entità: `packages/serra.yaml` del server va
sostituito (tenendone una copia), poi **riavvio** di Home Assistant, perché
la modifica agli `utility_meter` non si applica con il solo "Ricarica YAML". Prima di riavviare conviene la verifica:

```
docker exec homeassistant python3 -m homeassistant --script check_config -c /config
```

Questioni aperte:

- **Modulo LoRa del ponte che non risponde** (dal 10/11/2026 alle 08:46,
  quando il ponte è stato riavviato dopo 20 giorni). Prima il registro di
  versione letto a 0x00 o 0xFF, a tratti; dall'11/11 alle 13:34 tutti i
  registri giusti ma il modo fermo a 0x80, sleep. Non è l'antenna (darebbe
  segnale basso e CRC errati, non registri sbagliati) e non è il firmware (il
  codice della radio è lo stesso dalla 2.5.0). Da controllare, nell'ordine:
  **3,3 V e massa** del modulo, poi MISO (GPIO 19), NSS (5), SCK (18), MOSI
  (23), RST (14); togliere e ridare corrente; se non basta, cambiare il Ra-01.
- **microSD del nodo non rilevata** (`bl = -1`) dal 20/10/2026. Sospetto
  principale: modulo alimentato a 3,3 V invece di 5 V. Con il nodo 2.6.0 la
  riserva RTC limita i danni, e il montaggio a 1 MHz potrebbe bastare a
  riportarla in servizio.
- **Segnale LoRa in calo:** circa -82 dBm a fine settembre, -99 il 21/10, -110
  di media adesso, con punte a -122 (al limite della sensibilità a SF7). Il
  4-5/11 l'SNR medio è sceso a 4-6 dB e si è perso più di metà dei cicli. Da
  controllare antenne e connettori, è la causa di quasi tutti i cicli persi.
- **Filo di reset della radio del ponte** (RST → GPIO 14) da verificare:
  `rst_hw = 0` nella diagnostica.
