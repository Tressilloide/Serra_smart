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
| Alimentazione sensori | 16 | Gate del MOSFET |

Restano liberi: **15** (strapping, usare con cautela), **2**, **0**.

---

## 2. Avvertenze hardware — leggere prima di collegare

### 2.1 Sensori di umidità del terreno → alimentarli a 3,3 V, NON a 5 V

L'uscita analogica di questi moduli **segue la tensione di alimentazione**:
alimentati a 5 V manderebbero fino a 5 V sul pin ADC dell'ESP32, che tollera al
massimo 3,3 V. Il pin si danneggia.

Il modulo dichiara 3,3–12 V, quindi alimentarlo a 3,3 V è nelle specifiche.

```
Modulo terreno        ESP32
  VCC  ────────────►  rail 3,3 V commutato (drain del MOSFET)
  GND  ────────────►  GND
  AO   ────────────►  GPIO 34  (e GPIO 35 per il secondo)
  DO   ─────  non collegato
```

**Perché la DO resta scollegata:** la soglia si regola con un trimmer sul
modulo, è imprecisa e va rifatta a mano a ogni cambio di terreno. L'uscita
analogica con calibrazione software (comando `CAL`) è più affidabile e si
regola da Home Assistant senza toccare nulla.

**Alimentazione commutata — non è un dettaglio.** I sensori a due punte
metalliche sono resistivi: sotto tensione continua le punte si **corrodono per
elettrolisi** e in poche settimane diventano inutilizzabili. Il firmware
accende il rail solo per la frazione di secondo della misura
(`PWR_SETTLE_MS`, 250 ms) e lo spegne subito. Con due sensori il consumo di
picco è di circa 16 mA.

```
        3,3 V
          │
          ├──── S ──┐
                    │  MOSFET P (es. AO3401)
          ┌──── D ──┘
          │
          └────────► VCC dei sensori terreno

  GPIO 16 ──[10k]──► G     (+ resistenza da 100k tra G e S)
```

Un P-MOSFET lato alto è la soluzione pulita. In alternativa, con due soli
sensori, si può pilotare direttamente il VCC da GPIO 16: è al limite ma
funziona, a patto di dissaldare i LED di alimentazione dei moduli.

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
                       ═╪═ 100 nF
                        │
                       GND
```

Con 10 kΩ / 15 kΩ i 5 V diventano circa 3,0 V: dentro le specifiche. Il
condensatore da 100 nF filtra il rumore della pompa. In alternativa va bene un
level shifter bidirezionale.

**Calibrazione:** il datasheet dà `F = 7,5 × Q[L/min]`, cioè 450 impulsi/litro,
ma varia parecchio da esemplare a esemplare. Riempi un contenitore da 1 litro e
usa il conteggio reale con `CAL,acqua,<impulsi>`.

### 2.3 GPIO 16 e 17 → non disponibili sui moduli WROVER

Sui moduli **ESP32-WROVER** (quelli con PSRAM) i GPIO 16 e 17 sono cablati alla
memoria PSRAM e non sono utilizzabili. Il firmware lo rileva al boot con
`psramFound()` e stampa un avviso sul monitor seriale.

In quel caso, in `config.h`:

```c
#define PIN_FLUSSO       15
#define PIN_PWR_SENSORI  2
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
