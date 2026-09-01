# Pinout e collegamenti — Serra Smart

Riferimento: [`serra_nodo/config.h`](../serra_nodo/config.h) e
[`camera_ponte/config.h`](../camera_ponte/config.h).

---

## 1. Nodo serra (ESP32)

### Pin già impegnati

| Funzione | GPIO | Note |
|---|---|---|
| I2C SDA / SCL | 21 / 22 | BME280 + DS1307 (+ espansioni future) |
| microSD (VSPI) | SCK 18, MISO 19, MOSI 23, CS 4 | Bus SPI n°1 |
| LoRa Ra-01 (HSPI) | SCK 27, MISO 12, MOSI 13, NSS 5, RST 14, DIO0 26 | Bus SPI n°2, dedicato |
| Relay irrigazione | 25 | Pin del dominio RTC |
| Fotoresistenza | 32 | ADC1 |
| Partitore tensione | 33 | ADC1 |

### Pin aggiunti nella versione 2.0

| Funzione | GPIO | Note |
|---|---|---|
| Umidità terreno 1 | 34 | ADC1, **solo input** |
| Umidità terreno 2 | 35 | ADC1, **solo input** |
| *(riserva analogica)* | 36 (VP), 39 (VN) | ADC1, solo input |
| Flussometro YF-S201 | 17 | Interrupt su fronte di discesa |
| Alimentazione sensori | 15 | *Predisposto ma **non attivo**, vedi 2.1.1* |

Restano liberi: **15** (finche' l'alimentazione commutata resta disattivata),
**16**, **2**, **0**.

---

## 2. Avvertenze hardware — leggere prima di collegare

### 2.1 Sensori di umidità del terreno → alimentarli a 3,3 V, NON a 5 V

L'uscita analogica di questi moduli **segue la tensione di alimentazione**:
alimentati a 5 V manderebbero fino a 5 V sul pin ADC dell'ESP32, che tollera al
massimo 3,3 V. Il pin si danneggia.

Il modulo dichiara 3,3–12 V, quindi alimentarlo a 3,3 V è nelle specifiche.

```
Modulo terreno        ESP32
  VCC  ────────────►  rail 3,3 V commutato (vedi 2.1.1)
  GND  ────────────►  GND
  AO   ────────────►  GPIO 34  (e GPIO 35 per il secondo)
  DO   ─────  non collegato
```

**Perché la DO resta scollegata:** la soglia si regola con un trimmer sul
modulo, è imprecisa e va rifatta a mano a ogni cambio di terreno. L'uscita
analogica con calibrazione software (comando `CAL`) è più affidabile e si
regola da Home Assistant senza toccare nulla.

#### 2.1.1 Alimentazione commutata — predisposta ma disattivata

> **Stato attuale: `PIN_PWR_SENSORI` vale `-1`**, quindi i sensori restano
> costantemente alimentati e non serve cablare nessun interruttore.
> Il codice c'è tutto ed è condizionato a quel valore: per attivarla basta
> rimettere `15` in `config.h` e collegare il relay o il MOSFET.

I sensori a due punte metalliche sono resistivi: sotto tensione continua le
punte si **corrodono per elettrolisi** e in poche settimane iniziano a dare
letture sbagliate, per poi smettere del tutto.

**Il motivo non è il consumo.** Due sensori assorbono circa 16 mA, del tutto
trascurabili accanto agli ~80 mA che l'ESP32 assorbe mentre è sveglio, e in
ogni caso solo per i pochi secondi del risveglio. Il motivo è la durata delle
sonde.

Il sintomo da tenere d'occhio quindi non è la batteria che cala prima del
previsto, ma **l'umidità del terreno che deriva verso valori sempre più bassi
e che i due sensori smettono di concordare fra loro**. Quando succede, o
sostituisci le sonde o attivi l'alimentazione commutata (meglio entrambe).

Se e quando decidi di attivarla, servono un interruttore comandato dall'ESP32
e il pin `PIN_PWR_SENSORI` (GPIO 15) per pilotarlo. Vanno bene entrambe le
soluzioni qui sotto.

