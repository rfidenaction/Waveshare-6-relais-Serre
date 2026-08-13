// Utils/StatusReport.cpp
#include "Utils/StatusReport.h"

#include "Config/TimingConfig.h"
#include "Core/DataBus.h"
#include "Connectivity/NTPManager.h"
#include "Actuators/ValveManager.h"

char StatusReport::_message[StatusReport::MESSAGE_SIZE] = { '\0' };
bool StatusReport::_latched   = false;
bool StatusReport::_published = false;

uint16_t StatusReport::_errorCount = 0;
uint16_t StatusReport::_warnCount  = 0;
char     StatusReport::_firstError[StatusReport::FIRST_MSG_SIZE] = { '\0' };
char     StatusReport::_firstWarn [StatusReport::FIRST_MSG_SIZE] = { '\0' };

uint32_t StatusReport::_nextReportMs = 0;

// Protège les compteurs et les buffers : les crochets sont appelés depuis la
// boucle principale comme depuis la tâche esp-mqtt, sur l'autre cœur.
static portMUX_TYPE statusReportMux = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// Composition "[tag] message" dans un buffer de taille fixe.
// Aucune allocation. Virgules, guillemets et sauts de ligne remplacés par une
// espace : le texte traverse ensuite le CSV sans dépendre d'un échappement.
// -----------------------------------------------------------------------------
static void composeMessage(char* dst, size_t dstSize,
                           const String& tag, const String& message)
{
    size_t w = 0;

    auto put = [&](char c) {
        if (w + 1 >= dstSize) return;
        if (c == ',' || c == '"' || c == '\n' || c == '\r') c = ' ';
        dst[w++] = c;
    };

    if (!tag.isEmpty()) {
        put('[');
        for (size_t i = 0; i < tag.length(); i++) put(tag.charAt(i));
        put(']');
        put(' ');
    }

    for (size_t i = 0; i < message.length(); i++) put(message.charAt(i));

    dst[w] = '\0';
}

// -----------------------------------------------------------------------------
// Publication d'un texte sur un DataId, via le chemin normal du système.
// -----------------------------------------------------------------------------
static void publishText(DataId id, const char* text)
{
    BusItem item = {};
    item.type       = getMeta(id).type;
    item.id         = id;
    item.valueKind  = 1;
    item.valueFloat = 0.0f;
    strncpy(item.valueText, text, sizeof(item.valueText) - 1);
    item.valueText[sizeof(item.valueText) - 1] = '\0';

    DataBus::publish(item);
}

// -----------------------------------------------------------------------------
// Verdict de démarrage — premier arrivé, seul retenu
// -----------------------------------------------------------------------------
void StatusReport::note(const String& tag, const String& message)
{
    // Sortie anticipée : une fois le verrou posé, plus rien à faire.
    if (_latched) {
        return;
    }

    char buf[MESSAGE_SIZE];
    composeMessage(buf, sizeof(buf), tag, message);

    portENTER_CRITICAL(&statusReportMux);
    if (!_latched) {
        memcpy(_message, buf, sizeof(buf));
        _latched = true;
    }
    portEXIT_CRITICAL(&statusReportMux);
}

// -----------------------------------------------------------------------------
// Crochets de Console::log()
//
// Le test du compteur hors section critique est délibéré : il évite de composer
// un message à chaque WARN alors que seul le premier de la fenêtre est retenu.
// La relecture sous verrou tranche les rares égalités entre threads.
// -----------------------------------------------------------------------------
void StatusReport::onConsoleError(const String& tag, const String& message)
{
    note(tag, message);

    char buf[FIRST_MSG_SIZE];
    bool compose = (_errorCount == 0);
    if (compose) composeMessage(buf, sizeof(buf), tag, message);

    portENTER_CRITICAL(&statusReportMux);
    if (compose && _errorCount == 0) memcpy(_firstError, buf, sizeof(buf));
    if (_errorCount < UINT16_MAX)    _errorCount++;
    portEXIT_CRITICAL(&statusReportMux);
}

