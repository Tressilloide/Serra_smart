# Serra Smart — Guida completa

Sistema di monitoraggio e irrigazione automatica basato su due ESP32 collegati
via LoRa e un server Linux con Home Assistant.

**Firmware 2.0** — comunicazione bidirezionale, sensori modulari, controllo
completo da Home Assistant.

---

## 1. Architettura

```
┌──────────────────────────┐      LoRa 433 MHz      ┌────────────────────────┐
│       NODO SERRA         │  ───── dati ────────►  │   PONTE IN CAMERA      │
│        (ESP32)           │                        │       (ESP32)          │
│                          │  ◄── ACK + comandi ──  │                        │
│  • BME280 (I2C)          │                        │  • LoRa Ra-01          │
│  • DS1307 RTC (I2C)      │                        │  • WiFi + MQTT         │
│  • Luce (GPIO 32)        │                        │  • Coda comandi        │
│  • Tensione (GPIO 33)    │                        │  • Ora NTP             │
│  • Terreno ×2 (34, 35)   │                        │  • Discovery HA        │
│  • Flussometro (17)      │                        │  • Sempre acceso       │
│  • microSD (backlog)     │                        │                        │
│  • Relay irrigazione     │                        └───────────┬────────────┘
│  • Deep sleep 15 min     │                                    │ WiFi / MQTT
└──────────────────────────┘                                    ▼
                                                    ┌────────────────────────┐
                                                    │   SERVER IN GARAGE     │
                                                    │     192.168.1.36       │
                                                    │  • Mosquitto           │
                                                    │  • Home Assistant      │
                                                    │  • Tailscale           │
                                                    └────────────────────────┘
```

### Come fluiscono i dati

1. Ogni 15 minuti il nodo si sveglia, legge i sensori e trasmette via LoRa.
2. Il ponte pubblica il pacchetto su MQTT e **solo dopo** risponde con un ACK.
3. Se l'ACK non arriva (ponte spento, WiFi giù, server in manutenzione), il
   nodo salva il pacchetto su microSD e lo ritrasmette al prossimo risveglio.
4. Home Assistant riceve i dati e crea le entità da solo via MQTT Discovery.

### Come fluiscono i comandi (novità della 2.0)

Il nodo dorme il 99% del tempo: non può restare in ascolto. L'unico momento in
cui la sua radio è accesa in ricezione è l'attesa dell'ACK dopo la
trasmissione. **È lì che viene infilato il comando.**

```
nodo  ── dati ─────────────────────────────►  ponte ──► MQTT ──► HA
nodo  ◄── ACK;s=42;now=…;c=7;o=IRR;a=120 ───  ponte
        [esegue]
nodo  ── dati;res=7;rc=0;det=ok ───────────►  ponte ──► serra/nodo/cmd/res
```

Costo aggiuntivo: **zero**. Nessuna finestra di ascolto in più, nessun consumo
extra, nessun pacchetto aggiuntivo nel caso normale.

Home Assistant pubblica i comandi con `retain: true`, quindi **è il broker MQTT
a fare da coda**: se premi un bottone mentre la serra dorme, il comando resta
lì e sopravvive anche a un riavvio del ponte.

**Latenza:** fino a un intervallo di deep sleep (15 minuti di default). La
dashboard mostra "comando in coda" finché non arriva l'esito, così l'attesa è
visibile e non sembra un malfunzionamento.

Poiché anche il pacchetto di esito riceve un ACK, e anche quell'ACK può portare
un comando, più comandi si svuotano a catena nello stesso risveglio.

---

## 2. File del progetto

