// Web/WebServer.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
//
// lastDataForWeb[] hébergé ici, protégé par portMUX.
// buildBundleHeader() utilise typeLabel/jsonEscape (MetaDataModel.h).
// handleCommandFinal() utilise DataBus::parseCommand/publishCommand.
#include "Web/WebServer.h"

#include "Web/Pages/PagePrincipale.h"
#include "Web/Pages/PageLogs.h"
#include "Web/Pages/PageActuators.h"
#include "Connectivity/WiFiManager.h"
#include "Storage/DataLogger.h"
#include "Core/DataBus.h"
#include "Config/MetaDataModel.h"
#include "Utils/Console.h"

#include <LittleFS.h>
#include <time.h>

static const char* TAG = "WebServer";

// ─────────────────────────────────────────────────────────────
// Chart.js embarqué en flash (PROGMEM)
// ─────────────────────────────────────────────────────────────
extern const char chart_js_start[] asm("_binary_embed_chart_umd_min_js_start");
extern const char chart_js_end[]   asm("_binary_embed_chart_umd_min_js_end");

AsyncWebServer WebServer::server(80);

// ─── lastDataForWeb — variables statiques ────────────────────────────────────
std::array<LastDataForWeb, META_COUNT> WebServer::lastDataForWeb{};
std::array<bool,           META_COUNT> WebServer::lastDataForWebHas{};
portMUX_TYPE WebServer::lastDataMux = portMUX_INITIALIZER_UNLOCKED;

// ─── updateLastData() — appelé par DataBus::distribute() ─────────────────────
void WebServer::updateLastData(const BusItem& item)
{
    int idx = findMetaIndex((uint8_t)item.id);
    if (idx < 0) return;

    taskENTER_CRITICAL(&lastDataMux);
    LastDataForWeb& w = lastDataForWeb[idx];
    if (item.valueKind == 0) {
        w.value = item.valueFloat;
    } else {
        w.value = String(item.valueText);
    }
    w.timestamp        = item.timestamp;
    w.VClock_available = item.VClock_available;
    w.VClock_reliable  = item.VClock_reliable;
    lastDataForWebHas[idx] = true;
    taskEXIT_CRITICAL(&lastDataMux);
}