**Opzione A — un canale libero del modulo relay** (nessun componente da comprare)

Se il tuo modulo relay ha quattro canali e ne usi uno solo per l'irrigazione,
un secondo canale fa benissimo da interruttore:

```
  Modulo relay                         Sensori terreno
    IN2   ◄──────── GPIO 15
    COM2  ◄──────── 3,3 V dell'ESP32
    NO2   ────────────────────────────► VCC di entrambi i sensori
```

Usa il contatto **NO** (normalmente aperto): a relay diseccitato i sensori
sono spenti, che è lo stato sicuro.

In `config.h` deve esserci `#define PWR_SENSORI_ON LOW`, perché i moduli relay
comuni sono **attivi bassi** come quello dell'irrigazione. È già il valore
predefinito.

Tre cose da sapere su questa soluzione:

- **Durata del relay.** Scatta due volte per risveglio, circa 190 volte al
  giorno. Sembra tanto, ma sta commutando 16 mA a 3,3 V contro i 10 A per cui
  è costruito: l'usura elettrica dei contatti è praticamente nulla e conta
  solo quella meccanica, dell'ordine di 10 milioni di manovre. Sono decenni.
  Il firmware inoltre memorizza la lettura del terreno e la riusa nello stesso
  risveglio, quindi nella pratica lo scatto è **uno solo**, tranne nel ciclo
  in cui irriga davvero.
- **Contatti e correnti basse.** I contatti argentati vogliono una corrente
  minima per "sfondare" l'ossido che si forma nel tempo. 16 mA è nella fascia
  bassa: se dopo qualche mese le letture del terreno diventassero
  intermittenti, è questa la causa, e la soluzione è passare a un MOSFET.
- **Rumore.** Sentirai un clic in serra a ogni risveglio. Innocuo.

**Opzione B — MOSFET P lato alto** (la soluzione pulita)

```
        3,3 V
          │
          ├──── S ──┐
                    │  MOSFET P (es. AO3401)
          ┌──── D ──┘
          │
          └────────► VCC dei sensori terreno

  GPIO 15 ──[10k]──► G     (+ resistenza da 100k tra G e S)
```

Con questa opzione metti `#define PWR_SENSORI_ON HIGH` in `config.h`.
Consumo nullo, nessuna parte in movimento, nessun limite di durata.

> **Perché GPIO 15 e non 16.** GPIO 15 appartiene al dominio RTC, quindi il
> firmware può **mantenerne** il livello anche durante il deep sleep
> (`gpio_hold_en`). Con un relay è importante: un pin lasciato flottante per
> 15 minuti potrebbe farlo eccitare da solo, tenendo i sensori sotto tensione
> tutta la notte — cioè esattamente quello che l'alimentazione commutata deve
> evitare. In più GPIO 15 ha un pull-up interno al reset, quindi si presenta
> ALTO: con un relay attivo basso questo significa "spento" già prima che il
> firmware parta, senza bisogno di resistenze esterne.

### 2.2 Flussometro YF-S201 → serve un partitore, l'uscita è a 5 V

Il sensore va alimentato a 5 V e la sua uscita a effetto Hall commuta a 5 V:
**non è collegabile direttamente** a un GPIO dell'ESP32.

```
  YF-S201                                    ESP32
   Rosso  ──► +5 V
   Nero   ──► GND ────────────────────────►  GND
   Giallo ──┬──[ 10k ]──┬──[ 15k ]── GND
            │           │
         (segnale)      └──────────────────►  GPIO 17
                        │
                       ═╪═ 100 nF  ← FACOLTATIVO, vedi sotto
                        │
                       GND
```

Con 10 kΩ / 15 kΩ i 5 V diventano circa 3,0 V: dentro le specifiche.
In alternativa va bene un level shifter bidirezionale.

