// Web/WebServer.cpp
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
#include "Config/IO-Config.h"          // RS485_TX_PIN, RS485_RX_PIN
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
    server.on("/logs/clear/status", HTTP_GET, handleLogsClearStatus);
    server.on("/logs", HTTP_GET, handleLogs);

    server.on("/actuators", HTTP_GET, handleActuators);

    server.on("/rs485",         HTTP_GET,  handleRS485);
    server.on("/rs485/setaddr", HTTP_POST, handleRS485SetAddr);
    server.on("/rs485/exit",    HTTP_POST, handleRS485Exit);

    server.on("/rs485/read-soil",    HTTP_POST, handleRS485ReadSoil);
    server.on("/rs485/program-soil", HTTP_POST, handleRS485ProgramSoil);

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

// Les handlers sol (read-soil / program-soil) sont plus bas, avec les
// utilitaires Modbus et les tables de baud.

// ─────────────────────────────────────────────────────────────────────────────

void WebServer::handleLogsClear(AsyncWebServerRequest *request)
{
    if (!HistoryQuery::requestArchiveClear()) {
        request->send(409, "application/json", "{\"status\":\"busy\"}");
        return;
    }

    request->send(202, "application/json", "{\"status\":\"pending\"}");
    Console::info(TAG, "Suppression des archives demandée par l'utilisateur");
}

void WebServer::handleLogsClearStatus(AsyncWebServerRequest *request)
{
    const char* status = "idle";

    switch (HistoryQuery::getArchiveClearStatus()) {
        case HistoryQuery::ArchiveClearStatus::Idle:    status = "idle";    break;
        case HistoryQuery::ArchiveClearStatus::Pending: status = "pending"; break;
        case HistoryQuery::ArchiveClearStatus::Running: status = "running"; break;
        case HistoryQuery::ArchiveClearStatus::Success: status = "success"; break;
        case HistoryQuery::ArchiveClearStatus::Failed:  status = "failed";  break;
    }

    request->send(200, "application/json",
                  String("{\"status\":\"") + status + "\"}");
}

// ═════════════════════════════════════════════════════════════════════════════
// Utilitaires Modbus RTU — configuration de capteurs via RS485
//
// Briques élémentaires partagées par les handlers de configuration de capteurs
// RS485 : CRC, purge du buffer, transaction, lecture et écriture d'un registre.
//
// Protocole : Modbus RTU standard, fonctions 0x03 (Read Holding Register) et
// 0x06 (Write Single Register). Les registres et les particularités de chaque
// capteur sont documentés dans la section qui l'utilise.
// ═════════════════════════════════════════════════════════════════════════════

// ── CRC16 Modbus RTU (copie autonome, n'utilise pas SoilSensorRS485) ────────

static uint16_t rs485ConfigCrc16(const uint8_t* data, size_t len)
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

static void rs485ConfigDrainRx()
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

static size_t rs485ConfigTransaction(const uint8_t* request, size_t requestLen,
                                     uint8_t* response, size_t expectedLen,
                                     unsigned long timeoutMs = 200)
{
    rs485ConfigDrainRx();

    Serial1.write(request, requestLen);
    Serial1.flush();

    size_t idx = 0;
    unsigned long startMs = millis();
    while (idx < expectedLen && (millis() - startMs) < timeoutMs) {
        if (Serial1.available()) {
            response[idx++] = Serial1.read();
        } else {
            yield();
        }
    }
    return idx;
}

// ── Envoi d'une requête Modbus 0x03 (Read Holding Register) ─────────────────
//
// Lit 1 registre à regAddr sur le device à deviceAddr.
// Retourne true si la réponse est valide, et place la valeur 16 bits
// dans outValue.

