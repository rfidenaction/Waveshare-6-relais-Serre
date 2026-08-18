// Web/WebServer.cpp
// Portage Waveshare ESP32-S3-Relay-6CH
//
// lastDataForWeb[] hébergé ici, protégé par portMUX.
// buildBundleHeader() utilise typeLabel/jsonEscape (MetaDataModel.h).
// handleCommandFinal() utilise DataBus::parseCommand/publish.
#include "Web/WebServer.h"

#include "Web/Pages/PagePrincipale.h"
#include "Web/Pages/PageLogs.h"
#include "Web/Pages/PageActuators.h"
#include "Web/Pages/PageRS485.h"
#include "Sensors/SoilSensorRS485.h"
#include "Connectivity/WiFiManager.h"
#include "Storage/DataLogger.h"
#include "Storage/HistoryQuery.h"
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

    server.on("/rs485",         HTTP_GET,  handleRS485);
    server.on("/rs485/setaddr", HTTP_POST, handleRS485SetAddr);
    server.on("/rs485/exit",    HTTP_POST, handleRS485Exit);

    // ── Capteurs air Ebyte KTH2-R — configuration ─────────────────
    server.on("/rs485/read-ebyte",    HTTP_POST, handleRS485ReadEbyte);
    server.on("/rs485/program-ebyte", HTTP_POST, handleRS485ProgramEbyte);

    // ── Analog Input 8CH (B) — configuration ────────────────────────
    server.on("/rs485/read-analog",         HTTP_POST, handleRS485ReadAnalog);
    server.on("/rs485/program-analog",      HTTP_POST, handleRS485ProgramAnalog);
    server.on("/rs485/read-analog-channel",  HTTP_POST, handleRS485ReadAnalogChannel);
    server.on("/rs485/write-analog-channel", HTTP_POST, handleRS485WriteAnalogChannel);

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

    DataBus::publish(item);

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
    for (uint8_t t = 0; t <= (uint8_t)DataType::CommandConditional; t++) {
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

// ─────────────────────────────────────────────────────────────────────────────
// RS485 — Page de programmation des adresses capteurs
// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleRS485(AsyncWebServerRequest *request)
{
    String html = PageRS485::getHtml();
    request->send(200, "text/html", html);
}

void WebServer::handleRS485SetAddr(AsyncWebServerRequest *request)
{
    if (!request->hasParam("to")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètre to requis\"}");
        return;
    }

    uint8_t toAddr = request->getParam("to")->value().toInt();

    if (toAddr < 1 || toAddr > 15) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Adresse cible hors bornes (1-15)\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    uint8_t currentAddr = SoilSensorRS485::findCurrentAddress();

    if (currentAddr == 0) {
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Aucun capteur détecté sur le bus\"}");
        return;
    }

    if (currentAddr == toAddr) {
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"Le capteur est déjà à cette adresse\"}");
        return;
    }

    bool ok = SoilSensorRS485::setAddress(currentAddr, toAddr);
    SoilSensorRS485::setMaintenanceMode(false);

    if (ok) {
        request->send(200, "application/json", "{\"ok\":true}");
    } else {
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Échec de programmation\"}");
    }
}

void WebServer::handleRS485Exit(AsyncWebServerRequest *request)
{
    SoilSensorRS485::setMaintenanceMode(false);
    request->send(204);
}

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogsClear(AsyncWebServerRequest *request)
{
    // Un scan d'historique peut tenir un fichier journal ouvert en lecture.
    // Supprimer un fichier dans cet état sort du domaine défini de littlefs :
    // on referme d'abord.
    HistoryQuery::abortScan();

    DataLogger::clearHistory();
    request->send(200, "text/plain", "Historique supprimé avec succès");
    Console::info(TAG, "Logs supprimés par l'utilisateur");
}