**Il condensatore da 100 nF è facoltativo: puoi partire senza.** Serve a
filtrare i disturbi che la pompa induce sul cavo di segnale, ma il firmware ha
già un filtro software: l'interrupt scarta gli impulsi che arrivano a meno di
1 ms l'uno dall'altro (`FLUSSO_MIN_INTERVALLO_US` in `config.h`), mentre alla
portata massima del YF-S201 gli impulsi veri distano circa 4,4 ms.

Come accorgersi se servisse davvero: il conteggio dei litri risulterebbe
**più alto** del reale, perché i disturbi vengono contati come impulsi. In quel
caso, in ordine di efficacia:

1. allontana il cavo del flussometro da quello della pompa, e non farli correre
   paralleli nella stessa canalina;
2. alza `FLUSSO_MIN_INTERVALLO_US` a 2000 e riflasha;
3. aggiungi il condensatore.

Un secondo sintomo, opposto e più insidioso: se il flussometro contasse
impulsi **a valvola chiusa**, si tratterebbe quasi certamente di disturbi e non
di una perdita d'acqua — e lì il condensatore serve.

**Calibrazione:** il datasheet dà `F = 7,5 × Q[L/min]`, cioè 450 impulsi/litro,
ma varia parecchio da esemplare a esemplare. Riempi un contenitore da 1 litro e
usa il conteggio reale con `CAL,acqua,<impulsi>`.

### 2.3 GPIO 17 → non disponibile sui moduli WROVER

Sui moduli **ESP32-WROVER** (quelli con PSRAM) i GPIO 16 e 17 sono cablati alla
memoria PSRAM e non sono utilizzabili. Il firmware lo rileva al boot con
`psramFound()` e stampa un avviso sul monitor seriale.

Riguarda solo il flussometro, perché l'alimentazione dei sensori sta su
GPIO 15. Se hai un WROVER, in `config.h` sposta `PIN_FLUSSO` su un pin libero
con interrupt: **GPIO 4** è già usato dal CS della microSD, quindi la scelta
più pratica è rinunciare a un canale ADC1 e usare **GPIO 39**, che va bene
come ingresso digitale.

```c
#define PIN_FLUSSO  39   // solo su moduli WROVER
```

### 2.4 GPIO 12 (LoRa MISO) → il pin più delicato della scheda

GPIO 12 è lo **strapping pin MTDI**. Se al reset si trova a livello alto,
l'ESP32 configura VDD_SDIO a 1,8 V e la scheda **può non avviarsi affatto**.

Il rischio è concreto: durante il boot il pin NSS del modulo LoRa è flottante,
quindi l'SX1278 potrebbe considerarsi selezionato e pilotare la linea MISO
proprio in quell'istante.

**Rimedio (due resistenze):**

```
  GPIO 12 ──[ 10k ]── GND     (pulldown: garantisce il livello basso al reset)
  GPIO  5 ──[ 10k ]── 3,3 V   (pull-up su NSS: tiene il LoRa deselezionato)
```

Se il nodo a volte non parte e si sblocca togliendo e ridando corrente, è quasi
certamente questo.

### 2.5 Relay → attivo basso e pin del dominio RTC

GPIO 25 è stato scelto perché appartiene al dominio RTC: può **mantenere
attivamente** il livello logico durante il deep sleep (`gpio_deep_sleep_hold_en`),
invece di restare flottante per ore.

Il codice assume un modulo **attivo basso** (`LOW` = relay eccitato), il tipo
più diffuso. Se il tuo è attivo alto, inverti in `config.h`:

```c
#define RELAY_ON  HIGH
#define RELAY_OFF LOW
```

Con un modulo attivo basso, un pin flottante corrisponde a relay **spento**:
anche nell'istante precedente all'avvio del firmware la pompa non può partire.

Consigliato un modulo relay con **fotoaccoppiatore e alimentazione JD-VCC
separata**, per isolare la pompa dall'ESP32. Se la pompa è a 220 V, rispetta le
distanze di isolamento; se non hai esperienza con la rete elettrica, usa una
pompa o un'elettrovalvola a 12 V.

