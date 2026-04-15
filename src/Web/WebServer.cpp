// Web/WebServer.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
//
// Refactoring META (source de vérité unique) :
//  - Suppression BUNDLE_ID_TO_TYPE[] (DataType vient de META)
//  - Suppression BUNDLE_TYPE_LABELS[] (typeLabel vient de META)
//  - Suppression jsonEscape() locale (centralisée dans DataLogger)
//  - buildBundleHeader() génère tout depuis META
//
// Suppression route /graphdata :
//  - Les graphiques sont désormais servis par /logs/download (bundle complet)
//  - Le filtrage par DataId, le sous-échantillonnage et la construction
//    du graphique sont faits côté client dans PagePrincipale.cpp
//  - Même principe que le client MQTT distant (RecepteurV4_serre.html)
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

// ─────────────────────────────────────────────────────────────
// Chart.js embarqué en flash (PROGMEM)
// Fichier intégré au firmware via board_build.embed_txtfiles
// Null-terminated grâce à embed_txtfiles (vs embed_files)
// ─────────────────────────────────────────────────────────────
extern const char chart_js_start[] asm("_binary_embed_chart_umd_min_js_start");
extern const char chart_js_end[]   asm("_binary_embed_chart_umd_min_js_end");

AsyncWebServer WebServer::server(80);

void WebServer::init()
{
    // Configuration des routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/ap-toggle", HTTP_POST, handleApToggle);
    server.on("/reset", HTTP_POST, handleReset);

    // ── Chart.js embarqué — servi depuis la flash ────────────
    server.on("/js/chart.min.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        size_t len = chart_js_end - chart_js_start - 1;
        AsyncWebServerResponse *response = request->beginChunkedResponse(
            "application/javascript",
            [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                size_t total = chart_js_end - chart_js_start - 1;
                if (index >= total) return 0;
                size_t remaining = total - index;
                size_t toSend = (remaining < maxLen) ? remaining : maxLen;
                memcpy(buffer, chart_js_start + index, toSend);
                return toSend;
            }
        );
        response->addHeader("Cache-Control", "public, max-age=86400");
        request->send(response);
        Console::info(TAG, "Chart.js servi depuis flash (" + String(len) + " octets)");
    });

    // Routes de gestion des logs
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
    Console::info(TAG, "handleLogs appelé");
    LogFileStats stats = DataLogger::getLogFileStats();
    Console::info(TAG, "Stats OK, génération HTML...");
    String html = PageLogs::getHtml(stats);
    Console::info(TAG, "HTML généré, taille=" + String(html.length()));
    request->send(200, "text/html", html);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bundle download — format texte structuré
//
// Format :
//   #SERRE_BUNDLE
//   #SCHEMA_JSON_BEGIN
//   { ... schéma JSON généré depuis META (source de vérité unique) ... }
//   #SCHEMA_JSON_END
//   #DATA_CSV_BEGIN
//   ... contenu brut de /datalog.csv (aucune transformation) ...
//   #DATA_CSV_END
//
// Cette route sert deux usages :
//  1. Téléchargement utilisateur (bouton "Télécharger les données")
//  2. Source de données pour les graphiques locaux (fetch depuis JS)
// Dans les deux cas, c'est le même pattern chunked response prouvé fiable.
// ─────────────────────────────────────────────────────────────────────────────