// ═════════════════════════════════════════════════════════════════════════════
// Analog Input 8CH (B) — configuration via RS485
//
// Fonctions utilitaires et handlers pour lire la configuration actuelle
// du module Waveshare Modbus RTU Analog Input 8CH (B) et le reprogrammer
// (baud rate + adresse Modbus).
//
// Protocole : Modbus RTU standard.
//   - Lecture adresse   : fonction 0x03, registre 0x4000
//   - Lecture version   : fonction 0x03, registre 0x8000
//   - Écriture baud rate: fonction 0x06, registre 0x2000
//   - Écriture adresse  : fonction 0x06, registre 0x4000
//   - Adresse broadcast : 0x00
//
// Ref : https://www.waveshare.com/wiki/Modbus_RTU_Analog_Input_8CH_(B)
// ═════════════════════════════════════════════════════════════════════════════

static const char* TAG_AI = "AnalogInput";

// ── CRC16 Modbus RTU (copie autonome, n'utilise pas SoilSensorRS485) ────────

static uint16_t analogInputCrc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// ── Purge du buffer RX de Serial1 ───────────────────────────────────────────

static void analogInputDrainRx()
{
    while (Serial1.available()) {
        Serial1.read();
    }
}

// ── Transaction Modbus : envoi d'une trame et réception de la réponse ───────
//
// Envoie request (requestLen octets, CRC déjà inclus) sur Serial1,
// attend jusqu'à expectedLen octets pendant timeoutMs.
// Retourne le nombre d'octets reçus dans response[].

static size_t analogInputTransaction(const uint8_t* request, size_t requestLen,
                                     uint8_t* response, size_t expectedLen,
                                     unsigned long timeoutMs = 200)
{
    analogInputDrainRx();

    Serial1.write(request, requestLen);
    Serial1.flush();

    size_t idx = 0;
    unsigned long startMs = millis();
    while (idx < expectedLen && (millis() - startMs) < timeoutMs) {
        if (Serial1.available()) {
            response[idx++] = Serial1.read();
        }
    }
    return idx;
}

// ── Envoi d'une requête Modbus 0x03 (Read Holding Register) ─────────────────
//
// Lit 1 registre à regAddr sur le device à deviceAddr.
// Retourne true si la réponse est valide, et place la valeur 16 bits
// dans outValue.

static bool analogInputReadRegister(uint8_t deviceAddr, uint16_t regAddr,
                                    uint16_t& outValue,
                                    unsigned long timeoutMs = 200)
{
    uint8_t request[8];
    request[0] = deviceAddr;
    request[1] = 0x03;
    request[2] = (regAddr >> 8) & 0xFF;
    request[3] = regAddr & 0xFF;
    request[4] = 0x00;
    request[5] = 0x01;

    uint16_t crc = analogInputCrc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    uint8_t response[16];
    size_t rxLen = analogInputTransaction(request, 8, response, 7, timeoutMs);

    if (rxLen < 7) return false;

    uint16_t rxCrc  = response[5] | ((uint16_t)response[6] << 8);
    uint16_t chkCrc = analogInputCrc16(response, 5);
    if (rxCrc != chkCrc) return false;

    if (response[1] != 0x03) return false;
    if (response[2] != 0x02) return false;

    outValue = ((uint16_t)response[3] << 8) | response[4];
    return true;
}

// ── Envoi d'une requête Modbus 0x06 (Write Single Register) ─────────────────
//
// Écrit value dans le registre regAddr du device à deviceAddr.
// Le module renvoie un écho identique si l'écriture réussit.

static bool analogInputWriteRegister(uint8_t deviceAddr, uint16_t regAddr,
                                     uint16_t value)
{
    uint8_t request[8];
    request[0] = deviceAddr;
    request[1] = 0x06;
    request[2] = (regAddr >> 8) & 0xFF;
    request[3] = regAddr & 0xFF;
    request[4] = (value >> 8) & 0xFF;
    request[5] = value & 0xFF;

    uint16_t crc = analogInputCrc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    uint8_t response[16];
    size_t rxLen = analogInputTransaction(request, 8, response, 8);

    if (rxLen < 8) return false;

    return (memcmp(request, response, 8) == 0);
}

