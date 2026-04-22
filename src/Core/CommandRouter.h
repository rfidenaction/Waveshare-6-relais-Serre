// Core/CommandRouter.h
// Routeur des commandes validées vers le manager d'actionneur propriétaire.
//
// Rôle :
//   Unique endroit du code qui relie une commande (identifiée par son DataId
//   META, ex : CommandValve3) à l'exécution physique. Ne fait QUE ce lookup :
//   pas de parsing, pas de validation, pas de journalisation. Ces étapes
//   sont gérées en amont par DataLogger (parseCommand / traceCommand).
//
// Algorithme :
//   Parcourt RELAYS[] (Config/IO-Config.h), cherche la ligne dont le champ
//   command correspond au cmdId reçu, et invoque son handler
//   enqueue(entity, durationMs). Le handler pointe vers la fonction
//   enqueueByEntity du manager propriétaire (ValveManager aujourd'hui, et
//   demain LightManager, FanManager… selon le câblage).
//
// Ajouter un nouveau type d'actionneur :
//   1. Créer le manager avec sa fonction static bool enqueueByEntity(DataId,
//      uint32_t) de signature RelayEnqueueFn (cf. IO-Config.h).
//   2. Remplacer la ligne concernée dans RELAYS[] pour pointer vers ce
//      nouveau handler.
//   Ni ce fichier ni les dispatchers (MqttManager, WebServer) n'ont à être
//   modifiés.
#pragma once

#include "Storage/DataLogger.h"   // DataId

class CommandRouter {
public:
    // Route une commande validée vers l'actionneur cible.
    //
    //   cmdId      : DataId META de la commande (ex : CommandValve3).
    //                Doit correspondre à la colonne command d'une ligne de
    //                RELAYS[] ; sinon retourne false (commande non câblée).
    //   durationMs : durée demandée, transmise telle quelle au handler.
    //
    // Retour :
    //   true  si une ligne correspondante a été trouvée ET si le handler
    //         du manager propriétaire a accepté la commande.
    //   false si cmdId est absent de RELAYS[] OU si le manager a refusé
    //         (queue pleine, système pas encore prêt).
    //
    // Appelable depuis n'importe quel thread : la fonction ne touche
    // qu'à des données constantes (RELAYS[]) et délègue aux handlers, qui
    // sont eux-mêmes thread-safe (xQueueSend FreeRTOS).
    static bool route(DataId cmdId, uint32_t durationMs);
};