void StatusReport::onConsoleWarn(const String& tag, const String& message)
{
    char buf[FIRST_MSG_SIZE];
    bool compose = (_warnCount == 0);
    if (compose) composeMessage(buf, sizeof(buf), tag, message);

    portENTER_CRITICAL(&statusReportMux);
    if (compose && _warnCount == 0) memcpy(_firstWarn, buf, sizeof(buf));
    if (_warnCount < UINT16_MAX)    _warnCount++;
    portEXIT_CRITICAL(&statusReportMux);
}

// -----------------------------------------------------------------------------
// Rapport périodique — compteurs de la fenêtre écoulée, puis remise à zéro
// -----------------------------------------------------------------------------
void StatusReport::publishReport()
{
    uint16_t errors;
    uint16_t warns;
    char     firstError[FIRST_MSG_SIZE];
    char     firstWarn[FIRST_MSG_SIZE];

    portENTER_CRITICAL(&statusReportMux);
    errors = _errorCount;
    warns  = _warnCount;
    memcpy(firstError, _firstError, sizeof(firstError));
    memcpy(firstWarn,  _firstWarn,  sizeof(firstWarn));
    _errorCount    = 0;
    _warnCount     = 0;
    _firstError[0] = '\0';
    _firstWarn[0]  = '\0';
    portEXIT_CRITICAL(&statusReportMux);

    char text[REPORT_SIZE];

    if (errors == 0 && warns == 0) {
        strcpy(text, "OK");
    } else {
        int n = snprintf(text, sizeof(text), "%u ERROR / %u WARN",
                         (unsigned)errors, (unsigned)warns);
        if (n < 0) n = 0;
        if (n > (int)sizeof(text) - 1) n = (int)sizeof(text) - 1;

        if (errors > 0 && firstError[0] != '\0') {
            int m = snprintf(text + n, sizeof(text) - n, " | E:%s", firstError);
            if (m > 0) n += m;
            if (n > (int)sizeof(text) - 1) n = (int)sizeof(text) - 1;
        }

        if (warns > 0 && firstWarn[0] != '\0') {
            snprintf(text + n, sizeof(text) - n, " | W:%s", firstWarn);
        }
    }

    publishText(DataId::Error, text);
}

// -----------------------------------------------------------------------------
// Tâche périodique
// -----------------------------------------------------------------------------
void StatusReport::handle()
{
    const uint32_t now = millis();

    // ─── Verdict de démarrage — publication unique à l'échéance ───────────
    if (!_published) {
        if (now < BOOT_VERDICT_DELAY_MS) {
            return;
        }

        // Critères jugeables seulement maintenant. Évalués uniquement si rien
        // n'a encore été mémorisé : une anomalie plus ancienne reste prioritaire.
        if (!_latched) {
            if (!NTPManager::hasEverSynced()) {
                note("NTP", "Aucune synchro NTP depuis le démarrage");
            } else if (!ValveManager::isReady()) {
                note("ValveManager", "Système vannes non opérationnel");
            }
        }

        char verdict[MESSAGE_SIZE];
        portENTER_CRITICAL(&statusReportMux);
        if (_latched) {
            memcpy(verdict, _message, MESSAGE_SIZE);
        } else {
            strcpy(verdict, "OK");
        }
        portEXIT_CRITICAL(&statusReportMux);

        publishText(DataId::Boot, verdict);

        // Levé quel que soit le résultat de publish() : le verdict est un
        // instantané de fin de démarrage, il ne se rejoue pas.
        _published = true;

        // Le premier rapport part dans le même passage.
        _nextReportMs = now;
    }

    // ─── Rapport périodique ───────────────────────────────────────────────
    // Soustraction signée : insensible au débordement de millis().
    if ((int32_t)(now - _nextReportMs) < 0) {
        return;
    }

    _nextReportMs = now + STATUS_REPORT_PERIOD_MS;
    publishReport();
}
