// Web/Pages/PageActuators.cpp
// Page de pilotage des actionneurs installés (NEO).
//
// Principes :
//  - Liste construite depuis META, filtrée par Neo::find : un actionneur
//    présent dans META mais absent de ce matériel n'est pas affiché.
//  - La nature META de la commande liée distingue vanne (metrique, durée)
//    et lumière (etat, 0=OFF / 1=ON).
//  - État actuel lu via WebServer::hasLastData (vue RAM alimentée
//    par DataBus::publish côté manager propriétaire).
//  - Commande envoyée en POST text/plain vers /command, payload = CSV 7 champs
//    identique au format MQTT serre/cmd (timestamp,VClock_available,
//    VClock_reliable,type,id,valueType,value) — les 3 premiers champs vides,
//    type = 5
//    (CommandManual), id = DataId de la commande récupéré depuis NEO à la
//    génération de la page, valueType = 0, value = durée en secondes pour une
//    vanne, 0 ou 1 pour une lumière.
//  - Style aligné sur PagePrincipale (fond bleu, cartes transparentes).
#include "Web/Pages/PageActuators.h"

#include "Web/WebServer.h"
#include "Config/MetaDataModel.h"
#include "Config/Neo.h"
#include "Utils/Console.h"

#include <time.h>

// Tag pour logs
static const char* TAG = "PageActuators";

// ─────────────────────────────────────────────
// Helpers temps (alignés sur PagePrincipale)
// ─────────────────────────────────────────────

static String formatUtcActuators(time_t t)
{
    struct tm tmLocal;
    localtime_r(&t, &tmLocal);
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%y %H:%M:%S", &tmLocal);
    return String(buf);
}

static String formatSinceActuators(uint32_t ageMs)
{
    uint32_t s = ageMs / 1000;
    uint32_t m = s / 60; s %= 60;
    uint32_t h = m / 60; m %= 60;

    String out = "Depuis ";
    if (h) out += String(h) + "h ";
    if (m) out += String(m) + "m ";
    out += String(s) + "s";
    return out;
}

static String timeHtmlActuators(const LastDataForWeb& d)
{
    if (d.VClock_available && d.VClock_reliable) {
        return formatUtcActuators(d.timestamp);
    }
    if (d.VClock_available && !d.VClock_reliable) {
        return formatUtcActuators(d.timestamp) + " <em>(Imprécis)</em>";
    }
    uint32_t ageMs = millis() - static_cast<uint32_t>(d.timestamp);
    return "<span class=\"age\" data-age-ms=\"" +
           String(ageMs) + "\">" +
           formatSinceActuators(ageMs) +
           "</span>";
}

// ─────────────────────────────────────────────
// Helper : résout le label d'état depuis META
// ─────────────────────────────────────────────
static String stateLabel(const DataMeta& m, int intVal)
{
    if (m.stateLabels != nullptr && intVal >= 0 && intVal < m.stateLabelCount) {
        if (m.stateLabels[intVal] != nullptr) {
            return String(m.stateLabels[intVal]);
        }
    }
    return String(intVal);
}

// ─────────────────────────────────────────────
// Helper : récupère le DataId de la commande associée à une entité actionneur.
// NEO porte la correspondance entité ↔ commande dans son champ idLie.
// Invariant : chaque relais a toujours une commande, donc tout actionneur
// installé sur ce matériel a forcément son id lié renseigné.
// ─────────────────────────────────────────────
static DataId commandIdForActuatorEntity(DataId entity)
{
    const NeoEntry* entry = Neo::find(entity);
    if (entry == nullptr || entry->relayCh == 0) return (DataId)0;
    return entry->idLie;
}