// ── Scan : recherche du module sur les adresses 1–30, à un baud rate donné ──
//
// Essaie de lire le registre d'adresse (0x4000) via broadcast (0x00).
// Si le module répond, on obtient son adresse dans la réponse.
// Si broadcast échoue, scanne individuellement les adresses 1–30.
// Retourne l'adresse trouvée, ou 0 si aucun module ne répond.

static uint8_t analogInputScan(uint32_t baudRate)
{
    Serial1.end();
    Serial1.begin(baudRate, SERIAL_8N1, 18, 17);  // RX=GPIO18, TX=GPIO17
    delay(20);

    uint16_t readAddr = 0;

    // Tentative broadcast d'abord (rapide, un seul module sur le bus)
    // Timeout 50 ms : le module répond en < 15 ms, même à 4800 bauds.
    if (analogInputReadRegister(0x00, 0x4000, readAddr, 50)) {
        Console::info(TAG_AI, "Module trouvé via broadcast à " + String(baudRate)
                              + " bauds — adresse " + String(readAddr));
        return (uint8_t)readAddr;
    }

    // Scan individuel adresses 1–30
    for (uint8_t addr = 1; addr <= 30; addr++) {
        if (analogInputReadRegister(addr, 0x4000, readAddr, 50)) {
            Console::info(TAG_AI, "Module trouvé à l'adresse " + String(addr)
                                  + " (" + String(baudRate) + " bauds)");
            return addr;
        }
    }

    return 0;
}

// ── Correspondance code baud rate ↔ valeur numérique ────────────────────────

static const uint32_t AI_BAUD_TABLE[] = {
    4800, 9600, 19200, 38400, 57600, 115200, 128000, 256000
};
static const size_t AI_BAUD_TABLE_SIZE = sizeof(AI_BAUD_TABLE) / sizeof(AI_BAUD_TABLE[0]);

static uint32_t analogInputBaudFromCode(uint8_t code)
{
    if (code < AI_BAUD_TABLE_SIZE) return AI_BAUD_TABLE[code];
    return 0;
}

static uint8_t analogInputCodeFromBaud(uint32_t baud)
{
    for (uint8_t i = 0; i < AI_BAUD_TABLE_SIZE; i++) {
        if (AI_BAUD_TABLE[i] == baud) return i;
    }
    return 0xFF;
}

// ═════════════════════════════════════════════════════════════════════════════
// Capteurs air Ebyte KTH2-R — configuration via RS485
//
// Registres Modbus (Holding Registers, 0x03 lecture / 0x10 écriture) :
//   - 0x000C : adresse esclave (1–254)
//   - 0x000D : baud rate (0=1200, 1=2400, 2=4800, 3=9600, 4=19200)
//   - 0x000E : parité (0=aucune, 1=impaire, 2=paire)
//   - 0x0300 : température (lecture seule, valeur × 0.1 °C)
//   - 0x0301 : humidité (lecture seule, valeur × 0.1 %RH)
// Défauts usine : adresse 1, 9600 bauds, pas de parité.
// Le KTH2-R n'échoie pas le 0x06 (Write Single Register). L'écriture passe
// par 0x10 ; l'écho, s'il arrive, est encore à l'ancienne vitesse.
//
// Réutilise analogInputCrc16 / analogInputTransaction / analogInputReadRegister.
// ═════════════════════════════════════════════════════════════════════════════

static const char* TAG_EB = "Ebyte";

// ── Scan Ebyte : adresses 1–16, à un baud rate donné ────────────────────────
// Pas de broadcast pour la lecture (le capteur Ebyte suit le standard Modbus
// où broadcast = écriture seule, pas de réponse).

