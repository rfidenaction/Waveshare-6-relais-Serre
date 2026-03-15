// Web/WebServer.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
// Changements :
//  - Suppression CellularManager (pas de modem)
//  - Suppression route /wifi-toggle (STA toujours actif)
//  - Suppression route /gsm-toggle (pas de modem)
//  - Suppression garde GSM dans handleLogsDownload
//  - handleLogs : suppression paramètre gsmActive
//  - handleGraphData : BatteryVoltage → SupplyVoltage
//  - BUNDLE_ID_TO_TYPE reconstruit sur nouvel enum (11 entrées)
//  - BUNDLE_TYPE_LABELS : Battery → Power
//  - Logger → Console
#include "Web/WebServer.h"

#include "Web/Pages/PagePrincipale.h"
#include "Web/Pages/PageLogs.h"
#include "Connectivity/WiFiManager.h"
#include "Storage/DataLogger.h"
#include "Utils/Console.h"

#include <SPIFFS.h>
#include <time.h>

// Tag pour logs
static const char* TAG = "WebServer";

AsyncWebServer WebServer::server(80);

void WebServer::init()
{
    // Configuration des routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/ap-toggle", HTTP_POST, handleApToggle);
    server.on("/graphdata", HTTP_GET, handleGraphData);
    server.on("/reset", HTTP_POST, handleReset);

    // Routes de gestion des logs
    // ⚠️ CORRECTION : routes spécifiques AVANT /logs
    server.on("/logs/download", HTTP_GET, handleLogsDownload);
    server.on("/logs/clear", HTTP_POST, handleLogsClear);
    server.on("/logs", HTTP_GET, handleLogs);

    // Démarrage du serveur asynchrone
    server.begin();
    Console::info(TAG, "Serveur web démarré");
}

// ─────────────────────────────────────────────────────────────────────────────
// Page principale
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleRoot(AsyncWebServerRequest *request)
{
    String html = PagePrincipale::getHtml();
    request->send(200, "text/html", html);
}

