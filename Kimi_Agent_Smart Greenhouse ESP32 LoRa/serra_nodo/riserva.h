/*
 * ============================================================================
 *  SERRA SMART — NODO SERRA : RISERVA IN MEMORIA RTC
 * ============================================================================
 *
 *  ---------------------------------------------------------------------------
 *  IL PROBLEMA (ottobre-novembre 2026)
 *  ---------------------------------------------------------------------------
 *  Dal 20/10 la microSD del nodo non risponde piu' (bl=-1), e con lei se n'e'
 *  andata l'unica rete di sicurezza: un pacchetto che il ponte non conferma
 *  non ha piu' dove aspettare, ed e' perso. Nello stesso periodo il segnale
 *  LoRa e' sceso a circa -110 dBm, e fra il 31/10 e il 10/11 Home Assistant
 *  ha ricevuto 834 cicli sui 959 attesi: 125 misure sparite, quasi tutte una
 *  alla volta (un ciclo saltato e il successivo che passa), alcune a
 *  blocchi di qualche ora.
 *
 *  ---------------------------------------------------------------------------
 *  IL RIMEDIO
 *  ---------------------------------------------------------------------------
 *  Una seconda coda, piccola, nella RAM del dominio RTC: l'unica memoria che
 *  il deep sleep non spegne. Entra in gioco solo quando la microSD non
 *  c'e' o rifiuta la scrittura, e al primo ACK buono del ciclo successivo
 *  viene svuotata verso il ponte, prima del backlog su SD, perche' e' la piu'
 *  fragile delle due: sopravvive a deep sleep, watchdog e panic, non a una
 *  mancanza di corrente.
 *
 *  Per farci stare piu' ore, le righe in riserva perdono i campi di
 *  configurazione (sAuto, sOra, sMin, sDur, sSoil, slp): servono alle entita'
 *  di comando di Home Assistant, che leggono solo lo stato attuale, mai lo
 *  storico. Con circa 140 byte a riga, i 6 KB di RISERVA_BYTE sono una
 *  quarantina di cicli, cioe' una decina d'ore di collegamento perso. Oltre,
 *  si sacrificano le righe piu' vecchie.
 *
 *  Il numero di righe in attesa viaggia nel campo "rs" del pacchetto
 *  principale, solo se diverso da zero.
 * ============================================================================
 */

#pragma once

#include <Arduino.h>
#include "config.h"
#include "backlog.h"   // FnInvioRecord

// Da chiamare a ogni risveglio. memoriaPersa = tracciaMemoriaPersa(): dopo
// una mancanza di corrente la RAM RTC contiene spazzatura e va azzerata.
void riservaInit(bool memoriaPersa);

uint16_t riservaConta();
uint32_t riservaScartati();

// Accoda una riga gia' pronta, marcatore bk=1 compreso.
bool riservaAccoda(const char* riga);

/*
 * Consegna le righe in attesa, dalla piu' vecchia. Ognuna esce dalla coda
 * appena il ponte la conferma, non alla fine: se il nodo si resetta a meta',
 * nessuna riga viene consegnata due volte e nessuna va persa.
 * Si ferma al primo fallimento o dopo maxInvii righe; ritorna le consegnate.
 */
uint16_t riservaDrena(FnInvioRecord invia, uint16_t maxInvii);

// Svuota la riserva (comando CLRBL, insieme al backlog su SD).
void riservaSvuota();