// ─── hasLastData() — lecture thread-safe pour les pages web ──────────────────
bool WebServer::hasLastData(DataId id, LastDataForWeb& out)
{
    int idx = findMetaIndex((uint8_t)id);
    if (idx < 0) return false;

    taskENTER_CRITICAL(&lastDataMux);
    bool has = lastDataForWebHas[idx];
    if (has) {
        out = lastDataForWeb[idx];
    }
    taskEXIT_CRITICAL(&lastDataMux);
    return has;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilitaire : collecte et tri des fichiers log_*.csv
//
// Retourne un tableau alloué dynamiquement de chemins triés par nom
// (ordre chronologique, les noms sont en YYYY-MM-DD).
// L'appelant doit libérer avec delete[].
// ─────────────────────────────────────────────────────────────────────────────
static String* collectSortedLogFiles(size_t& outCount)
{
    outCount = 0;

    // Premier passage : compter les fichiers
    size_t count = 0;
    File root = LittleFS.open("/");
    if (!root) return nullptr;

    File f = root.openNextFile();
    while (f) {
        String name = String(f.name());
        f.close();
        const char* p = name.c_str();
        if (p[0] == '/') p++;
        if (strncmp(p, "log_", 4) == 0 && strstr(p, ".csv") != nullptr) {
            count++;
        }
        f = root.openNextFile();
    }
    root.close();

    if (count == 0) return nullptr;

    // Allocation et remplissage
    String* paths = new (std::nothrow) String[count];
    if (!paths) return nullptr;

    size_t idx = 0;
    root = LittleFS.open("/");
    if (!root) { delete[] paths; return nullptr; }

    f = root.openNextFile();
    while (f && idx < count) {
        String name = String(f.name());
        f.close();
        const char* p = name.c_str();
        if (p[0] == '/') p++;
        if (strncmp(p, "log_", 4) == 0 && strstr(p, ".csv") != nullptr) {
            paths[idx++] = name.startsWith("/") ? name : ("/" + name);
        }
        f = root.openNextFile();
    }
    root.close();
    count = idx;

    // Tri par insertion (correct pour <500 fichiers, noms YYYY-MM-DD = tri chrono)
    for (size_t i = 1; i < count; i++) {
        String key = paths[i];
        int j = (int)i - 1;
        while (j >= 0 && paths[j] > key) {
            paths[j + 1] = paths[j];
            j--;
        }
        paths[j + 1] = key;
    }

    outCount = count;
    return paths;
}

// ─── rebuildLastDataFromFlash() — reconstruction au boot ─────────────────────
// Parse tous les fichiers log_*.csv dans l'ordre chronologique et garde la
// dernière valeur par DataId. Appelée une fois au boot depuis main.cpp.
void WebServer::rebuildLastDataFromFlash()
{
    size_t fileCount = 0;
    String* files = collectSortedLogFiles(fileCount);
    if (!files || fileCount == 0) {
        delete[] files;
        return;
    }

    struct LastSeen {
        bool found = false;
        uint32_t timestamp = 0;
        bool VClock_available = false;
        bool VClock_reliable  = false;
        std::variant<float, String> value;
    };
    LastSeen lastSeen[META_COUNT];

    // Lecture de tous les fichiers, du plus ancien au plus récent.
    // La dernière valeur vue pour chaque DataId l'emporte.
    for (size_t fi = 0; fi < fileCount; fi++) {
        File file = LittleFS.open(files[fi], FILE_READ);
        if (!file) continue;

        while (file.available()) {
            String line = file.readStringUntil('\n');
            if (line.length() == 0) continue;

            int c1 = line.indexOf(',');
            int c2 = line.indexOf(',', c1 + 1);
            int c3 = line.indexOf(',', c2 + 1);
            int c4 = line.indexOf(',', c3 + 1);
            int c5 = line.indexOf(',', c4 + 1);
            int c6 = line.indexOf(',', c5 + 1);

            if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1 || c5 == -1 || c6 == -1) {
                continue;
            }

            unsigned long ts     = line.substring(0, c1).toInt();
            uint8_t avail        = line.substring(c1 + 1, c2).toInt();
            uint8_t reliable     = line.substring(c2 + 1, c3).toInt();
            uint8_t idByte       = line.substring(c4 + 1, c5).toInt();
            uint8_t valueType    = line.substring(c5 + 1, c6).toInt();
            String valueStr      = line.substring(c6 + 1);

            int metaIdx = findMetaIndex(idByte);
            if (metaIdx < 0) continue;

            LastSeen& ls = lastSeen[metaIdx];
            ls.found            = true;
            ls.timestamp        = ts;
            ls.VClock_available = (avail != 0);
            ls.VClock_reliable  = (reliable != 0);

            if (valueType == 0) {
                ls.value = valueStr.toFloat();
            } else {
                valueStr.trim();
                // Dé-échappement CSV inline
                if (valueStr.length() >= 2 &&
                    valueStr.charAt(0) == '"' &&
                    valueStr.charAt(valueStr.length() - 1) == '"') {
                    String unescaped;
                    for (size_t i = 1; i < valueStr.length() - 1; i++) {
                        char c = valueStr.charAt(i);
                        if (c == '"' && i + 1 < valueStr.length() - 1 &&
                            valueStr.charAt(i + 1) == '"') {
                            unescaped += '"';
                            i++;
                        } else {
                            unescaped += c;
                        }
                    }
                    ls.value = unescaped;
                } else {
                    ls.value = valueStr;
                }
            }
        }

        file.close();
    }

    delete[] files;

    taskENTER_CRITICAL(&lastDataMux);
    for (size_t m = 0; m < META_COUNT; m++) {
        if (lastSeen[m].found) {
            LastDataForWeb& w = lastDataForWeb[m];
            w.value            = lastSeen[m].value;
            w.timestamp        = lastSeen[m].timestamp;
            w.VClock_available = lastSeen[m].VClock_available;
            w.VClock_reliable  = lastSeen[m].VClock_reliable;
            lastDataForWebHas[m] = true;
        }
    }
    taskEXIT_CRITICAL(&lastDataMux);
}

void WebServer::init()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/ap-toggle", HTTP_POST, handleApToggle);
    server.on("/reset", HTTP_POST, handleReset);

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

    server.on("/logs/download", HTTP_GET, handleLogsDownload);
    server.on("/logs/clear", HTTP_POST, handleLogsClear);
    server.on("/logs", HTTP_GET, handleLogs);

    server.on("/actuators", HTTP_GET, handleActuators);

    server.on("/command", HTTP_POST,
              handleCommandFinal,
              nullptr,
              handleCommandBody);

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

