// Sensors/SupplyVoltage.h
// Lecture de la tension d'alimentation via la carte Waveshare Analog Input 8CH (B)
// sur le bus RS485 (Modbus RTU, adresse 16, canal 1).
//
// L'entrée analogique reçoit la tension d'alimentation divisée par 4
// (pont résistif). La valeur lue est multipliée par 4 pour reconstituer
// la tension réelle.
#pragma once

#include <Arduino.h>
#include "Config/MetaDataModel.h"
#include "Config/Neo.h"

class SupplyVoltage {
public:
    static void init();
    static void handle();   // Appelé périodiquement par TaskManager

    // ─── Déclaration NEO ─────────────────────────────────────────────────
    // Ce module décrit lui-même les données qu'il produit ; il ne les reçoit
    // d'aucune table extérieure. Neo::build() l'interroge au démarrage.
    //
    // Les deux grandeurs sont décrites ici et non dans un descripteur en
    // rotation : il n'y a qu'un seul appareil sur le bus (carte Analog Input
    // 8CH à l'adresse 16) et une seule transaction Modbus livre les deux
    // canaux. Le canal 1 donne la tension de la batterie d'alimentation, le
    // canal 2 la présence du secteur après application du seuil dans ce
    // module.

    static uint8_t  neoCount();
    static NeoEntry neoAt(uint8_t index);

    // ─── Mesure à la demande ─────────────────────────────────────────────
    // Interroge immédiatement la carte et publie les deux canaux sur DataBus
    // — même chemin que handle(), donc même validation, même horodatage,
    // même journalisation CSV, et même détection de front sur l'état secteur.
    // Appelée depuis le thread TaskManager uniquement (bus RS485 partagé).
    // Retourne false si l'id est inconnu du module, si le bus est indisponible
    // (mode maintenance, délai de démarrage) ou si la carte n'a pas répondu.
    static bool measureNow(DataId id);

private:
    // L'adresse Modbus n'est pas reprise ici : elle vit une seule fois dans
    // DEVICE_ADDRESS (.cpp), où les trames sont bâties, et neoAt l'y lit.
    static constexpr NeoMeasure MEASURES[] = {
        { DataId::SupplyVoltage, Grandeur::Tension,  Concerne::Batterie },
        { DataId::AcPower,       Grandeur::Presence, Concerne::Secteur  },
    };

    // Transaction Modbus pure : lecture des deux canaux, décodage, pas de
    // publication ni d'effet de bord. Retourne true si la carte a répondu.
    static bool readHardware(float& voltage, float& acPower);

    // Publication des deux valeurs sur DataBus (SupplyVoltage + AcPower).
    static void publishValues(float voltage, float acPower);

    static uint16_t crc16(const uint8_t* data, size_t len);
};