### 2.6 Bus I2C → attenzione ai pull-up in parallelo

Ogni modulo I2C porta i propri pull-up. Con tre o più moduli sullo stesso bus
le resistenze si sommano in parallelo, il valore risultante scende troppo e i
fronti si deformano. Con 3+ moduli, dissalda i pull-up di tutti tranne uno.

Il DS1307 è un modulo a 5 V e di solito ha pull-up verso 5 V: rimuovili, oppure
alimenta il modulo a 3,3 V.

### 2.7 microSD

Modulo **a 3,3 V con level shifter integrato** (i più comuni). Alimentalo a 5 V
solo se ha il regolatore a bordo. Scheda formattata **FAT32**.

### 2.8 LoRa Ra-01

**Monta sempre l'antenna prima di dare corrente**: trasmettere senza antenna
danneggia lo stadio finale. Il Ra-01 classico è a 433 MHz (SX1278); se hai la
versione a 868 MHz cambia `LORA_BAND` in `868E6` **su entrambi gli sketch**.

---

## 3. Ponte in camera (ESP32)

| Funzione | GPIO |
|---|---|
| LoRa SCK / MISO / MOSI | 18 / 19 / 23 |
| LoRa NSS / RST / DIO0 | 5 / 14 / 26 |

Nessun altro collegamento. Alimentalo con un alimentatore USB: resta sempre
acceso e in ascolto.

---

## 4. Espansione futura

### 4.1 Altri sensori analogici — ADS1115

Restano solo due canali ADC1 liberi (36 e 39). Quando serviranno più ingressi
analogici, un **ADS1115** sul bus I2C (indirizzo `0x48`) ne aggiunge quattro
senza occupare un solo pin nuovo, con 16 bit di risoluzione e guadagno
programmabile fino a ±0,256 V — indispensabile per un piranometro, che dà
qualche millivolt.

Il codice è già pronto: in `config.h` metti `USA_ADS1115` a 1, installa la
libreria "Adafruit ADS1X15" e cambia il campo `canale` dei `CtxAnalogico` da
GPIO a indice 0-3. Non c'è altro da toccare: `leggiCanaleRaw()` è l'unico punto
che sa quale hardware c'è sotto.

Fino a quattro ADS1115 (`0x48`–`0x4B`) = 16 canali analogici.

### 4.2 Sensori UV e irraggiamento

Indirizzi I2C liberi e senza conflitti con BME280 (`0x76`) e DS1307 (`0x68`):

| Sensore | Indirizzo | Cosa misura |
|---|---|---|
| LTR390-UV | `0x53` | Indice UV + luce ambiente, digitale |
| VEML6075 | `0x10` | UVA/UVB |
| BH1750 | `0x23` o `0x5C` | Lux calibrati |
| ADS1115 | `0x48` | 4 canali analogici (GUVA-S12SD, piranometro) |

Un sensore I2C non richiede alcun pin nuovo: aggiungi la funzione di lettura e
una riga alla tabella `SENSORI[]`, come descritto in
[PROTOCOLLO.md §8](PROTOCOLLO.md#8-aggiungere-un-sensore--la-procedura-completa).

Per l'irraggiamento in W/m² la strada corretta è un piranometro con uscita in
millivolt letto dall'ADS1115 ad alto guadagno. Con un BH1750 si ottiene una
stima ragionevole dividendo i lux per circa 120 (fattore tipico della luce
solare), ma resta appunto una stima.

### 4.3 Consumi in deep sleep

Il nodo dorme quasi sempre, ma **le periferiche consumano anche a ESP32
spento**: il modulo microSD e il modulo relay possono assorbire diversi mA, che
in un mese contano più di tutti i risvegli messi insieme. Se vai a batteria, il
passo successivo è un MOSFET che tolga alimentazione all'intero rail delle
periferiche durante il sonno — la stessa tecnica già usata per i sensori
terreno, estesa a tutto il resto.