void WebServer::handleApToggle(AsyncWebServerRequest *request)
{
    bool wantOn = request->hasParam("state", true);
    request->send(204);
    if (!wantOn) {
        WiFiManager::disableAP();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleReset(AsyncWebServerRequest *request)
{
    request->send(200, "text/plain", "Redémarrage...");
    delay(300);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogs(AsyncWebServerRequest *request)
{
    Console::info(TAG, "handleLogs appelé");
    FlashUsageStats stats = DataLogger::getFlashUsageStats();
    Console::info(TAG, "Stats OK, génération HTML...");
    String html = PageLogs::getHtml(stats);
    Console::info(TAG, "HTML généré, taille=" + String(html.length()));
    request->send(200, "text/html", html);
}

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleActuators(AsyncWebServerRequest *request)
{
    String html = PageActuators::getHtml();
    request->send(200, "text/html", html);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /command — via DataBus
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleCommandBody(AsyncWebServerRequest *request,
                                  uint8_t *data, size_t len,
                                  size_t index, size_t total)
{
    if (index == 0) {
        if (total > 256) {
            Console::warn(TAG, "POST /command : body " + String((uint32_t)total) +
                          " octets rejeté (>256)");
            return;
        }
        String* body = new String();
        if (!body) return;
        body->reserve(total);
        request->_tempObject = body;

        request->onDisconnect([request]() {
            if (request->_tempObject) {
                delete (String*)request->_tempObject;
                request->_tempObject = nullptr;
            }
        });
    }

    String* body = (String*)request->_tempObject;
    if (!body) return;
    for (size_t i = 0; i < len; i++) body->concat((char)data[i]);
}

void WebServer::handleCommandFinal(AsyncWebServerRequest *request)
{
    String* body = (String*)request->_tempObject;

    if (!body || body->length() == 0) {
        request->send(400, "text/plain", "Body vide ou trop volumineux");
        return;
    }

    BusItem item;
    auto res = DataBus::parseCommand(body->c_str(), body->length(), item);

    delete body;
    request->_tempObject = nullptr;

    switch (res) {
        case CommandParseResult::OK:
            break;
        case CommandParseResult::BadFormat:
            request->send(400, "text/plain", "CSV format invalide");
            return;
        case CommandParseResult::TimestampSet:
            request->send(400, "text/plain", "Horodatage non autorise");
            return;
        case CommandParseResult::InvalidType:
            request->send(400, "text/plain", "type doit etre 5 (Manual) ou 6 (Auto)");
            return;
        case CommandParseResult::UnknownId:
            request->send(400, "text/plain", "id inconnu de META");
            return;
        case CommandParseResult::NotACommand:
            request->send(400, "text/plain", "id n'est pas une commande");
            return;
        case CommandParseResult::BadValueType:
            request->send(400, "text/plain", "valueType doit etre 0");
            return;
        case CommandParseResult::BadValue:
            request->send(400, "text/plain", "value doit etre > 0");
            return;
    }

    DataBus::publishCommand(item);

    Console::info(TAG, "Commande HTTP acceptée : id=" +
                  String((uint8_t)item.id) +
                  " durée=" + String((uint32_t)(item.valueFloat * 1000.0f)) + "ms");
    request->send(204);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bundle download — multi-fichiers log_*.csv
// ─────────────────────────────────────────────────────────────────────────────

static void buildBundleHeader(String& p)
{
    p += "#SERRE_BUNDLE\n";
    p += "#SCHEMA_JSON_BEGIN\n";
    p += "{\n";

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

    p += "  \"csvColumns\": [\"timestamp\", \"VClock_available\", \"VClock_reliable\", "
         "\"type\", \"id\", \"valueType\", \"value\"],\n";

    p += "  \"dataTypes\": [\n";
    bool firstType = true;
    for (uint8_t t = 0; t <= (uint8_t)DataType::CommandAuto; t++) {
        if (!firstType) p += ",\n";
        firstType = false;
        p += "    {\"id\": "; p += t;
        p += ", \"label\": \"";
        p += jsonEscape(typeLabel((DataType)t));
        p += "\"}";
    }
    p += "\n  ],\n";

    p += "  \"dataIds\": [\n";
    for (size_t i = 0; i < META_COUNT; i++) {
        const DataMeta& m = META[i];

        p += "    {\"id\": "; p += (uint8_t)m.id;
        p += ", \"label\": \""; p += jsonEscape(m.label); p += "\"";
        p += ", \"unit\": \"";  p += jsonEscape(m.unit);  p += "\"";

        const char* natureStr =
            (m.nature == DataNature::metrique) ? "metrique" :
            (m.nature == DataNature::etat)     ? "etat"     : "texte";
        p += ", \"nature\": \""; p += natureStr; p += "\"";

        p += ", \"type\": "; p += (uint8_t)m.type;

        if (m.nature == DataNature::metrique) {
            p += ", \"min\": "; p += String(m.min, 1);
            p += ", \"max\": "; p += String(m.max, 1);
        }

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
        if (i < META_COUNT - 1) p += ",";
        p += "\n";
    }
    p += "  ]\n";
    p += "}\n";
    p += "#SCHEMA_JSON_END\n";
    p += "#DATA_CSV_BEGIN\n";
}

// ─────────────────────────────────────────────────────────────────────────────

struct BundleContext {
    String  pending;
    String* filePaths;        // Tableau trié des chemins log_*.csv
    size_t  fileCount;        // Nombre total de fichiers
    size_t  currentFileIdx;   // Index du fichier en cours de lecture
    File    currentFile;      // Handle du fichier en cours
    bool    headerDone;
    bool    footerDone;
    bool    deleted;
    char    filename[44];

    BundleContext()
        : filePaths(nullptr), fileCount(0), currentFileIdx(0),
          headerDone(false), footerDone(false), deleted(false)
    {
        filename[0] = '\0';
        pending.reserve(4096);
    }
};

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogsDownload(AsyncWebServerRequest *request)
{
    // Collecte et tri des fichiers log
    size_t fileCount = 0;
    String* files = collectSortedLogFiles(fileCount);

    if (!files || fileCount == 0) {
        delete[] files;
        request->send(404, "text/plain", "Aucune donnée disponible");
        Console::warn(TAG, "Bundle download demandé mais aucun fichier log");
        return;
    }

    BundleContext* ctx = new (std::nothrow) BundleContext();
    if (!ctx) {
        delete[] files;
        request->send(500, "text/plain", "Mémoire insuffisante");
        Console::error(TAG, "Bundle download : allocation contexte échouée");
        return;
    }

    ctx->filePaths = files;
    ctx->fileCount = fileCount;

    // Ouvrir le premier fichier
    ctx->currentFile = LittleFS.open(files[0], FILE_READ);
    ctx->currentFileIdx = 0;

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
            if (ctx->currentFile) ctx->currentFile.close();
            Console::warn("BundleCtx", "Bundle interrompu (disconnect client)");
        } else {
            Console::debug("BundleCtx", "Libération contexte (transfert terminé)");
        }
        delete[] ctx->filePaths;
        ctx->filePaths = nullptr;
        delete ctx;
    });

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "text/plain; charset=utf-8",
        [ctx](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {

            if (ctx->deleted) return 0;

            // ── Vider le pending (reste d'un envoi précédent) ──────────
            if (ctx->pending.length() > 0) {
                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // ── Header (une seule fois) ────────────────────────────────
            if (!ctx->headerDone) {
                buildBundleHeader(ctx->pending);
                ctx->headerDone = true;

                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // ── Données : lecture séquentielle des fichiers ────────────
            while (ctx->currentFileIdx < ctx->fileCount) {
                // Ouvrir le fichier courant s'il ne l'est pas
                if (!ctx->currentFile) {
                    ctx->currentFile = LittleFS.open(
                        ctx->filePaths[ctx->currentFileIdx], FILE_READ);
                    if (!ctx->currentFile) {
                        // Fichier inaccessible, passer au suivant
                        ctx->currentFileIdx++;
                        continue;
                    }
                }

                // Lire depuis le fichier courant
                if (ctx->currentFile.available()) {
                    return ctx->currentFile.read(buffer, maxLen);
                }

                // Fichier terminé → fermer et passer au suivant
                ctx->currentFile.close();
                ctx->currentFileIdx++;
            }

            // ── Footer (une seule fois, après tous les fichiers) ───────
            if (!ctx->footerDone) {
                ctx->pending += "\n#DATA_CSV_END\n";
                ctx->footerDone = true;

                size_t toSend = min(ctx->pending.length(), maxLen);
                memcpy(buffer, ctx->pending.c_str(), toSend);
                ctx->pending = ctx->pending.substring(toSend);
                return toSend;
            }

            // ── Terminé ────────────────────────────────────────────────
            Console::info("BundleCtx",
                String("Bundle terminé → ") + ctx->filename
                + " (" + String(ctx->fileCount) + " fichiers)");
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
    Console::info(TAG, String("Bundle download démarré → ") + ctx->filename
                  + " (" + String(fileCount) + " fichiers)");
}

void WebServer::handleLogsClear(AsyncWebServerRequest *request)
{
    DataLogger::clearHistory();
    request->send(200, "text/plain", "Historique supprimé avec succès");
    Console::info(TAG, "Logs supprimés par l'utilisateur");
}