| Percorso | Contenuto |
|---|---|
| `serra_nodo/serra_nodo.ino` | Orchestrazione del ciclo di vita del nodo |
| `serra_nodo/config.h` | **Tutti i parametri e il pinout** |
| `serra_nodo/protocollo.h` | Formato dei pacchetti (copia identica nel ponte) |
| `serra_nodo/sensori.{h,cpp}` | Registro sensori modulare |
| `serra_nodo/irrigazione.{h,cpp}` | Logica e protezioni dell'irrigazione |
| `serra_nodo/backlog.{h,cpp}` | Coda su microSD |
| `serra_nodo/impostazioni.{h,cpp}` | Persistenza in NVS |
| `serra_nodo/comandi.{h,cpp}` | Esecuzione dei comandi |
| `serra_nodo/orologio.{h,cpp}` | DS1307 e sincronizzazione |
| `serra_nodo/radio.{h,cpp}` | LoRa |
| `serra_nodo/watchdog.h` | Wrapper TWDT compatibile core 2.x e 3.x |
| `camera_ponte/camera_ponte.ino` | Ponte LoRa → WiFi → MQTT |
| `camera_ponte/secrets.h` | **Credenziali (non versionato)** |
| `camera_ponte/discovery.{h,cpp}` | Generazione entità Home Assistant |
| `camera_ponte/comandi.{h,cpp}` | Coda comandi MQTT → LoRa |
| `test_protocollo/` | Autotest del protocollo, da caricare su un ESP32 qualsiasi |
| `homeassistant/packages/serra.yaml` | Contatori, sensori calcolati, automazioni |
| `homeassistant/lovelace/serra_core.yaml` | Dashboard senza dipendenze |
| `homeassistant/lovelace/serra_hacs.yaml` | Dashboard con Mushroom + ApexCharts |
| `server/` | Broker MQTT Mosquitto |
| `docs/PROTOCOLLO.md` | Specifica del protocollo e dei comandi |
| `docs/PINOUT.md` | Pinout, schemi e avvertenze hardware |

> L'IDE Arduino richiede che ogni sketch stia in una cartella con lo stesso
> nome del file `.ino`. Non rinominare le cartelle. Tutti i `.h` e `.cpp` nella
> cartella dello sketch vengono compilati automaticamente.

---

## 3. Preparazione dell'IDE Arduino

1. Installa il core **esp32 by Espressif** (va bene sia 2.x che 3.x: il codice
   gestisce entrambi).
2. Dal Gestore librerie installa:
   - **LoRa** by Sandeep Mistry
   - **Adafruit BME280 Library** (accetta le dipendenze, *Adafruit Unified Sensor* inclusa)
   - **RTClib** by Adafruit
   - **PubSubClient** by Nick O'Leary
   - *(solo se attivi `USA_ADS1115`)* **Adafruit ADS1X15**
3. Scheda: **ESP32 Dev Module**, monitor seriale a **115200**.

---

## 4. Configurazione del ponte

```bash
cd camera_ponte
cp secrets.h.example secrets.h
```

Apri `secrets.h` e compila WiFi, MQTT e password OTA. Il file è escluso dal
versionamento: nella versione precedente le credenziali finivano dentro git.

Carica lo sketch e apri il monitor seriale. Dovresti vedere `In ascolto
continuo`, la connessione WiFi e poi MQTT. Se il server non è ancora pronto
vedrai tentativi ogni 5 secondi: è normale, e nel frattempo la serra accumula
su SD.

**Garanzie del ponte:**

- Riconnessione WiFi e MQTT **non bloccanti**: la radio non smette mai di ascoltare.
- L'ACK parte **solo dopo** la pubblicazione MQTT riuscita.
- Last Will: se il ponte muore, Home Assistant segna subito tutto come non disponibile.
- Watchdog sul loop (30 s) e riavvio **solo** se il WiFi resta giù per 15 minuti.
- Aggiornamento via WiFi (ArduinoOTA), comodo perché il ponte spesso sta dietro un mobile.

> **Cambiato rispetto alla 1.0:** il ponte non si riavvia più ogni ora. Durante
> i secondi di riavvio la radio non ascolta e, con il nodo che trasmette ogni
> 15 minuti, c'era una probabilità concreta di perdere proprio quel pacchetto.