// ─────────────────────────────────────────────
// Helper : construit la carte HTML d'une vanne
// ─────────────────────────────────────────────
static String buildValveCard(const DataMeta& m)
{
    uint8_t idByte  = (uint8_t)m.id;
    uint8_t cmdByte = (uint8_t)commandIdForActuatorEntity(m.id);

    // État actuel
    String stateText     = "—";
    String stateClass    = "";
    String tsHtml        = "";
    int    intVal        = 0;

    LastDataForWeb d;
    if (WebServer::hasLastData(m.id, d)) {
        if (std::holds_alternative<float>(d.value)) {
            intVal = (int)(std::get<float>(d.value) + 0.5f);
        }
        stateText = stateLabel(m, intVal);
        if (intVal == 1) stateClass = " opened";
        tsHtml = timeHtmlActuators(d);
    }

    String html;
    html.reserve(1024);

    html += "<div class=\"valve-card\" id=\"card-";
    html += idByte; html += "\">";

    // Header : label + état
    html += "<div class=\"valve-header\">";
    html += "<div class=\"valve-label\">"; html += m.label; html += "</div>";
    html += "<div class=\"valve-state" + stateClass + "\" id=\"state-";
    html += idByte; html += "\">"; html += stateText; html += "</div>";
    html += "</div>";

    // Timestamp
    html += "<div class=\"valve-timestamp\">"; html += tsHtml; html += "</div>";

    // Boutons de durée (3 / 5 / 10 / 30 s)
    html += "<div class=\"duration-choices\" data-id=\""; html += idByte; html += "\">";
    html += "<button class=\"duration-btn selected\" data-sec=\"3\" onclick=\"selectDuration(this)\">3 s</button>";
    html += "<button class=\"duration-btn\"          data-sec=\"5\" onclick=\"selectDuration(this)\">5 s</button>";
    html += "<button class=\"duration-btn\"          data-sec=\"10\" onclick=\"selectDuration(this)\">10 s</button>";
    html += "<button class=\"duration-btn\"          data-sec=\"30\" onclick=\"selectDuration(this)\">30 s</button>";
    html += "</div>";

    // Bouton d'action — data-cmd-id = DataId CommandValveN (pour CSV /command),
    // data-id = DataId de la vanne (utilisé uniquement pour cibler card-N).
    html += "<button class=\"action-btn\" data-id=\""; html += idByte;
    html += "\" data-cmd-id=\""; html += cmdByte;
    html += "\" onclick=\"sendCommand(this)\">Arroser</button>";

    html += "</div>";
    return html;
}

// ─────────────────────────────────────────────
// Helper : carte HTML d'une lumière (ON/OFF)
//
// Pas de choix de durée : la commande pose l'état, le relais y reste.
// ─────────────────────────────────────────────
static String buildLightCard(const DataMeta& m)
{
    uint8_t idByte  = (uint8_t)m.id;
    uint8_t cmdByte = (uint8_t)commandIdForActuatorEntity(m.id);

    String stateText  = "—";
    String stateClass = "";
    String tsHtml     = "";
    int    intVal     = 0;

    LastDataForWeb d;
    if (WebServer::hasLastData(m.id, d)) {
        if (std::holds_alternative<float>(d.value)) {
            intVal = (int)(std::get<float>(d.value) + 0.5f);
        }
        stateText = stateLabel(m, intVal);
        if (intVal == 1) stateClass = " opened";
        tsHtml = timeHtmlActuators(d);
    }

    String html;
    html.reserve(768);

    html += "<div class=\"valve-card\" id=\"card-";
    html += idByte; html += "\">";

    html += "<div class=\"valve-header\">";
    html += "<div class=\"valve-label\">"; html += m.label; html += "</div>";
    html += "<div class=\"valve-state" + stateClass + "\" id=\"state-";
    html += idByte; html += "\">"; html += stateText; html += "</div>";
    html += "</div>";

    html += "<div class=\"valve-timestamp\">"; html += tsHtml; html += "</div>";

    html += "<div class=\"light-choices\">";
    html += "<button class=\"action-btn light-off\" data-id=\""; html += idByte;
    html += "\" data-cmd-id=\""; html += cmdByte;
    html += "\" data-etat=\"0\" onclick=\"sendLightCommand(this)\">Éteindre</button>";
    html += "<button class=\"action-btn\" data-id=\""; html += idByte;
    html += "\" data-cmd-id=\""; html += cmdByte;
    html += "\" data-etat=\"1\" onclick=\"sendLightCommand(this)\">Allumer</button>";
    html += "</div>";

    html += "</div>";
    return html;
}

