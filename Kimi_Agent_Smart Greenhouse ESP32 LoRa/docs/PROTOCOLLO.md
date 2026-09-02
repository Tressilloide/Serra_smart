# Protocollo LoRa v2 — Serra Smart

Specifica del dialogo tra il **nodo serra** e il **ponte in camera**.
Il file di riferimento è [`protocollo.h`](../serra_nodo/protocollo.h), che deve
essere **identico** nelle due cartelle degli sketch.

---

## 1. Perché non più il CSV posizionale

Il protocollo v1 era:

```
GH1,42,1755500400,24.10,61.30,1012.50,78.4,12.35,0
```

Il significato di ogni campo dipendeva dalla sua **posizione**. Aggiungere un
sensore significava modificare il nodo *e* il ponte insieme e riflasharli in
modo coordinato: finché non erano allineati, i dati venivano interpretati male
senza che nulla segnalasse l'errore.

Il v2 usa coppie `chiave=valore`:

```
GH1;v=2;s=42;t=1755500400;temp=24.10;hum=61.3;soil1=42.5;acqua=3.210;bl=0
```

Il ponte non ha più bisogno di sapere quali sensori esistono: copia ogni coppia
nel JSON MQTT così com'è, e Home Assistant crea l'entità da solo anche per una
chiave mai vista prima. **Aggiungere un sensore alla serra è diventata una
modifica al solo firmware del nodo.**

Il costo è di pochi byte per pacchetto, del tutto irrilevante: a SF7/BW125 un
pacchetto pieno occupa la radio circa 400 ms, quattro volte l'ora.

Dimensioni reali: **217 byte** nella configurazione attuale (2 sensori terreno
+ flussometro), **239 byte** con 4 sensori terreno. Il margine c'è ma è tutto
usato: aggiungendo altri sensori valuta se disabilitarne qualcuno con `SENS`,
oppure alza `PROTO_MAX_PAYLOAD` fino a 255.

---

## 2. Formato dei messaggi

Separatore di campo: `;` — separatore di argomenti dentro un campo: `,`
Tetto operativo: **250 byte** (`PROTO_MAX_PAYLOAD`); il limite fisico LoRa è 255.

### 2.1 Dati — nodo → ponte

```
GH1;v=2;s=<seq>;t=<epoch>;<chiave>=<valore>;...
```

| Campo | Significato |
|---|---|
| `GH1` | Prefisso: identificativo del nodo (`NODE_ID`) |
| `v` | Versione del protocollo |
| `s` | Numero di sequenza, cresce a ogni pacchetto |
| `t` | Timestamp Unix del dato. **`0` = il nodo non conosce l'ora** |
| altre | Letture dei sensori e stato, vedi §3 |

### 2.2 ACK — ponte → nodo

```
ACK;s=<seq>;now=<epoch>
ACK;s=<seq>;now=<epoch>;c=<cmdId>;o=<OPCODE>;a=<arg1>,<arg2>
```

| Campo | Significato |
|---|---|
| `s` | Sequenza confermata: deve coincidere o l'ACK viene ignorato |
| `now` | Ora NTP del ponte. Il nodo ci risincronizza il DS1307 |
| `c` | Id del comando accodato |
| `o` | Opcode |
| `a` | Argomenti, separati da virgola |

Il ponte invia l'ACK **solo dopo** che la pubblicazione MQTT è riuscita. Se
MQTT o il WiFi sono giù, l'ACK non parte, il nodo non riceve conferma e
conserva il dato sulla microSD: è così che la catena "nessun dato perso" resta
intatta.

### 2.3 Esito di un comando — nodo → ponte

È un normale pacchetto dati con in più:

```
...;res=<cmdId>;rc=<codice>;det=<testo>
```

| `rc` | Significato |
|---|---|
| 0 | Eseguito |
| 1 | Opcode sconosciuto |
| 2 | Argomenti non validi |
| 3 | Rifiutato da un limite di sicurezza |
| 4 | Hardware richiesto assente (es. flussometro) |
| 5 | Errore interno |