---

## 5. Configurazione del nodo

Tutto sta in `serra_nodo/config.h`. I valori più importanti:

```c
#define SLEEP_TIME_SEC   900     // risveglio ogni 15 minuti
#define IRRIG_ORA_DEF    17      // irrigazione alle 17:00
#define IRRIG_SEC_DEF    300     // per 5 minuti
#define LORA_BAND        433E6   // deve coincidere con il ponte

#define USA_SOIL         1       // sensori terreno collegati?
#define N_SOIL           2
#define USA_FLUSSO       1       // flussometro collegato?
```

Metti a `0` i sensori che non hai ancora collegato: il codice si adatta, il
pacchetto si accorcia e Home Assistant non mostra entità fantasma.

> **Nota importante:** i valori di irrigazione qui sono solo il **default di
> primo avvio**. Una volta scritti in NVS diventano modificabili da Home
> Assistant, e da quel momento il valore in NVS ha la precedenza su quello
> compilato. Per tornare ai default usa il comando `RESETCFG`.

### 5.1 Le protezioni dell'irrigazione

Sette livelli, dal più esterno al più interno:

1. **`relayOffImmediato()` è la primissima istruzione del `setup()`.** Dopo
   qualunque reset — watchdog, brownout, panic — la valvola si chiude entro
   pochi millisecondi dall'avvio.
2. **`gpio_hold_en()` durante il deep sleep.** GPIO 25 è nel dominio RTC e
   mantiene *attivamente* il livello OFF per tutte le ore di sonno, invece di
   restare flottante.
3. **Il contatore in NVS viene aggiornato PRIMA di aprire la valvola.** Se il
   nodo muore a valvola aperta, al riavvio sa di aver già irrigato e non
   riprova. Meglio un giorno senza acqua che una serra allagata.
4. **Watchdog hardware** armato per la durata + 60 s di margine.
5. **Timeout software** con `millis()` nel ciclo di attesa.
6. **Limiti compilati** che nessun comando può superare: durata massima 900 s,
   intervallo minimo 30 minuti, massimo 4 irrigazioni al giorno, budget 50 L.
7. **Con il flussometro:** chiusura immediata al raggiungimento del budget
   giornaliero, anche a metà irrigazione.

> **Cambiato rispetto alla 1.0:** il marker anti-doppione stava solo sulla
> microSD, quindi **senza SD l'irrigazione non partiva mai**. Ora è in NVS, che
> sopravvive a deep sleep e blackout ed è indipendente dalla scheda.

### 5.2 Irrigazione condizionata al terreno

Imposta la soglia da Home Assistant (`number.serra_soglia_umidita_terreno`) o
con `SOIL,<percentuale>`. Il nodo irriga solo se il sensore **più secco** è
sotto soglia.

Si usa il minimo e non la media di proposito: se anche una sola zona è
asciutta conviene bagnare, mentre una media alta potrebbe nascondere una zona
completamente secca.

`-1` disattiva la condizione: si torna al solo criterio orario.

### 5.3 Irrigazione volumetrica

Con il flussometro puoi chiedere litri invece di minuti:

```bash
mosquitto_pub -t "serra/nodo/cmd/IRRVOL" -m "5,600" -r
```

5 litri, con un tetto di 600 secondi. Se l'acqua non arriva, il nodo chiude
comunque e segnala `nessun_flusso`.

### 5.4 Calibrazioni

| Cosa | Come |
|---|---|
| **Tensione** | Misura la batteria col multimetro (`V_reale`), leggi il valore in HA (`V_letta`), calcola `nuovo = attuale × V_reale / V_letta`, invia `CAL,volt,<nuovo>` |
| **Terreno** | Leggi il grezzo con il sensore in aria e in un bicchiere d'acqua, poi `CAL,soil1,<aria>,<acqua>` |
| **Flussometro** | Riempi un contenitore da 1 litro, conta gli impulsi, `CAL,acqua,<impulsi>` |