static bool rs485ConfigReadRegister(uint8_t deviceAddr, uint16_t regAddr,
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

    uint16_t crc = rs485ConfigCrc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    uint8_t response[16];
    size_t rxLen = rs485ConfigTransaction(request, 8, response, 7, timeoutMs);

    if (rxLen < 7) return false;

    uint16_t rxCrc  = response[5] | ((uint16_t)response[6] << 8);
    uint16_t chkCrc = rs485ConfigCrc16(response, 5);
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

static bool rs485ConfigWriteRegister(uint8_t deviceAddr, uint16_t regAddr,
                                     uint16_t value)
{
    uint8_t request[8];
    request[0] = deviceAddr;
    request[1] = 0x06;
    request[2] = (regAddr >> 8) & 0xFF;
    request[3] = regAddr & 0xFF;
    request[4] = (value >> 8) & 0xFF;
    request[5] = value & 0xFF;

    uint16_t crc = rs485ConfigCrc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = (crc >> 8) & 0xFF;

    uint8_t response[16];
    size_t rxLen = rs485ConfigTransaction(request, 8, response, 8);

    if (rxLen < 8) return false;

    return (memcmp(request, response, 8) == 0);
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
// Réutilise rs485ConfigCrc16 / rs485ConfigTransaction / rs485ConfigReadRegister.
// ═════════════════════════════════════════════════════════════════════════════

static const char* TAG_EB = "Ebyte";

// Timeout de lecture : 15 octets (req 8 + resp 7) × 11 bits + marge isolateur.
static unsigned long ebyteReadTimeoutMs(uint32_t baudRate)
{
    unsigned long ms = (15UL * 11UL * 1000UL) / baudRate + 40UL;
    return (ms < 50UL) ? 50UL : ms;
}

static const uint32_t EB_SCAN_BAUDS[] = { 9600, 4800, 19200, 2400, 1200 };
static const size_t   EB_SCAN_BAUD_COUNT = sizeof(EB_SCAN_BAUDS) / sizeof(EB_SCAN_BAUDS[0]);

struct EbyteParityOpt {
    uint32_t config;
    uint8_t  code;
};

static const EbyteParityOpt EB_SCAN_PARITIES[] = {
    { SERIAL_8N1, 0 },
    { SERIAL_8O1, 1 },
    { SERIAL_8E1, 2 },
};

static const char* ebyteParityName(uint8_t code)
{
    switch (code) {
        case 1:  return "impaire";
        case 2:  return "paire";
        default: return "aucune";
    }
}

static void ebyteRestoreBus()
{
    Serial1.end();
    Serial1.begin(4800, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
}

// Scan d'une plage d'adresses à un baud et une config série donnés.
// Pas de broadcast en lecture (standard Modbus : broadcast = écriture seule).
// La boucle utilise uint16_t : addrMax peut valoir 254, un uint8_t bouclerait.
static uint8_t ebyteScan(uint32_t baudRate, uint32_t serialConfig,
                         uint8_t addrMin, uint8_t addrMax)
{
    Serial1.end();
    Serial1.begin(baudRate, serialConfig, RS485_RX_PIN, RS485_TX_PIN);
    delay(20);

    const unsigned long timeoutMs = ebyteReadTimeoutMs(baudRate);
    uint16_t readAddr = 0;

    for (uint16_t addr = addrMin; addr <= addrMax; addr++) {
        yield();
        if (rs485ConfigReadRegister((uint8_t)addr, 0x000C, readAddr, timeoutMs)) {
            Console::info(TAG_EB, "Capteur trouvé à l'adresse " + String(addr)
                                  + " (" + String(baudRate) + " bauds)");
            return (uint8_t)addr;
        }
    }

    return 0;
}

// Chemin historique : 8N1, adresses 1–16. Utilisé par la programmation.
static uint8_t ebyteScan(uint32_t baudRate)
{
    return ebyteScan(baudRate, SERIAL_8N1, 1, 16);
}

static bool ebyteBaudAllowed(uint32_t baud)
{
    for (size_t i = 0; i < EB_SCAN_BAUD_COUNT; i++) {
        if (baud == EB_SCAN_BAUDS[i]) return true;
    }
    return false;
}

static uint32_t ebyteSerialConfig(uint8_t parityCode)
{
    for (size_t p = 0; p < 3; p++) {
        if (EB_SCAN_PARITIES[p].code == parityCode) {
            return EB_SCAN_PARITIES[p].config;
        }
    }
    return SERIAL_8N1;
}

// ── Correspondance code baud rate Ebyte ↔ valeur numérique ──────────────────

static const uint32_t EB_BAUD_TABLE[] = { 1200, 2400, 4800, 9600, 19200 };
static const size_t EB_BAUD_TABLE_SIZE = sizeof(EB_BAUD_TABLE) / sizeof(EB_BAUD_TABLE[0]);

static uint32_t ebyteBaudFromCode(uint8_t code)
{
    if (code < EB_BAUD_TABLE_SIZE) return EB_BAUD_TABLE[code];
    return 0;
}

static int ebyteCodeFromBaud(uint32_t baud)
{
    for (size_t i = 0; i < EB_BAUD_TABLE_SIZE; i++) {
        if (EB_BAUD_TABLE[i] == baud) return (int)i;
    }
    return -1;
}

static void ebyteOpen(uint32_t baud, uint8_t parityCode)
{
    Serial1.end();
    Serial1.begin(baud, ebyteSerialConfig(parityCode), RS485_RX_PIN, RS485_TX_PIN);
    delay(20);
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

    uint16_t crc = rs485ConfigCrc16(request, 9);
    request[9]  = crc & 0xFF;
    request[10] = (crc >> 8) & 0xFF;

    uint8_t response[16];
    size_t rxLen = rs485ConfigTransaction(request, 11, response, 8);

    if (rxLen < 8) return false;

    uint16_t rxCrc  = response[6] | ((uint16_t)response[7] << 8);
    uint16_t chkCrc = rs485ConfigCrc16(response, 6);
    if (rxCrc != chkCrc) return false;

    // Changement d'adresse : l'écho peut déjà porter la nouvelle adresse.
    if (regAddr == 0x000C) {
        if (response[0] != deviceAddr && response[0] != (uint8_t)value) return false;
    } else if (response[0] != deviceAddr) {
        return false;
    }
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
    uint32_t baud = 9600;
    uint8_t  parityCode = 0;
    uint16_t addrFrom = 1;
    uint16_t addrTo = 16;

    if (request->hasParam("baud")) {
        baud = (uint32_t)request->getParam("baud")->value().toInt();
    }
    if (request->hasParam("parity")) {
        parityCode = (uint8_t)request->getParam("parity")->value().toInt();
    }
    if (request->hasParam("from")) {
        addrFrom = (uint16_t)request->getParam("from")->value().toInt();
    }
    if (request->hasParam("to")) {
        addrTo = (uint16_t)request->getParam("to")->value().toInt();
    }

    if (!ebyteBaudAllowed(baud) || parityCode > 2
            || addrFrom < 1 || addrTo < addrFrom || addrTo > 254
            || (addrTo - addrFrom + 1) > 16) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres de scan invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    uint8_t foundAddr = ebyteScan(baud, ebyteSerialConfig(parityCode),
                                  (uint8_t)addrFrom, (uint8_t)addrTo);

    if (foundAddr == 0) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json", "{\"ok\":false}");
        return;
    }

    uint16_t baudCode = 0;
    rs485ConfigReadRegister(foundAddr, 0x000D, baudCode,
                            ebyteReadTimeoutMs(baud));
    uint32_t reportedBaud = ebyteBaudFromCode((uint8_t)baudCode);
    if (reportedBaud == 0) reportedBaud = baud;

    uint16_t parityReg = 0;
    uint8_t reportedParity = parityCode;
    if (rs485ConfigReadRegister(foundAddr, 0x000E, parityReg,
                                ebyteReadTimeoutMs(baud))) {
        if (parityReg <= 2) {
            reportedParity = (uint8_t)parityReg;
        }
    }

    ebyteRestoreBus();
    SoilSensorRS485::setMaintenanceMode(false);

    Console::info(TAG_EB, "Capteur détecté — adresse=" + String(foundAddr)
                          + " baud=" + String(reportedBaud)
                          + " parité=" + ebyteParityName(reportedParity));

    String json = "{\"ok\":true,\"address\":" + String(foundAddr)
                + ",\"baudrate\":" + String(reportedBaud)
                + ",\"parity\":" + String(reportedParity)
                + ",\"parityName\":\"" + String(ebyteParityName(reportedParity)) + "\"}";
    request->send(200, "application/json", json);
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/program-ebyte — programmation baud rate + adresse
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ProgramEbyte(AsyncWebServerRequest *request)
{
    const bool hasAll =
        request->hasParam("from") && request->hasParam("fromBaud")
        && request->hasParam("fromParity") && request->hasParam("to")
        && request->hasParam("baud") && request->hasParam("parity");

    if (!hasAll) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres from/fromBaud/fromParity/to/baud/parity requis\"}");
        return;
    }

    uint8_t  fromAddr   = (uint8_t)request->getParam("from")->value().toInt();
    uint32_t fromBaud   = (uint32_t)request->getParam("fromBaud")->value().toInt();
    uint8_t  fromParity = (uint8_t)request->getParam("fromParity")->value().toInt();
    uint8_t  toAddr     = (uint8_t)request->getParam("to")->value().toInt();
    uint32_t toBaud     = (uint32_t)request->getParam("baud")->value().toInt();
    uint8_t  toParity   = (uint8_t)request->getParam("parity")->value().toInt();

    int toBaudCode = ebyteCodeFromBaud(toBaud);

    if (fromAddr < 1 || fromAddr > 16 || toAddr < 1 || toAddr > 16
            || !ebyteBaudAllowed(fromBaud) || !ebyteBaudAllowed(toBaud)
            || fromParity > 2 || toParity > 2 || toBaudCode < 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres de programmation invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    // ── Étape 1 : parler au capteur avec la config lue ───────────────
    ebyteOpen(fromBaud, fromParity);

    uint16_t seenAddr = 0;
    if (!rs485ConfigReadRegister(fromAddr, 0x000C, seenAddr, 200)) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Capteur non détecté à l'adresse "
                      + String(fromAddr) + " — relancez une lecture\"}");
        return;
    }

    delay(80);

    uint8_t  workingAddr   = fromAddr;
    uint32_t workingBaud   = fromBaud;
    uint8_t  workingParity = fromParity;

    bool needAddrChange   = (fromAddr != toAddr);
    bool needParityChange = (fromParity != toParity);
    bool needBaudChange   = (fromBaud != toBaud);

    // ── Étape 2 : adresse, puis pause EEPROM avant toute autre trame ─
    // Une lecture trop tôt sur le bus peut empêcher l'écriture de tenir.
    if (needAddrChange) {
        bool addrOk = false;
        for (int attempt = 0; attempt < 3 && !addrOk; attempt++) {
            ebyteWriteRegister(fromAddr, 0x000C, (uint16_t)toAddr);
            delay(200);
            uint16_t v = 0;
            if (rs485ConfigReadRegister(toAddr, 0x000C, v, 200) && v == toAddr) {
                addrOk = true;
            }
        }
        if (!addrOk) {
            uint16_t still = 0;
            bool stillOld = rs485ConfigReadRegister(fromAddr, 0x000C, still, 200);
            ebyteRestoreBus();
            SoilSensorRS485::setMaintenanceMode(false);
            if (stillOld) {
                Console::warn(TAG_EB, "Adresse inchangée (" + String(fromAddr) + ")");
                request->send(200, "application/json",
                              "{\"ok\":false,\"error\":\"L'adresse n'a pas changé (toujours "
                              + String(fromAddr) + ")\"}");
            } else {
                Console::warn(TAG_EB, "Vérification échouée après écriture d'adresse");
                request->send(200, "application/json",
                              "{\"ok\":false,\"error\":\"Commandes envoyées mais vérification échouée — relancez une lecture pour vérifier\"}");
            }
            return;
        }
        Console::info(TAG_EB, "Adresse changée de " + String(fromAddr)
                              + " vers " + String(toAddr));
        workingAddr = toAddr;
    }

    // ── Étape 3 : parité, puis bascule Serial1 si elle a changé ──────
    if (needParityChange) {
        ebyteWriteRegister(workingAddr, 0x000E, (uint16_t)toParity);
        delay(200);
        workingParity = toParity;
        ebyteOpen(workingBaud, workingParity);
    }

    // ── Étape 4 : baud en dernier (l'écho 0x10 arrive encore à l'ancienne vitesse)
    if (needBaudChange) {
        ebyteWriteRegister(workingAddr, 0x000D, (uint16_t)toBaudCode);
        delay(200);
        workingBaud = toBaud;
        ebyteOpen(workingBaud, workingParity);
    }

    // ── Étape 5 : vérification à la config cible ─────────────────────
    delay(50);
    uint16_t verifyAddr = 0;
    bool verified = rs485ConfigReadRegister(toAddr, 0x000C, verifyAddr, 200);

    ebyteRestoreBus();
    SoilSensorRS485::setMaintenanceMode(false);

    if (verified && verifyAddr == toAddr) {
        String msg = "Capteur configuré — adresse " + String(toAddr)
                   + ", " + String(toBaud) + " bauds, parité "
                   + ebyteParityName(toParity);
        if (!needAddrChange && !needBaudChange && !needParityChange) {
            msg = "Capteur déjà configuré à l'adresse " + String(toAddr)
                + ", " + String(toBaud) + " bauds, parité "
                + ebyteParityName(toParity);
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
// Capteurs sol ZTS-3000 — lecture / programmation adresse + baud
//
// Registres : 0x07D0 adresse (1–254), 0x07D1 baud (0=2400, 1=4800, 2=9600).
// Écriture 0x06. Pas de registre de parité documenté (8N1 usine).
// ═════════════════════════════════════════════════════════════════════════════

static const uint32_t SOIL_BAUD_TABLE[] = { 2400, 4800, 9600 };
static const size_t   SOIL_BAUD_TABLE_SIZE = sizeof(SOIL_BAUD_TABLE) / sizeof(SOIL_BAUD_TABLE[0]);

static int soilCodeFromBaud(uint32_t baud)
{
    for (size_t i = 0; i < SOIL_BAUD_TABLE_SIZE; i++) {
        if (SOIL_BAUD_TABLE[i] == baud) return (int)i;
    }
    return -1;
}

static uint32_t soilBaudFromCode(uint16_t code)
{
    if (code < SOIL_BAUD_TABLE_SIZE) return SOIL_BAUD_TABLE[code];
    return 0;
}

static bool soilBaudAllowed(uint32_t baud)
{
    return soilCodeFromBaud(baud) >= 0;
}

static uint8_t soilScan(uint32_t baudRate, uint32_t serialConfig,
                        uint8_t addrMin, uint8_t addrMax)
{
    Serial1.end();
    Serial1.begin(baudRate, serialConfig, RS485_RX_PIN, RS485_TX_PIN);
    delay(20);

    const unsigned long timeoutMs = ebyteReadTimeoutMs(baudRate);
    uint16_t value = 0;

    for (uint16_t addr = addrMin; addr <= addrMax; addr++) {
        yield();
        if (!rs485ConfigReadRegister((uint8_t)addr, 0x07D0, value, timeoutMs)) {
            continue;
        }
        if (value < 1 || value > 254) {
            continue;
        }

        uint16_t ebyteAddr = 0;
        if (rs485ConfigReadRegister((uint8_t)addr, 0x000C, ebyteAddr, timeoutMs)
                && ebyteAddr == addr) {
            continue;
        }

        uint16_t analogAddr = 0;
        if (rs485ConfigReadRegister((uint8_t)addr, 0x4000, analogAddr, timeoutMs)
                && analogAddr == addr) {
            continue;
        }

        return (uint8_t)addr;
    }
    return 0;
}

void WebServer::handleRS485ReadSoil(AsyncWebServerRequest *request)
{
    uint32_t baud = 4800;
    uint8_t  parityCode = 0;
    uint16_t addrFrom = 1;
    uint16_t addrTo = 15;

    if (request->hasParam("baud")) {
        baud = (uint32_t)request->getParam("baud")->value().toInt();
    }
    if (request->hasParam("parity")) {
        parityCode = (uint8_t)request->getParam("parity")->value().toInt();
    }
    if (request->hasParam("from")) {
        addrFrom = (uint16_t)request->getParam("from")->value().toInt();
    }
    if (request->hasParam("to")) {
        addrTo = (uint16_t)request->getParam("to")->value().toInt();
    }

    if (!soilBaudAllowed(baud) || parityCode > 2
            || addrFrom < 1 || addrTo < addrFrom || addrTo > 254
            || (addrTo - addrFrom + 1) > 16) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres de scan invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    uint8_t foundAddr = soilScan(baud, ebyteSerialConfig(parityCode),
                                 (uint8_t)addrFrom, (uint8_t)addrTo);

    if (foundAddr == 0) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json", "{\"ok\":false}");
        return;
    }

    uint16_t baudReg = 0;
    uint32_t reportedBaud = baud;
    if (rs485ConfigReadRegister(foundAddr, 0x07D1, baudReg,
                                ebyteReadTimeoutMs(baud))) {
        uint32_t decoded = soilBaudFromCode(baudReg);
        if (decoded != 0) reportedBaud = decoded;
    }

    ebyteRestoreBus();
    SoilSensorRS485::setMaintenanceMode(false);

    Console::info(TAG, "Sol détecté — adresse=" + String(foundAddr)
                       + " baud=" + String(reportedBaud));

    String json = "{\"ok\":true,\"address\":" + String(foundAddr)
                + ",\"baudrate\":" + String(reportedBaud)
                + ",\"parity\":" + String(parityCode)
                + ",\"parityName\":\"" + String(ebyteParityName(parityCode)) + "\"}";
    request->send(200, "application/json", json);
}

void WebServer::handleRS485ProgramSoil(AsyncWebServerRequest *request)
{
    const bool hasAll =
        request->hasParam("from") && request->hasParam("fromBaud")
        && request->hasParam("fromParity") && request->hasParam("to")
        && request->hasParam("baud") && request->hasParam("parity");

    if (!hasAll) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres from/fromBaud/fromParity/to/baud/parity requis\"}");
        return;
    }

    uint8_t  fromAddr   = (uint8_t)request->getParam("from")->value().toInt();
    uint32_t fromBaud   = (uint32_t)request->getParam("fromBaud")->value().toInt();
    uint8_t  fromParity = (uint8_t)request->getParam("fromParity")->value().toInt();
    uint8_t  toAddr     = (uint8_t)request->getParam("to")->value().toInt();
    uint32_t toBaud     = (uint32_t)request->getParam("baud")->value().toInt();
    uint8_t  toParity   = (uint8_t)request->getParam("parity")->value().toInt();
    int      toBaudCode = soilCodeFromBaud(toBaud);

    if (fromAddr < 1 || fromAddr > 15 || toAddr < 1 || toAddr > 15
            || !soilBaudAllowed(fromBaud) || toBaudCode < 0
            || fromParity > 2 || toParity > 2) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres de programmation invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);
    ebyteOpen(fromBaud, fromParity);

    uint16_t seen = 0;
    if (!rs485ConfigReadRegister(fromAddr, 0x07D0, seen, 200)
            && !rs485ConfigReadRegister(fromAddr, 0x0000, seen, 200)) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Capteur non détecté à l'adresse "
                      + String(fromAddr) + " — relancez une lecture\"}");
        return;
    }

    delay(80);

    uint8_t  workingAddr   = fromAddr;
    uint32_t workingBaud   = fromBaud;
    uint8_t  workingParity = fromParity;
    bool needAddrChange = (fromAddr != toAddr);
    bool needBaudChange = (fromBaud != toBaud);

    if (needAddrChange) {
        bool addrOk = false;
        for (int attempt = 0; attempt < 3 && !addrOk; attempt++) {
            rs485ConfigWriteRegister(fromAddr, 0x07D0, (uint16_t)toAddr);
            delay(200);
            uint16_t v = 0;
            if (rs485ConfigReadRegister(toAddr, 0x07D0, v, 200) && v == toAddr) {
                addrOk = true;
            }
        }
        if (!addrOk) {
            ebyteRestoreBus();
            SoilSensorRS485::setMaintenanceMode(false);
            request->send(200, "application/json",
                          "{\"ok\":false,\"error\":\"L'adresse n'a pas changé (toujours "
                          + String(fromAddr) + ")\"}");
            return;
        }
        workingAddr = toAddr;
    }

    if (needBaudChange) {
        rs485ConfigWriteRegister(workingAddr, 0x07D1, (uint16_t)toBaudCode);
        delay(200);
        workingBaud = toBaud;
        ebyteOpen(workingBaud, workingParity);
    }

    delay(50);
    uint16_t verifyAddr = 0;
    bool verified = rs485ConfigReadRegister(toAddr, 0x07D0, verifyAddr, 200);

    ebyteRestoreBus();
    SoilSensorRS485::setMaintenanceMode(false);

    if (verified && verifyAddr == toAddr) {
        String msg = "Capteur configuré — adresse " + String(toAddr)
                   + ", " + String(toBaud) + " bauds";
        Console::info(TAG, msg);
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"" + msg + "\"}");
    } else {
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Commandes envoyées mais vérification échouée — relancez une lecture pour vérifier\"}");
    }
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
//   - Lecture/écriture du mode d'un canal : registre 0x1000 + (canal - 1)
//   - Écriture baud rate: fonction 0x06, registre 0x2000
//   - Écriture adresse  : fonction 0x06, registre 0x4000
//   - Adresse broadcast : 0x00
//
// Réutilise rs485ConfigCrc16 / rs485ConfigDrainRx / rs485ConfigTransaction /
// rs485ConfigReadRegister / rs485ConfigWriteRegister.
//
// Ref : https://www.waveshare.com/wiki/Modbus_RTU_Analog_Input_8CH_(B)
// ═════════════════════════════════════════════════════════════════════════════