Anche questo pacchetto riceve un ACK, che può contenere il **comando
successivo**: così una coda di più comandi si svuota tutta nello stesso
risveglio, senza finestre di ascolto aggiuntive.

---

## 3. Chiavi in uso

### Trasporto e controllo

`v` `s` `t` `now` `c` `o` `a` `res` `rc` `det` `trunc` `ping` `h`

`trunc=1` compare quando il pacchetto avrebbe superato il tetto e alcuni campi
sono stati **omessi**: meglio un pacchetto valido e incompleto che uno tagliato
a metà e non interpretabile.

I campi vengono scritti in ordine di importanza — sensori, poi stato, infine
diagnostica (`fw`, `rst`) — quindi è la diagnostica a saltare per prima.
In ogni caso l'omissione non fa danni: i template di Home Assistant rendono
stringa vuota per le chiavi assenti, e gli aggiornamenti vuoti vengono scartati,
così ogni entità conserva l'ultimo valore buono fino al pacchetto successivo.

`h` è **riservato a un tag di autenticazione HMAC**, oggi non generato né
verificato. Il parser lo tratta come un campo qualunque, quindi potrà essere
attivato in futuro senza rompere la compatibilità.

### Sensori (tabella in [`sensori.cpp`](../serra_nodo/sensori.cpp))

| Chiave | Unità | Descrizione |
|---|---|---|
| `temp` `hum` `pres` | °C, %, hPa | BME280 |
| `luce` | % | Fotoresistenza |
| `volt` | V | Tensione batteria |
| `soil1`…`soil4` | % | Umidità terreno |
| `acqua` | L | Litri dell'ultima irrigazione |
| `acquaTot` | L | Contatore cumulativo (`total_increasing` in HA) |

Il valore **`-127`** è la sentinella di "sensore non disponibile". Viene usata
al posto di `nan` perché `nan` non è JSON valido e romperebbe il parsing in
Home Assistant. Il ponte la converte in stringa vuota, così Home Assistant
scarta l'aggiornamento e conserva l'ultimo valore buono invece di sporcare i
grafici con dei -127.

### Stato del nodo

| Chiave | Descrizione |
|---|---|
| `irr` | Esito dell'ultimo ciclo di irrigazione (vedi §5) |
| `bl` | Record in attesa nel backlog su microSD |
| `sAuto` | Irrigazione automatica attiva (0/1) |
| `sOra` `sMin` | Orario programmato |
| `sDur` | Durata programmata in secondi |
| `sSoil` | Soglia umidità terreno (−1 = disattivata) |
| `slp` | Intervallo di deep sleep in secondi |
| `fw` `rst` | Versione firmware e motivo dell'ultimo reset (solo dopo un reset anomalo) |

I campi `s*` esistono perché Home Assistant mostri la configurazione **reale
del nodo** invece di quella che crede di aver impostato: se un comando si è
perso, la dashboard se ne accorge da sola al risveglio successivo.

---

## 4. Comandi

Home Assistant pubblica su `serra/nodo/cmd/<OPCODE>` con **`retain: true`**.
Il broker fa da coda persistente; il ponte cancella il messaggio ritenuto nel
momento in cui consegna il comando al nodo.

| Opcode | Argomenti | Effetto |
|---|---|---|
| `IRR` | `secondi` (0 = durata configurata) | Irrigazione manuale a tempo |
| `IRRVOL` | `litri[,max_sec]` | Irrigazione volumetrica (serve il flussometro) |
| `STOP` | — | Blocca l'irrigazione per il resto della giornata |
| `AUTO` | `0\|1` | Abilita/disabilita la programmazione automatica |
| `SCHED` | `ora,minuto,durata_s` | Imposta tutto insieme |
| `DUR` | `secondi` | Solo la durata |
| `ORA` | `HH:MM[:SS]` | Solo l'orario |
| `SOIL` | `soglia%` (−1 disattiva) | Soglia per l'irrigazione condizionata |
| `SLEEP` | `secondi` | Intervallo di deep sleep |
| `TIME` | `epoch` | Forza la sincronizzazione dell'orologio |
| `CAL` | `chiave,p1[,p2]` | Calibrazione (vedi sotto) |
| `SENS` | `chiave,0\|1` | Abilita/disabilita un sensore |
| `WAKE` | `secondi` | Finestra di manutenzione |
| `CLRBL` | — | Svuota il backlog |
| `RESETCFG` | — | Ripristina le impostazioni di fabbrica |
| `RESET` | — | Riavvia il nodo |