// ─────────────────────────────────────────────────────────────────────────────
// Commande AP Wi-Fi
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleApToggle(AsyncWebServerRequest *request)
{
    bool wantOn = request->hasParam("state", true);
    request->send(204);
    if (!wantOn) {
        WiFiManager::disableAP();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Graphique tension alimentation
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleGraphData(AsyncWebServerRequest *request)
{
    String csv = DataLogger::getGraphCsv(DataId::SupplyVoltage, 30);
    request->send(200, "text/plain", csv);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reset système
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleReset(AsyncWebServerRequest *request)
{
    request->send(200, "text/plain", "Redémarrage...");
    delay(300);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// Gestion des logs
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogs(AsyncWebServerRequest *request)
{
    LogFileStats stats = DataLogger::getLogFileStats();
    String html = PageLogs::getHtml(stats);
    request->send(200, "text/html", html);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bundle download — format texte structuré
//
// Format :
//   #SERRE_BUNDLE
//   #SCHEMA_JSON_BEGIN
//   { ... schéma JSON généré depuis DataLogger::getMeta() ... }
//   #SCHEMA_JSON_END
//   #DATA_CSV_BEGIN
//   ... contenu brut de /datalog.csv (aucune transformation) ...
//   #DATA_CSV_END
//
// Avantages :
//   - Transfert maximal : la phase DATA lit le fichier directement dans le
//     buffer TCP, sans aucune allocation String ni parsing
//   - Schéma JSON auto-généré depuis META : source de vérité unique
//   - Format lisible dans un éditeur texte, parsable facilement en Python
// ─────────────────────────────────────────────────────────────────────────────

// ── Mapping DataId → DataType (pour le schéma JSON) ──────────────────────────
// Duplicat minimal nécessaire : DataType n'est pas dans DataMeta (axe distinct).
// CONTRAT : synchronisé avec l'enum DataId dans DataLogger.h.
static const uint8_t BUNDLE_ID_TO_TYPE[(uint8_t)DataId::Count] = {
    0,  // SupplyVoltage    (0)  → Power
    1,  // AirTemperature1  (1)  → Sensor
    1,  // AirHumidity1     (2)  → Sensor
    1,  // SoilMoisture1    (3)  → Sensor
    2,  // Valve1           (4)  → Actuator
    3,  // WifiStaEnabled   (5)  → System
    3,  // WifiStaConnected (6)  → System
    3,  // WifiApEnabled    (7)  → System
    3,  // WifiRssi         (8)  → System
    3,  // Boot             (9)  → System
    3,  // Error            (10) → System
};

static const char* BUNDLE_TYPE_LABELS[] = {
    "Power", "Sensor", "Actuator", "System"
};

// ── Échappement JSON minimal (caractères critiques uniquement) ────────────────
// Les accents et caractères UTF-8 multi-octets sont valides en JSON sans escape.
static String jsonEscape(const char* s)
{
    String out;
    if (!s) return out;
    out.reserve(strlen(s) + 4);
    while (*s) {
        char c = *s++;
        if      (c == '"')  { out += '\\'; out += '"';  }
        else if (c == '\\') { out += '\\'; out += '\\'; }
        else if (c == '\n') { out += '\\'; out += 'n';  }
        else if (c == '\r') { out += '\\'; out += 'r';  }
        else                { out += c; }
    }
    return out;
}

// ── Génération du schéma JSON + marqueurs bundle dans pending ─────────────────
// Produit tout ce qui précède les données brutes :
//   #SERRE_BUNDLE
//   #SCHEMA_JSON_BEGIN
//   { ... }
//   #SCHEMA_JSON_END
//   #DATA_CSV_BEGIN
static void buildBundleHeader(String& p)
{
    p += "#SERRE_BUNDLE\n";
    p += "#SCHEMA_JSON_BEGIN\n";
    p += "{\n";
    p += "  \"version\": \"1\",\n";

    // Timestamp de génération (heure locale France)
    char dateBuf[24] = "";
    {
        time_t now = time(nullptr);
        struct tm tmLocal;
        localtime_r(&now, &tmLocal);
        if (tmLocal.tm_year > 120) {
            strftime(dateBuf, sizeof(dateBuf), "%d-%m-%Y %H:%M:%S", &tmLocal);
        }
    }
    p += "  \"generated\": \""; p += dateBuf; p += "\",\n";

    // Table DataType (domaine / regroupement)
    p += "  \"dataTypes\": [\n";
    for (int i = 0; i < 4; i++) {
        p += "    {\"id\": "; p += i;
        p += ", \"label\": \""; p += BUNDLE_TYPE_LABELS[i]; p += "\"}";
        if (i < 3) p += ",";
        p += "\n";
    }
    p += "  ],\n";

    // Table DataId (métadonnées complètes depuis DataLogger::getMeta)
    p += "  \"dataIds\": [\n";
    const uint8_t count = (uint8_t)DataId::Count;
    for (uint8_t i = 0; i < count; i++) {
        const DataMeta& m = DataLogger::getMeta((DataId)i);

        p += "    {\"id\": "; p += i;
        p += ", \"label\": \""; p += jsonEscape(m.label); p += "\"";
        p += ", \"unit\": \"";  p += jsonEscape(m.unit);  p += "\"";

        const char* natureStr =
            (m.nature == DataNature::metrique) ? "metrique" :
            (m.nature == DataNature::etat)     ? "etat"     : "texte";
        p += ", \"nature\": \""; p += natureStr; p += "\"";

        p += ", \"type\": "; p += BUNDLE_ID_TO_TYPE[i];

        // Mapping états (uniquement pour nature == etat)
        if (m.nature == DataNature::etat && m.stateLabels != nullptr) {
            p += ", \"states\": [";
            for (uint8_t s = 0; s < m.stateLabelCount; s++) {
                if (s > 0) p += ", ";
                p += "{\"value\": "; p += s;
                p += ", \"label\": \"";
                if (m.stateLabels[s] != nullptr) {
                    p += jsonEscape(m.stateLabels[s]);
                }
                p += "\"}";
            }
            p += "]";
        }

        p += "}";
        if (i < count - 1) p += ",";
        p += "\n";
    }
    p += "  ]\n";
    p += "}\n";
    p += "#SCHEMA_JSON_END\n";
    p += "#DATA_CSV_BEGIN\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Contexte bundle
//
// Cycle de vie :
//   Phase 1 (headerDone=false) : buildBundleHeader → pending → drain TCP
//   Phase 2 (headerDone=true, !footerDone, file.available()) :
//             file.read() direct dans buffer TCP (zéro allocation)
//   Phase 3 (footerDone=false, EOF) : écriture #DATA_CSV_END → pending → drain
//   Phase 4 (footerDone=true) : return 0 → fin transfert
//
// Libération :
//   - Fin normale    : dans le callback (return 0)
//   - Abort/disconnect : request->onDisconnect() + guard "deleted"
// ─────────────────────────────────────────────────────────────────────────────

struct BundleContext {
    File   file;
    String pending;
    bool   headerDone;
    bool   footerDone;
    bool   deleted;
    char   filename[44];

    BundleContext()
        : headerDone(false), footerDone(false), deleted(false)
    {
        filename[0] = '\0';
        pending.reserve(4096); // Schéma JSON (~3 KB) + marqueurs
    }
};

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogsDownload(AsyncWebServerRequest *request)
{
    // ── Gardes ───────────────────────────────────────────────────────────────
    if (!SPIFFS.exists("/datalog.csv")) {
        request->send(404, "text/plain", "Aucune donnée disponible");
        Console::warn(TAG, "Bundle download demandé mais fichier inexistant");
        return;
    }

    // ── Allocation du contexte ────────────────────────────────────────────────
    BundleContext* ctx = new BundleContext();
    if (!ctx) {
        request->send(500, "text/plain", "Mémoire insuffisante");
        Console::error(TAG, "Bundle download : allocation contexte échouée");
        return;
    }

    ctx->file = SPIFFS.open("/datalog.csv", FILE_READ);
    if (!ctx->file) {
        delete ctx;
        request->send(500, "text/plain", "Impossible d'ouvrir le fichier");
        Console::error(TAG, "Bundle download : ouverture /datalog.csv échouée");
        return;
    }

    // ── Nom de fichier avec date locale ───────────────────────────────────────
    strncpy(ctx->filename, "serre_bundle.txt", sizeof(ctx->filename));
    {
        time_t now = time(nullptr);
        struct tm tmLocal;
        localtime_r(&now, &tmLocal);
        if (tmLocal.tm_year > 120) {
            strftime(ctx->filename, sizeof(ctx->filename),
                     "serre_bundle_%Y-%m-%d.txt", &tmLocal);
        }
    }

    // ── Libération sur abort/disconnect ──────────────────────────────────────
    request->onDisconnect([ctx]() {
        if (!ctx->deleted) {
            ctx->deleted = true;
            if (ctx->file) ctx->file.close();
            Console::warn("BundleCtx", "Bundle interrompu (disconnect client)");
            delete ctx;
        }
    });

    // ── Réponse chunkée ───────────────────────────────────────────────────────
    // Callback appelé au rythme du TCP. Retourne :
    //   N > 0             → N octets à envoyer
    //   0                 → fin du transfert
    //   RESPONSE_TRY_AGAIN→ rien pour l'instant, réessayer plus tard
    //
    // L'argument "index" est ignoré : tout l'état est dans BundleContext.
    // La lib n'appelle pas ce callback en parallèle.

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "text/plain; charset=utf-8",
        [ctx](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {

            // Guard : contexte déjà libéré par onDisconnect
            if (ctx->deleted) return 0;

            // ── Drain pending (prioritaire sur toute autre action) ─────────────
            if (ctx->pending.length() > 0) {
                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // ── Phase 1 : schéma JSON + marqueurs (une seule fois) ────────────
            if (!ctx->headerDone) {
                buildBundleHeader(ctx->pending);
                ctx->headerDone = true;

                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // ── Phase 2 : données brutes (lecture directe SPIFFS → buffer TCP) ─
            // Zéro allocation String, zéro parsing : débit maximal.
            if (ctx->file.available()) {
                return ctx->file.read(buffer, maxLen);
            }

            // ── Phase 3 : marqueur de fin données (une seule fois) ────────────
            if (!ctx->footerDone) {
                ctx->pending += "\n#DATA_CSV_END\n";
                ctx->footerDone = true;

                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // ── Phase 4 : fin du transfert ────────────────────────────────────
            ctx->file.close();
            Console::info("BundleCtx",
                String("Bundle terminé → ") + ctx->filename);
            ctx->deleted = true;
            delete ctx;
            return 0;
        }
    );

    char disposition[64];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", ctx->filename);
    response->addHeader("Content-Disposition", disposition);
    response->addHeader("Cache-Control", "no-store");

    request->send(response);
    Console::info(TAG, String("Bundle download démarré → ") + ctx->filename);
}

void WebServer::handleLogsClear(AsyncWebServerRequest *request)
{
    DataLogger::clearHistory();
    request->send(200, "text/plain", "Historique supprimé avec succès");
    Console::info(TAG, "Logs supprimés par l'utilisateur");
}