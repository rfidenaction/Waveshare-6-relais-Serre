// Connectivity/NTPManager.h
#pragma once

#include <Arduino.h>
#include <time.h>

/*
 * NTPManager — Agent NTP pur
 *
 *  - Au boot : tentatives rapides (30s) jusqu'à 10 essais
 *  - En routine : un essai toutes les 50 min (VClock pas encore available)
 *                 ou tous les 25 tours (~20h50, VClock déjà available)
 *  - À chaque synchro NTP réussie :
 *      RTCManager::write(utc)   (si la carte répond)
 *      VirtualClock::sync(utc)  (toujours — l'heure NTP domine quand elle
 *                                existe et est correcte)
 *
 * NTPManager n'est jamais interrogé pour lire l'heure — la source unique
 * du temps est VirtualClock::read().
 */

class NTPManager {
public:
    static void init();
    static void handle();   // à appeler régulièrement (TaskManager)

private:
    // ── Machine d'états NTP (non-bloquante) ──
    enum class NtpState : uint8_t { IDLE, WAITING };

    static void startNtp();       // Lance sntp, passe en WAITING
    static void checkNtp();       // Vérifie si NTP a répondu

    static NtpState _ntpState;
    static uint32_t _ntpStartMs;  // millis() au lancement de la tentative

    static bool     _everSynced;
    static uint32_t _networkUpSinceMs;
    static uint32_t _lastAttemptMs;
    static uint32_t _lastTourMs;
    static uint8_t  _bootAttempts;
    static uint8_t  _tourCount;
};
