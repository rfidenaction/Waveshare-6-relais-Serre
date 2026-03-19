// Connectivity/ManagerUTC.h
#pragma once

#include <Arduino.h>
#include <time.h>

/*
 * ManagerUTC — Agent NTP
 *
 * Rôle unique : obtenir l'heure via NTP et la transmettre à :
 *  - RTCManager   → écriture DS3231
 *  - VirtualClock → recalage horloge virtuelle
 *
 * Politique :
 *  - Au boot : tentatives rapides (30s) jusqu'à 10 essais
 *  - Après premier succès : resync toutes les 24h
 *
 * Ce module ne fournit AUCUNE API de lecture du temps.
 * Pour lire l'heure  → RTCManager::read()
 * Pour savoir si l'heure est fiable → RTCManager::isReliable()
 */

class ManagerUTC {
public:
    static void init();
    static void handle();   // à appeler régulièrement (loop)

private:
    static bool trySync();

    static bool     _everSynced;
    static uint32_t _networkUpSinceMs;
    static uint32_t _lastAttemptMs;
    static uint32_t _lastSyncMs;
    static uint8_t  _bootAttempts;
};