Nessuna di queste richiede di ricompilare: i valori vivono in NVS.

### 5.5 Orologio

Se il DS1307 ha perso l'ora (CR2032 scarica), il nodo **lo dichiara e non
irriga**, poi si risincronizza da solo: il ponte allega l'ora NTP a ogni ACK e
il DS1307 si corregge al primo contatto.

> **Cambiato rispetto alla 1.0:** prima, con l'orologio perso, veniva impostata
> l'*ora di compilazione*. Il risultato era un orologio che sembrava
> funzionante ma segnava un momento arbitrario nel passato: timestamp falsi in
> Home Assistant e finestra di irrigazione imprevedibile.

---

## 6. Server

### 6.1 Broker MQTT

Copia la cartella `server/` sul server, poi:

```bash
cd server
docker compose up -d
```

Oppure, da CasaOS: App Store → Custom Install → importa `docker-compose.yml`.

### 6.2 Utente e password MQTT

```bash
docker run --rm -it -v "$PWD/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2 \
  mosquitto_passwd -c /mosquitto/config/password.txt utente_serra
docker compose restart
```

Metti la stessa password in `secrets.h` del ponte.

### 6.3 Integrazione MQTT in Home Assistant

Impostazioni → Dispositivi e servizi → Aggiungi integrazione → **MQTT**
Broker `192.168.1.36`, porta `1883`, utente e password creati sopra.

La discovery è attiva di default: appena il ponte si collega compare il
dispositivo **Serra** con sensori **e comandi**.

### 6.4 Package e dashboard

> **Prima di tutto: questa sezione è FACOLTATIVA.**
> Dopo il punto 6.3 il sistema già funziona: sensori e comandi compaiono da
> soli in Home Assistant grazie alla MQTT Discovery del ponte, senza scrivere
> un solo file YAML. Quello che segue aggiunge le cose che la discovery *non
> può* fare, ed è diviso in due parti indipendenti che puoi fare in qualsiasi
> ordine, o non fare affatto.

|  | Cosa aggiunge | Serve? |
|---|---|---|
| **A. Package** | Contatori acqua giornaliero/mensile, VPD, ultimo contatto, automazioni di allarme | No, ma è la parte più utile |
| **B. Dashboard** | La disposizione grafica delle schede | No: senza, HA genera da solo una vista del dispositivo "Serra" |

---

#### Dove sta la cartella di configurazione

Entrambe le parti richiedono di mettere dei file dentro la cartella di
configurazione di Home Assistant, quella dove sta `configuration.yaml`.
Il percorso dipende da come hai installato HA:

| Installazione | Percorso |
|---|---|
| **Docker / CasaOS** (il tuo caso) | La cartella che hai montato su `/config` nel container. La trovi con `docker inspect <nome_container> \| grep -A3 '"/config"'` |
| Home Assistant OS / Supervised | `/config` (visibile dagli add-on) |
| Core in venv | `~/.homeassistant` |

**Come modificarne i file.** Se non hai già un modo comodo:

- **Docker/CasaOS**: la cartella è sul filesystem del server, quindi ci arrivi
  via SSH, oppure con l'editor di file di CasaOS, oppure via Samba se l'hai
  configurato. È la strada più diretta nel tuo caso.
- **HA OS**: Impostazioni → Add-on → installa **File editor** oppure
  **Studio Code Server**, che ti danno un editor dentro l'interfaccia di HA.

---

#### A. Installare il package

Un "package" è semplicemente un file YAML che raggruppa più tipi di
configurazione insieme (sensori, contatori, automazioni) invece di doverli
spargere in sezioni diverse di `configuration.yaml`. È il modo pulito di
aggiungere un blocco di roba che riguarda un solo progetto.

