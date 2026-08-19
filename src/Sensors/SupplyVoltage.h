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

class SupplyVoltage {
public:
    static void init();
    static void handle();   // Appelé périodiquement par TaskManager

    // ─── Mesure à la demande ─────────────────────────────────────────────
    // Ce module déclare les DataId qu'il produit ; il ne les reçoit d'aucune
    // table extérieure. OnDemandMeasure l'interroge au démarrage pour
    // construire sa vue id → propriétaire, comme ValveManager se construit
    // depuis RELAYS[].
    //
    // Les deux ids sont écrits ici et non dans un descripteur en rotation :
    // il n'y a qu'un seul appareil sur le bus (carte Analog Input 8CH à
    // l'adresse 16) et une seule transaction Modbus livre les deux canaux.
    // Le canal 1 donne une métrique (tension), le canal 2 un état (secteur
    // présent ou absent) après application du seuil dans ce module.

    // Nombre de DataId produits : SupplyVoltage et AcPower.
    static uint8_t measurableCount();

    // DataId numéro `index`, avec index < measurableCount().
    static DataId measurableAt(uint8_t index);

    // Interroge immédiatement la carte et publie les deux canaux sur DataBus
    // — même chemin que handle(), donc même validation, même horodatage,
    // même journalisation CSV, et même détection de front sur l'état secteur.
    // Appelée depuis le thread TaskManager uniquement (bus RS485 partagé).
    // Retourne false si l'id est inconnu du module, si le bus est indisponible
    // (mode maintenance, délai de démarrage) ou si la carte n'a pas répondu.
    static bool measureNow(DataId id);

private:
    // Transaction Modbus pure : lecture des deux canaux, décodage, pas de
    // publication ni d'effet de bord. Retourne true si la carte a répondu.
    static bool readHardware(float& voltage, float& acPower);

    // Publication des deux valeurs sur DataBus (SupplyVoltage + AcPower).
    static void publishValues(float voltage, float acPower);

    static uint16_t crc16(const uint8_t* data, size_t len);
};
