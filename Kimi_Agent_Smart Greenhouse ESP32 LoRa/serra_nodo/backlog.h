/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : BACKLOG SU MICROSD
 * ============================================================================
 *
 *  Quando il ponte non risponde (spento, WiFi giu', server in manutenzione) i
 *  pacchetti vengono accodati qui e ritrasmessi al primo risveglio utile.
 *
 *  ---------------------------------------------------------------------------
 *  IL BUG CHE QUESTO FILE CORREGGE
 *  ---------------------------------------------------------------------------
 *  La versione precedente leggeva al massimo 100 record in RAM, poi faceva
 *  SD.remove() dell'INTERO file e lo riscriveva partendo solo da quei 100.
 *  Tutti i record oltre il centesimo sparivano senza essere mai stati inviati.
 *  Con un risveglio ogni 15 minuti (96 pacchetti al giorno) bastava un giorno
 *  di ponte offline per iniziare a perdere dati in silenzio.
 *
 *  Qui il file non viene mai riscritto a partire dalla RAM: si tiene traccia
 *  dell'offset dell'ultimo record consegnato e la CODA ANCORA DA INVIARE viene
 *  copiata byte per byte dal file originale a un temporaneo, che poi lo
 *  sostituisce. Quello che non e' stato letto non puo' essere perso.
 *
 *  In piu':
 *    - l'accodamento e' in append, non riscrive tutto il file a ogni ciclo
 *      (meno usura della scheda e molto piu' veloce)
 *    - c'e' un tetto alla dimensione del file: superato BACKLOG_MAX_BYTE
 *      vengono scartati i record PIU' VECCHI, contandoli in NVS cosi' che
 *      Home Assistant possa segnalare la perdita invece di subirla in silenzio
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include "config.h"

// Callback di consegna: ritorna true se il record e' stato confermato dal ponte.
typedef bool (*FnInvioRecord)(const char* riga);

// Monta la microSD. Ritorna false se la scheda manca: il resto del sistema
// continua comunque a funzionare, semplicemente senza rete di sicurezza.
bool backlogInit();
bool backlogDisponibile();

// Numero di record attualmente in coda (0 se la SD non c'e').
uint32_t backlogConta();

// Accoda un record. Applica il tetto di dimensione scartando i piu' vecchi.
bool backlogAccoda(const char* riga);

/*
 * Prova a consegnare i record in coda, dal piu' vecchio al piu' recente.
 * Si ferma al primo fallimento (link caduto) o dopo BACKLOG_MAX_INVII record.
 * I record non consegnati restano in coda, TUTTI, compresi quelli mai letti.
 * Ritorna il numero di record effettivamente consegnati.
 */
uint32_t backlogDrena(FnInvioRecord invia);

// Svuota completamente la coda (comando CLRBL da Home Assistant).
void backlogSvuota();

// Scrive una riga nel log eventi su SD, con tetto di dimensione.
void backlogLog(const String& messaggio);