**Passo 1** — abilita i package. Apri `configuration.yaml` e controlla se c'è
già una riga `homeassistant:` all'inizio.

Se **non** c'è, aggiungi in cima al file:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Se **c'è già**, aggiungi solo la riga `packages:` sotto, rispettando
l'indentazione di due spazi:

```yaml
homeassistant:
  name: Casa              # <- roba tua che c'era già
  latitude: 45.0
  packages: !include_dir_named packages    # <- aggiungi questa
```

**Passo 2** — crea la cartella `packages` **dentro** la cartella di
configurazione, accanto a `configuration.yaml`:

```
<config>/
  configuration.yaml
  packages/          <- da creare, se non esiste
```

**Passo 3** — copia dentro il file:

```
homeassistant/packages/serra.yaml   →   <config>/packages/serra.yaml
```

**Passo 4** — verifica e ricarica. In Home Assistant:
Strumenti per sviluppatori → scheda **YAML** → premi prima
**"Controlla configurazione"**, e solo se dice che è valida premi
**"Ricarica tutto"**. Se segnala un errore, la configurazione vecchia resta
attiva: non hai rotto nulla.

> Se "Controlla configurazione" non compare, ti manca `default_config:` in
> `configuration.yaml`. In alternativa riavvia Home Assistant.

**Passo 5** — verifica che sia andata: Strumenti per sviluppatori → **Stati**,
scrivi `serra_vpd` nel filtro. Se compare `sensor.serra_vpd`, il package è
attivo.

> **Se qualche sensore risulta `unavailable`** è quasi sempre un entity_id che
> non coincide. Home Assistant li genera da "nome dispositivo + nome entità",
> ma se in passato esistevano entità con lo stesso nome li numera
> (`sensor.serra_temperatura_2`). Filtra per `serra` in Stati, guarda i nomi
> veri e correggili dentro `serra.yaml`.

---

#### B. Installare la dashboard

**Passo 1** — scegli quale delle due:

- `serra_core.yaml` — solo schede incluse in Home Assistant. Nessuna
  installazione, nessuna manutenzione. **Parti da questa.**
- `serra_hacs.yaml` — più curata graficamente. Richiede che tu installi prima
  da **HACS → Frontend** le schede **Mushroom** e **apexcharts-card**, e poi
  ricarichi la pagina con CTRL+F5. Se le incolli senza averle installate vedi
  dei riquadri rossi "Custom element doesn't exist".

**Passo 2** — crea una dashboard vuota:
Impostazioni → Dashboard → **+ Aggiungi dashboard** → **Nuova dashboard da
zero** → chiamala "Serra".

**Passo 3** — aprila, poi in alto a destra la matita (**Modifica dashboard**).

**Passo 4** — sempre in alto a destra, i tre puntini ⋮ → **Editor di
configurazione raw**.

**Passo 5** — cancella tutto quello che c'è nell'editor, incolla il contenuto
del file scelto, **Salva**.

> A differenza del package, la dashboard non richiede di copiare file sul
> server: copi e incolli il testo dentro l'interfaccia di Home Assistant.
> Questi due file stanno nel repository solo per comodità tua.

### 6.5 Verifica del traffico

```bash
docker exec -it mosquitto mosquitto_sub -u utente_serra -P <password> -t "serra/#" -v
```

---

## 7. Aggiungere un sensore

Il punto centrale della versione 2.0: **aggiungere un sensore è una modifica al
solo firmware del nodo.**

1. In `sensori.cpp` scrivi la funzione di lettura (ritorna `NAN` se il sensore
   non risponde) e aggiungi **una riga** a `SENSORI[]`.
2. Aggiungi il bit corrispondente all'enum `BitSensore` in `sensori.h`.
3. Riflasha il nodo.