static const char* TAG_AI = "AnalogInput";

static const uint32_t AI_BAUD_TABLE[] = {
    4800, 9600, 19200, 38400, 57600, 115200, 128000, 256000
};
static const size_t AI_BAUD_TABLE_SIZE = sizeof(AI_BAUD_TABLE) / sizeof(AI_BAUD_TABLE[0]);

static int analogCodeFromBaud(uint32_t baud)
{
    for (size_t i = 0; i < AI_BAUD_TABLE_SIZE; i++) {
        if (AI_BAUD_TABLE[i] == baud) return (int)i;
    }
    return -1;
}

static uint32_t analogBaudFromCode(uint8_t code)
{
    if (code < AI_BAUD_TABLE_SIZE) return AI_BAUD_TABLE[code];
    return 0;
}

static bool analogBaudAllowed(uint32_t baud)
{
    return analogCodeFromBaud(baud) >= 0;
}

// Wiki 0x2000 : poids fort = parité (0=aucune, 1=paire, 2=impaire),
// poids faible = code baud. L'UI utilise 0=aucune, 1=impaire, 2=paire.
static uint8_t analogParityRegFromUi(uint8_t ui)
{
    if (ui == 1) return 2;
    if (ui == 2) return 1;
    return 0;
}

static uint8_t analogParityUiFromReg(uint8_t reg)
{
    if (reg == 1) return 2;
    if (reg == 2) return 1;
    return 0;
}