// ─────────────────────────────────────────────
// Génération HTML de la page
// ─────────────────────────────────────────────
String PageActuators::getHtml()
{
    Console::info(TAG, "Génération page actionneurs");

    String valveCards;
    String lightCards;
    valveCards.reserve(4096);
    lightCards.reserve(1536);

    for (size_t i = 0; i < META_COUNT; i++) {
        const DataMeta& m = META[i];
        if (m.type != DataType::Actuator || m.nature != DataNature::etat) continue;

        const NeoEntry* neo = Neo::find(m.id);
        if (neo == nullptr || neo->relayCh == 0) continue;

        if (getMeta(neo->idLie).nature == DataNature::etat) {
            lightCards += buildLightCard(m);
        } else {
            valveCards += buildValveCard(m);
        }
    }

    String cards;
    if (valveCards.length() > 0) {
        cards += "<h2>Arrosage</h2>";
        cards += valveCards;
    }
    if (lightCards.length() > 0) {
        cards += "<h2>Lumières</h2>";
        cards += lightCards;
    }

    String html = R"HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Actionneurs - Serre de Marie-Pierre</title>
<style>
body { font-family: Arial; background: #1976d2; color: white; text-align: center; margin: 0; padding: 20px; }
h1 { background: #0d47a1; padding: 20px; border-radius: 10px; }
h2 { margin: 28px auto 8px; max-width: 600px; text-align: left; font-size: 1.15em; }
.card { background: rgba(255,255,255,0.2); margin: 20px auto; max-width: 600px; padding: 20px; border-radius: 15px; }
.subtext { font-size: 1.2em; margin-top: 15px; }
small { font-size: 0.8em; }

.valve-card {
  background: rgba(255,255,255,0.2);
  margin: 15px auto;
  max-width: 600px;
  padding: 20px;
  border-radius: 15px;
}

.valve-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
}

.valve-label {
  font-size: 1.1em;
  font-weight: bold;
}

.valve-state {
  font-size: 1em;
  padding: 4px 10px;
  border-radius: 6px;
  background: rgba(255,255,255,0.15);
}

.valve-state.opened {
  background: #4caf50;
  color: white;
  font-weight: bold;
}

.valve-timestamp {
  font-size: 0.85em;
  opacity: 0.75;
  margin-bottom: 12px;
  text-align: left;
}

.valve-timestamp em {
  font-style: italic;
}

.duration-choices {
  display: flex;
  gap: 8px;
  justify-content: center;
  margin-bottom: 12px;
}

.duration-btn {
  flex: 1;
  padding: 10px 4px;
  font-size: 1em;
  border: 2px solid rgba(255,255,255,0.4);
  background: transparent;
  color: white;
  border-radius: 8px;
  cursor: pointer;
  transition: background 0.2s, border-color 0.2s;
}

.duration-btn:hover {
  background: rgba(255,255,255,0.1);
}

.duration-btn.selected {
  background: #0d47a1;
  border-color: #0d47a1;
  font-weight: bold;
}

.action-btn {
  width: 100%;
  padding: 14px;
  font-size: 1.2em;
  font-weight: bold;
  background: #4caf50;
  color: white;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  transition: background 0.2s;
}

.action-btn:hover { background: #43a047; }
.action-btn:active { background: #388e3c; }
.action-btn:disabled { background: #9e9e9e; cursor: not-allowed; }

.light-choices {
  display: flex;
  gap: 8px;
}

.light-choices .action-btn { width: auto; flex: 1; }
.action-btn.light-off { background: #c62828; }
.action-btn.light-off:hover { background: #b71c1c; }

.back-link {
  display: inline-block;
  margin-top: 30px;
  color: white;
  text-decoration: underline;
  font-size: 1.1em;
}

#status-message {
  margin: 10px auto;
  max-width: 600px;
  min-height: 1.4em;
  font-size: 0.95em;
  opacity: 0;
  transition: opacity 0.3s;
}

#status-message.visible {
  opacity: 1;
}

@keyframes flash {
  0%   { background: rgba(255,255,255,0.45); }
  100% { background: rgba(255,255,255,0.2); }
}
.valve-card.flash {
  animation: flash 0.6s ease-out;
}
</style>

<script>
// Sélection d'une durée pour une vanne
function selectDuration(btn) {
  var parent = btn.parentElement;
  var btns = parent.querySelectorAll('.duration-btn');
  btns.forEach(function(b) { b.classList.remove('selected'); });
  btn.classList.add('selected');
}

// Envoi d'une commande via POST /command. Body = CSV 7 champs identique
// au format MQTT serre/cmd. Les 3 premiers champs (timestamp, VClock_available,
// VClock_reliable) sont laissés vides : la carte les remplit à réception.
// type=5 = CommandManual, valueType=0 = float (durée en secondes).
function sendCommand(btn) {
  var id    = btn.dataset.id;       // DataId de la vanne (pour l'UI seulement)
  var cmdId = btn.dataset.cmdId;    // DataId de la commande (CommandValveN)
  var card = document.getElementById('card-' + id);
  if (!card) return;

  var selected = card.querySelector('.duration-btn.selected');
  if (!selected) return;
  var sec = selected.dataset.sec;

  var status = document.getElementById('status-message');
  status.textContent = 'Envoi de la commande...';
  status.classList.add('visible');

  // Ordre des champs : timestamp,VClock_available,VClock_reliable,type,id,valueType,value
  var body = ',,,5,' + cmdId + ',0,' + sec;

  fetch('/command', {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: body
  })
  .then(function(response) {
    if (response.ok || response.status === 204) {
      status.textContent = '✅ Commande envoyée (cmdId=' + cmdId + ', ' + sec + ' s)';
      card.classList.remove('flash');
      void card.offsetWidth;
      card.classList.add('flash');
      // Rechargement dans 1,5 s pour voir le nouvel état
      setTimeout(function() { location.reload(); }, 1500);
    } else {
      return response.text().then(function(text) {
        status.textContent = '❌ Erreur : ' + (text || ('HTTP ' + response.status));
      });
    }
  })
  .catch(function(err) {
    status.textContent = '❌ Erreur réseau : ' + err;
  });
}

// Même CSV que sendCommand, la valeur portant l'état demandé au lieu d'une
// durée : 0 = éteindre, 1 = allumer.
function sendLightCommand(btn) {
  var id    = btn.dataset.id;
  var cmdId = btn.dataset.cmdId;
  var etat  = btn.dataset.etat;
  var card  = document.getElementById('card-' + id);
  if (!card) return;

  var status = document.getElementById('status-message');
  status.textContent = 'Envoi de la commande...';
  status.classList.add('visible');

  var body = ',,,5,' + cmdId + ',0,' + etat;

  fetch('/command', {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: body
  })
  .then(function(response) {
    if (response.ok || response.status === 204) {
      status.textContent = '✅ Commande envoyée (cmdId=' + cmdId + ', etat=' + etat + ')';
      card.classList.remove('flash');
      void card.offsetWidth;
      card.classList.add('flash');
      setTimeout(function() { location.reload(); }, 1500);
    } else {
      return response.text().then(function(text) {
        status.textContent = '❌ Erreur : ' + (text || ('HTTP ' + response.status));
      });
    }
  })
  .catch(function(err) {
    status.textContent = '❌ Erreur réseau : ' + err;
  });
}

// Rafraîchissement des âges relatifs
setInterval(function() {
  document.querySelectorAll('.age').forEach(function(e) {
    var ms = parseInt(e.dataset.ageMs);
    ms += 1000;
    e.dataset.ageMs = ms;

    var s = Math.floor(ms / 1000);
    var m = Math.floor(s / 60); s %= 60;
    var h = Math.floor(m / 60); m %= 60;

    e.textContent = 'Depuis ' +
      (h ? h + 'h ' : '') +
      (m ? m + 'm ' : '') +
      s + 's';
  });
}, 1000);

// Rafraîchissement périodique de la page (comme PagePrincipale)
setInterval(function() { location.reload(); }, 30000);
</script>
</head>
<body>

<h1>💧 Actionneurs</h1>

<div id="status-message"></div>

)HTML" + cards + R"HTML(

<a href="/" class="back-link">← Retour à la page principale</a>

</body>
</html>
)HTML";

    return html;
}