Il sensore compare in Home Assistant al primo pacchetto, con nome uguale alla
chiave, perché il ponte crea un'entità generica per ogni chiave che non
conosce. Quando vuoi nome leggibile, unità e icona, aggiungi una riga alla
tabella `ENTITA[]` in `camera_ponte/discovery.cpp` e riflasha il ponte — ma nel
frattempo il dato è già al sicuro e storicizzato.

Per un sensore analogico non serve nemmeno una funzione nuova: riusa
`leggiPercentuale()` con un `CtxAnalogico`, come fanno i sensori terreno.

Dettagli in [PROTOCOLLO.md §8](PROTOCOLLO.md#8-aggiungere-un-sensore--la-procedura-completa).

---

## 8. Collaudo prima del montaggio

Da fare sul banco, con il relay che pilota una lampadina o solo il LED del modulo.

| # | Prova | Come |
|---|---|---|
| 1 | **Irrigazione** | Metti `IRRIG_SEC_DEF 10` e `IRRIG_ORA_DEF` all'ora corrente. Verifica: relay ON → 10 s → OFF; secondo risveglio nella stessa ora che **non** riapre |
| 2 | **Sicurezza al reset** | Premi EN a valvola aperta: al boot il relay dev'essere OFF entro pochi ms e non deve riprovare |
| 3 | **Limite di durata** | `mosquitto_pub -t serra/nodo/cmd/IRR -m "99999" -r` → il seriale deve dire "clampata al tetto di 900 s" |
| 4 | **Comandi** | Premi "Irriga ora" in HA, guarda il seriale del ponte (consegna nell'ACK) e `serra/nodo/cmd/res` |
| 5 | **Backlog** | Spegni il ponte, metti `SLEEP_TIME_SEC 60`, accumula oltre 120 pacchetti, riaccendi il ponte: devono arrivare **tutti** |
| 6 | **Flussometro** | Fai girare la turbina a mano e conta gli impulsi; calibra con 1 litro |
| 7 | **Terreno** | Calibra in aria e in acqua con `CAL` |
| 8 | **Orologio** | Sballa l'ora del DS1307 e verifica che venga corretta dal campo `now` dell'ACK |
| 9 | **Protocollo** | Carica `test_protocollo/` su un ESP32 qualsiasi (anche senza nulla collegato) e leggi il monitor seriale: devono passare tutte le verifiche |

La prova 5 **fallisce sul firmware 1.0**: è la riproduzione del bug descritto
al punto successivo.

---

## 9. Cosa è cambiato dalla versione 1.0

### Bug corretti

| Problema | Conseguenza | Correzione |
|---|---|---|
| **Perdita di dati nel backlog** | `leggiBacklog()` leggeva max 100 record, `riscriviBacklog()` cancellava l'**intero file** e riscriveva solo quelli. Tutto oltre il centesimo spariva: con 96 pacchetti al giorno bastava un giorno di ponte offline | La coda non consegnata viene copiata **dal file**, mai ricostruita dalla RAM |
| **Il backlog sporcava lo stato corrente** | Fresco e storico finivano sullo stesso topic; HA mostrava come "adesso" una lettura di ore prima | Topic `serra/nodo/storico` separato |
| **Riavvio orario del ponte** | Durante il riavvio la radio non ascoltava: pacchetti persi senza motivo | Watchdog + riavvio solo se il WiFi resta giù 15 minuti |
| **Senza microSD niente irrigazione** | Il marker anti-doppione stava solo su SD | Marker in NVS |
| **Watchdog solo durante l'irrigazione** | Un blocco su SD, LoRa o I2C lasciava il nodo appeso fino a scaricare la batteria | Watchdog globale su tutto il `setup()` |
| **Nessun anti-duplicati** | Un ACK perso causava doppioni nello storico e nel conteggio dell'acqua | Memoria delle ultime 16 coppie (seq, timestamp) |
| **Watchdog mai riconfigurato su core 3.x** | `esp_task_wdt_init()` falliva in silenzio e il timeout restava 5 s | Fallback su `esp_task_wdt_reconfigure()` |
| **Ora di compilazione come ripiego** | Orologio apparentemente valido ma sbagliato | Il nodo dichiara l'ora non attendibile e aspetta il ponte |
| **Stato MQTT non retained** | Dopo un riavvio di HA i sensori restavano vuoti fino a 15 minuti | `retain: true` sullo stato |
| **Credenziali nel repository** | Password MQTT dentro git | `secrets.h` in `.gitignore` |
| **Buffer del pacchetto da 128 byte** | Troncamento silenzioso appena si aggiungevano sensori | Tetto a 250 byte con flag `trunc` esplicito |

### Funzionalità nuove

- Comandi da Home Assistant, consegnati sull'ACK
- Protocollo `chiave=valore`, con compatibilità in lettura per il v1
- Registro sensori modulare e discovery generica
- Sensori umidità terreno con calibrazione e alimentazione commutata
- Flussometro, irrigazione volumetrica, budget litri, allarme "nessun flusso"
- Irrigazione condizionata all'umidità del terreno
- Impostazioni persistenti in NVS, modificabili da remoto
- Sincronizzazione automatica dell'orologio
- Diagnostica del ponte e aggiornamento OTA
- Package Home Assistant e due dashboard

---

## 10. Risoluzione problemi

| Sintomo | Cosa controllare |
|---|---|
| `[LoRa] ERRORE: modulo non trovato` | Cablaggio SPI, NSS su GPIO 5, alimentazione 3,3 V, **antenna montata** |
| Il nodo a volte non parte | GPIO 12 alto al boot: manca il pulldown da 10k. Vedi [PINOUT.md §2.4](PINOUT.md) |
| Nessun ACK | Stessi `LORA_BAND`, SF, BW e CR sui due sketch? Ponte acceso e in ascolto? |
| `[SD] ERRORE: scheda non trovata` | FAT32, CS su GPIO 4, modulo a 3,3 V |
| Sensori a −127 | Il sensore non risponde. BME280: indirizzo 0x76 o 0x77 (li prova entrambi) |
| Irrigazione mai eseguita | Guarda `sensor.serra_esito_irrigazione`: ti dice **il motivo** |
| `ora_non_attendibile` | CR2032 del DS1307 scarica. Si corregge da sola al primo contatto col ponte |
| `terreno_umido` | Sta funzionando: il terreno è sopra soglia. Abbassa la soglia o mettila a −1 |
| `nessun_flusso` | Serbatoio vuoto, pompa guasta, filtro otturato o flussometro scollegato |
| Comando premuto ma non succede nulla | Normale: viene eseguito al prossimo risveglio. Controlla `sensor.serra_comandi_in_coda` |
| Comando rifiutato con `rc=3` | Bloccato da un limite di sicurezza. Il motivo è nel campo `det` |
| Entità mancanti in HA | Riavvia il ponte: ripubblica tutta la discovery a ogni riconnessione MQTT |
| `rc=5` / `rc=4` in MQTT | Password errata (5) o broker irraggiungibile (4) |
| Riavvii con motivo 9 (brownout) | L'alimentazione non regge i picchi della radio |
| Luce al 90% di notte | Modulo con logica invertita: metti `LUCE_INVERTITA 1` |

---

## 11. Prossimi passi possibili

- **Sensori UV e irraggiamento** — il framework è pronto, serve solo la
  funzione di lettura. Vedi [PINOUT.md §4.2](PINOUT.md)
- **ADS1115** per più canali analogici, già supportato da un `#define`
- **Autenticazione dei comandi** — il campo `h` è previsto dal protocollo
- **MOSFET generale** per togliere corrente a tutte le periferiche nel sonno
- **Ingest su InfluxDB** dal topic `serra/nodo/storico`, per recuperare
  davvero la cronologia dei dati arretrati con il loro timestamp originale