static uint8_t ebyteScan(uint32_t baudRate)
{
    Serial1.end();
    Serial1.begin(baudRate, SERIAL_8N1, 18, 17);
    delay(20);

    uint16_t readAddr = 0;

    for (uint8_t addr = 1; addr <= 16; addr++) {
        if (analogInputReadRegister(addr, 0x000C, readAddr, 50)) {
            Console::info(TAG_EB, "Capteur trouvé à l'adresse " + String(addr)
                                  + " (" + String(baudRate) + " bauds)");
            return addr;
        }
    }

    return 0;
}

// ── Correspondance code baud rate Ebyte ↔ valeur numérique ──────────────────

static const uint32_t EB_BAUD_TABLE[] = { 1200, 2400, 4800, 9600, 19200 };
static const size_t EB_BAUD_TABLE_SIZE = sizeof(EB_BAUD_TABLE) / sizeof(EB_BAUD_TABLE[0]);

static uint32_t ebyteBaudFromCode(uint8_t code)
{
    if (code < EB_BAUD_TABLE_SIZE) return EB_BAUD_TABLE[code];
    return 0;
}

// Écriture d'un holding register Ebyte — fonction 0x10 (1 registre).
// Réponse attendue (8 octets) : [addr] [0x10] [regH] [regL] [00] [01] [crcL] [crcH]
static bool ebyteWriteRegister(uint8_t deviceAddr, uint16_t regAddr, uint16_t value)
{
    uint8_t request[11];
    request[0] = deviceAddr;
    request[1] = 0x10;
    request[2] = (regAddr >> 8) & 0xFF;
    request[3] = regAddr & 0xFF;
    request[4] = 0x00;
    request[5] = 0x01;
    request[6] = 0x02;
    request[7] = (value >> 8) & 0xFF;
    request[8] = value & 0xFF;

    uint16_t crc = analogInputCrc16(request, 9);
    request[9]  = crc & 0xFF;
    request[10] = (crc >> 8) & 0xFF;

    uint8_t response[16];
    size_t rxLen = analogInputTransaction(request, 11, response, 8);

    if (rxLen < 8) return false;

    uint16_t rxCrc  = response[6] | ((uint16_t)response[7] << 8);
    uint16_t chkCrc = analogInputCrc16(response, 6);
    if (rxCrc != chkCrc) return false;

    if (response[0] != deviceAddr) return false;
    if (response[1] != 0x10) return false;
    if (response[2] != request[2] || response[3] != request[3]) return false;
    if (response[4] != 0x00 || response[5] != 0x01) return false;

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/read-ebyte — lecture de la configuration actuelle
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ReadEbyte(AsyncWebServerRequest *request)
{
    SoilSensorRS485::setMaintenanceMode(true);

    uint8_t foundAddr = 0;
    uint32_t foundBaud = 0;

    foundAddr = ebyteScan(9600);
    if (foundAddr != 0) {
        foundBaud = 9600;
    } else {
        foundAddr = ebyteScan(4800);
        if (foundAddr != 0) {
            foundBaud = 4800;
        }
    }

    if (foundAddr == 0) {
        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        SoilSensorRS485::setMaintenanceMode(false);

        Console::warn(TAG_EB, "Aucun capteur Ebyte détecté (scan 1-16, 9600+4800)");
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Aucun capteur détecté (adresses 1–16, 9600 et 4800 bauds)\"}");
        return;
    }

    // Lire le baud rate configuré (registre 0x000D) pour confirmation
    uint16_t baudCode = 0;
    analogInputReadRegister(foundAddr, 0x000D, baudCode);
    uint32_t reportedBaud = ebyteBaudFromCode((uint8_t)baudCode);
    if (reportedBaud == 0) reportedBaud = foundBaud;

    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    SoilSensorRS485::setMaintenanceMode(false);

    Console::info(TAG_EB, "Capteur détecté — adresse=" + String(foundAddr)
                          + " baud=" + String(reportedBaud));

    String json = "{\"ok\":true,\"address\":" + String(foundAddr)
                + ",\"baudrate\":" + String(reportedBaud) + "}";
    request->send(200, "application/json", json);
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/program-ebyte — programmation baud rate + adresse
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ProgramEbyte(AsyncWebServerRequest *request)
{
    if (!request->hasParam("to")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètre 'to' requis\"}");
        return;
    }

    uint8_t toAddr = request->getParam("to")->value().toInt();
    if (toAddr < 1 || toAddr > 16) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Adresse cible hors bornes (1–16)\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    // ── Étape 1 : trouver le capteur ────────────────────────────────────
    uint8_t foundAddr = 0;
    uint32_t foundBaud = 0;

    foundAddr = ebyteScan(9600);
    if (foundAddr != 0) {
        foundBaud = 9600;
    } else {
        foundAddr = ebyteScan(4800);
        if (foundAddr != 0) {
            foundBaud = 4800;
        }
    }

    if (foundAddr == 0) {
        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        SoilSensorRS485::setMaintenanceMode(false);

        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Capteur non détecté — programmation annulée\"}");
        return;
    }

    bool needBaudChange = (foundBaud != 4800);
    bool needAddrChange = (foundAddr != toAddr);

    // ── Étape 2 : changer le baud rate vers 4800 si nécessaire ──────────
    //
    // Écriture 0x10 (le 0x06 n'obtient pas d'écho sur ce capteur). On attend
    // la réponse à l'ancienne vitesse, puis on bascule Serial1 à 4800.
    // Si l'écho manque, la lecture de vérif tranche.
    if (needBaudChange) {
        bool echoOk = ebyteWriteRegister(foundAddr, 0x000D, 2);  // code 2 = 4800

        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        delay(50);

        uint16_t verifyVal = 0;
        bool baudOk = analogInputReadRegister(foundAddr, 0x000C, verifyVal, 200);
        if (!baudOk) {
            Serial1.end();
            Serial1.begin(foundBaud, SERIAL_8N1, 18, 17);
            delay(20);
            uint16_t stillThere = 0;
            bool stillAtOld = analogInputReadRegister(foundAddr, 0x000C, stillThere, 200);

            Serial1.end();
            Serial1.begin(4800, SERIAL_8N1, 18, 17);
            SoilSensorRS485::setMaintenanceMode(false);

            if (stillAtOld) {
                Console::warn(TAG_EB, "Capteur toujours à " + String(foundBaud)
                                  + (echoOk ? " malgré l'écho" : " (pas d'écho)"));
                request->send(200, "application/json",
                              "{\"ok\":false,\"error\":\"Le capteur est toujours à "
                              + String(foundBaud) + " bauds\"}");
            } else {
                Console::warn(TAG_EB, "Capteur muet à 4800 et à " + String(foundBaud));
                request->send(200, "application/json",
                              "{\"ok\":false,\"error\":\"Changement de baud rate envoyé mais le capteur ne répond ni à 4800 ni à "
                              + String(foundBaud) + " bauds\"}");
            }
            return;
        }
        Console::info(TAG_EB, "Baud rate changé de " + String(foundBaud) + " vers 4800 (vérifié)");
    }

    // ── Étape 3 : changer l'adresse si nécessaire ───────────────────────
    // Même 0x10. Si l'écho manque, l'étape 4 tranche par lecture.
    if (needAddrChange) {
        bool ok = ebyteWriteRegister(foundAddr, 0x000C, (uint16_t)toAddr);
        if (ok) {
            Console::info(TAG_EB, "Adresse changée de " + String(foundAddr) + " vers " + String(toAddr));
        } else {
            Console::warn(TAG_EB, "Pas d'écho 0x10 pour l'adresse " + String(toAddr)
                              + " — vérification par lecture");
        }
    }

    // ── Étape 4 : vérification — lire l'adresse à la nouvelle adresse ───
    delay(50);
    uint16_t verifyAddr = 0;
    bool verified = analogInputReadRegister(toAddr, 0x000C, verifyAddr);

    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    SoilSensorRS485::setMaintenanceMode(false);

    if (verified && verifyAddr == toAddr) {
        String msg = "Capteur configuré — adresse " + String(toAddr) + ", 4800 bauds";
        if (!needBaudChange && !needAddrChange) {
            msg = "Capteur déjà configuré à l'adresse " + String(toAddr) + " en 4800 bauds";
        }
        Console::info(TAG_EB, msg);
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"" + msg + "\"}");
    } else {
        Console::warn(TAG_EB, "Vérification échouée après programmation");
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Commandes envoyées mais vérification échouée — relancez une lecture pour vérifier\"}");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Fin — Capteurs air Ebyte KTH2-R
// ═════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/read-analog — lecture de la configuration actuelle
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ReadAnalog(AsyncWebServerRequest *request)
{
    SoilSensorRS485::setMaintenanceMode(true);

    uint8_t foundAddr = 0;
    uint32_t foundBaud = 0;

    // Essai à 9600 d'abord (défaut usine), puis 4800 (valeur cible)
    foundAddr = analogInputScan(9600);
    if (foundAddr != 0) {
        foundBaud = 9600;
    } else {
        foundAddr = analogInputScan(4800);
        if (foundAddr != 0) {
            foundBaud = 4800;
        }
    }

    if (foundAddr == 0) {
        // Restaurer Serial1 à 4800 pour les capteurs sol
        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        SoilSensorRS485::setMaintenanceMode(false);

        Console::warn(TAG_AI, "Aucun module Analog Input détecté (scan 1-30, 9600+4800)");
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Aucun module détecté (adresses 1–30, 9600 et 4800 bauds)\"}");
        return;
    }

    // Lire la version firmware (registre 0x8000)
    uint16_t rawVersion = 0;
    analogInputReadRegister(foundAddr, 0x8000, rawVersion);
    String versionStr = "V" + String(rawVersion / 100) + "."
                        + String((rawVersion % 100) / 10)
                        + String(rawVersion % 10);

    // Restaurer Serial1 à 4800 pour les capteurs sol
    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    SoilSensorRS485::setMaintenanceMode(false);

    Console::info(TAG_AI, "Module détecté — adresse=" + String(foundAddr)
                          + " baud=" + String(foundBaud)
                          + " version=" + versionStr);

    String json = "{\"ok\":true,\"address\":" + String(foundAddr)
                + ",\"baudrate\":" + String(foundBaud)
                + ",\"version\":\"" + versionStr + "\"}";
    request->send(200, "application/json", json);
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/program-analog — programmation baud rate + adresse
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ProgramAnalog(AsyncWebServerRequest *request)
{
    if (!request->hasParam("to")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètre 'to' requis\"}");
        return;
    }

    uint8_t toAddr = request->getParam("to")->value().toInt();
    if (toAddr < 16 || toAddr > 30) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Adresse cible hors bornes (16–30)\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    // ── Étape 1 : trouver le module ─────────────────────────────────────
    uint8_t foundAddr = 0;
    uint32_t foundBaud = 0;

    foundAddr = analogInputScan(9600);
    if (foundAddr != 0) {
        foundBaud = 9600;
    } else {
        foundAddr = analogInputScan(4800);
        if (foundAddr != 0) {
            foundBaud = 4800;
        }
    }

    if (foundAddr == 0) {
        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        SoilSensorRS485::setMaintenanceMode(false);

        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Module non détecté — programmation annulée\"}");
        return;
    }

    bool needBaudChange = (foundBaud != 4800);
    bool needAddrChange = (foundAddr != toAddr);

    // ── Étape 2 : changer le baud rate vers 4800 si nécessaire ──────────
    //
    // Le module bascule immédiatement après réception de la commande.
    // L'écho de confirmation arrive donc à la NOUVELLE vitesse (4800),
    // illisible si Serial1 est encore à l'ancienne (9600).
    // → On envoie la commande sans vérifier l'écho, on bascule Serial1
    //   à 4800, puis on vérifie en lisant un registre.
    if (needBaudChange) {
        uint8_t cmdBaud[8];
        cmdBaud[0] = 0x00;   // broadcast
        cmdBaud[1] = 0x06;   // Write Single Register
        cmdBaud[2] = 0x20;   // registre 0x2000 (UART parameter)
        cmdBaud[3] = 0x00;
        cmdBaud[4] = 0x00;   // pas de parité
        cmdBaud[5] = 0x00;   // code 0 = 4800 bauds
        uint16_t crc = analogInputCrc16(cmdBaud, 6);
        cmdBaud[6] = crc & 0xFF;
        cmdBaud[7] = (crc >> 8) & 0xFF;

        analogInputDrainRx();
        Serial1.write(cmdBaud, 8);
        Serial1.flush();

        // Basculer Serial1 à 4800 et vérifier que le module répond
        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        delay(50);

        uint16_t verifyVal = 0;
        bool baudOk = analogInputReadRegister(foundAddr, 0x4000, verifyVal);
        if (!baudOk) {
            SoilSensorRS485::setMaintenanceMode(false);

            Console::warn(TAG_AI, "Baud rate envoyé mais module muet à 4800");
            request->send(200, "application/json",
                          "{\"ok\":false,\"error\":\"Changement de baud rate envoyé mais le module ne répond pas à 4800\"}");
            return;
        }
        Console::info(TAG_AI, "Baud rate changé de " + String(foundBaud) + " vers 4800 (vérifié)");
    }

    // ── Étape 3 : changer l'adresse si nécessaire ───────────────────────
    if (needAddrChange) {
        bool ok = analogInputWriteRegister(0x00, 0x4000, (uint16_t)toAddr);
        if (!ok) {
            Serial1.end();
            Serial1.begin(4800, SERIAL_8N1, 18, 17);
            SoilSensorRS485::setMaintenanceMode(false);

            Console::warn(TAG_AI, "Échec du changement d'adresse vers " + String(toAddr));
            request->send(200, "application/json",
                          "{\"ok\":false,\"error\":\"Baud rate OK mais échec du changement d'adresse\"}");
            return;
        }
        Console::info(TAG_AI, "Adresse changée de " + String(foundAddr) + " vers " + String(toAddr));
    }

    // ── Étape 4 : vérification — lire l'adresse à la nouvelle adresse ───
    delay(50);
    uint16_t verifyAddr = 0;
    bool verified = analogInputReadRegister(toAddr, 0x4000, verifyAddr);

    // Restaurer Serial1 à 4800 pour les capteurs sol
    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    SoilSensorRS485::setMaintenanceMode(false);

    if (verified && verifyAddr == toAddr) {
        String msg = "Module configuré — adresse " + String(toAddr) + ", 4800 bauds";
        if (!needBaudChange && !needAddrChange) {
            msg = "Module déjà configuré à l'adresse " + String(toAddr) + " en 4800 bauds";
        }
        Console::info(TAG_AI, msg);
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"" + msg + "\"}");
    } else {
        Console::warn(TAG_AI, "Vérification échouée après programmation");
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Commandes envoyées mais vérification échouée — relancez une lecture pour vérifier\"}");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/read-analog-channel — lecture du mode d'un canal
//
// Paramètres GET :
//   ch   = numéro de canal (1–8)
//   addr = adresse Modbus du module (trouvée lors du scan initial)
//
// Lit le registre 0x1000 + (ch-1) via fonction 0x03 (Read Holding Register).
// Retourne le mode (0–4) correspondant à la plage configurée.
//   Version B : 0=0–10V, 1=2–10V, 2=0–20mA, 3=4–20mA, 4=code brut 4096
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ReadAnalogChannel(AsyncWebServerRequest *request)
{
    if (!request->hasParam("ch") || !request->hasParam("addr")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres 'ch' et 'addr' requis\"}");
        return;
    }

    uint8_t channel = request->getParam("ch")->value().toInt();
    uint8_t addr    = request->getParam("addr")->value().toInt();

    if (channel < 1 || channel > 8) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Canal hors bornes (1–8)\"}");
        return;
    }

    if (addr < 1 || addr > 255) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Adresse hors bornes (1–255)\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    delay(20);

    uint16_t regAddr = 0x1000 + (channel - 1);
    uint16_t modeValue = 0;
    bool ok = analogInputReadRegister(addr, regAddr, modeValue, 200);

    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    SoilSensorRS485::setMaintenanceMode(false);

    if (!ok) {
        Console::warn(TAG_AI, "Échec lecture mode canal " + String(channel)
                              + " (adresse " + String(addr) + ")");
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Le module ne répond pas — vérifiez qu'il est toujours connecté\"}");
        return;
    }

    Console::info(TAG_AI, "Canal " + String(channel) + " — mode=" + String(modeValue));

    String json = "{\"ok\":true,\"channel\":" + String(channel)
                + ",\"mode\":" + String(modeValue) + "}";
    request->send(200, "application/json", json);
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/write-analog-channel — écriture du mode d'un canal
//
// Paramètres GET :
//   ch   = numéro de canal (1–8)
//   addr = adresse Modbus du module
//   mode = valeur du mode à écrire (0–4)
//
// Écrit dans le registre 0x1000 + (ch-1) via fonction 0x06, puis relit
// le registre pour vérifier que l'écriture a pris effet.
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485WriteAnalogChannel(AsyncWebServerRequest *request)
{
    if (!request->hasParam("ch") || !request->hasParam("addr") || !request->hasParam("mode")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres 'ch', 'addr' et 'mode' requis\"}");
        return;
    }

    uint8_t  channel  = request->getParam("ch")->value().toInt();
    uint8_t  addr     = request->getParam("addr")->value().toInt();
    uint16_t newMode  = request->getParam("mode")->value().toInt();

    if (channel < 1 || channel > 8) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Canal hors bornes (1–8)\"}");
        return;
    }

    if (addr < 1 || addr > 255) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Adresse hors bornes (1–255)\"}");
        return;
    }

    if (newMode > 4) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Mode hors bornes (0–4)\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    delay(20);

    uint16_t regAddr = 0x1000 + (channel - 1);

    bool writeOk = analogInputWriteRegister(addr, regAddr, newMode);

    if (!writeOk) {
        Serial1.end();
        Serial1.begin(4800, SERIAL_8N1, 18, 17);
        SoilSensorRS485::setMaintenanceMode(false);

        Console::warn(TAG_AI, "Échec écriture mode canal " + String(channel));
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Échec de l'écriture — le module n'a pas confirmé\"}");
        return;
    }

    delay(50);

    uint16_t verifyMode = 0xFFFF;
    bool readOk = analogInputReadRegister(addr, regAddr, verifyMode, 200);

    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, 18, 17);
    SoilSensorRS485::setMaintenanceMode(false);

    if (readOk && verifyMode == newMode) {
        Console::info(TAG_AI, "Canal " + String(channel) + " — mode corrigé à " + String(newMode));
        String json = "{\"ok\":true,\"channel\":" + String(channel)
                    + ",\"mode\":" + String(verifyMode) + "}";
        request->send(200, "application/json", json);
    } else if (readOk) {
        Console::warn(TAG_AI, "Canal " + String(channel) + " — écriture envoyée mais relecture="
                              + String(verifyMode) + " (attendu " + String(newMode) + ")");
        String json = "{\"ok\":false,\"error\":\"Écriture envoyée mais relecture incohérente (lu "
                    + String(verifyMode) + " au lieu de " + String(newMode)
                    + ") — réessayez\",\"mode\":" + String(verifyMode) + "}";
        request->send(200, "application/json", json);
    } else {
        Console::warn(TAG_AI, "Canal " + String(channel) + " — écriture OK mais relecture échouée");
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Écriture envoyée mais relecture échouée — vérifiez la connexion\"}");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Fin — Analog Input 8CH (B)
// ═════════════════════════════════════════════════════════════════════════════