// Web/Pages/PageActuators.cpp
// Page de pilotage des actionneurs (vannes).
//
// Principes :
//  - Liste construite dynamiquement depuis META (type == Actuator, nature == etat).
//    Aucune vanne en dur : si on ajoute un actionneur dans DATA_ID_LIST,
//    la page le reflète automatiquement.
//  - État actuel lu via DataLogger::hasLastDataForWeb (vue RAM alimentée
//    par DataLogger::push côté ValveManager).
//  - Commande envoyée en POST local vers /actuators/open (pas de MQTT).
//  - Style aligné sur PagePrincipale (fond bleu, cartes transparentes).
#include "Web/Pages/PageActuators.h"

#include "Storage/DataLogger.h"
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
    if (d.UTC_available && d.UTC_reliable) {
        return formatUtcActuators(d.timestamp);
    }
    if (d.UTC_available && !d.UTC_reliable) {
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
// Helper : construit la carte HTML d'une vanne
// ─────────────────────────────────────────────
static String buildValveCard(const DataMeta& m)
{
    uint8_t idByte = (uint8_t)m.id;

    // État actuel
    String stateText     = "—";
    String stateClass    = "";
    String tsHtml        = "";
    int    intVal        = 0;

    LastDataForWeb d;
    if (DataLogger::hasLastDataForWeb(m.id, d)) {
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

    // Bouton d'action
    html += "<button class=\"action-btn\" data-id=\""; html += idByte;
    html += "\" onclick=\"sendCommand(this)\">Arroser</button>";

    html += "</div>";
    return html;
}

// ─────────────────────────────────────────────
// Génération HTML de la page
// ─────────────────────────────────────────────
String PageActuators::getHtml()
{
    Console::info(TAG, "Génération page actionneurs");

    // Construction de toutes les cartes vannes depuis META
    String cards;
    cards.reserve(4096);
    for (size_t i = 0; i < META_COUNT; i++) {
        const DataMeta& m = META[i];
        if (m.type == DataType::Actuator && m.nature == DataNature::etat) {
            cards += buildValveCard(m);
        }
    }

    String html = R"HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Arrosage - Serre de Marie-Pierre</title>
<style>
body { font-family: Arial; background: #1976d2; color: white; text-align: center; margin: 0; padding: 20px; }
h1 { background: #0d47a1; padding: 20px; border-radius: 10px; }
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

// Envoi d'une commande d'ouverture via POST HTTP local
function sendCommand(btn) {
  var id = btn.dataset.id;
  var card = document.getElementById('card-' + id);
  if (!card) return;

  var selected = card.querySelector('.duration-btn.selected');
  if (!selected) return;
  var sec = selected.dataset.sec;

  var status = document.getElementById('status-message');
  status.textContent = 'Envoi de la commande...';
  status.classList.add('visible');

  var body = 'id=' + encodeURIComponent(id) + '&duration=' + encodeURIComponent(sec);

  fetch('/actuators/open', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body
  })
  .then(function(response) {
    if (response.ok || response.status === 204) {
      status.textContent = '✅ Commande envoyée (vanne id=' + id + ', ' + sec + ' s)';
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

<h1>💧 Arrosage</h1>

<div id="status-message"></div>

)HTML" + cards + R"HTML(

<a href="/" class="back-link">← Retour à la page principale</a>

</body>
</html>
)HTML";

    return html;
}