static bool analogIsOtherFamily(uint8_t addr, unsigned long timeoutMs)
{
    uint16_t v = 0;
    if (rs485ConfigReadRegister(addr, 0x000C, v, timeoutMs) && v == addr) {
        return true;
    }
    if (rs485ConfigReadRegister(addr, 0x07D0, v, timeoutMs) && v == addr) {
        return true;
    }
    return false;
}

// Broadcast d'abord (renvoie l'adresse réelle), puis plage demandée.
static uint8_t analogInputScan(uint32_t baudRate, uint32_t serialConfig,
                               uint8_t addrMin, uint8_t addrMax, bool tryBroadcast)
{
    Serial1.end();
    Serial1.begin(baudRate, serialConfig, RS485_RX_PIN, RS485_TX_PIN);
    delay(20);

    uint16_t readAddr = 0;

    if (tryBroadcast && rs485ConfigReadRegister(0x00, 0x4000, readAddr, 50)
            && readAddr >= 1 && readAddr <= 254
            && !analogIsOtherFamily((uint8_t)readAddr, 50)) {
        Console::info(TAG_AI, "Module trouvé via broadcast à " + String(baudRate)
                              + " bauds — adresse " + String(readAddr));
        return (uint8_t)readAddr;
    }

    for (uint16_t addr = addrMin; addr <= addrMax; addr++) {
        yield();
        if (!rs485ConfigReadRegister((uint8_t)addr, 0x4000, readAddr, 50)) {
            continue;
        }
        if (readAddr != addr) {
            continue;
        }
        if (analogIsOtherFamily((uint8_t)addr, 50)) {
            continue;
        }
        Console::info(TAG_AI, "Module trouvé à l'adresse " + String(addr)
                              + " (" + String(baudRate) + " bauds)");
        return (uint8_t)addr;
    }

    return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/read-analog — lecture de la configuration actuelle
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ReadAnalog(AsyncWebServerRequest *request)
{
    uint32_t baud = 9600;
    uint8_t  parityCode = 0;
    uint16_t addrFrom = 1;
    uint16_t addrTo = 30;

    if (request->hasParam("baud")) {
        baud = (uint32_t)request->getParam("baud")->value().toInt();
    }
    if (request->hasParam("parity")) {
        parityCode = (uint8_t)request->getParam("parity")->value().toInt();
    }
    if (request->hasParam("from")) {
        addrFrom = (uint16_t)request->getParam("from")->value().toInt();
    }
    if (request->hasParam("to")) {
        addrTo = (uint16_t)request->getParam("to")->value().toInt();
    }

    if (!analogBaudAllowed(baud) || parityCode > 2
            || addrFrom < 1 || addrTo < addrFrom || addrTo > 254
            || (addrTo - addrFrom + 1) > 30) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres de scan invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);

    const bool tryBroadcast = (addrFrom == 1);
    uint8_t foundAddr = analogInputScan(baud, ebyteSerialConfig(parityCode),
                                        (uint8_t)addrFrom, (uint8_t)addrTo,
                                        tryBroadcast);

    if (foundAddr == 0) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json", "{\"ok\":false}");
        return;
    }

    uint16_t rawVersion = 0;
    rs485ConfigReadRegister(foundAddr, 0x8000, rawVersion, 200);
    String versionStr = "V" + String(rawVersion / 100) + "."
                        + String((rawVersion % 100) / 10)
                        + String(rawVersion % 10);

    uint16_t uart = 0;
    uint8_t reportedParity = parityCode;
    uint32_t reportedBaud = baud;
    if (rs485ConfigReadRegister(foundAddr, 0x2000, uart, 200)) {
        uint32_t decoded = analogBaudFromCode((uint8_t)(uart & 0xFF));
        if (decoded != 0) reportedBaud = decoded;
        reportedParity = analogParityUiFromReg((uint8_t)(uart >> 8));
    }

    ebyteRestoreBus();
    SoilSensorRS485::setMaintenanceMode(false);

    Console::info(TAG_AI, "Module détecté — adresse=" + String(foundAddr)
                          + " baud=" + String(reportedBaud)
                          + " version=" + versionStr);

    String json = "{\"ok\":true,\"address\":" + String(foundAddr)
                + ",\"baudrate\":" + String(reportedBaud)
                + ",\"parity\":" + String(reportedParity)
                + ",\"parityName\":\"" + String(ebyteParityName(reportedParity)) + "\""
                + ",\"version\":\"" + versionStr + "\"}";
    request->send(200, "application/json", json);
}