**Calibrazione (`CAL`)**

| Chiave | p1 | p2 |
|---|---|---|
| `soil1`…`soil4` | grezzo misurato in aria (secco) | grezzo misurato in acqua (bagnato) |
| `volt` | rapporto del partitore | — |
| `acqua` | impulsi per litro | — |

Esempio dal server:

```bash
mosquitto_pub -h 192.168.1.36 -u utente_serra -P <password> \
  -t "serra/nodo/cmd/CAL" -m "soil1,3010,1290" -r
```

### 4.1 Come arriva un comando a un nodo che dorme

```
nodo  ── dati ──────────────────────────────►  ponte ──► MQTT ──► Home Assistant
nodo  ◄── ACK;s=42;now=…;c=7;o=IRR;a=120 ────  ponte
        [il nodo esegue]
nodo  ── dati;res=7;rc=0;det=ok ────────────►  ponte ──► serra/nodo/cmd/res
```

Il comando viaggia **dentro l'ACK che il nodo aspetta già**: nessuna finestra
di ascolto aggiuntiva, nessun consumo extra di batteria, nessun pacchetto in
più nel caso normale.

**Latenza:** un comando viene eseguito al primo risveglio utile, quindi entro
un intervallo di deep sleep (15 minuti con la configurazione predefinita).
Home Assistant lo mostra come "in coda" finché non arriva l'esito, così
l'attesa è visibile e non sembra un malfunzionamento.

**Semantica di consegna:** *at-most-once*. Il comando viene rimosso dalla coda
alla **consegna**, non alla ricezione dell'esito. Se l'esito si perde per
strada, l'irrigazione **non** viene ripetuta: è la direzione sicura, coerente
con il principio del progetto — meglio un'irrigazione mancata che una serra
allagata.

### 4.2 Sicurezza

Il collegamento LoRa è **in chiaro e non autenticato**: è una scelta esplicita.
Di conseguenza la protezione sta interamente nei limiti compilati nel firmware
del nodo ([`config.h`](../serra_nodo/config.h)), che nessun comando può superare:

| Limite | Default | Cosa impedisce |
|---|---|---|
| `IRRIG_MAX_SEC` | 900 s | Durata massima assoluta: `IRR,99999` viene clampato |
| `IRRIG_MIN_INTERVALLO_M` | 30 min | Comandi ripetuti a raffica |
| `IRRIG_MAX_AL_GIORNO` | 4 | Irrigazioni ripetute nella stessa giornata |
| `BUDGET_LITRI_GIORNO` | 50 L | Chiude la valvola anche a metà irrigazione |

Se in futuro vorrai autenticare i comandi, il campo `h` è già previsto dal
formato: basta calcolare un HMAC troncato su nodo e ponte, senza toccare il
resto.

---

## 5. Esiti dell'irrigazione (campo `irr`)

| Valore | Significato |
|---|---|
| `ok` | Eseguita regolarmente |
| `fuori_orario` | Non è l'ora programmata (il caso normale, 95 risvegli su 96) |
| `auto_disattivata` | Programmazione automatica spenta |
| `gia_fatta` | Raggiunto il massimo giornaliero |
| `troppo_presto` | Meno di `IRRIG_MIN_INTERVALLO_M` dall'ultima |
| `terreno_umido` | Il terreno è già sopra soglia: acqua risparmiata |
| `budget_esaurito` | Superato il budget litri della giornata |
| `ora_non_attendibile` | Orologio non sincronizzato: non si rischia |
| `nessun_flusso` | Valvola aperta ma il flussometro non ha contato nulla |