// ── Génération du schéma JSON + marqueurs bundle ────────────────────────────
// Tout est lu depuis META — aucun tableau local de mapping.
static void buildBundleHeader(String& p)
{
    p += "#SERRE_BUNDLE\n";
    p += "#SCHEMA_JSON_BEGIN\n";
    p += "{\n";

    // Timestamp de génération (heure locale)
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

    // Description des colonnes CSV
    p += "  \"csvColumns\": [\"timestamp\", \"UTC_available\", \"UTC_reliable\", "
         "\"type\", \"id\", \"valueType\", \"value\"],\n";

    // ── Table DataType (dédupliquée depuis META) ────────────────────────────
    // Parcourt les valeurs DataType 0-3, trouve le typeLabel dans META
    p += "  \"dataTypes\": [\n";
    bool firstType = true;
    for (uint8_t t = 0; t <= 3; t++) {
        // Chercher le premier META avec ce type pour obtenir le typeLabel
        const char* typeLabel = nullptr;
        for (size_t m = 0; m < META_COUNT; m++) {
            if ((uint8_t)META[m].type == t) {
                typeLabel = META[m].typeLabel;
                break;
            }
        }
        if (!typeLabel) continue;  // Aucun DataId de ce type

        if (!firstType) p += ",\n";
        firstType = false;
        p += "    {\"id\": "; p += t;
        p += ", \"label\": \""; p += DataLogger::jsonEscape(typeLabel); p += "\"}";
    }
    p += "\n  ],\n";

    // ── Table DataId (depuis META) ──────────────────────────────────────────
    p += "  \"dataIds\": [\n";
    for (size_t i = 0; i < META_COUNT; i++) {
        const DataMeta& m = META[i];

        p += "    {\"id\": "; p += (uint8_t)m.id;
        p += ", \"label\": \""; p += DataLogger::jsonEscape(m.label); p += "\"";
        p += ", \"unit\": \"";  p += DataLogger::jsonEscape(m.unit);  p += "\"";

        const char* natureStr =
            (m.nature == DataNature::metrique) ? "metrique" :
            (m.nature == DataNature::etat)     ? "etat"     : "texte";
        p += ", \"nature\": \""; p += natureStr; p += "\"";

        p += ", \"type\": "; p += (uint8_t)m.type;

        // Min/Max (uniquement pour metrique)
        if (m.nature == DataNature::metrique) {
            p += ", \"min\": "; p += String(m.min, 1);
            p += ", \"max\": "; p += String(m.max, 1);
        }

        // Mapping états (uniquement pour nature == etat)
        if (m.nature == DataNature::etat && m.stateLabels != nullptr) {
            p += ", \"states\": [";
            for (uint8_t s = 0; s < m.stateLabelCount; s++) {
                if (s > 0) p += ", ";
                p += "{\"value\": "; p += s;
                p += ", \"label\": \"";
                if (m.stateLabels[s] != nullptr) {
                    p += DataLogger::jsonEscape(m.stateLabels[s]);
                }
                p += "\"}";
            }
            p += "]";
        }

        p += "}";
        if (i < META_COUNT - 1) p += ",";
        p += "\n";
    }
    p += "  ]\n";
    p += "}\n";
    p += "#SCHEMA_JSON_END\n";
    p += "#DATA_CSV_BEGIN\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Contexte bundle (inchangé)
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
        pending.reserve(4096);
    }
};

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogsDownload(AsyncWebServerRequest *request)
{
    if (!SPIFFS.exists("/datalog.csv")) {
        request->send(404, "text/plain", "Aucune donnée disponible");
        Console::warn(TAG, "Bundle download demandé mais fichier inexistant");
        return;
    }

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

    request->onDisconnect([ctx]() {
        if (!ctx->deleted) {
            if (ctx->file) ctx->file.close();
            Console::warn("BundleCtx", "Bundle interrompu (disconnect client)");
        } else {
            Console::debug("BundleCtx", "Libération contexte (transfert terminé)");
        }
        delete ctx;
    });

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "text/plain; charset=utf-8",
        [ctx](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {

            if (ctx->deleted) return 0;

            // Drain pending
            if (ctx->pending.length() > 0) {
                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // Phase 1 : schéma JSON + marqueurs
            if (!ctx->headerDone) {
                buildBundleHeader(ctx->pending);
                ctx->headerDone = true;

                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // Phase 2 : données brutes
            if (ctx->file.available()) {
                return ctx->file.read(buffer, maxLen);
            }

            // Phase 3 : marqueur de fin
            if (!ctx->footerDone) {
                ctx->pending += "\n#DATA_CSV_END\n";
                ctx->footerDone = true;

                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // Phase 4 : fin du transfert
            ctx->file.close();
            Console::info("BundleCtx",
                String("Bundle terminé → ") + ctx->filename);
            ctx->deleted = true;
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