// ═════════════════════════════════════════════════════════════════════════════
// Handler POST /rs485/program-analog — programmation baud rate + adresse
// ═════════════════════════════════════════════════════════════════════════════

void WebServer::handleRS485ProgramAnalog(AsyncWebServerRequest *request)
{
    const bool hasAll =
        request->hasParam("from") && request->hasParam("fromBaud")
        && request->hasParam("fromParity") && request->hasParam("to")
        && request->hasParam("baud") && request->hasParam("parity");

    if (!hasAll) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres from/fromBaud/fromParity/to/baud/parity requis\"}");
        return;
    }

    uint8_t  fromAddr   = (uint8_t)request->getParam("from")->value().toInt();
    uint32_t fromBaud   = (uint32_t)request->getParam("fromBaud")->value().toInt();
    uint8_t  fromParity = (uint8_t)request->getParam("fromParity")->value().toInt();
    uint8_t  toAddr     = (uint8_t)request->getParam("to")->value().toInt();
    uint32_t toBaud     = (uint32_t)request->getParam("baud")->value().toInt();
    uint8_t  toParity   = (uint8_t)request->getParam("parity")->value().toInt();
    int      toBaudCode = analogCodeFromBaud(toBaud);

    if (fromAddr < 1 || toAddr < 1 || toAddr > 30
            || !analogBaudAllowed(fromBaud) || toBaudCode < 0
            || fromParity > 2 || toParity > 2) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Paramètres de programmation invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);
    ebyteOpen(fromBaud, fromParity);

    uint16_t seen = 0;
    if (!rs485ConfigReadRegister(fromAddr, 0x4000, seen, 200)) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Module non détecté à l'adresse "
                      + String(fromAddr) + " — relancez une lecture\"}");
        return;
    }

    delay(80);

    uint8_t  workingAddr   = fromAddr;
    uint32_t workingBaud   = fromBaud;
    uint8_t  workingParity = fromParity;
    bool needAddrChange   = (fromAddr != toAddr);
    bool needUartChange   = (fromBaud != toBaud) || (fromParity != toParity);

    if (needAddrChange) {
        bool ok = rs485ConfigWriteRegister(fromAddr, 0x4000, (uint16_t)toAddr);
        if (!ok) {
            ok = rs485ConfigWriteRegister(0x00, 0x4000, (uint16_t)toAddr);
        }
        delay(200);
        uint16_t v = 0;
        if (!ok && !(rs485ConfigReadRegister(toAddr, 0x4000, v, 200) && v == toAddr)) {
            ebyteRestoreBus();
            SoilSensorRS485::setMaintenanceMode(false);
            request->send(200, "application/json",
                          "{\"ok\":false,\"error\":\"L'adresse n'a pas changé (toujours "
                          + String(fromAddr) + ")\"}");
            return;
        }
        workingAddr = toAddr;
    }

    // UART 0x2000 : l'écho arrive à la NOUVELLE vitesse — on n'attend pas l'écho.
    if (needUartChange) {
        uint8_t cmd[8];
        cmd[0] = workingAddr;
        cmd[1] = 0x06;
        cmd[2] = 0x20;
        cmd[3] = 0x00;
        cmd[4] = analogParityRegFromUi(toParity);
        cmd[5] = (uint8_t)toBaudCode;
        uint16_t crc = rs485ConfigCrc16(cmd, 6);
        cmd[6] = crc & 0xFF;
        cmd[7] = (crc >> 8) & 0xFF;
        rs485ConfigDrainRx();
        Serial1.write(cmd, 8);
        Serial1.flush();
        delay(50);
        workingBaud   = toBaud;
        workingParity = toParity;
        ebyteOpen(workingBaud, workingParity);
        delay(50);
    }

    uint16_t verifyAddr = 0;
    bool verified = rs485ConfigReadRegister(toAddr, 0x4000, verifyAddr, 200);

    ebyteRestoreBus();
    SoilSensorRS485::setMaintenanceMode(false);

    if (verified && verifyAddr == toAddr) {
        String msg = "Module configuré — adresse " + String(toAddr)
                   + ", " + String(toBaud) + " bauds, parité "
                   + ebyteParityName(toParity);
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

    uint32_t baud = 4800;
    uint8_t  parityCode = 0;
    if (request->hasParam("baud")) {
        baud = (uint32_t)request->getParam("baud")->value().toInt();
    }
    if (request->hasParam("parity")) {
        parityCode = (uint8_t)request->getParam("parity")->value().toInt();
    }
    if (!analogBaudAllowed(baud) || parityCode > 2) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Baud ou parité invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);
    ebyteOpen(baud, parityCode);

    uint16_t regAddr = 0x1000 + (channel - 1);
    uint16_t modeValue = 0;
    bool ok = rs485ConfigReadRegister(addr, regAddr, modeValue, 200);

    ebyteRestoreBus();
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

    uint32_t baud = 4800;
    uint8_t  parityCode = 0;
    if (request->hasParam("baud")) {
        baud = (uint32_t)request->getParam("baud")->value().toInt();
    }
    if (request->hasParam("parity")) {
        parityCode = (uint8_t)request->getParam("parity")->value().toInt();
    }
    if (!analogBaudAllowed(baud) || parityCode > 2) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"Baud ou parité invalides\"}");
        return;
    }

    SoilSensorRS485::setMaintenanceMode(true);
    ebyteOpen(baud, parityCode);

    uint16_t regAddr = 0x1000 + (channel - 1);

    bool writeOk = rs485ConfigWriteRegister(addr, regAddr, newMode);

    if (!writeOk) {
        ebyteRestoreBus();
        SoilSensorRS485::setMaintenanceMode(false);

        Console::warn(TAG_AI, "Échec écriture mode canal " + String(channel));
        request->send(200, "application/json",
                      "{\"ok\":false,\"error\":\"Échec de l'écriture — le module n'a pas confirmé\"}");
        return;
    }

    delay(50);

    uint16_t verifyMode = 0xFFFF;
    bool readOk = rs485ConfigReadRegister(addr, regAddr, verifyMode, 200);

    ebyteRestoreBus();
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