`nessun_flusso` è l'allarme più utile del sistema: segnala serbatoio vuoto,
pompa guasta, filtro otturato, tubo staccato — oppure semplicemente il
flussometro scollegato.

Non scatta subito, perché l'acqua impiega qualche secondo a percorrere il tubo
e a far girare la turbina: c'è una finestra di grazia (`FLUSSO_GRAZIA_SEC`,
5 s) durante la quale gli impulsi vengono contati ma il flusso non viene
giudicato, seguita da un'attesa vera e propria (`FLUSSO_TIMEOUT_SEC`, 10 s).
Se l'irrigazione finisce prima che il conto arrivi a fine, il verdetto viene
comunque emesso alla chiusura della valvola: zero impulsi dopo la sola
finestra di grazia significa zero acqua. Senza questo controllo finale
un'irrigazione manuale breve — proprio quella che si fa per provare
l'impianto — passerebbe a vuoto senza segnalare nulla.

---

## 6. Topic MQTT

| Topic | Direzione | Retained | Contenuto |
|---|---|---|---|
| `serra/nodo/stato` | ponte → HA | sì | Dati **freschi** in JSON |
| `serra/nodo/storico` | ponte → HA | no | Record recuperati dal backlog |
| `serra/nodo/cmd/<OPCODE>` | HA → ponte | sì | Comando in coda |
| `serra/nodo/cmd/res` | ponte → HA | no | Esito di un comando |
| `serra/nodo/cmd/pending` | ponte → HA | sì | Comandi ancora in attesa |
| `serra/ponte/stato` | ponte → HA | sì | `online`/`offline` (Last Will) |
| `serra/ponte/diag` | ponte → HA | sì | Diagnostica del ponte |
| `homeassistant/…/config` | ponte → HA | sì | MQTT Discovery |

**Perché due topic distinti per i dati.** Il nodo trasmette prima il pacchetto
fresco e subito dopo la coda arretrata recuperata dalla microSD. Se finissero
tutti su `serra/nodo/stato`, Home Assistant mostrerebbe come "valore attuale"
una lettura di ore prima — l'ultima arrivata, non la più recente.

Va detto con chiarezza: **MQTT non permette di retrodatare la cronologia** di
Home Assistant. I record del backlog arriverebbero comunque con il timestamp di
ricezione. Con la separazione almeno non sporcano più lo stato corrente, e
restano disponibili su un topic dedicato per un eventuale ingest futuro
(InfluxDB o SQL, che accettano il timestamp esplicito).

---

## 7. Compatibilità con il v1

Il ponte riconosce la versione dal separatore: `;` è v2, `,` è v1. I pacchetti
v1 vengono convertiti al volo (`protoParseV1`) mappando le posizioni sulle
chiavi corrispondenti.

Serve durante l'aggiornamento: sulla microSD del nodo possono esserci record
accodati dalla versione precedente del firmware, e vanno consegnati lo stesso
invece di essere buttati. **Aggiorna prima il ponte, poi il nodo**, così il
backlog residuo viene recuperato.

---

## 8. Aggiungere un sensore — la procedura completa

1. **Nodo** — in [`sensori.cpp`](../serra_nodo/sensori.cpp) scrivi la funzione
   di lettura (`NAN` = non disponibile) e aggiungi una riga a `SENSORI[]`.
   Aggiungi il bit corrispondente a `BitSensore` in `sensori.h`.
   Per un sensore analogico non serve nemmeno una funzione nuova: riusa
   `leggiPercentuale()` con un `CtxAnalogico`, come fanno i sensori terreno.
2. **Riflasha il nodo.** Fine della parte obbligatoria: il sensore compare in
   Home Assistant al primo pacchetto, con nome uguale alla chiave.
3. *(facoltativo)* — in [`discovery.cpp`](../camera_ponte/discovery.cpp)
   aggiungi una riga a `ENTITA[]` per dargli nome leggibile, unità,
   `device_class` e icona, poi riflasha il ponte.

Il passo 3 è opzionale proprio per questo: il dato è già al sicuro e
storicizzato mentre decidi come chiamarlo.
